/**
 * online.cpp — Dusklight online core.
 *
 * Transport (host/client TCP) + framed opcode message bus + N-player table +
 * identity. Feature modules live under src/dusk/online/ and plug in via the bus.
 * Engine-agnostic: depends only on platform sockets + the standard library.
 */

#include "dusk/online.h"
#include "dusk/logging.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#define DUSK_ONLINE_SOCKETS 1
#else
#define DUSK_ONLINE_SOCKETS 0
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

namespace dusk::online {
namespace {

// --- config ---
Mode g_mode = Mode::Off;
std::string g_hostAddr = "127.0.0.1";
uint16_t g_port = 7777;
std::string g_localName;
uint8_t g_localColor[3] = {255, 255, 255};

int g_localId = 0;
int g_remoteId = 1;

// --- player table ---
std::mutex g_playersMutex;
PlayerState g_players[kMaxPlayers];

// --- skeletal pose table (puppet animation) ---
struct PlayerPose {
    bool valid = false;
    int jointCount = 0;
    float baseTR[12] = {0};
    float joints[kMaxJoints * 12] = {0};
};
std::mutex g_poseMutex;
PlayerPose g_poses[kMaxPlayers];

// --- transport ---
std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_connected{false};
std::atomic<uint64_t> g_localTick{0};

std::mutex g_sendMutex;
socket_t g_peer = kInvalidSocket;

std::string g_statusText = "off";
std::mutex g_statusMutex;

// --- message bus ---
MessageHandler g_handlers[256] = {nullptr};

// PLAYER_STATE wire payload (same-arch byte copy; both peers are the same build).
#pragma pack(push, 1)
struct PlayerStateMsg {
    uint64_t tick;
    float pos[3];
    int16_t angleY;
    uint8_t isWolf;
    uint16_t animId;
    float animFrame;
    uint8_t input[kInputBytes];
};
struct HelloMsg {
    uint32_t version;
    uint8_t id;
    uint8_t color[3];
    char name[24];
};
#pragma pack(pop)

void set_status(std::string s) {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    g_statusText = std::move(s);
}

const char* kNamePool[] = {
    "Hero",   "Ordon",  "Faron",  "Eldin",  "Lanayru", "Hyrule", "Twili", "Ilia",
    "Rusl",   "Colin",  "Beth",   "Talo",   "Malo",    "Epona",  "Midna", "Zelda",
};

std::string pick_random_name() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> d(0, (int)(sizeof(kNamePool) / sizeof(kNamePool[0])) - 1);
    std::uniform_int_distribution<int> n(10, 99);
    return std::string(kNamePool[d(gen)]) + std::to_string(n(gen));
}

void pick_color_from_name(const std::string& name, uint8_t out[3]) {
    // Deterministic pastel from a hash of the name.
    uint32_t h = 2166136261u;
    for (char c : name) {
        h = (h ^ (uint8_t)c) * 16777619u;
    }
    out[0] = 128 + (h & 0x7F);
    out[1] = 128 + ((h >> 8) & 0x7F);
    out[2] = 128 + ((h >> 16) & 0x7F);
}

#if DUSK_ONLINE_SOCKETS

void close_socket(socket_t s) {
    if (s != kInvalidSocket) closesocket(s);
}
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

void send_framed(uint8_t opcode, const void* data, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_sendMutex);
    if (g_peer == kInvalidSocket) return;
    uint8_t header[5];
    header[0] = opcode;
    std::memcpy(header + 1, &len, 4);
    if (!send_all(g_peer, reinterpret_cast<const char*>(header), 5)) return;
    if (len) send_all(g_peer, reinterpret_cast<const char*>(data), (int)len);
}

void send_hello() {
    HelloMsg h{};
    h.version = kProtocolVersion;
    h.id = (uint8_t)g_localId;
    h.color[0] = g_localColor[0];
    h.color[1] = g_localColor[1];
    h.color[2] = g_localColor[2];
    std::strncpy(h.name, g_localName.c_str(), sizeof(h.name) - 1);
    send_framed(kOpHello, &h, sizeof(h));
}

