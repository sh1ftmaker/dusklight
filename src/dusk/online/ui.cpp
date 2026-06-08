/**
 * online/ui.cpp — in-game ImGui surface + per-frame module pump for the online
 * features. Owned by the core integration layer (not a plug-in feature module):
 * it pulls together chat, desync, voice, snapshot, and pings into one overlay
 * (toggled with F7) and drives their per-frame call sites.
 *
 * Hotkeys: F7 toggles the overlay, F12 drops a map ping at the local player.
 */

#include "dusk/online_ui.h"

#include "dusk/online.h"
#include "dusk/online_chat.h"
#include "dusk/online_desync.h"
#include "dusk/online_snapshot.h"
#include "dusk/online_puppet.h"
#include "dusk/online_voice.h"
#include "dusk/online_ping.h"
#include "dusk/online_directory.h"

#include "imgui.h"

#include "d/d_com_inf_game.h"   // dComIfGd_getViewMtx, dComIfGp_getPlayer/setNextStage
#include "m_Do/m_Do_lib.h"      // mDoLib_project
#include "m_Do/m_Do_mtx.h"      // cMtx_multVec
#include "m_Do/m_Do_graphic.h"  // mDoGph_gInf_c (game framebuffer dims)

#include <cstring>

namespace dusk::online::ui {
namespace {

bool s_showOverlay = true;
bool s_showNameplates = true;

// World-space height above a puppet's feet to float its nameplate (Link is ~170
// units tall; sit a little above the head).
constexpr float kNameplateHeight = 215.0f;

ImVec4 player_color(const PlayerState* p) {
    return ImVec4(p->colorR / 255.0f, p->colorG / 255.0f, p->colorB / 255.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// World -> screen projection (shared by nameplates and map pings)
// ---------------------------------------------------------------------------
// mDoLib_project returns coordinates in the game framebuffer's space; we map
// those into ImGui's display space in case the two differ (internal-res scaling).
struct ProjCtx {
    bool  ok = false;
    float minx = 0, miny = 0, sx = 1, sy = 1;
};

ProjCtx begin_projection() {
    ProjCtx c;
    // No camera/view yet (logo/title scenes before gameplay) — bail before
    // touching the view matrix; dComIfGd_getViewMtx() dereferences a null view.
    if (dComIfGd_getView() == nullptr) return c;
    const float gw = mDoGph_gInf_c::getWidthF();
    const float gh = mDoGph_gInf_c::getHeightF();
    if (gw <= 0.0f || gh <= 0.0f) return c;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    c.minx = mDoGph_gInf_c::getMinXF();
    c.miny = mDoGph_gInf_c::getMinYF();
    c.sx = disp.x / gw;
    c.sy = disp.y / gh;
    c.ok = true;
    return c;
}

// Projects world point w to screen (x,y in ImGui display space). Returns false
// if behind the camera. Camera looks down -Z (GC convention) → visible z < 0.
bool project_point(const ProjCtx& c, Vec w, float& outX, float& outY) {
    Vec viewPos;
    cMtx_multVec(dComIfGd_getViewMtx(), &w, &viewPos);
    if (viewPos.z >= 0.0f) return false;
    Vec proj;
    mDoLib_project(&w, &proj);
    outX = (proj.x - c.minx) * c.sx;
    outY = (proj.y - c.miny) * c.sy;
    return true;
}

// ---------------------------------------------------------------------------
// Floating nameplates over each in-room remote puppet
// ---------------------------------------------------------------------------
void draw_nameplates(const ProjCtx& c) {
    if (!s_showNameplates) return;
    const int n = remote_count();
    if (n <= 0) return;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int i = 0; i < n; ++i) {
        const PlayerState* rp = remote_player(i);
        if (rp == nullptr) continue;
        if (!in_local_room(rp)) continue;  // only label peers in our room

        // Anchor to the puppet's actual drawn position when available (exact
        // match); fall back to the streamed transform if it wasn't drawn.
        float wp[3];
        Vec head;
        if (puppet::get_puppet_world_pos(rp->id, wp)) {
            head = {wp[0], wp[1] + kNameplateHeight, wp[2]};
        } else {
            head = {rp->pos[0], rp->pos[1] + kNameplateHeight, rp->pos[2]};
        }

        float x, y;
        if (!project_point(c, head, x, y)) continue;
        if (x < -200.0f || x > disp.x + 200.0f || y < -100.0f || y > disp.y + 100.0f) continue;

        const char* label = rp->name;
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const ImVec2 pos(x - ts.x * 0.5f, y - ts.y);
        const ImU32 col = IM_COL32(rp->colorR, rp->colorG, rp->colorB, 255);
        const ImU32 shadow = IM_COL32(0, 0, 0, 200);
        dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), shadow, label);  // legibility
        dl->AddText(pos, col, label);
    }
}

// ---------------------------------------------------------------------------
// Map pings ("look here" markers)
// ---------------------------------------------------------------------------
void draw_pings(const ProjCtx& c) {
    ping::Marker markers[kMaxPlayers];
    const int n = ping::get_markers(markers, kMaxPlayers);
    if (n <= 0) return;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int i = 0; i < n; ++i) {
        const ping::Marker& m = markers[i];
        // Float the marker a touch above the pinged spot so it reads as a beacon.
        Vec at = {m.pos[0], m.pos[1] + 80.0f, m.pos[2]};
        float x, y;
        if (!project_point(c, at, x, y)) continue;
        if (x < 0.0f || x > disp.x || y < 0.0f || y > disp.y) continue;

        const float a = m.ttl / ping::kMarkerLifetime;  // fade out over lifetime
        const ImU32 col = IM_COL32(m.r, m.g, m.b, (int)(255.0f * (a < 1.0f ? a : 1.0f)));
        const ImU32 edge = IM_COL32(0, 0, 0, (int)(180.0f * a));

        // Diamond beacon.
        const float s = 9.0f;
        const ImVec2 up(x, y - s), dn(x, y + s), lf(x - s, y), rt(x + s, y);
        dl->AddQuadFilled(up, rt, dn, lf, col);
        dl->AddQuad(up, rt, dn, lf, edge, 2.0f);

        const PlayerState* p = player_by_id(m.fromId);
        if (p != nullptr) {
            const char* nm = p->name;
            const ImVec2 ts = ImGui::CalcTextSize(nm);
            const ImVec2 tp(x - ts.x * 0.5f, y - s - ts.y - 2.0f);
            dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), edge, nm);
            dl->AddText(tp, col, nm);
        }
    }
}

