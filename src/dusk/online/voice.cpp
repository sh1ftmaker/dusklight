/**
 * online/voice.cpp — voice chat (TPOnline Voice/Mixer + DSG audio).
 *
 * Owns opcode kOpVoice (36). Captures the local microphone at a fixed low
 * rate, streams 20 ms frames to peers, mixes incoming peer streams, and plays
 * them back.
 *
 * Design constraints / thread safety:
 *  - All SDL audio handles are file-static.  They are created and destroyed
 *    only on the game thread (inside update()), never on the IO thread.
 *  - on_voice() runs on the network IO thread.  It only writes raw bytes into
 *    g_mix_buf under g_mix_mutex.  It never touches SDL handles.
 *  - update() runs on the game thread.  It reads g_mix_buf under g_mix_mutex,
 *    feeds it to the playback stream, and drains mic capture to send frames.
 *
 * Wire format: raw S16LE mono samples, kVoiceFrameBytes (640 bytes = 320
 * samples = 20 ms at 16 kHz).  No additional header — sender identity is
 * supplied by the bus fromId argument and is intentionally not forwarded in
 * the payload (first-pass simplicity; per-player volume control can be added
 * on top later by prefixing a sender byte).
 */

#include "dusk/online_voice.h"
#include "dusk/online.h"
#include "dusk/logging.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace dusk::online {
namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr uint8_t kOpVoice           = kFirstUserOpcode + 4;  // 36
constexpr int     kVoiceSampleRate   = 16000;   // 16 kHz — low-bandwidth voice
constexpr int     kVoiceChannels     = 1;       // mono
constexpr int     kVoiceFrameMs      = 20;      // 20 ms per network frame
constexpr int     kVoiceFrameSamples = kVoiceSampleRate * kVoiceFrameMs / 1000; // 320
constexpr int     kVoiceFrameBytes   = kVoiceFrameSamples * static_cast<int>(sizeof(int16_t)); // 640

// Maximum bytes we will accumulate in the mix buffer before discarding excess
// (prevents runaway growth when the game thread stalls).
constexpr int kMixBufMaxBytes = kVoiceFrameBytes * 32;  // ~640 ms of audio

// ---------------------------------------------------------------------------
// File-static SDL3 state  (game thread only — never touched from IO thread)
// ---------------------------------------------------------------------------

static SDL_AudioStream* s_capture_stream  = nullptr;   // recording device stream
static SDL_AudioStream* s_playback_stream = nullptr;   // voice playback stream

// ---------------------------------------------------------------------------
// Shared mix buffer  (written by IO thread, drained by game thread)
// ---------------------------------------------------------------------------

static std::mutex              g_mix_mutex;
static std::vector<int16_t>    g_mix_buf;   // accumulated peer audio (S16 mono)

// ---------------------------------------------------------------------------
// Module state  (game thread only for the bool; atomic enough for a bool
// because set_enabled is intended to be called from the game thread too)
// ---------------------------------------------------------------------------

static bool s_enabled = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr SDL_AudioSpec kVoiceSpec = {
    SDL_AUDIO_S16,
    kVoiceChannels,
    kVoiceSampleRate,
};

// Open the recording (mic) stream.  Returns false and logs on failure.
static bool open_capture() {
    if (s_capture_stream != nullptr) return true;  // already open

    s_capture_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_RECORDING,
        &kVoiceSpec,
        nullptr,   // no callback — we poll in update()
        nullptr);

    if (s_capture_stream == nullptr) {
        DuskLog.warn("[voice] failed to open recording device: {}", SDL_GetError());
        return false;
    }

    SDL_ResumeAudioStreamDevice(s_capture_stream);
    DuskLog.info("[voice] recording device opened ({} Hz S16 mono)", kVoiceSampleRate);
    return true;
}

// Open the voice playback stream.  Returns false and logs on failure.
static bool open_playback() {
    if (s_playback_stream != nullptr) return true;  // already open

    s_playback_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &kVoiceSpec,
        nullptr,   // no callback — we push in update()
        nullptr);

    if (s_playback_stream == nullptr) {
        DuskLog.warn("[voice] failed to open playback device: {}", SDL_GetError());
        return false;
    }

    SDL_ResumeAudioStreamDevice(s_playback_stream);
    DuskLog.info("[voice] voice playback device opened ({} Hz S16 mono)", kVoiceSampleRate);
    return true;
}

// Destroy both SDL streams (game thread).
static void close_devices() {
    if (s_capture_stream != nullptr) {
        SDL_DestroyAudioStream(s_capture_stream);
        s_capture_stream = nullptr;
        DuskLog.info("[voice] recording device closed");
    }
    if (s_playback_stream != nullptr) {
        SDL_DestroyAudioStream(s_playback_stream);
        s_playback_stream = nullptr;
        DuskLog.info("[voice] voice playback device closed");
    }
}

