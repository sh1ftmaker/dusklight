#ifndef DUSK_ONLINE_DESYNC_H
#define DUSK_ONLINE_DESYNC_H

#include <cstdint>

// Desync detector for input-lockstep multiplayer.
//
// Each sim tick the engine calls submit_local(tick). That function reads the
// local RNG triple (r0,r1,r2) via cM_getRndState, stores it in a ring history,
// and broadcasts it to all peers via the kOpChecksum opcode (33).
//
// The on_checksum handler (IO thread) receives each peer's triple for the same
// tick and compares it to the stored local triple. Mismatches increment a
// counter; >= kDesyncThreshold consecutive mismatches set is_desynced() = true.
// Call reset() after a successful resync to clear all state.

namespace dusk::online::desync {

// Maximum consecutive mismatches before is_desynced() returns true.
constexpr int kDesyncThreshold = 10;

// Called once per sim tick by the engine's sim loop.
// Reads the current RNG state, stores it, and broadcasts to peers.
void submit_local(uint64_t tick);

// Returns true once kDesyncThreshold mismatches have accumulated.
bool is_desynced();

// Returns the tick at which the first mismatch occurred, or 0 if none.
uint64_t first_desync_tick();

// Clear all history, counters, and flags. Call after a successful resync.
void reset();

}  // namespace dusk::online::desync

#endif  // DUSK_ONLINE_DESYNC_H
