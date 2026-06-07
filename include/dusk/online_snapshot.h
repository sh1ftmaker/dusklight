#ifndef DUSK_ONLINE_SNAPSHOT_H
#define DUSK_ONLINE_SNAPSHOT_H

// online_snapshot.h — World Snapshot + Mid-Game Join API
//
// Feature module for dusk::online.  The host captures the authoritative game
// state (save data + RNG + current stage name) into a byte blob and ships it to
// newly-connected clients, so both sides converge on the same world.
//
// Thread-safety contract
// ----------------------
//  * capture()            — must be called on the GAME THREAD (reads engine state).
//  * poll_apply()         — must be called on the GAME THREAD once per frame.
//  * on_connected_request() — may be called from any thread (posts a network msg).
//  * has_pending()        — may be called from any thread.
//
// Call sites the parent must wire
// --------------------------------
//  1. Each frame, on the game thread:
//       dusk::online::snapshot::poll_apply();
//  2. When the local player detects a fresh peer connection (client role):
//       dusk::online::snapshot::on_connected_request();

#include <cstdint>
#include <vector>

namespace dusk::online::snapshot {

// Serialize the current authoritative world state into a self-contained byte
// blob.  Returns an empty vector if the save-data pointer is null (e.g. title
// screen) or if any required engine state is unavailable.
// Must be called on the GAME THREAD.
[[nodiscard]] std::vector<uint8_t> capture();

// Called once per frame on the GAME THREAD.  If a snapshot payload has been
// queued by the IO-thread handler (kOpSnapshot), atomically consumes it and
// applies: restores RNG state and memcpy's save data.
// A TODO stub is left for the stage-load/warp request (see implementation).
void poll_apply();

// Called by the parent when a fresh peer connection is detected and the local
// session is in Client role.  Sends kOpSnapshotReq to the host.
// Safe to call from any thread.
void on_connected_request();

// Returns true if a snapshot payload is sitting in the pending buffer waiting
// for poll_apply() to consume it on the game thread.
// Safe to call from any thread.
[[nodiscard]] bool has_pending();

}  // namespace dusk::online::snapshot

#endif  // DUSK_ONLINE_SNAPSHOT_H
