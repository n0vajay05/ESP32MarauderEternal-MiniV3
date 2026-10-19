#pragma once

#include <atomic>
#include <cstdint>

namespace marauder { namespace mirror {

enum Button : uint8_t { Up = 1, Down = 2, Left = 4, Right = 8, Center = 16 };
constexpr uint8_t kButtonMask = 31;
constexpr uint32_t kInputLeaseMs = 600;

// The mirror task writes; foreground menus and blocking games read. No input
// queue or allocation is needed on the Mini. A missing keyboard heartbeat must
// release keys even when the foreground is blocked or screen polling continues.
class InputState {
 public:
  void start(uint32_t now) {
    keys_ = 0;
    updated_ = now;
    enabled_ = true;
  }
  void stop() { enabled_ = false; keys_ = 0; }
  bool update(uint32_t keys, uint32_t now) {
    if (!enabled_ || (keys & ~uint32_t(kButtonMask))) return false;
    keys_ = keys;
    updated_ = now;
    return true;
  }
  uint8_t mask(uint32_t now) const {
    // A foreground timestamp can precede a concurrent worker update by a tick.
    // Treat that tiny negative age as fresh instead of an unsigned timeout.
    if (!enabled_ || int32_t(now - updated_.load()) >= int32_t(kInputLeaseMs)) return 0;
    return keys_.load();
  }

 private:
  std::atomic<bool> enabled_{false};
  std::atomic<uint8_t> keys_{0};
  std::atomic<uint32_t> updated_{0};
};

extern InputState inputState;
} }
