/**
 * online/ping.cpp — map "look here" pings (opcode kFirstUserOpcode + 7 = 39).
 *
 * A player broadcasts a single world position; every peer (and the sender)
 * shows a fading marker there for kMarkerLifetime seconds. Markers are keyed by
 * sender id, so each player has at most one active marker — a fresh ping simply
 * replaces their previous one. The renderer (online/ui.cpp) projects the marker
 * to screen using the same path as the nameplates.
 *
 * Wire format: 3 little-endian float32 (x, y, z). Sender identity comes from the
 * bus fromId argument.
 *
 * Thread safety: on_marker() runs on the IO thread; send_marker(), update(), and
 * get_markers() run on the game thread. All touch g_markers under g_mutex.
 */

#include "dusk/online.h"
#include "dusk/online_ping.h"
#include "dusk/online_chat.h"
#include "dusk/logging.h"

#include <cstring>
#include <mutex>
#include <string>

namespace dusk::online {
namespace {

constexpr uint8_t kOpPingMarker = kFirstUserOpcode + 7;  // 39

std::mutex   g_mutex;
ping::Marker g_markers[kMaxPlayers] = {};  // indexed by sender id; ttl<=0 = idle

void place(uint8_t fromId, const float pos[3]) {
    if (fromId >= kMaxPlayers) return;
    uint8_t r = 255, g = 255, b = 255;
    if (const PlayerState* p = player_by_id(fromId)) {
        r = p->colorR;
        g = p->colorG;
        b = p->colorB;
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    ping::Marker& m = g_markers[fromId];
    m.pos[0] = pos[0];
    m.pos[1] = pos[1];
    m.pos[2] = pos[2];
    m.r = r;
    m.g = g;
    m.b = b;
    m.fromId = fromId;
    m.ttl = ping::kMarkerLifetime;
}

// IO thread: a peer pinged a location.
void on_marker(uint8_t fromId, const uint8_t* data, uint32_t len) {
    if (data == nullptr || len < 3 * sizeof(float)) return;
    float pos[3];
    std::memcpy(pos, data, sizeof(pos));
    place(fromId, pos);

    const PlayerState* p = player_by_id(fromId);
    const char* name = (p && p->name[0]) ? p->name : "A player";
    std::string notice = std::string(name) + " pinged a location";
    chat::system_line(notice.c_str());
}

}  // namespace

namespace ping {

void send_marker(const float pos[3]) {
    if (!is_connected()) return;
    send_message(kOpPingMarker, pos, 3 * sizeof(float));
    place((uint8_t)local_id(), pos);  // show our own ping immediately
}

void update(float dt) {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& m : g_markers) {
        if (m.ttl > 0.0f) m.ttl -= dt;
    }
}

int get_markers(Marker* out, int max) {
    if (out == nullptr || max <= 0) return 0;
    std::lock_guard<std::mutex> lk(g_mutex);
    int n = 0;
    for (const auto& m : g_markers) {
        if (m.ttl > 0.0f && n < max) out[n++] = m;
    }
    return n;
}

}  // namespace ping

namespace modules {
void init_ping() {
    register_handler(kOpPingMarker, &on_marker);
    DuskLog.info("[ping] module registered (opcode {})", kOpPingMarker);
}
}  // namespace modules

}  // namespace dusk::online