// ---------------------------------------------------------------------------
// Warp the local player to a peer's stage+room (regroup)
// ---------------------------------------------------------------------------
void warp_to_player(const PlayerState* rp) {
    if (rp == nullptr) return;
    // Only when we're actually in gameplay (same gate as the Warp menu / join).
    if (dComIfGp_getPlayer(0) == nullptr) return;
    if (rp->room < 0 || rp->stage[0] == '\0') return;
    char stage[9] = {0};
    std::memcpy(stage, rp->stage, 8);
    // Point 0 is the default entrance of the room; lands us in the same room as
    // the peer (not their exact spot, but together).
    dComIfGp_setNextStage(stage, /*point=*/0, (s8)rp->room, /*layer=*/0);
}

void ping_local_player() {
    const PlayerState* me = local_player();
    if (me != nullptr) ping::send_marker(me->pos);
}

}  // namespace

void draw_menu() {
    if (!is_active()) return;
    if (ImGui::BeginMenu("Online")) {
        ImGui::MenuItem("Overlay (F7)", nullptr, &s_showOverlay);
        ImGui::MenuItem("Nameplates", nullptr, &s_showNameplates);

        bool v = voice::enabled();
        if (ImGui::MenuItem("Voice Chat", nullptr, &v)) {
            voice::set_enabled(v && is_connected());
        }

        if (ImGui::MenuItem("Ping my location (F12)", nullptr, false, is_connected())) {
            ping_local_player();
        }

        if (mode() == Mode::Client && ImGui::MenuItem("Request Snapshot")) {
            snapshot::on_connected_request();
        }

        ImGui::Separator();
        ImGui::TextDisabled("%s", is_connected() ? "connected" : status());
        ImGui::EndMenu();
    }
}

