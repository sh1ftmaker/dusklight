#ifndef DUSK_ONLINE_DIRECTORY_CLIENT_H
#define DUSK_ONLINE_DIRECTORY_CLIENT_H

#include <functional>
#include <string>
#include <vector>

#include "dusk/online_directory.h"

// Game-side helpers for talking to the room directory server. Implemented in
// src/dusk/online/directory.cpp; NOT used by the standalone server (which only
// needs the wire structs in online_directory.h).
namespace dusk::online::directory {

// One-shot blocking query: connect to the directory, request the room list, and
// fill `out`. Returns false on any socket/protocol error. `addr` may be an IPv4
// literal or a hostname. Safe to call repeatedly (e.g. from the IO thread).
bool fetch_rooms(const char* addr, uint16_t port, std::vector<RoomInfo>& out);

// Host registration loop: keeps a room registered with the directory until
// `keepRunning()` returns false. Reconnects if the directory drops, and re-sends
// an updated RoomInfo (current player count + stage) every couple of seconds.
// Pulls the live counts from the online core, so it must run while the game is up.
void run_host_registration(std::string dirAddr, uint16_t dirPort, std::string roomName,
                           uint16_t gamePort, uint8_t maxPlayers,
                           std::function<bool()> keepRunning);

}  // namespace dusk::online::directory

#endif  // DUSK_ONLINE_DIRECTORY_CLIENT_H
