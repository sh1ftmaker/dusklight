/**
 * test_input.cpp — synthetic controller injection over UDP (see test_input.h).
 * Engine-free: depends only on platform sockets + the standard library.
 */

#include "dusk/test_input.h"
#include "dusk/logging.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#define DUSK_TI_SOCKETS 1
#else
#define DUSK_TI_SOCKETS 0
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

namespace dusk::test_input {
namespace {

constexpr uint32_t kMagic = 0x4e495444;  // 'DTIN'
constexpr auto kStaleTimeout = std::chrono::milliseconds(1000);

#pragma pack(push, 1)
struct Packet {
    uint32_t magic;
    uint32_t buttons;
    float stickX, stickY;
    float cStickX, cStickY;
    float triggerL, triggerR;
};
#pragma pack(pop)

bool g_active = false;
uint16_t g_port = 0;

std::thread g_thread;
std::atomic<bool> g_running{false};
socket_t g_sock = kInvalidSocket;

std::mutex g_mutex;
InputState g_state;
std::chrono::steady_clock::time_point g_lastPacket;
bool g_haveState = false;

#if DUSK_TI_SOCKETS
void io_thread_main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        DuskLog.warn("[test_input] WSAStartup failed");
        return;
    }
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSocket) {
        WSACleanup();
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(g_port);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        DuskLog.warn("[test_input] bind failed on UDP port {}", g_port);
        closesocket(s);
        WSACleanup();
        return;
    }
    // 200 ms recv timeout so we can notice shutdown.
    DWORD tv = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    g_sock = s;
    DuskLog.info("[test_input] listening for synthetic input on UDP 127.0.0.1:{}", g_port);

    Packet pkt{};
    while (g_running.load()) {
        int n = recv(s, (char*)&pkt, sizeof(pkt), 0);
        if (n == (int)sizeof(pkt) && pkt.magic == kMagic) {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_state.buttons = pkt.buttons;
            g_state.stickX = pkt.stickX;
            g_state.stickY = pkt.stickY;
            g_state.cStickX = pkt.cStickX;
            g_state.cStickY = pkt.cStickY;
            g_state.triggerL = pkt.triggerL;
            g_state.triggerR = pkt.triggerR;
            g_lastPacket = std::chrono::steady_clock::now();
            g_haveState = true;
        }
    }
    closesocket(s);
    g_sock = kInvalidSocket;
    WSACleanup();
}
#endif

}  // namespace

void init() {
    const char* portEnv = std::getenv("DUSK_INPUT_PORT");
    if (!portEnv || !portEnv[0]) {
        return;
    }
    int p = std::atoi(portEnv);
    if (p <= 0 || p >= 65536) {
        return;
    }
    g_port = (uint16_t)p;
    g_active = true;
#if DUSK_TI_SOCKETS
    g_running.store(true);
    g_thread = std::thread(io_thread_main);
#else
    g_active = false;
#endif
}

void shutdown() {
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
    g_active = false;
}

bool active() { return g_active; }

bool get_state(InputState* out) {
    if (!g_active || !out) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_haveState) return false;
    if (std::chrono::steady_clock::now() - g_lastPacket > kStaleTimeout) {
        return false;  // stale — revert to physical input
    }
    *out = g_state;
    return true;
}

}  // namespace dusk::test_input
