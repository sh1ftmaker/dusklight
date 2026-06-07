#ifndef DUSK_ONLINE_H
#define DUSK_ONLINE_H

#include <cstdint>

// Dusklight online core (see Dusklight_online_features.md).
//
// Provides the session transport (host/client over TCP), a framed opcode message
// bus, an N-player table, and player identity. Feature modules (chat, desync
// detector, snapshot, voice) live in their own files under src/dusk/online/ and
// plug in via register_handler()/send_message() WITHOUT editing this core.
//
// Launch selection via environment variables (two instances side by side):
//   DUSK_ONLINE_MODE = off | host | client   (default off)
//   DUSK_ONLINE_PORT = <tcp port>            (default 7777)
//   DUSK_ONLINE_HOST = <addr>                (default 127.0.0.1, client only)
//   DUSK_ONLINE_NAME = <display name>        (default: random from name pool)
//   DUSK_ONLINE_PUPPET_OFFSET = <world units> (debug: offset remote puppet)
namespace dusk::online {

enum class Mode { Off, Host, Client };

// Wire protocol version; bumped when the framing/opcodes change. Peers with a
// mismatching version are rejected at handshake.
constexpr uint32_t kProtocolVersion = 2;

constexpr int kMaxPlayers = 16;
constexpr int kInputBytes = 64;  // sizeof(interface_of_controller_pad)
constexpr int kMaxJoints = 80;   // max skeletal joints streamed for puppet animation

// Reserved core opcodes. Feature modules MUST use opcodes >= kFirstUserOpcode.
enum : uint8_t {
    kOpHello = 1,        // handshake: version, id, name, color
    kOpWelcome = 2,      // host -> client: assigned id (+ later: peer roster)
    kOpLeave = 3,
    kOpPing = 4,
    kOpPong = 5,
    kOpPlayerState = 16, // per-tick: transform + anim + input
    kOpPose = 17,        // per-frame: skeletal pose (base TR + joint matrices)
    kFirstUserOpcode = 32,
};

struct PlayerState {
    bool active = false;
    uint8_t id = 0;
    char name[24] = {0};
    uint8_t colorR = 255, colorG = 255, colorB = 255;

    float pos[3] = {0, 0, 0};
    int16_t angleY = 0;
    bool isWolf = false;

    uint16_t animId = 0;   // game-defined animation id (0 = unset)
    float animFrame = 0.f;

    uint8_t input[kInputBytes] = {0};
    uint32_t pingMs = 0;
    uint64_t lastTick = 0;
};

// ---- lifecycle ----
void init();
void shutdown();
Mode mode();
bool is_active();     // mode() != Off
bool is_connected();  // at least one peer linked
const char* status();

// ---- local player publish (engine -> network), called each tick/frame ----
void set_local_transform(const float pos[3], int16_t angleY, bool isWolf);
void set_local_anim(uint16_t animId, float frame);
void set_local_input(const void* pad64);

// ---- player table (network -> engine) ----
// --- skeletal pose streaming (puppet animation) ---
// Each is a row-major Mtx (f32[3][4] = 12 floats). baseTR is the model's base
// transform; jointMtx is jointCount model-space joint matrices (getAnmMtx).
void set_local_pose(const float* baseTR, const float* jointMtx, int jointCount);
// Copies the latest remote pose for player id. Returns false if none.
bool get_remote_pose(int id, float* baseTR_out, float* jointMtx_out, int* jointCount_out);

int local_id();
const PlayerState* local_player();
int player_count();                 // total active incl. local
int remote_count();                 // active remote players
const PlayerState* remote_player(int idx);  // 0..remote_count()-1, or nullptr
const PlayerState* player_by_id(int id);
bool get_remote_input(int id, void* pad64);

// ---- identity ----
const char* local_name();
void local_color(uint8_t* r, uint8_t* g, uint8_t* b);

// ---- message bus (feature modules) ----
// Handler receives the sender's player id and the message payload. Invoked on
// the network IO thread — handlers must be thread-safe / lock their own state.
using MessageHandler = void (*)(uint8_t fromId, const uint8_t* data, uint32_t len);
void register_handler(uint8_t opcode, MessageHandler fn);
// Send a framed message to the connected peer(s). Safe from any thread.
void send_message(uint8_t opcode, const void* data, uint32_t len);

// ---- misc ----
uint64_t local_tick();
float puppet_offset();

// Feature-module init hooks. Each module defines one of these in its own .cpp;
// the core calls them all from init() so modules register their handlers.
namespace modules {
void init_chat();
void init_desync();
void init_snapshot();
void init_savesync();
void init_voice();
}  // namespace modules

}  // namespace dusk::online

#endif  // DUSK_ONLINE_H
