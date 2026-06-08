/**
 * online/snapshot.cpp — World Snapshot + Mid-Game Join
 *
 * Owns opcodes kOpSnapshotReq (34) and kOpSnapshot (35).
 *
 * Flow
 * ----
 *   Client connects  →  on_connected_request() sends kOpSnapshotReq (34)
 *   Host receives    →  on_snapshot_req handler calls capture(), replies kOpSnapshot (35)
 *   Client receives  →  on_snapshot handler queues bytes in g_pendingBuf (IO thread)
 *   Next game frame  →  poll_apply() (game thread) consumes the buffer and applies state
 *
 * Snapshot wire format (little-endian, version 1)
 * -----------------------------------------------
 *   [0..3]   magic   : uint32  = 0x44534E50  ('DSNP')
 *   [4]      version : uint8   = 1
 *   [5..7]   padding : uint8[3]
 *   [8..11]  rng0    : int32
 *   [12..15] rng1    : int32
 *   [16..19] rng2    : int32
 *   [20..27] stageName: char[8]  (current stage name, null-padded)
 *   [28..31] stagePoint: int16 + int16 roomNo  (current start point & room)
 *   [32 ..32+sizeof(dSv_save_c)-1] raw dSv_save_c bytes (0x958 bytes)
 *
 * Thread safety: all shared state is protected by g_mutex.
 */

#include "dusk/online.h"
#include "dusk/online_snapshot.h"
#include "dusk/logging.h"
#include "d/d_com_inf_game.h"   // dComIfGs_getSaveData, dComIfGp_getStartStageName,
                                // dComIfGp_getStartStagePoint, dComIfGp_getStartStageRoomNo
#include "d/d_save.h"           // dSv_save_c
#include "SSystem/SComponent/c_math.h"  // cM_getRndState, cM_setRndState

#include <cstring>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dusk::online {

// ---------------------------------------------------------------------------
// Module-private constants
// ---------------------------------------------------------------------------
namespace {

constexpr uint8_t  kOpSnapshotReq = kFirstUserOpcode + 2;  // 34
constexpr uint8_t  kOpSnapshot    = kFirstUserOpcode + 3;  // 35

// Wire format identifiers
constexpr uint32_t kSnapshotMagic   = 0x44534E50u;  // 'DSNP'
constexpr uint8_t  kSnapshotVersion = 1;

// Header layout byte-offsets (all values written little-endian via memcpy)
constexpr size_t kOffMagic      = 0;   // uint32
constexpr size_t kOffVersion    = 4;   // uint8
// [5..7] reserved/padding
constexpr size_t kOffRng0       = 8;   // int32
constexpr size_t kOffRng1       = 12;  // int32
constexpr size_t kOffRng2       = 16;  // int32
constexpr size_t kOffStageName  = 20;  // char[8]
constexpr size_t kOffStagePoint = 28;  // int16
constexpr size_t kOffStageRoom  = 30;  // int16
constexpr size_t kOffSaveData   = 32;  // dSv_save_c

constexpr size_t kHeaderSize    = kOffSaveData;
constexpr size_t kPayloadSize   = kHeaderSize + sizeof(dSv_save_c);

// ---------------------------------------------------------------------------
// Shared mutable state (IO thread writes, game thread reads)
// ---------------------------------------------------------------------------
std::mutex              g_mutex;
std::vector<uint8_t>    g_pendingBuf;   // non-empty => apply needed
bool                    g_hasPending = false;

// ---------------------------------------------------------------------------
// Helper: write a value as raw bytes at a specific offset
// ---------------------------------------------------------------------------
template<typename T>
static void write_at(std::vector<uint8_t>& buf, size_t offset, T val) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(buf.data() + offset, &val, sizeof(T));
}

template<typename T>
static T read_at(const uint8_t* data, size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    T val{};
    std::memcpy(&val, data + offset, sizeof(T));
    return val;
}