void handle_hello(const uint8_t* data, uint32_t len) {
    if (len < sizeof(HelloMsg)) return;
    HelloMsg h{};
    std::memcpy(&h, data, sizeof(h));
    if (h.version != kProtocolVersion) {
        DuskLog.warn("[online] peer protocol mismatch (theirs={}, ours={}) — disconnecting",
                     h.version, kProtocolVersion);
        set_status("version mismatch");
        g_running.store(false);
        return;
    }
    std::lock_guard<std::mutex> lk(g_playersMutex);
    PlayerState& p = g_players[g_remoteId];
    p.active = true;
    p.id = (uint8_t)g_remoteId;
    std::memcpy(p.name, h.name, sizeof(p.name));
    p.name[sizeof(p.name) - 1] = 0;
    p.colorR = h.color[0];
    p.colorG = h.color[1];
    p.colorB = h.color[2];
    DuskLog.info("[online] peer '{}' joined as id {}", p.name, g_remoteId);
}

void handle_player_state(const uint8_t* data, uint32_t len) {
    if (len < sizeof(PlayerStateMsg)) return;
    PlayerStateMsg m{};
    std::memcpy(&m, data, sizeof(m));
    std::lock_guard<std::mutex> lk(g_playersMutex);
    PlayerState& p = g_players[g_remoteId];
    p.active = true;
    p.id = (uint8_t)g_remoteId;
    p.pos[0] = m.pos[0];
    p.pos[1] = m.pos[1];
    p.pos[2] = m.pos[2];
    p.angleY = m.angleY;
    p.isWolf = m.isWolf != 0;
    p.animId = m.animId;
    p.animFrame = m.animFrame;
    std::memcpy(p.input, m.input, kInputBytes);
    p.lastTick = m.tick;
}

void handle_pose(const uint8_t* data, uint32_t len) {
    if (len < 2) return;
    uint16_t jointCount;
    std::memcpy(&jointCount, data, 2);
    if (jointCount > kMaxJoints) jointCount = kMaxJoints;
    const uint32_t need = 2u + (1u + (uint32_t)jointCount) * 12u * sizeof(float);
    if (len < need) return;
    const float* f = reinterpret_cast<const float*>(data + 2);
    std::lock_guard<std::mutex> lk(g_poseMutex);
    PlayerPose& p = g_poses[g_remoteId];
    p.jointCount = jointCount;
    std::memcpy(p.baseTR, f, 12 * sizeof(float));
    std::memcpy(p.joints, f + 12, (size_t)jointCount * 12 * sizeof(float));
    p.valid = true;
}

void dispatch(uint8_t opcode, const uint8_t* data, uint32_t len) {
    switch (opcode) {
    case kOpHello:
        handle_hello(data, len);
        break;
    case kOpPlayerState:
        handle_player_state(data, len);
        break;
    case kOpPose:
        handle_pose(data, len);
        break;
    case kOpPing:
        send_framed(kOpPong, data, len);
        break;
    default:
        break;
    }
    if (g_handlers[opcode]) {
        g_handlers[opcode]((uint8_t)g_remoteId, data, len);
    }
}

socket_t accept_host() {
    socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) return kInvalidSocket;
    BOOL yes = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(g_port);
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listener, 4) != 0) {
        DuskLog.warn("[online] host bind/listen failed on port {}", g_port);
        close_socket(listener);
        return kInvalidSocket;
    }
    set_status("hosting on " + std::to_string(g_port));
    DuskLog.info("[online] hosting on port {}, waiting for a peer...", g_port);
    u_long nb = 1;
    ioctlsocket(listener, FIONBIO, &nb);
    socket_t peer = kInvalidSocket;
    while (g_running.load()) {
        peer = accept(listener, nullptr, nullptr);
        if (peer != kInvalidSocket) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    close_socket(listener);
    if (peer != kInvalidSocket) {
        u_long blocking = 0;
        ioctlsocket(peer, FIONBIO, &blocking);
    }
    return peer;
}

socket_t connect_client() {
    set_status("connecting to " + g_hostAddr + ":" + std::to_string(g_port));
    DuskLog.info("[online] connecting to {}:{}...", g_hostAddr, g_port);
    while (g_running.load()) {
        socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != kInvalidSocket) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(g_port);
            inet_pton(AF_INET, g_hostAddr.c_str(), &addr.sin_addr);
            if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) return s;
            close_socket(s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return kInvalidSocket;
}

