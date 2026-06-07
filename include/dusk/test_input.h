#ifndef DUSK_TEST_INPUT_H
#define DUSK_TEST_INPUT_H

#include <cstdint>

// Synthetic controller injection for automated testing / self-driving.
//
// When DUSK_INPUT_PORT is set, a UDP listener accepts datagrams describing a
// desired GameCube-pad state and overlays it onto local pad slot 0 each tick
// (see mDoCPd_c::read). Each instance can use a different port, so two instances
// can be driven INDEPENDENTLY — unlike a shared physical/virtual gamepad.
//
// State is level-triggered: the last received state persists until a new
// datagram arrives, or until kStaleTimeout elapses with no packets (then it
// reverts to neutral so a dead sender can't lock input).
//
// Wire datagram (little-endian, one packet = full desired state):
//   uint32 magic = 'DTIN' (0x4e495444)
//   uint32 buttons         (PAD_BUTTON_* / PAD_TRIGGER_* bitmask)
//   float  stickX, stickY   (-1..1, main analog stick)
//   float  cStickX, cStickY (-1..1, C-stick)
//   float  triggerL, triggerR (0..1)
namespace dusk::test_input {

struct InputState {
    uint32_t buttons = 0;
    float stickX = 0, stickY = 0;
    float cStickX = 0, cStickY = 0;
    float triggerL = 0, triggerR = 0;
};

void init();
void shutdown();
bool active();

// Copies the current desired input into out. Returns false if no state is
// active (never received, or went stale). Safe to call each tick.
bool get_state(InputState* out);

}  // namespace dusk::test_input

#endif  // DUSK_TEST_INPUT_H