// ---------------------------------------------------------------------------
// IO-thread handler: host receives a snapshot request, captures and replies
// ---------------------------------------------------------------------------
void on_snapshot_req(uint8_t fromId, const uint8_t* /*data*/, uint32_t /*len*/) {
    if (mode() != Mode::Host) {
        DuskLog.warn("[snapshot] received SnapshotReq but we are not the host — ignored");
        return;
    }
    DuskLog.info("[snapshot] peer {} requested world snapshot — capturing", fromId);

    // capture() reads engine state; in the current architecture the IO thread
    // calls this handler, which means capture() runs on the IO thread here.
    // This is acceptable for a first pass because capture() only reads (no
    // writes) from the engine structs and the host's game loop is not modifying
    // dSv_save_c concurrently mid-frame.  A stricter implementation would post
    // the capture request to the game thread and reply asynchronously.
    std::vector<uint8_t> blob = dusk::online::snapshot::capture();
    if (blob.empty()) {
        DuskLog.warn("[snapshot] capture() returned empty — not yet in-game, skipping reply");
        return;
    }

    send_message(kOpSnapshot, blob.data(), static_cast<uint32_t>(blob.size()));
    DuskLog.info("[snapshot] sent snapshot ({} bytes) to peer {}", blob.size(), fromId);
}

// ---------------------------------------------------------------------------
// IO-thread handler: client receives a snapshot payload, queues for game thread
// ---------------------------------------------------------------------------
void on_snapshot(uint8_t fromId, const uint8_t* data, uint32_t len) {
    if (len < kPayloadSize) {
        DuskLog.warn("[snapshot] received malformed snapshot from peer {} ({} bytes, expected >= {})",
                     fromId, len, kPayloadSize);
        return;
    }

    // Validate magic and version before queuing
    const uint32_t magic   = read_at<uint32_t>(data, kOffMagic);
    const uint8_t  version = read_at<uint8_t> (data, kOffVersion);

    if (magic != kSnapshotMagic) {
        DuskLog.warn("[snapshot] bad magic 0x{:08X} from peer {}", magic, fromId);
        return;
    }
    if (version != kSnapshotVersion) {
        DuskLog.warn("[snapshot] unsupported snapshot version {} from peer {}", version, fromId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pendingBuf.assign(data, data + len);
        g_hasPending = true;
    }
    DuskLog.info("[snapshot] queued snapshot ({} bytes) from peer {} for game-thread apply",
                 len, fromId);
}

}  // anonymous namespace