void io_thread_main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        set_status("WSAStartup failed");
        return;
    }
    socket_t peer = (g_mode == Mode::Host) ? accept_host() : connect_client();
    if (peer == kInvalidSocket) {
        set_status("no connection");
        WSACleanup();
        return;
    }
    BOOL nodelay = TRUE;
    setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    {
        std::lock_guard<std::mutex> lk(g_sendMutex);
        g_peer = peer;
    }
    g_connected.store(true);
    set_status("connected");
    DuskLog.info("[online] peer connected");
    send_hello();

    std::vector<uint8_t> buf;
    while (g_running.load()) {
        uint8_t header[5];
        if (!recv_all(peer, (char*)header, 5)) break;
        uint8_t opcode = header[0];
        uint32_t len;
        std::memcpy(&len, header + 1, 4);
        if (len > 1u << 20) break;  // sanity
        buf.resize(len);
        if (len && !recv_all(peer, (char*)buf.data(), (int)len)) break;
        dispatch(opcode, buf.data(), len);
    }

    g_connected.store(false);
    set_status("disconnected");
    DuskLog.info("[online] peer disconnected");
    {
        std::lock_guard<std::mutex> lk(g_playersMutex);
        g_players[g_remoteId].active = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_poseMutex);
        g_poses[g_remoteId].valid = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_sendMutex);
        g_peer = kInvalidSocket;
    }
    close_socket(peer);
    WSACleanup();
}

#else  // !DUSK_ONLINE_SOCKETS
void send_framed(uint8_t, const void*, uint32_t) {}
void io_thread_main() {}
#endif

Mode parse_mode(const char* v) {
    if (!v) return Mode::Off;
    if (_stricmp(v, "host") == 0) return Mode::Host;
    if (_stricmp(v, "client") == 0) return Mode::Client;
    return Mode::Off;
}

}  // namespace

void init() {
    g_mode = parse_mode(std::getenv("DUSK_ONLINE_MODE"));
    if (g_mode == Mode::Off) {
        set_status("off");
        return;
    }
    if (const char* p = std::getenv("DUSK_ONLINE_PORT")) {
        int v = std::atoi(p);
        if (v > 0 && v < 65536) g_port = (uint16_t)v;
    }
    if (const char* h = std::getenv("DUSK_ONLINE_HOST")) {
        if (h[0]) g_hostAddr = h;
    }
    if (const char* n = std::getenv("DUSK_ONLINE_NAME")) {
        if (n[0]) g_localName = n;
    }
    if (g_localName.empty()) g_localName = pick_random_name();
    pick_color_from_name(g_localName, g_localColor);

    g_localId = (g_mode == Mode::Host) ? 0 : 1;
    g_remoteId = (g_mode == Mode::Host) ? 1 : 0;
    {
        std::lock_guard<std::mutex> lk(g_playersMutex);
        PlayerState& me = g_players[g_localId];
        me.active = true;
        me.id = (uint8_t)g_localId;
        std::strncpy(me.name, g_localName.c_str(), sizeof(me.name) - 1);
        me.colorR = g_localColor[0];
        me.colorG = g_localColor[1];
        me.colorB = g_localColor[2];
    }

    // Let feature modules register their handlers.
    modules::init_chat();
    modules::init_desync();
    modules::init_snapshot();
    modules::init_savesync();
    modules::init_enemy();
    modules::init_voice();

#if DUSK_ONLINE_SOCKETS
    g_running.store(true);
    g_thread = std::thread(io_thread_main);
    DuskLog.info("[online] init: mode={}, port={}, name='{}'",
                 g_mode == Mode::Host ? "host" : "client", g_port, g_localName);
#else
    g_mode = Mode::Off;
#endif
}

void shutdown() {
    g_running.store(false);
#if DUSK_ONLINE_SOCKETS
    {
        std::lock_guard<std::mutex> lk(g_sendMutex);
        if (g_peer != kInvalidSocket) ::shutdown(g_peer, SD_BOTH);
    }
#endif
    if (g_thread.joinable()) g_thread.join();
    g_connected.store(false);
}

Mode mode() { return g_mode; }
bool is_active() { return g_mode != Mode::Off; }
bool is_connected() { return g_connected.load(); }

const char* status() {
    static thread_local std::string snap;
    std::lock_guard<std::mutex> lk(g_statusMutex);
    snap = g_statusText;
    return snap.c_str();
}

void set_local_transform(const float pos[3], int16_t angleY, bool isWolf) {
    if (g_mode == Mode::Off) return;
    std::lock_guard<std::mutex> lk(g_playersMutex);
    PlayerState& me = g_players[g_localId];
    me.pos[0] = pos[0];
    me.pos[1] = pos[1];
    me.pos[2] = pos[2];
    me.angleY = angleY;
    me.isWolf = isWolf;
}

