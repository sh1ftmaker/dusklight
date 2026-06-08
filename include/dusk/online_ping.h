#ifndef DUSK_ONLINE_PING_H
#define DUSK_ONLINE_PING_H

#include <cstdint>

// Map "look here" pings: a player broadcasts a world position and every peer
// shows a fading marker there for a few seconds. One active marker per player
// (a new ping replaces that player's previous one). Opcode kFirstUserOpcode+7.
namespace dusk::online::ping {

struct Marker {
    float   pos[3];
    uint8_t r, g, b;
    uint8_t fromId;
    float   ttl;  // seconds of life remaining (> 0 means active)
};

// Total lifetime of a freshly placed marker, in seconds.
constexpr float kMarkerLifetime = 6.0f;

// Broadcast a ping at the given world position; also shows it locally.
void send_marker(const float pos[3]);

// Game thread: decay active markers by dt seconds. Call once per frame.
void update(float dt);

// Game thread: copy active markers into out (up to max); returns the count.
int get_markers(Marker* out, int max);

}  // namespace dusk::online::ping

#endif  // DUSK_ONLINE_PING_H