// ---------------------------------------------------------------------------
// Bus handler — IO thread
// ---------------------------------------------------------------------------

// Called on the network IO thread for every kOpVoice message received from a
// peer.  Mixes the incoming S16 mono samples into g_mix_buf (sum + clamp),
// resizing as needed.  Never touches SDL handles.
void on_voice(uint8_t /*fromId*/, const uint8_t* data, uint32_t len) {
    if (data == nullptr || len == 0) return;
    if (len % sizeof(int16_t) != 0) return;  // malformed — must be whole samples

    const int num_samples = static_cast<int>(len / sizeof(int16_t));
    const int16_t* incoming = reinterpret_cast<const int16_t*>(data);

    std::lock_guard<std::mutex> lock(g_mix_mutex);

    // Guard against runaway growth if the game thread isn't draining.
    const int current_bytes = static_cast<int>(g_mix_buf.size()) * static_cast<int>(sizeof(int16_t));
    if (current_bytes >= kMixBufMaxBytes) return;

    // Grow the mix buffer if it is shorter than the incoming frame.
    if (static_cast<int>(g_mix_buf.size()) < num_samples) {
        g_mix_buf.resize(static_cast<size_t>(num_samples), 0);
    }

    // Simple additive mix with S16 clamp — mirrors TPOnline's root.Mixer logic.
    for (int i = 0; i < num_samples; ++i) {
        const int32_t mixed = static_cast<int32_t>(g_mix_buf[static_cast<size_t>(i)])
                            + static_cast<int32_t>(incoming[i]);
        g_mix_buf[static_cast<size_t>(i)] = static_cast<int16_t>(
            std::clamp(mixed, static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
    }
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public API  (namespace dusk::online::voice)
// ---------------------------------------------------------------------------

namespace voice {

void set_enabled(bool en) {
    s_enabled = en;
}

bool enabled() {
    return s_enabled;
}

void update() {
    if (!s_enabled) {
        // Lazy close: release any open devices when disabled.
        if (s_capture_stream != nullptr || s_playback_stream != nullptr) {
            close_devices();
            // Also discard any buffered peer audio so stale data doesn't play
            // if voice is re-enabled later.
            std::lock_guard<std::mutex> lock(g_mix_mutex);
            g_mix_buf.clear();
        }
        return;
    }

    // Only do work when a peer is reachable.
    if (!is_connected()) return;

    // ---- Lazy device open -----------------------------------------------

    if (s_capture_stream == nullptr) {
        if (!open_capture()) {
            // Device unavailable — disable voice to avoid repeated log spam.
            s_enabled = false;
            return;
        }
    }

    if (s_playback_stream == nullptr) {
        if (!open_playback()) {
            s_enabled = false;
            return;
        }
    }

    // ---- Capture: read mic frames and send to peers ----------------------
    //
    // We drain in fixed kVoiceFrameBytes chunks so every packet sent over the
    // network is exactly one frame (320 S16 samples = 20 ms).

    {
        // Stack buffer for one frame.
        static int16_t capture_frame[kVoiceFrameSamples];

        // SDL returns bytes actually read; keep looping while a full frame is
        // available to keep latency low even if update() is called infrequently.
        for (;;) {
            const int got = SDL_GetAudioStreamData(
                s_capture_stream,
                capture_frame,
                kVoiceFrameBytes);

            if (got < kVoiceFrameBytes) {
                // Not enough data yet (got == 0 normally, < 0 on error).
                break;
            }

            // Broadcast raw PCM to all connected peers.
            send_message(kOpVoice, capture_frame, static_cast<uint32_t>(kVoiceFrameBytes));
        }
    }

    // ---- Playback: feed accumulated peer mix into the SDL stream ---------
    //
    // Swap the mix buffer out under the lock, then push it to SDL outside the
    // lock so the IO thread is not blocked during SDL I/O.

    std::vector<int16_t> to_play;
    {
        std::lock_guard<std::mutex> lock(g_mix_mutex);
        to_play.swap(g_mix_buf);
    }

    if (!to_play.empty()) {
        SDL_PutAudioStreamData(
            s_playback_stream,
            to_play.data(),
            static_cast<int>(to_play.size() * sizeof(int16_t)));
    }
}

void shutdown() {
    close_devices();
    {
        std::lock_guard<std::mutex> lock(g_mix_mutex);
        g_mix_buf.clear();
        g_mix_buf.shrink_to_fit();
    }
    s_enabled = false;
}

}  // namespace voice

// ---------------------------------------------------------------------------
// Module registration hook  (called by dusk::online::init() via online.cpp)
// ---------------------------------------------------------------------------

namespace modules {
void init_voice() {
    register_handler(kOpVoice, &on_voice);
}
}  // namespace modules

}  // namespace dusk::online
