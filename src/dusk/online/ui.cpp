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

namespace dusk::online::ui {
namespace {

bool s_showWindow = true;

ImVec4 player_color(const PlayerState* p) {
    return ImVec4(p->colorR / 255.0f, p->colorG / 255.0f, p->colorB / 255.0f, 1.0f);
}

}  // namespace

void draw_menu() {
    if (!is_active()) return;
    if (ImGui::BeginMenu("Online")) {
        ImGui::MenuItem("Status Window", nullptr, &s_showWindow);

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
