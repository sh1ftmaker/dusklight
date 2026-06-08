#ifndef DUSK_ONLINE_DIRECTORY_H
#define DUSK_ONLINE_DIRECTORY_H

#include <cstdint>

// Dusk Online — room directory protocol (shared by the game and the standalone
// directory server `tools/dusk_directory`).
//
// The directory is a lightweight "phone book": game hosts REGISTER a room with it
// and clients LIST the rooms to discover who is hosting. It is NOT a relay — once a
// client picks a room it connects DIRECTLY to that host's ip:port using the normal
// gameplay transport (see online.cpp). The directory therefore only ever carries
// small room-metadata messages, never gameplay traffic.
//
// A room's lifetime is tied to the host's registration TCP connection: the server
// drops the room when that connection closes, so no heartbeat timer is needed
// (the host keeps the socket open and periodically re-sends an updated RoomInfo).
//
// Wire frame (little-endian; both ends are same-endian in practice):
//   [magic u32][version u16][op u8][len u32][payload (len bytes)]
//
// Payloads:
//   kMsgRegister : one RoomInfo. The server overrides .id (assigns) and .host
//                  (fills from the connection's source address); the host supplies
//                  name/gamePort/stage/curPlayers/maxPlayers/protocolVersion.
//   kMsgList     : empty.
//   kMsgRoomList : [count u16][count × RoomInfo].
namespace dusk::online::directory {

constexpr uint16_t kDefaultPort = 7778;       // distinct from the gameplay default (7777)
constexpr uint32_t kMagic = 0x444C4259u;      // 'DLBY' — frame sentinel
constexpr uint16_t kVersion = 1;              // directory protocol version
constexpr uint16_t kMaxRooms = 256;           // cap on a single list response

// Directory message opcodes (independent of the gameplay opcode space).
enum : uint8_t {
    kMsgRegister = 1,   // host   -> server: open/update a room
    kMsgList     = 2,   // client -> server: request the room list
    kMsgRoomList = 3,   // server -> client: the current rooms
};

#pragma pack(push, 1)
// A single advertised room. Fixed-size; char fields are null-padded. Kept binary
// (not JSON) to match the house wire-format style and keep the server dependency-free.
struct RoomInfo {
    uint16_t id;               // server-assigned room id (0 in a register request)
    uint16_t gamePort;         // the host's gameplay TCP port (what clients connect to)
    uint8_t  curPlayers;       // players currently in the room (incl. host)
    uint8_t  maxPlayers;       // capacity (0 = unspecified)
    uint32_t protocolVersion;  // gameplay protocol version, so clients can filter mismatches
    char     name[32];         // room display name
    char     host[46];         // host ip string; server fills it, host leaves blank on register
    char     stage[8];         // current stage name (informational)
};

// On-wire frame header, written/read explicitly on both ends.
struct FrameHeader {
    uint32_t magic;
    uint16_t version;
    uint8_t  op;
    uint32_t len;
};
#pragma pack(pop)

}  // namespace dusk::online::directory

#endif  // DUSK_ONLINE_DIRECTORY_H
