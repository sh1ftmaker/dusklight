#ifndef DUSK_ONLINE_ENEMY_H
#define DUSK_ONLINE_ENEMY_H

// online/enemy — host-authoritative enemy & boss synchronization.
//
// The host simulates all enemies/bosses as usual and, each frame, broadcasts a
// compact table of their live state (position, facing, health). Clients match
// each entry to their own copy of the actor and overwrite that state, so enemies
// move and die in lockstep with the host's simulation.
//
// Net identity: enemies placed by the stage share a stable (profName, setID,
// room) key across peers, because every peer loads the SAME stage data. That key
// is the wire identity — no per-actor ID negotiation is needed. Dynamically
// spawned actors (setID 0xFFFF) are skipped for now; full spawn/despawn
// replication is future work (see CLAUDE.md roadmap).
//
// Death is handled implicitly: when the host's health for an entry hits 0, the
// client writes 0 into its local actor, and that actor's own death logic takes
// over. The client never force-deletes, keeping us out of the actor lifecycle.
//
// Clients are NOT authoritative: their local enemy AI still ticks, but the
// streamed state is applied AFTER the sim each frame, so the host always wins.

namespace dusk::online::enemy {

// Called once per frame on the GAME THREAD (host: gather+broadcast; client: apply
// the most recent table). No-op when not in-game or no peer is connected.
void frame_update();

}  // namespace dusk::online::enemy

#endif  // DUSK_ONLINE_ENEMY_H