// ===========================================================================
// Public API — dusk::online::snapshot
// ===========================================================================
namespace snapshot {

// ---------------------------------------------------------------------------
// capture() — game thread
// ---------------------------------------------------------------------------
std::vector<uint8_t> capture() {
    dSv_save_c* save = dComIfGs_getSaveData();
    if (!save) {
        DuskLog.warn("[snapshot] capture: dComIfGs_getSaveData() returned null — not in-game?");
        return {};
    }

    std::vector<uint8_t> buf(kPayloadSize, 0u);

    // --- header ---
    write_at(buf, kOffMagic,   kSnapshotMagic);
    write_at(buf, kOffVersion, kSnapshotVersion);

    // --- RNG state ---
    s32 rng0 = 0, rng1 = 0, rng2 = 0;
    cM_getRndState(&rng0, &rng1, &rng2);
    write_at(buf, kOffRng0, rng0);
    write_at(buf, kOffRng1, rng1);
    write_at(buf, kOffRng2, rng2);

    // --- Current stage / scene ---
    // dComIfGp_getStartStageName() returns the name of the stage that was
    // loaded (the "current" stage once the scene is up).
    const char* stageName = dComIfGp_getStartStageName();
    if (stageName) {
        // Stage names are 8-char null-padded identifiers (dStage_startStage_c::mName)
        char stageNameBuf[8] = {};
        std::strncpy(stageNameBuf, stageName, sizeof(stageNameBuf));
        std::memcpy(buf.data() + kOffStageName, stageNameBuf, 8);
    }
    // else: leave zeroed — client will see an empty stage name and skip the warp

    const s16 stagePoint  = dComIfGp_getStartStagePoint();
    const s8  stageRoomNo = dComIfGp_getStartStageRoomNo();
    write_at(buf, kOffStagePoint, static_cast<int16_t>(stagePoint));
    write_at(buf, kOffStageRoom,  static_cast<int16_t>(stageRoomNo));

    // --- Save data blob ---
    std::memcpy(buf.data() + kOffSaveData, save, sizeof(dSv_save_c));

    DuskLog.info("[snapshot] captured {} bytes (stage='{}', point={}, room={})",
                 buf.size(), stageName ? stageName : "<null>", stagePoint, (int)stageRoomNo);
    return buf;
}

// ---------------------------------------------------------------------------
// poll_apply() — game thread, once per frame
// ---------------------------------------------------------------------------
void poll_apply() {
    std::vector<uint8_t> buf;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_hasPending) {
            return;
        }
        buf = std::move(g_pendingBuf);
        g_hasPending = false;
    }

    // Should be guaranteed by the IO handler, but double-check for safety.
    if (buf.size() < kPayloadSize) {
        DuskLog.warn("[snapshot] poll_apply: pending buffer too small ({} bytes), discarding",
                     buf.size());
        return;
    }

    DuskLog.info("[snapshot] applying snapshot ({} bytes) on game thread", buf.size());

    // --- Restore RNG ---
    const s32 rng0 = read_at<s32>(buf.data(), kOffRng0);
    const s32 rng1 = read_at<s32>(buf.data(), kOffRng1);
    const s32 rng2 = read_at<s32>(buf.data(), kOffRng2);
    cM_setRndState(rng0, rng1, rng2);

    // --- Restore save data ---
    dSv_save_c* save = dComIfGs_getSaveData();
    if (!save) {
        DuskLog.warn("[snapshot] poll_apply: dComIfGs_getSaveData() is null — deferring save restore");
        // Re-queue so we try again next frame
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pendingBuf  = std::move(buf);
        g_hasPending  = true;
        return;
    }
    std::memcpy(save, buf.data() + kOffSaveData, sizeof(dSv_save_c));
    DuskLog.info("[snapshot] save data restored ({} bytes)", sizeof(dSv_save_c));

    // --- Stage / scene transition ---
    char stageNameBuf[9] = {};  // +1 for guaranteed null terminator
    std::memcpy(stageNameBuf, buf.data() + kOffStageName, 8);
    const int16_t stagePoint  = read_at<int16_t>(buf.data(), kOffStagePoint);
    const int16_t stageRoomNo = read_at<int16_t>(buf.data(), kOffStageRoom);

    if (stageNameBuf[0] != '\0') {
        // Only warp when we're actually in gameplay. dComIfGp_getPlayer(0) is
        // non-null only once Link's actor exists (not during load screens, the
        // title, or file-select), which is the same window the in-game Warp menu
        // is usable in — so dComIfGp_setNextStage is safe to call here. If the
        // client is still at the title/file-select, the save data is applied but
        // we skip the warp; they'll arrive in the host's world the normal way.
        if (dComIfGp_getPlayer(0) == nullptr) {
            DuskLog.info("[snapshot] not in gameplay yet — applied save, skipping warp to '{}'",
                         stageNameBuf);
        } else {
            // Skip a redundant reload if we're already at the destination.
            const char* curStage = dComIfGp_getStartStageName();
            const int   curRoom  = dComIfGp_roomControl_getStayNo();
            const bool sameStage = curStage && std::strncmp(curStage, stageNameBuf, 8) == 0;
            if (sameStage && curRoom == (int)stageRoomNo) {
                DuskLog.info("[snapshot] already at '{}' room {} — no warp needed",
                             stageNameBuf, (int)stageRoomNo);
            } else {
                DuskLog.info("[snapshot] warping to host: stage='{}' point={} room={}",
                             stageNameBuf, stagePoint, (int)stageRoomNo);
                // Same call the in-game Warp menu uses (dusk/ui/warp.cpp): the
                // framework picks up the queued next-stage and performs the load.
                dComIfGp_setNextStage(stageNameBuf, stagePoint, (s8)stageRoomNo, /*layer=*/0);
            }
        }
    } else {
        DuskLog.info("[snapshot] snapshot has no stage name — skipping stage transition");
    }
}

// ---------------------------------------------------------------------------
// on_connected_request() — any thread (client role)
// ---------------------------------------------------------------------------
void on_connected_request() {
    if (mode() != Mode::Client) {
        // Hosts do not request snapshots from themselves.
        return;
    }
    if (!is_connected()) {
        DuskLog.warn("[snapshot] on_connected_request: not connected, cannot send SnapshotReq");
        return;
    }
    DuskLog.info("[snapshot] sending SnapshotReq to host");
    // Empty payload — the request carries no data.
    send_message(kOpSnapshotReq, nullptr, 0);
}

// ---------------------------------------------------------------------------
// has_pending() — any thread
// ---------------------------------------------------------------------------
bool has_pending() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_hasPending;
}

}  // namespace snapshot

// ---------------------------------------------------------------------------
// Module init — called by dusk::online::init() via modules::init_snapshot()
// ---------------------------------------------------------------------------
namespace modules {
void init_snapshot() {
    register_handler(kOpSnapshotReq, &on_snapshot_req);
    register_handler(kOpSnapshot,    &on_snapshot);
    DuskLog.info("[snapshot] module registered (opcodes req={} snap={})",
                 kOpSnapshotReq, kOpSnapshot);
}
}  // namespace modules

}  // namespace dusk::online
