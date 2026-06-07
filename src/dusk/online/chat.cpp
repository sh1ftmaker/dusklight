/**
 * online/chat.cpp — text chat feature module (opcode kOpChat = 32).
 *
 * Owns opcode kOpChat (kFirstUserOpcode + 0 = 32).
 * Plugs into the core message bus via register_handler / send_message.
 *
 * Wire format: raw UTF-8 text bytes, no header.  Sender identity comes from
 * the fromId argument provided by the bus.
 *
 * Thread safety: on_chat() is called on the network IO thread; all shared
 * state (g_ring, g_unread) is protected by g_mutex.  draw_imgui() and
 * send_local() run on the render/game thread and also acquire g_mutex.
 */

#include "dusk/online_chat.h"
#include "dusk/online.h"
#include "dusk/logging.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>

namespace dusk::online {
namespace {

// ---- opcode -----------------------------------------------------------------

constexpr uint8_t kOpChat = kFirstUserOpcode + 0;  // 32

// ---- ring buffer ------------------------------------------------------------

constexpr int   kRingCapacity = 64;
constexpr int   kMaxTextLen   = 256;  // inclusive max payload bytes (+ NUL)
constexpr int   kMaxNameLen   = 24;   // matches PlayerState::name array size

struct ChatEntry {
    char      senderName[kMaxNameLen + 1] = {};
    uint8_t   colorR = 255, colorG = 255, colorB = 255;
    char      text[kMaxTextLen + 1] = {};
};

// Fixed-size ring: g_ring[g_head % kRingCapacity] is the next write slot.
// g_count tracks how many valid entries exist (capped at kRingCapacity).
std::array<ChatEntry, kRingCapacity> g_ring{};
int    g_head  = 0;   // write index (wraps)
int    g_count = 0;   // number of valid entries (0..kRingCapacity)
int    g_unread = 0;  // incremented on receive, reset when window is shown

std::mutex g_mutex;

// ---- helpers ----------------------------------------------------------------

// Copy at most (capacity - 1) characters from src into dst, always NUL-terminate.
static void safe_copy(char* dst, int capacity, const char* src) {
    if (src == nullptr || capacity <= 0) {
        if (capacity > 0) dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, static_cast<size_t>(capacity - 1));
    dst[capacity - 1] = '\0';
}

static void push_entry(const char* name, uint8_t r, uint8_t g, uint8_t b, const char* text) {
    // Caller holds g_mutex.
    ChatEntry& e = g_ring[static_cast<size_t>(g_head % kRingCapacity)];
    safe_copy(e.senderName, static_cast<int>(sizeof(e.senderName)), name);
    e.colorR = r;
    e.colorG = g;
    e.colorB = b;
    safe_copy(e.text, static_cast<int>(sizeof(e.text)), text);
    g_head = (g_head + 1) % kRingCapacity;
    if (g_count < kRingCapacity) ++g_count;
}

// Return a read-only pointer to entry i (0 = oldest visible, count-1 = newest).
static const ChatEntry& get_entry(int i) {
    // Caller holds g_mutex.
    // When the ring is not yet full, entries start at index 0.
    // When full, the oldest entry sits at g_head (which just wrapped past it).
    int base = (g_count < kRingCapacity) ? 0 : g_head;
    int idx  = (base + i) % kRingCapacity;
    return g_ring[static_cast<size_t>(idx)];
}

// ---- bus handler (runs on the IO thread) ------------------------------------

void on_chat(uint8_t fromId, const uint8_t* data, uint32_t len) {
    if (data == nullptr || len == 0) return;

    // Clamp payload length.
    uint32_t clamped = std::min(len, static_cast<uint32_t>(kMaxTextLen));

    // Build a NUL-terminated copy of the text.
    char text[kMaxTextLen + 1];
    std::memcpy(text, data, clamped);
    text[clamped] = '\0';

    // Look up sender identity.
    const PlayerState* ps = player_by_id(static_cast<int>(fromId));
    const char* name = (ps != nullptr && ps->name[0] != '\0') ? ps->name : "player";
    uint8_t r = (ps != nullptr) ? ps->colorR : 255u;
    uint8_t g = (ps != nullptr) ? ps->colorG : 255u;
    uint8_t b = (ps != nullptr) ? ps->colorB : 255u;

    DuskLog.info("[online] chat <{}>: {}", name, text);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        push_entry(name, r, g, b, text);
        ++g_unread;
    }
}

}  // anonymous namespace

// ---- public API (namespace dusk::online::chat) ------------------------------

namespace chat {

void send_local(const char* text) {
    if (text == nullptr || text[0] == '\0') return;

    // Clamp to wire limit.
    uint32_t len = static_cast<uint32_t>(
        std::min(static_cast<size_t>(kMaxTextLen), std::strlen(text)));

    // Send to peer(s).
    send_message(kOpChat, text, len);

    // Append locally so the sender sees their own message immediately.
    const char* name = local_name();
    if (name == nullptr || name[0] == '\0') name = "you";

    uint8_t r = 255, g = 255, b = 255;
    local_color(&r, &g, &b);

    // Build a clamped NUL-terminated copy in case text is longer.
    char buf[kMaxTextLen + 1];
    std::memcpy(buf, text, len);
    buf[len] = '\0';

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        push_entry(name, r, g, b, buf);
        // Do NOT increment g_unread for self-sent messages.
    }
}

void draw_imgui() {
    if (!is_active()) return;

    // Position the window in the bottom-left corner, but allow the user to
    // drag it away (ImGuiCond_FirstUseEver).
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float padding = 10.0f;
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + padding,
               vp->WorkPos.y + vp->WorkSize.y - padding),
        ImGuiCond_FirstUseEver,
        ImVec2(0.0f, 1.0f)   // pivot: bottom-left
    );
    ImGui::SetNextWindowSize(ImVec2(330.0f, 165.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.55f);

    bool open = true;
    if (!ImGui::Begin("Online Chat", &open,
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::End();
        return;
    }

    // Reset unread counter while the window is being shown.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_unread = 0;
    }

    // --- message log ---------------------------------------------------------
    const float inputHeight = ImGui::GetFrameHeightWithSpacing();
    const float logHeight   = ImGui::GetContentRegionAvail().y - inputHeight;

    ImGui::BeginChild("##chat_log", ImVec2(0.0f, logHeight), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const int count = g_count;
        for (int i = 0; i < count; ++i) {
            const ChatEntry& e = get_entry(i);
            // Draw the sender name in their colour.
            ImGui::TextColored(
                ImVec4(e.colorR / 255.0f, e.colorG / 255.0f, e.colorB / 255.0f, 1.0f),
                "%s:", e.senderName);
            ImGui::SameLine();
            ImGui::TextUnformatted(e.text);
        }
    }

    // Auto-scroll to the bottom on new messages.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();

    // --- input field ---------------------------------------------------------
    // Static buffer persists across frames.
    static char s_inputBuf[kMaxTextLen + 1] = {};

    // Give the input field the full remaining width.
    ImGui::PushItemWidth(-1.0f);

    const bool submitted = ImGui::InputText(
        "##chat_input",
        s_inputBuf,
        sizeof(s_inputBuf),
        ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::PopItemWidth();

    if (submitted && s_inputBuf[0] != '\0') {
        send_local(s_inputBuf);
        s_inputBuf[0] = '\0';
        // Re-focus the input box so the user can keep typing.
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::End();
}

int unread_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_unread;
}

}  // namespace chat

// ---- module registration ----------------------------------------------------

namespace modules {
void init_chat() {
    register_handler(kOpChat, &on_chat);
}
}  // namespace modules

}  // namespace dusk::online