void set_local_anim(uint16_t animId, float frame) {
    if (g_mode == Mode::Off) return;
    std::lock_guard<std::mutex> lk(g_playersMutex);
    g_players[g_localId].animId = animId;
    g_players[g_localId].animFrame = frame;
}

void set_local_input(const void* pad64) {
    if (g_mode == Mode::Off) return;
    uint64_t tick = g_localTick.fetch_add(1) + 1;
    PlayerStateMsg m{};
    {
        std::lock_guard<std::mutex> lk(g_playersMutex);
        PlayerState& me = g_players[g_localId];
        std::memcpy(me.input, pad64, kInputBytes);
        me.lastTick = tick;
        m.tick = tick;
        m.pos[0] = me.pos[0];
        m.pos[1] = me.pos[1];
        m.pos[2] = me.pos[2];
        m.angleY = me.angleY;
        m.isWolf = me.isWolf ? 1 : 0;
        m.animId = me.animId;
        m.animFrame = me.animFrame;
        std::memcpy(m.input, pad64, kInputBytes);
    }
    send_framed(kOpPlayerState, &m, sizeof(m));
}

void set_local_pose(const float* baseTR, const float* jointMtx, int jointCount) {
    if (g_mode == Mode::Off) return;
    if (jointCount < 0) jointCount = 0;
    if (jointCount > kMaxJoints) jointCount = kMaxJoints;
    // Wire: [u16 jointCount][12 floats baseTR][jointCount*12 floats]
    uint8_t buf[2 + (1 + kMaxJoints) * 12 * sizeof(float)];
    uint16_t jc = (uint16_t)jointCount;
    std::memcpy(buf, &jc, 2);
    std::memcpy(buf + 2, baseTR, 12 * sizeof(float));
    std::memcpy(buf + 2 + 12 * sizeof(float), jointMtx, (size_t)jointCount * 12 * sizeof(float));
    uint32_t len = 2u + (1u + (uint32_t)jointCount) * 12u * sizeof(float);
    send_framed(kOpPose, buf, len);
}

bool get_remote_pose(int id, float* baseTR_out, float* jointMtx_out, int* jointCount_out) {
    if (id < 0 || id >= kMaxPlayers) return false;
    std::lock_guard<std::mutex> lk(g_poseMutex);
    PlayerPose& p = g_poses[id];
    if (!p.valid) return false;
    std::memcpy(baseTR_out, p.baseTR, 12 * sizeof(float));
    std::memcpy(jointMtx_out, p.joints, (size_t)p.jointCount * 12 * sizeof(float));
    if (jointCount_out) *jointCount_out = p.jointCount;
    return true;
}

int local_id() { return g_localId; }

const PlayerState* local_player() {
    return &g_players[g_localId];
}

int player_count() {
    std::lock_guard<std::mutex> lk(g_playersMutex);
    int c = 0;
    for (auto& p : g_players)
        if (p.active) c++;
    return c;
}

int remote_count() {
    std::lock_guard<std::mutex> lk(g_playersMutex);
    int c = 0;
    for (int i = 0; i < kMaxPlayers; i++)
        if (g_players[i].active && i != g_localId) c++;
    return c;
}

const PlayerState* remote_player(int idx) {
    std::lock_guard<std::mutex> lk(g_playersMutex);
    int c = 0;
    for (int i = 0; i < kMaxPlayers; i++) {
        if (g_players[i].active && i != g_localId) {
            if (c == idx) return &g_players[i];
            c++;
        }
    }
    return nullptr;
}

const PlayerState* player_by_id(int id) {
    if (id < 0 || id >= kMaxPlayers) return nullptr;
    return g_players[id].active ? &g_players[id] : nullptr;
}

bool get_remote_input(int id, void* pad64) {
    if (id < 0 || id >= kMaxPlayers) return false;
    std::lock_guard<std::mutex> lk(g_playersMutex);
    if (!g_players[id].active) return false;
    std::memcpy(pad64, g_players[id].input, kInputBytes);
    return true;
}

const char* local_name() { return g_localName.c_str(); }
void local_color(uint8_t* r, uint8_t* g, uint8_t* b) {
    if (r) *r = g_localColor[0];
    if (g) *g = g_localColor[1];
    if (b) *b = g_localColor[2];
}

void register_handler(uint8_t opcode, MessageHandler fn) { g_handlers[opcode] = fn; }
void send_message(uint8_t opcode, const void* data, uint32_t len) {
    send_framed(opcode, data, len);
}

uint64_t local_tick() { return g_localTick.load(); }

}  // namespace dusk::online
