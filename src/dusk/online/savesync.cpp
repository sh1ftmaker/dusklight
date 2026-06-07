/**
 * online/savesync.cpp — live world-state sync (host-authoritative)
 *
 * Owns opcode kOpSaveSync (37). See dusk/online_savesync.h for the design.
 *
 * Flow
 * ----
 *   Host, each frame  →  frame_update() gathers the synced save regions, diffs
 *                        against the last broadcast; if changed, sends kOpSaveSync.
 *   Client receives   →  on_savesync handler queues bytes (IO thread).
 *   Client, each frame→  frame_update() applies the queued regions (game thread).
 *
 * Wire format (little-endian, version 1)
 *   [0..3] magic 'DSVY' · [4] version=1 · [5..7] pad · [8..] region body
 *   body = event[256] ++ item[sizeof(dSv_player_item_c)] ++ getItem[sizeof(dSv_player_get_item_c)]
 */

#include "dusk/online.h"
#include "dusk/online_savesync.h"
#include "dusk/logging.h"
#include "d/d_com_inf_game.h"   // dComIfGs_getSaveData
#include "d/d_save.h"           // dSv_save_c, dSv_player_item_c, dSv_player_get_item_c

#include <cstring>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dusk::online {
namespace {

constexpr uint8_t  kOpSaveSync  = kFirstUserOpcode + 5;  // 37
constexpr uint32_t kSaveSyncMagic   = 0x44535659u;       // 'DSVY'
constexpr uint8_t  kSaveSyncVersion = 1;

// Region sizes (the global quest state — deliberately excludes vitals/counts).
constexpr size_t kEventSize   = 256;                          // dSv_event_c::mEvent
constexpr size_t kItemSize    = sizeof(dSv_player_item_c);    // 0x30
constexpr size_t kGetItemSize = sizeof(dSv_player_get_item_c);// 0x20
constexpr size_t kBodySize    = kEventSize + kItemSize + kGetItemSize;

constexpr size_t kOffMagic = 0;   // uint32
constexpr size_t kOffVer   = 4;   // uint8
constexpr size_t kOffBody  = 8;   // region body (after 3 pad bytes)
constexpr size_t kPayload  = kOffBody + kBodySize;

// Shared state: IO thread writes g_pending, game thread consumes it.
std::mutex           g_mutex;
std::vector<uint8_t> g_pending;
bool                 g_hasPending = false;

// Host-side: last broadcast region body, to diff against.
uint8_t g_lastSent[kBodySize];
bool    g_haveLast = false;

// Gather the synced regions into dst[kBodySize]; false if not in-game.
bool gather_regions(uint8_t* dst) {
    dSv_save_c* save = dComIfGs_getSaveData();
    if (!save) return false;
    size_t o = 0;
    std::memcpy(dst + o, save->getEvent().getPEventBit(), kEventSize);   o += kEventSize;
    std::memcpy(dst + o, &save->getPlayer().getItem(),    kItemSize);    o += kItemSize;
    std::memcpy(dst + o, &save->getPlayer().getGetItem(), kGetItemSize); o += kGetItemSize;
    return true;
}

// Apply region body src[kBodySize] back into the live save.
void scatter_regions(const uint8_t* src) {
    dSv_save_c* save = dComIfGs_getSaveData();
    if (!save) return;
    size_t o = 0;
    std::memcpy(save->getEvent().getPEventBit(), src + o, kEventSize);   o += kEventSize;
    std::memcpy(&save->getPlayer().getItem(),    src + o, kItemSize);    o += kItemSize;
    std::memcpy(&save->getPlayer().getGetItem(), src + o, kGetItemSize); o += kGetItemSize;
}

// IO-thread handler: client receives a sync, queues it for the game thread.
void on_savesync(uint8_t fromId, const uint8_t* data, uint32_t len) {
    if (mode() == Mode::Host) {
        // Host is the authority — ignore inbound state. (Client->host upstream
        // is future work; see PLAN §2.)
        return;
    }
    if (len < kPayload) {
        DuskLog.warn("[savesync] malformed packet from {} ({} bytes, need >= {})",
                     fromId, len, kPayload);
        return;
    }
    uint32_t magic;  std::memcpy(&magic, data + kOffMagic, sizeof(magic));
    uint8_t  ver = data[kOffVer];
    if (magic != kSaveSyncMagic) {
        DuskLog.warn("[savesync] bad magic 0x{:08X} from {}", magic, fromId);
        return;
    }
    if (ver != kSaveSyncVersion) {
        DuskLog.warn("[savesync] unsupported version {} from {}", ver, fromId);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.assign(data, data + len);
        g_hasPending = true;
    }
}

}  // anonymous namespace

namespace savesync {

void frame_update() {
    if (!is_active()) return;

    if (mode() == Mode::Host) {
        if (!is_connected()) return;

        uint8_t body[kBodySize];
        if (!gather_regions(body)) return;  // not in-game yet

        if (g_haveLast && std::memcmp(body, g_lastSent, kBodySize) == 0) {
            return;  // nothing changed
        }

        std::vector<uint8_t> buf(kPayload, 0u);
        std::memcpy(buf.data() + kOffMagic, &kSaveSyncMagic, sizeof(kSaveSyncMagic));
        buf[kOffVer] = kSaveSyncVersion;
        std::memcpy(buf.data() + kOffBody, body, kBodySize);
        send_message(kOpSaveSync, buf.data(), static_cast<uint32_t>(buf.size()));

        std::memcpy(g_lastSent, body, kBodySize);
        g_haveLast = true;
        DuskLog.info("[savesync] broadcast world state ({} bytes)", buf.size());
        return;
    }

    // Client: apply the most recent queued sync.
    std::vector<uint8_t> buf;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_hasPending) return;
        buf = std::move(g_pending);
        g_hasPending = false;
    }
    if (buf.size() < kPayload) return;
    scatter_regions(buf.data() + kOffBody);
    DuskLog.info("[savesync] applied world state ({} bytes)", buf.size());
}

}  // namespace savesync

namespace modules {
void init_savesync() {
    register_handler(kOpSaveSync, &on_savesync);
    DuskLog.info("[savesync] module registered (opcode={})", kOpSaveSync);
}
}  // namespace modules

}  // namespace dusk::online
