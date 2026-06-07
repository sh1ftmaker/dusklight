#ifndef DUSK_ONLINE_SAVESYNC_H
#define DUSK_ONLINE_SAVESYNC_H

// online/savesync — live world-state sync (host-authoritative).
//
// Keeps the global quest state in sync between peers DURING play (the snapshot
// module only does a one-time full save copy at join). Synced regions:
//   * event/story flags        (dSv_event_c::mEvent[256])
//   * main quest item "got" flags (dSv_player_get_item_c::mItemFlags)
//   * item slots               (dSv_player_item_c::mItems / mItemSlots)
//
// Health, rupees and consumable counts are intentionally NOT synced so each
// player keeps independent vitals. The host is the authority: it diffs these
// regions each frame and broadcasts when they change; clients apply and never
// send. Per-player inventory / client->host authority is future work (PLAN §2).

namespace dusk::online::savesync {

// Called once per frame on the GAME THREAD (host: diff+broadcast; client: apply).
void frame_update();

}  // namespace dusk::online::savesync

#endif  // DUSK_ONLINE_SAVESYNC_H
