/**
 * online/desync.cpp — desync detector (TPOnline Server$DesyncDetectorNode).
 *
 * Owns opcode kOpChecksum (33). Each sim tick every peer reports its RNG triple
 * (r0,r1,r2 from c_math.cpp). Compare against the peer's value for the same tick;
 * N consecutive mismatches => flag a desync (later: force an area resync).
 *
 * Implementation notes:
 *  - Expose: void submit_local(uint64_t tick, int32_t r0, int32_t r1, int32_t r2);
 *    the core sim loop calls this each tick (a call site is wired in by core).
 *  - Send your triple via send_message(kOpChecksum, ...); the handler stores the
 *    peer's latest {tick, r0,r1,r2}.
 *  - Compare same-tick triples; expose bool is_desynced() and the diverging tick
 *    for an overlay/log. Guard shared state with a mutex (handler = IO thread).
 *  - RNG accessor lives in c_math.cpp: cM_getRndState(&r0,&r1,&r2).
 */

#include "dusk/online_desync.h"
#include "dusk/online.h"
#include "dusk/logging.h"
#include "SSystem/SComponent/c_math.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <atomic>

namespace dusk::online {
namespace {

// ---- wire format -------------------------------------------------------

// Packed struct sent over the wire for kOpChecksum messages.
// All fields are in native byte order (peers share the same platform for PC).
#pragma pack(push, 1)
struct ChecksumMsg {
    uint64_t tick;
    int32_t  r0;
    int32_t  r1;
    int32_t  r2;
};
#pragma pack(pop)

static_assert(sizeof(ChecksumMsg) == 20, "ChecksumMsg layout mismatch");

constexpr uint8_t kOpChecksum = kFirstUserOpcode + 1;  // 33

// ---- internal state ----------------------------------------------------

struct Triple {
    int32_t r0, r1, r2;
};

// Protects all mutable state below. on_checksum runs on the IO thread;
// submit_local and the public query functions run on the sim thread.
static std::mutex g_mutex;

// Local RNG history keyed by tick. Trimmed to the most recent kHistoryCap
// ticks so memory is bounded even if the peer goes silent.
static constexpr size_t kHistoryCap = 256;
static std::unordered_map<uint64_t, Triple> g_localHistory;

// Desync bookkeeping.
static int      g_mismatchCount  = 0;
static bool     g_desynced       = false;
static uint64_t g_firstDesyncTick = 0;

// ---- helpers -----------------------------------------------------------

// Remove entries more than kHistoryCap ticks behind the newest stored tick
// to prevent the map from growing without bound.
static void trim_history_locked(uint64_t newest_tick) {
    if (g_localHistory.size() < kHistoryCap) return;

    // Find the oldest keys relative to newest_tick.
    const uint64_t cutoff = (newest_tick >= kHistoryCap)
                            ? (newest_tick - kHistoryCap)
                            : 0u;

    for (auto it = g_localHistory.begin(); it != g_localHistory.end(); ) {
        if (it->first <= cutoff) {
            it = g_localHistory.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- handler (IO thread) -----------------------------------------------

static void on_checksum(uint8_t fromId, const uint8_t* data, uint32_t len) {
    if (len < sizeof(ChecksumMsg)) return;

    ChecksumMsg msg{};
    std::memcpy(&msg, data, sizeof(ChecksumMsg));

    std::lock_guard<std::mutex> lock(g_mutex);

    // If we already flagged a desync we stop processing further.
    if (g_desynced) return;

    auto it = g_localHistory.find(msg.tick);
    if (it == g_localHistory.end()) {
        // We haven't computed this tick yet (or it was trimmed). Nothing to compare.
        return;
    }

    const Triple& local = it->second;
    const bool match = (local.r0 == msg.r0 &&
                        local.r1 == msg.r1 &&
                        local.r2 == msg.r2);

    if (!match) {
        ++g_mismatchCount;
        if (g_firstDesyncTick == 0) {
            g_firstDesyncTick = msg.tick;
        }
        // Log once per mismatch, rate-limited to first kDesyncThreshold events.
        if (g_mismatchCount <= desync::kDesyncThreshold) {
            DuskLog.warn("[desync] tick {} peer {} mismatch: local=({},{},{}) remote=({},{},{})",
                         msg.tick, static_cast<int>(fromId),
                         local.r0, local.r1, local.r2,
                         msg.r0,   msg.r1,   msg.r2);
        }
        if (g_mismatchCount >= desync::kDesyncThreshold) {
            g_desynced = true;
            DuskLog.warn("[desync] DESYNC DETECTED after {} mismatches (first at tick {})",
                         g_mismatchCount, g_firstDesyncTick);
        }
    } else {
        // Agreement on this tick: decay the counter toward zero.
        if (g_mismatchCount > 0) {
            --g_mismatchCount;
        }
    }
}

}  // namespace

// ---- public API (dusk::online::desync namespace) -----------------------

namespace desync {

void submit_local(uint64_t tick) {
    if (!is_connected()) return;

    // Sample the RNG state on the sim thread — this is the authoritative value
    // for this tick before any further RNG draws happen this frame.
    s32 r0 = 0, r1 = 0, r2 = 0;
    cM_getRndState(&r0, &r1, &r2);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        trim_history_locked(tick);
        g_localHistory[tick] = Triple{r0, r1, r2};
    }

    // Broadcast to all peers.
    ChecksumMsg msg{};
    msg.tick = tick;
    msg.r0   = r0;
    msg.r1   = r1;
    msg.r2   = r2;
    send_message(kOpChecksum, &msg, static_cast<uint32_t>(sizeof(msg)));
}

bool is_desynced() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_desynced;
}

uint64_t first_desync_tick() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_firstDesyncTick;
}

void reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_localHistory.clear();
    g_mismatchCount   = 0;
    g_desynced        = false;
    g_firstDesyncTick = 0;
}

}  // namespace desync

// ---- module init -------------------------------------------------------

namespace modules {
void init_desync() {
    register_handler(kOpChecksum, &on_checksum);
}
}  // namespace modules

}  // namespace dusk::online
