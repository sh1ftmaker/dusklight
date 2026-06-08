/**
 * online/directory.cpp — game-side client for the room directory server.
 *
 * Two jobs:
 *   - fetch_rooms()           : clients query the directory to discover rooms.
 *   - run_host_registration() : hosts keep their room advertised on the directory.
 *
 * Discovery is decoupled from gameplay: once a client picks a room from the list
 * it connects directly to that host's ip:port via the normal transport in
 * online.cpp. This file therefore only ever exchanges small RoomInfo records with
 * the directory; it never carries gameplay traffic.
 *
 * Windows/winsock only for now (mirrors online.cpp); a no-op elsewhere until the
 * transport is abstracted cross-platform.
 */

#include "dusk/online.h"
#include "dusk/online_directory_client.h"
#include "dusk/logging.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#define DUSK_DIR_SOCKETS 1
#else
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#define DUSK_DIR_SOCKETS 0
#endif

namespace dusk::online::directory {
namespace {

#if DUSK_DIR_SOCKETS

bool send_all(socket_t s, const char* d, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, d + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}
bool recv_all(socket_t s, char* d, int len) {
    int got = 0;
    while (got < len) {
        int n = recv(s, d + got, len - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}
bool write_frame(socket_t s, uint8_t op, const void* data, uint32_t len) {
    FrameHeader h{kMagic, kVersion, op, len};
    if (!send_all(s, reinterpret_cast<const char*>(&h), sizeof(h))) return false;
    if (len && !send_all(s, reinterpret_cast<const char*>(data), (int)len)) return false;
    return true;
}
bool read_frame(socket_t s, uint8_t& op, std::vector<char>& payload) {
    FrameHeader h{};
    if (!recv_all(s, reinterpret_cast<char*>(&h), sizeof(h))) return false;
    if (h.magic != kMagic || h.version != kVersion) return false;
    if (h.len > sizeof(RoomInfo) * (kMaxRooms + 2)) return false;
    payload.resize(h.len);
    if (h.len && !recv_all(s, payload.data(), (int)h.len)) return false;
    op = h.op;
    return true;
}

// Connect to addr:port (IPv4 literal or hostname) with a bounded timeout so a dead
// directory never stalls the IO/registration thread for the full OS connect wait.
socket_t dial(const char* addr, uint16_t port, int timeoutMs = 2500) {
    char portstr[8];
    std::snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(addr, portstr, &hints, &res) != 0 || !res) return kInvalidSocket;

    socket_t out = kInvalidSocket;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        socket_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kInvalidSocket) continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        int r = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        bool ok = (r == 0);
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            if (select(0, nullptr, &wf, nullptr, &tv) == 1) {
                int err = 0, el = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
                ok = (err == 0);
            }
        }
        if (ok) {
            u_long blocking = 0;
            ioctlsocket(s, FIONBIO, &blocking);
            out = s;
            break;
        }
        closesocket(s);
    }
    freeaddrinfo(res);
    return out;
}

#endif  // DUSK_DIR_SOCKETS

}  // namespace

bool fetch_rooms(const char* addr, uint16_t port, std::vector<RoomInfo>& out) {
    out.clear();
#if DUSK_DIR_SOCKETS
    socket_t s = dial(addr, port);
    if (s == kInvalidSocket) return false;

    bool ok = write_frame(s, kMsgList, nullptr, 0);
    if (ok) {
        uint8_t op = 0;
        std::vector<char> payload;
        if (read_frame(s, op, payload) && op == kMsgRoomList && payload.size() >= sizeof(uint16_t)) {
            uint16_t count = 0;
            std::memcpy(&count, payload.data(), sizeof(count));
            const size_t need = sizeof(count) + (size_t)count * sizeof(RoomInfo);
            if (payload.size() >= need) {
                out.resize(count);
                if (count) {
                    std::memcpy(out.data(), payload.data() + sizeof(count),
                                (size_t)count * sizeof(RoomInfo));
                }
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
    }
    closesocket(s);
    return ok;
#else
    (void)addr;
    (void)port;
    return false;
#endif
}

void run_host_registration(std::string dirAddr, uint16_t dirPort, std::string roomName,
                           uint16_t gamePort, uint8_t maxPlayers,
                           std::function<bool()> keepRunning) {
#if DUSK_DIR_SOCKETS
    auto nap = [&](int beats) {
        for (int i = 0; i < beats && keepRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };

    while (keepRunning()) {
        socket_t s = dial(dirAddr.c_str(), dirPort);
        if (s == kInvalidSocket) {
            DuskLog.warn("[directory] can't reach {}:{}, retrying...", dirAddr, dirPort);
            nap(30);  // ~3s before another attempt
            continue;
        }
        DuskLog.info("[directory] advertising room '{}' on {}:{} (game port {})",
                     roomName, dirAddr, dirPort, gamePort);

        bool alive = true;
        while (alive && keepRunning()) {
            RoomInfo room{};
            room.id = 0;  // server assigns
            room.gamePort = gamePort;
            room.maxPlayers = maxPlayers;
            room.protocolVersion = kProtocolVersion;
            std::strncpy(room.name, roomName.c_str(), sizeof(room.name) - 1);
            room.curPlayers = (uint8_t)player_count();
            if (const PlayerState* me = local_player())
                std::memcpy(room.stage, me->stage, sizeof(room.stage));
            // host[] is left blank — the server fills it from our source address.

            alive = write_frame(s, kMsgRegister, &room, sizeof(room));
            nap(20);  // re-advertise every ~2s
        }
        closesocket(s);
        DuskLog.info("[directory] registration connection closed");
    }
#else
    (void)dirAddr; (void)dirPort; (void)roomName;
    (void)gamePort; (void)maxPlayers; (void)keepRunning;
#endif
}

}  // namespace dusk::online::directory
