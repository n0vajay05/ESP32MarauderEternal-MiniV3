#include <cassert>
#include <cstdio>
#include "MirrorInput.h"
#include "Switches.h"
#include "GameInput.h"

namespace {
uint32_t now = 0;
int levels[8] = {};
}
uint32_t millis() { return now; }
int digitalRead(int pin) { return levels[pin]; }
void pinMode(int pin, int mode) { levels[pin] = mode == INPUT_PULLUP ? HIGH : LOW; }
void delay(unsigned ms) { now += ms; }

int main() {
  using namespace marauder::mirror;
  InputState state;
  assert(!state.update(Center, 0) && state.mask(0) == 0);
  state.start(100);
  assert(state.mask(100) == 0);
  assert(state.update(Up | Right, 100));
  assert(state.mask(99) == (Up | Right)); // Concurrent clock sampling.
  assert(state.mask(699) == (Up | Right));
  assert(state.mask(700) == 0); // Lease expiry despite an otherwise live viewer.
  assert(!state.update(32, 701) && state.mask(701) == 0);
  assert(state.update(Center, UINT32_MAX - 10));
  assert(state.mask(100) == Center && state.mask(589) == 0);
  state.stop();
  assert(state.mask(UINT32_MAX - 9) == 0 && !state.update(Center, 0));
  state.start(0);
  assert(state.mask(0) == 0); // A new session cannot inherit a held key.

  Switches up(1, 1000, true, Up), center(2, 1000, false, Center);
  Switches unmapped(3, 1000, true);
  inputState.stop();
  assert(!up.isDown() && !center.isDown());
  levels[1] = LOW;
  assert(up.isDown() && up.justPressed() && !up.justPressed());
  levels[1] = HIGH;
  assert(up.justReleased());

  inputState.start(now);
  inputState.update(Up | Center, now);
  assert(up.justPressed() && center.justPressed() && !unmapped.isDown());
  assert(GameInput::isDown(up)); // Raw held-input paths use the same state.
  now = 500;
  inputState.update(Up | Center, now);
  up.justPressed(); center.justPressed();
  assert(!up.isHeld() && !center.isHeld());
  now = 1001;
  inputState.update(Up | Center, now);
  up.justPressed(); center.justPressed();
  assert(up.isHeld() && center.isHeld());
  inputState.update(0, now);
  levels[2] = HIGH; // Releasing a PC key must not release a physical button.
  assert(up.justReleased() && center.isDown() && !center.justReleased());
  inputState.stop();
  assert(center.isDown());
  levels[2] = LOW;
  assert(center.justReleased());

  inputState.start(now);
  inputState.update(Up, now);
  assert(up.justPressed());
  now += kInputLeaseMs;
  assert(up.justReleased() && !GameInput::isDown(up) && !up.isHeld());
  inputState.stop();
  assert(!inputState.update(Center, now) && !center.isDown());
  std::puts("Mirror controls: session gating, key lease/rollover, edges, holds and physical-button coexistence passed");
}
