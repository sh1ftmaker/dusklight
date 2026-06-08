/**
 * dusk_directory — standalone room directory server for Dusk Online.
 *
 * A tiny TCP "phone book": game hosts REGISTER a room and clients LIST the rooms
 * to discover them. It is NOT a relay — clients connect directly to the host's
 * ip:port afterwards (see include/dusk/online_directory.h for the full rationale
 * and wire format). The only state is a table of live rooms, each tied to the
 * host's open registration connection: when that connection closes the room is
 * removed, so liveness needs no heartbeat timer.
 *
 * Usage:  dusk_directory [port]            (default 7778)
 *
 * Cross-platform (winsock / BSD sockets); thread-per-connection. Intended to run
 * on a small always-on host (a VPS, a LAN box) reachable by all players.
 */

#include "dusk/online_directory.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { if (s != kInvalidSocket) closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static void close_socket(socket_t s) { if (s != kInvalidSocket) close(s); }
#endif

using namespace dusk::online::directory;

namespace {

// ---- room table -----------------------------------------------------------
std::mutex g_mutex;
std::unordered_map<uint16_t, RoomInfo> g_rooms;  // keyed by assigned id
uint16_t g_nextId = 1;                            // 0 is reserved for "unassigned"

void log_line(const char* fmt, ...) {
    char ts[32];
    std::time_t t = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
    std::printf("[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

// ---- socket helpers -------------------------------------------------------
bool recv_all(socket_t s, char* d, int len) {
    int got = 0;
    while (got < len) {
        int n = (int)recv(s, d + got, len - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}
bool send_all(socket_t s, const char* d, int len) {
    int sent = 0;
    while (sent < len) {
        int n = (int)send(s, d + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// Reads one validated frame. Returns false on socket error or a bad/oversized
// frame. On success, op + payload are filled.
bool read_frame(socket_t s, uint8_t& op, std::vector<char>& payload) {
    FrameHeader h{};
    if (!recv_all(s, reinterpret_cast<char*>(&h), sizeof(h))) return false;
    if (h.magic != kMagic || h.version != kVersion) return false;
    if (h.len > sizeof(RoomInfo) * (kMaxRooms + 2)) return false;  // sanity bound
    payload.resize(h.len);
    if (h.len && !recv_all(s, payload.data(), (int)h.len)) return false;
    op = h.op;
    return true;
}
bool write_frame(socket_t s, uint8_t op, const void* data, uint32_t len) {
    FrameHeader h{kMagic, kVersion, op, len};
    if (!send_all(s, reinterpret_cast<const char*>(&h), sizeof(h))) return false;
    if (len && !send_all(s, reinterpret_cast<const char*>(data), (int)len)) return false;
    return true;
}

void fill_peer_ip(socket_t s, char out[46]) {
    sockaddr_storage ss{};
    socklen_t sl = sizeof(ss);
    out[0] = '\0';
    if (getpeername(s, reinterpret_cast<sockaddr*>(&ss), &sl) != 0) return;
    if (ss.ss_family == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(&ss);
        inet_ntop(AF_INET, &a->sin_addr, out, 46);
    } else if (ss.ss_family == AF_INET6) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
        inet_ntop(AF_INET6, &a->sin6_addr, out, 46);
    }
}

std::string sanitize(const char* s, size_t cap) {
    std::string out;
    for (size_t i = 0; i < cap && s[i]; ++i) {
        char c = s[i];
        out += (c >= 32 && c < 127) ? c : '?';
    }
    return out;
}

// ---- per-connection handler ----------------------------------------------
void handle_connection(socket_t peer) {
    int nodelay = 1;
    setsockopt(peer, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    char peerIp[46] = {0};  // zero-filled so bytes past the IP's null aren't leaked on the wire
    fill_peer_ip(peer, peerIp);

    uint16_t myRoomId = 0;  // this connection's registered room, if any

    uint8_t op;
    std::vector<char> payload;
    while (read_frame(peer, op, payload)) {
        if (op == kMsgRegister) {
            if (payload.size() < sizeof(RoomInfo)) break;
            RoomInfo room{};
            std::memcpy(&room, payload.data(), sizeof(room));
            std::memcpy(room.host, peerIp, sizeof(room.host));  // trust the socket, not the client
            room.host[sizeof(room.host) - 1] = '\0';

            std::lock_guard<std::mutex> lk(g_mutex);
            if (myRoomId == 0) {
                myRoomId = g_nextId++;
                if (g_nextId == 0) g_nextId = 1;  // wrap past the reserved 0
                room.id = myRoomId;
                g_rooms[myRoomId] = room;
                log_line("room %u opened: \"%s\" @ %s:%u (%u/%u) stage=%s",
                         myRoomId, sanitize(room.name, sizeof(room.name)).c_str(),
                         peerIp, room.gamePort, room.curPlayers, room.maxPlayers,
                         sanitize(room.stage, sizeof(room.stage)).c_str());
            } else {
                room.id = myRoomId;
                g_rooms[myRoomId] = room;  // update player count / stage
            }
        } else if (op == kMsgList) {
            std::vector<RoomInfo> snapshot;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                snapshot.reserve(g_rooms.size());
                for (auto& kv : g_rooms) {
                    snapshot.push_back(kv.second);
                    if (snapshot.size() >= kMaxRooms) break;
                }
            }
            std::vector<char> out;
            uint16_t count = (uint16_t)snapshot.size();
            out.resize(sizeof(count) + snapshot.size() * sizeof(RoomInfo));
            std::memcpy(out.data(), &count, sizeof(count));
            if (!snapshot.empty()) {
                std::memcpy(out.data() + sizeof(count), snapshot.data(),
                            snapshot.size() * sizeof(RoomInfo));
            }
            if (!write_frame(peer, kMsgRoomList, out.data(), (uint32_t)out.size())) break;
            log_line("listed %u room(s) to %s", count, peerIp);
        }
        // Unknown ops are ignored (forward-compat).
    }

    if (myRoomId != 0) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_rooms.erase(myRoomId);
        log_line("room %u closed (host %s disconnected)", myRoomId, peerIp);
    }
    close_socket(peer);
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = kDefaultPort;
    if (argc > 1) {
        int v = std::atoi(argv[1]);
        if (v > 0 && v < 65536) port = (uint16_t)v;
    }

#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        std::fprintf(stderr, "socket() failed\n");
        return 1;
    }
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener, 16) != 0) {
        std::fprintf(stderr, "bind/listen failed on port %u\n", port);
        return 1;
    }

    log_line("dusk_directory listening on port %u (protocol v%u)", port, kVersion);

    while (true) {
        socket_t peer = accept(listener, nullptr, nullptr);
        if (peer == kInvalidSocket) continue;
        std::thread(handle_connection, peer).detach();
    }
    // unreachable
}