void draw() {
    if (!is_active()) return;

    // Hotkeys: F7 toggles the overlay, F12 drops a map ping. (F9/F10 are taken
    // by the engine's camera/audio debug overlays.)
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) s_showOverlay = !s_showOverlay;
    if (ImGui::IsKeyPressed(ImGuiKey_F12, false) && is_connected()) ping_local_player();

    // Decay markers, then draw world-anchored overlays (nameplates + pings).
    ping::update(ImGui::GetIO().DeltaTime);
    const ProjCtx proj = begin_projection();
    if (proj.ok) {
        draw_nameplates(proj);
        draw_pings(proj);
    }

    // Chat window is always available when online.
    chat::draw_imgui();

    if (!s_showOverlay) return;

    // Tuck the overlay into the top-right corner, transparent, collapsed to its
    // title bar by default — glance-info, expand when wanted. The user can
    // drag/resize/expand; these only apply on first use.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 10.0f;
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x - pad, vp->WorkPos.y + pad),
        ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));  // pivot: top-right
    ImGui::SetNextWindowSize(ImVec2(320.0f, 300.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.60f);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Online (F7)", &s_showOverlay,
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::Text("Mode: %s  |  %s", mode() == Mode::Host ? "Host" : "Client",
                    is_connected() ? "connected" : status());
        ImGui::Separator();

        // --- Player roster -------------------------------------------------
        ImGui::Text("Players (%d):", player_count());
        const int me = local_id();
        for (int id = 0; id < kMaxPlayers; ++id) {
            const PlayerState* p = player_by_id(id);
            if (p == nullptr) continue;
            ImGui::TextColored(player_color(p), "[%d] %s%s", p->id, p->name,
                               id == me ? " (you)" : "");
            // Room / form / ping line.
            char stage[9] = {0};
            std::memcpy(stage, p->stage, 8);
            ImGui::SameLine();
            if (id == me) {
                ImGui::TextDisabled("  %s:%d%s", stage[0] ? stage : "?", p->room,
                                    p->isWolf ? " wolf" : "");
            } else {
                ImGui::TextDisabled("  %s:%d%s  %ums", stage[0] ? stage : "?", p->room,
                                    p->isWolf ? " wolf" : "", p->pingMs);
                // Warp-to-player button (regroup), in-gameplay only.
                if (dComIfGp_getPlayer(0) != nullptr && p->room >= 0 && stage[0]) {
                    ImGui::SameLine();
                    ImGui::PushID(id);
                    if (ImGui::SmallButton(in_local_room(p) ? "here" : "warp")) {
                        warp_to_player(p);
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::Separator();

        // --- Sync status ---------------------------------------------------
        if (desync::is_desynced()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "DESYNC at tick %llu",
                               (unsigned long long)desync::first_desync_tick());
        } else {
            ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.35f, 1.0f), "Synced");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("  tick %llu", (unsigned long long)local_tick());
        ImGui::Separator();

        // --- Room directory (discovery) ------------------------------------
        if (directory_address()[0]) {
            ImGui::Text("Directory: %s", directory_address());
            if (mode() == Mode::Host) {
                ImGui::TextDisabled("  advertising \"%s\"", room_name());
            } else {
                directory::RoomInfo rooms[16];
                int n = directory_rooms(rooms, 16);
                if (n == 0) {
                    ImGui::TextDisabled("  no rooms found");
                }
                for (int i = 0; i < n; ++i) {
                    const directory::RoomInfo& r = rooms[i];
                    char stage[9] = {0};
                    std::memcpy(stage, r.stage, 8);
                    ImGui::BulletText("%s  %s:%u  (%u/%u)  %s", r.name, r.host, r.gamePort,
                                      r.curPlayers, r.maxPlayers, stage[0] ? stage : "?");
                    // Click-to-join (only meaningful while not yet connected).
                    if (!is_connected()) {
                        ImGui::SameLine();
                        ImGui::PushID(i);
                        if (ImGui::SmallButton("Join")) join_room(r);
                        ImGui::PopID();
                    }
                }
            }
            ImGui::Separator();
        }

        // --- Toggles & actions ---------------------------------------------
        ImGui::Checkbox("Nameplates", &s_showNameplates);
        bool v = voice::enabled();
        if (ImGui::Checkbox("Voice chat", &v)) {
            voice::set_enabled(v && is_connected());
        }

        if (ImGui::Button("Ping location")) {
            ping_local_player();
        }
        if (mode() == Mode::Client) {
            ImGui::SameLine();
            if (ImGui::Button("Resync")) {
                snapshot::on_connected_request();
            }
            if (snapshot::has_pending()) {
                ImGui::SameLine();
                ImGui::TextDisabled("applying...");
            }
        }
    }
    ImGui::End();
}

void frame_update() {
    if (!is_active()) return;

    // Mid-game join: when a client first connects, ask the host for a snapshot.
    static bool s_wasConnected = false;
    bool now = is_connected();
    if (now && !s_wasConnected && mode() == Mode::Client) {
        snapshot::on_connected_request();
    }
    s_wasConnected = now;

    snapshot::poll_apply();
    voice::update();
}

}  // namespace dusk::online::ui
