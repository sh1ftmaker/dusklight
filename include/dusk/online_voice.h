#ifndef DUSK_ONLINE_VOICE_H
#define DUSK_ONLINE_VOICE_H

// online_voice.h — public interface for the voice chat feature module.
//
// Default state: disabled (push-to-talk gating; devices are opened lazily on
// first enable and closed lazily when disabled).
//
// Wire format for kOpVoice (opcode 36): raw interleaved S16 mono PCM samples
// at kVoiceSampleRate (16 kHz), no additional header.  Frame size is fixed at
// kVoiceFrameSamples (320 samples = 20 ms).  Sender identity comes from the
// fromId argument supplied by the message bus.
//
// Call site the parent must wire:
//   dusk::online::voice::update();   // once per game frame (any thread)
//
// To gate push-to-talk from the parent input layer:
//   dusk::online::voice::set_enabled(ptt_key_held && dusk::online::is_connected());

namespace dusk::online::voice {

// Enable or disable the voice subsystem.
//
// When transitioning true -> false, mic capture and playback devices are
// lazily closed on the next update() call so the game thread is never blocked
// inside a device-open call on the IO thread.
//
// When transitioning false -> true, devices are opened lazily on the first
// update() call that finds is_connected() true.
void set_enabled(bool enabled);

// Returns the current enabled state (default false).
bool enabled();

// Per-frame update.  Must be called once per game frame.
//
//  - If enabled() && is_connected(): drains captured mic frames from the SDL
//    recording stream and calls send_message(kOpVoice, ...) for each full
//    frame.  Then feeds the accumulated peer mix buffer into the SDL playback
//    stream.
//  - If !enabled(): lazily closes any open devices (no-op if already closed).
//
// All SDL audio device operations happen on the caller (game) thread.
// The bus handler on_voice() only writes to a mutex-guarded mix buffer and
// never touches SDL handles directly.
void update();

// Release all SDL audio resources and reset state.  Safe to call even if
// never enabled.  Called automatically by dusk::online::shutdown() via the
// online.cpp module teardown sequence; the parent does NOT need to call this
// separately unless it wants an explicit early teardown.
void shutdown();

}  // namespace dusk::online::voice

#endif  // DUSK_ONLINE_VOICE_H
