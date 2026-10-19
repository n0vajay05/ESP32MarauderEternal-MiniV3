#pragma once

#include <stdint.h>

namespace marauder {
namespace tracker {

// Separate from the BLE scan-cycle clock, which resets throughout discovery.
class ScanTimer {
 public:
  static constexpr uint32_t kDurationMs = 60000;

  void start(uint32_t now) { started_ = now; active_ = true; }
  void cancel() { active_ = false; }
  uint32_t elapsed(uint32_t now) const { return now - started_; }
  bool expired(uint32_t now) const {
    return active_ && elapsed(now) >= kDurationMs;
  }

 private:
  uint32_t started_ = 0;
  bool active_ = false;
};

}  // namespace tracker
}  // namespace marauder
