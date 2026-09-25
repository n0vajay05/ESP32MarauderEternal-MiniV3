#pragma once

#include <atomic>
#include <stdint.h>

// The main loop owns scheduling; the Wi-Fi task only calls complete(). Never
// submit another frame while the driver still owns the previous one.
class BeaconTxPacer {
 public:
  void reset(uint32_t now) {
    next_ms_ = now;
    started_ms_ = now;
    pending_.store(false);
  }

  bool ready(uint32_t now) const {
    return !pending_.load() && static_cast<int32_t>(now - next_ms_) >= 0;
  }

  void begin(uint32_t now) {
    started_ms_ = now;
    next_ms_ = now + 8;
    // Set before calling the driver: completion can precede the API return.
    pending_.store(true);
  }

  void complete() { pending_.store(false); }

  void rejected(uint32_t now) {
    next_ms_ = now + 50;
    pending_.store(false);
  }

  bool stalled(uint32_t now) const {
    return pending_.load() && static_cast<uint32_t>(now - started_ms_) >= 1000;
  }

 private:
  std::atomic<bool> pending_{false};
  uint32_t next_ms_ = 0;
  uint32_t started_ms_ = 0;
};
