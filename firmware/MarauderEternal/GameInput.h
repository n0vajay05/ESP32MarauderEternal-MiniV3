#pragma once

#include <Arduino.h>

#include "Switches.h"

namespace GameInput {

constexpr uint16_t DEBOUNCE_MS = 25;
constexpr uint16_t HOLD_MS = 1000;

inline bool isDown(Switches& button) {
  const bool level = digitalRead(button.getPin());
  return button.getPullup() ? level == LOW : level == HIGH;
}

// A menu invokes a game on the same button edge the game may use. Wait for a
// stable release so that the menu press, including its trailing bounce, cannot
// leak into gameplay as an action or an immediate exit.
inline void waitForRelease(Switches& button) {
  if (!isDown(button)) {
    button.justPressed();
    return;
  }

  bool releaseTiming = false;
  uint32_t releasedAt = 0;
  while (true) {
    const uint32_t now = millis();
    if (isDown(button)) {
      releaseTiming = false;
    }
    else if (!releaseTiming) {
      releaseTiming = true;
      releasedAt = now;
    }
    else if (static_cast<uint32_t>(now - releasedAt) >= DEBOUNCE_MS) {
      button.justPressed();
      return;
    }

    // Keep Switches' edge/hold state synchronized while waiting.
    button.justPressed();
    delay(3);
  }
}

enum class CenterEvent : uint8_t {
  None,
  Tap,
  Hold,
};

class TapHoldButton {
 public:
  explicit TapHoldButton(Switches& button) : button_(button) {}

  void reset(uint32_t now = millis()) {
    rawDown_ = isDown(button_);
    stableDown_ = rawDown_;
    rawChangedAt_ = now;
    pressedAt_ = now;
    holdReported_ = false;
    button_.justPressed();
  }

  CenterEvent poll(uint32_t now) {
    const bool rawDown = isDown(button_);
    button_.justPressed();

    if (rawDown != rawDown_) {
      rawDown_ = rawDown;
      rawChangedAt_ = now;
    }

    if (rawDown_ != stableDown_ &&
        static_cast<uint32_t>(now - rawChangedAt_) >= DEBOUNCE_MS) {
      stableDown_ = rawDown_;
      if (stableDown_) {
        pressedAt_ = now;
        holdReported_ = false;
      }
      else if (!holdReported_) {
        return CenterEvent::Tap;
      }
    }

    if (stableDown_ && !holdReported_ &&
        static_cast<uint32_t>(now - pressedAt_) >= HOLD_MS) {
      holdReported_ = true;
      return CenterEvent::Hold;
    }

    return CenterEvent::None;
  }

 private:
  Switches& button_;
  bool rawDown_ = false;
  bool stableDown_ = false;
  bool holdReported_ = false;
  uint32_t rawChangedAt_ = 0;
  uint32_t pressedAt_ = 0;
};

}  // namespace GameInput
