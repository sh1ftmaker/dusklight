/**
 * online/ui.cpp — in-game ImGui surface + per-frame module pump for the online
 * features. Owned by the core integration layer (not a plug-in feature module):
 * it pulls together chat, desync, voice, and snapshot into one visible panel and
 * drives their per-frame call sites.
 */

#include "dusk/online_ui.h"

#include "dusk/online.h"
#include "dusk/online_chat.h"
#include "dusk/online_desync.h"
#include "dusk/online_snapshot.h"
#include "dusk/online_voice.h"

#include "imgui.h"

#include "d/d_com_inf_game.h"   // dComIfGd_getViewMtx
#include "m_Do/m_Do_lib.h"      // mDoLib_project
#include "m_Do/m_Do_mtx.h"      // cMtx_multVec
#include "m_Do/m_Do_graphic.h"  // mDoGph_gInf_c (game framebuffer dims)

namespace dusk::online::ui {
namespace {

bool s_showWindow = true;
bool s_showNameplates = true;

// World-space height above a puppet's feet to float its nameplate (Link is ~170
// units tall; sit a little above the head).
constexpr float kNameplateHeight = 215.0f;

ImVec4 player_color(const PlayerState* p) {
    return ImVec4(p->colorR / 255.0f, p->colorG / 255.0f, p->colorB / 255.0f, 1.0f);
}

// Float nameplates over each remote puppet. Projects the puppet's head position
// to screen via the same path the game uses (mDoLib_project), then draws the name
// in the player's color on ImGui's foreground draw list. Runs during the ImGui
// pass, after the game render, so the camera matrices are current for this frame.
void draw_nameplates() {
    if (!s_showNameplates) return;
    const int n = remote_count();
    if (n <= 0) return;

    // No camera/view yet (e.g. logo/title scenes before gameplay) — bail before
    // touching the view matrix. dComIfGd_getViewMtx() dereferences getView(), which
    // is null here; mDoLib_project guards this internally but our behind-camera
    // check below would fault first.
    if (dComIfGd_getView() == nullptr) return;

    // mDoLib_project returns coordinates in the game framebuffer's space; map them
    // into ImGui's display space in case the two differ (internal-res scaling).
    const float gw = mDoGph_gInf_c::getWidthF();
    const float gh = mDoGph_gInf_c::getHeightF();
    if (gw <= 0.0f || gh <= 0.0f) return;
    const float minx = mDoGph_gInf_c::getMinXF();
    const float miny = mDoGph_gInf_c::getMinYF();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float sx = disp.x / gw;
    const float sy = disp.y / gh;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int i = 0; i < n; ++i) {
        const PlayerState* rp = remote_player(i);
        if (rp == nullptr) continue;

        Vec head = {rp->pos[0] + puppet_offset(), rp->pos[1] + kNameplateHeight, rp->pos[2]};

        // Skip when behind the camera. In view space the camera looks down -Z
        // (GC convention), so a visible point has negative Z.
        Vec viewPos;
        cMtx_multVec(dComIfGd_getViewMtx(), &head, &viewPos);
        if (viewPos.z >= 0.0f) continue;

        Vec proj;
        mDoLib_project(&head, &proj);
        const float x = (proj.x - minx) * sx;
        const float y = (proj.y - miny) * sy;
        if (x < -200.0f || x > disp.x + 200.0f || y < -100.0f || y > disp.y + 100.0f) continue;

        const char* label = rp->name;
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const ImVec2 pos(x - ts.x * 0.5f, y - ts.y);
        const ImU32 col = IM_COL32(rp->colorR, rp->colorG, rp->colorB, 255);
        const ImU32 shadow = IM_COL32(0, 0, 0, 200);
        // 1px drop shadow for legibility over any background.
        dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), shadow, label);
        dl->AddText(pos, col, label);
    }
}

}  // namespace

void draw_menu() {
    if (!is_active()) return;
    if (ImGui::BeginMenu("Online")) {
        ImGui::MenuItem("Status Window", nullptr, &s_showWindow);
        ImGui::MenuItem("Nameplates", nullptr, &s_showNameplates);

        bool v = voice::enabled();
        if (ImGui::MenuItem("Voice Chat", nullptr, &v)) {
            voice::set_enabled(v && is_connected());
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

    // Floating player nameplates over the puppets (independent of the status panel).
    draw_nameplates();

    // Chat window is always available when online.
    chat::draw_imgui();

    if (!s_showWindow) return;

    // Tuck the status panel into the top-right corner, transparent, and collapsed
    // to just its title bar by default — it's glance-info, expand when wanted. The
    // user can still drag/resize/expand it; these only apply on first use.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 10.0f;
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x - pad, vp->WorkPos.y + pad),
        ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));  // pivot: top-right
    ImGui::SetNextWindowSize(ImVec2(300.0f, 260.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.60f);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Online", &s_showWindow,
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::Text("Mode: %s  |  %s", mode() == Mode::Host ? "Host" : "Client",
                    is_connected() ? "connected" : status());
        ImGui::Separator();

        ImGui::Text("Players (%d):", player_count());
        for (int id = 0; id < kMaxPlayers; ++id) {
            const PlayerState* p = player_by_id(id);
            if (p == nullptr) continue;
            ImGui::TextColored(player_color(p), "  [%d] %s%s", p->id, p->name,
                               id == local_id() ? " (you)" : "");
            ImGui::SameLine();
            ImGui::TextDisabled("  %.0f, %.0f, %.0f%s", p->pos[0], p->pos[1], p->pos[2],
                                p->isWolf ? "  (wolf)" : "");
        }
        ImGui::Separator();

        if (desync::is_desynced()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "DESYNC at tick %llu",
                               (unsigned long long)desync::first_desync_tick());
        } else {
            ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.35f, 1.0f), "Synced");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("  tick %llu", (unsigned long long)local_tick());
        ImGui::Separator();

        bool v = voice::enabled();
        if (ImGui::Checkbox("Voice chat", &v)) {
            voice::set_enabled(v && is_connected());
        }

        if (mode() == Mode::Client) {
            if (ImGui::Button("Request snapshot")) {
                snapshot::on_connected_request();
            }
            if (snapshot::has_pending()) {
                ImGui::SameLine();
                ImGui::TextDisabled("applying...");
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("puppet offset: %.0f (DUSK_ONLINE_PUPPET_OFFSET)", puppet_offset());
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
