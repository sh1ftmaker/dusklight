#ifndef DUSK_ONLINE_CHAT_H
#define DUSK_ONLINE_CHAT_H

// online_chat.h — public interface for the text chat feature module.
//
// The parent overlay should call draw_imgui() once per frame (e.g. from
// ImGuiConsole::PreDraw or PostDraw).  Everything else is internal to
// src/dusk/online/chat.cpp.
//
// Wire format for kOpChat (opcode 32): raw UTF-8 bytes, no header.
// The sender identity is resolved from the fromId argument supplied by
// the message bus.

namespace dusk::online::chat {

// Encode text, transmit via send_message(kOpChat,...), and also append the
// message to the local ring buffer under the local player's name/colour so
// the sender sees their own messages immediately.  Silently clamps long input.
void send_local(const char* text);

// Render the "Online Chat" ImGui window (message log + input field).
// Only draws when dusk::online::is_active().  The parent calls this each frame
// from its overlay draw path — no additional wiring is required.
void draw_imgui();

// Returns the number of messages that have arrived since the last time the
// chat window was visible (useful for a notification badge in a HUD or menu).
int unread_count();

// Append a local-only system notice (e.g. "Hero joined", "Hero left") to the
// chat log in a neutral colour. Not transmitted — each peer logs its own.
// Safe to call from any thread.
void system_line(const char* text);

}  // namespace dusk::online::chat

#endif  // DUSK_ONLINE_CHAT_H
