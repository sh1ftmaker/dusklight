/**
 * online/enemy.cpp — host-authoritative enemy & boss sync
 *
 * Owns opcode kOpEnemySync (38). See dusk/online_enemy.h for the design.
 *
 * Flow
 * ----
 *   Host, each frame  →  frame_update() walks the actor list, collects every
 *                        stage-placed enemy's (pos, facing, health), and (throttled)
 *                        broadcasts the table as kOpEnemySync.
 *   Client receives   →  on_enemysync handler queues the bytes (IO thread).
 *   Client, each frame→  frame_update() parses the queued table and walks its own
 *                        actor list, overwriting matched enemies (game thread).
 *
 * Wire format (little-endian, version 1)
 *   [0..3]  magic 'DENY' · [4] version=1 · [5] pad · [6..7] count(u16)
 *   then `count` Wire entries (kEntrySize bytes each).
 */

#include "dusk/online.h"
#include "dusk/online_enemy.h"
#include "dusk/logging.h"

#include "f_op/f_op_actor.h"        // fopAc_ac_c, fopAc_ENEMY_e
#include "f_op/f_op_actor_mng.h"    // fopAcM_* accessors, dComIfGp_getPlayer
#include "f_op/f_op_actor_iter.h"   // fopAcIt_Executor

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace dusk::online {
namespace {

constexpr uint8_t  kOpEnemySync   = kFirstUserOpcode + 6;  // 38
constexpr uint32_t kEnemyMagic    = 0x59454E44u;           // 'DENY' (LE)
constexpr uint8_t  kEnemyVersion  = 1;

// Cap how many enemies we replicate per frame. Rooms rarely exceed this; extras
// are dropped (logged once). Bump if a boss arena packs more live enemies.
constexpr int kMaxEnemies = 64;

// Send at ~30 Hz (every other 60 Hz frame) to halve bandwidth — enemy motion is
// smooth enough at 30 Hz and clients still run local AI between updates.
constexpr int kSendEveryNFrames = 2;

#pragma pack(push, 1)
struct WireEnemy {
    int16_t  profName;  // fpcM_GetProfName — actor type, part of the net key
    uint16_t setID;     // stage placement id, stable across peers
    int8_t   room;      // current room number, disambiguates same-id actors
    int8_t   pad;
    float    pos[3];
    int16_t  angleY;    // shape_angle.y (facing)
    int16_t  health;    // fopAc_ac_c::health (0x562)
};  // 22 bytes
#pragma pack(pop)

constexpr size_t kEntrySize = sizeof(WireEnemy);
static_assert(kEntrySize == 22, "WireEnemy must be tightly packed");

constexpr size_t kHdrSize  = 8;  // magic(4) ver(1) pad(1) count(2)
constexpr size_t kOffMagic = 0;
constexpr size_t kOffVer   = 4;
constexpr size_t kOffCount  = 6;

// ---- shared state: IO thread queues, game thread consumes ----
std::mutex           g_mutex;
std::vector<uint8_t> g_pending;
bool                 g_hasPending = false;

// ---- host gather scratch (game thread only) ----
WireEnemy g_gather[kMaxEnemies];
int       g_gatherCount = 0;
bool      g_gatherTruncated = false;
int       g_frameCounter = 0;

// ---- client apply scratch (game thread only) ----
WireEnemy g_apply[kMaxEnemies];
int       g_applyCount = 0;

inline bool is_syncable_enemy(fopAc_ac_c* ac) {
    if (ac == nullptr) return false;
    if (fopAcM_GetGroup(ac) != fopAc_ENEMY_e) return false;
    // Skip actors without a stable stage id (dynamically spawned). 0xFFFF is the
    // "no set id" sentinel used by fast/child creation.
    if (ac->setID == 0xFFFF) return false;
    return true;
}

// Host: collect one enemy into g_gather.
int gather_cb(void* actorv, void* /*data*/) {
    fopAc_ac_c* ac = static_cast<fopAc_ac_c*>(actorv);
    if (!is_syncable_enemy(ac)) return 0;  // continue
    if (g_gatherCount >= kMaxEnemies) {
        g_gatherTruncated = true;
        return 0;
    }
    WireEnemy& e = g_gather[g_gatherCount++];
    e.profName = fopAcM_GetProfName(ac);
    e.setID    = ac->setID;
    e.room     = fopAcM_GetRoomNo(ac);
    e.pad      = 0;
    e.pos[0]   = ac->current.pos.x;
    e.pos[1]   = ac->current.pos.y;
    e.pos[2]   = ac->current.pos.z;
    e.angleY   = ac->shape_angle.y;
    e.health   = ac->health;
    return 0;  // continue
}

inline bool same_key(const WireEnemy& a, fopAc_ac_c* ac) {
    return a.setID == ac->setID &&
           a.profName == fopAcM_GetProfName(ac) &&
           a.room == fopAcM_GetRoomNo(ac);
}

// Client: find this actor in g_apply and overwrite its state.
int apply_cb(void* actorv, void* /*data*/) {
    fopAc_ac_c* ac = static_cast<fopAc_ac_c*>(actorv);
    if (!is_syncable_enemy(ac)) return 0;
    for (int i = 0; i < g_applyCount; ++i) {
        const WireEnemy& e = g_apply[i];
        if (!same_key(e, ac)) continue;
        ac->current.pos.set(e.pos[0], e.pos[1], e.pos[2]);
        ac->shape_angle.y = e.angleY;
        ac->current.angle.y = e.angleY;
        ac->health = e.health;  // 0 lets the actor's own death logic take over
        break;
    }
    return 0;
}

// IO-thread handler: client queues the most recent table.
void on_enemysync(uint8_t fromId, const uint8_t* data, uint32_t len) {
    if (mode() == Mode::Host) return;  // host is authoritative; ignore inbound
    if (len < kHdrSize) {
        DuskLog.warn("[enemy] short packet from {} ({} bytes)", fromId, len);
        return;
    }
    uint32_t magic;
    std::memcpy(&magic, data + kOffMagic, sizeof(magic));
    if (magic != kEnemyMagic) {
        DuskLog.warn("[enemy] bad magic 0x{:08X} from {}", magic, fromId);
        return;
    }
    if (data[kOffVer] != kEnemyVersion) {
        DuskLog.warn("[enemy] unsupported version {} from {}", data[kOffVer], fromId);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.assign(data, data + len);
        g_hasPending = true;
    }
}

bool in_game() { return dComIfGp_getPlayer(0) != nullptr; }

}  // anonymous namespace

namespace enemy {

void frame_update() {
    if (!is_active() || !is_connected() || !in_game()) return;

    if (mode() == Mode::Host) {
        if (++g_frameCounter < kSendEveryNFrames) return;
        g_frameCounter = 0;

        g_gatherCount = 0;
        g_gatherTruncated = false;
        fopAcIt_Executor(reinterpret_cast<fopAcIt_ExecutorFunc>(&gather_cb), nullptr);
        if (g_gatherTruncated) {
            DuskLog.warn("[enemy] enemy table TRUNCATED at {} (raise kMaxEnemies)", kMaxEnemies);
        }
        if (g_gatherCount == 0) return;

        const size_t payload = kHdrSize + static_cast<size_t>(g_gatherCount) * kEntrySize;
        std::vector<uint8_t> buf(payload, 0u);
        std::memcpy(buf.data() + kOffMagic, &kEnemyMagic, sizeof(kEnemyMagic));
        buf[kOffVer] = kEnemyVersion;
        uint16_t count = static_cast<uint16_t>(g_gatherCount);
        std::memcpy(buf.data() + kOffCount, &count, sizeof(count));
        std::memcpy(buf.data() + kHdrSize, g_gather, static_cast<size_t>(g_gatherCount) * kEntrySize);
        send_message(kOpEnemySync, buf.data(), static_cast<uint32_t>(buf.size()));
        return;
    }

    // Client: apply the most recent queued table.
    std::vector<uint8_t> buf;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_hasPending) return;
        buf = std::move(g_pending);
        g_hasPending = false;
    }
    if (buf.size() < kHdrSize) return;
    uint16_t count = 0;
    std::memcpy(&count, buf.data() + kOffCount, sizeof(count));
    if (count > kMaxEnemies) count = kMaxEnemies;
    const size_t need = kHdrSize + static_cast<size_t>(count) * kEntrySize;
    if (buf.size() < need) {
        DuskLog.warn("[enemy] truncated table ({} bytes, need {})", buf.size(), need);
        return;
    }
    std::memcpy(g_apply, buf.data() + kHdrSize, static_cast<size_t>(count) * kEntrySize);
    g_applyCount = count;
    fopAcIt_Executor(reinterpret_cast<fopAcIt_ExecutorFunc>(&apply_cb), nullptr);
}

}  // namespace enemy

namespace modules {
void init_enemy() {
    register_handler(kOpEnemySync, &on_enemysync);
    DuskLog.info("[enemy] module registered (opcode={})", kOpEnemySync);
}
}  // namespace modules

}  // namespace dusk::online
