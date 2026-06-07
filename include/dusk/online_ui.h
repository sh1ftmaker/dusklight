#ifndef DUSK_ONLINE_UI_H
#define DUSK_ONLINE_UI_H

// In-game ImGui surface for the online features (status, player list, desync,
// voice, snapshot) + the chat window. Also drives the per-frame module pumps.
namespace dusk::online::ui {

// Adds an "Online" menu to the main menu bar. Call between BeginMainMenuBar and
// EndMainMenuBar.
void draw_menu();

// Draws the Online status window + chat window. Call once per frame in the
// ImGui window render path.
void draw();

// Per-frame, game-thread pump for the feature modules (snapshot apply, voice,
// mid-game-join request on fresh connect). Call once per frame.
void frame_update();

}  // namespace dusk::online::ui

#endif  // DUSK_ONLINE_UI_H
