#pragma once

#include <atomic>
#include <stdint.h>

// A portal visit may queue a bounded batch. Channel changes must wait for all
// accepted frames, not merely for esp_wifi_80211_tx() to return.
class PortalTxTracker {
 public:
  void reset() {
    pending_.store(0);
    failures_.store(0);
  }
  void submitted() { pending_.fetch_add(1); }
  void rejected() { release(); }
  void completed(bool success) {
    if (release() && !success)
      failures_.fetch_add(1);
  }
  uint32_t pending() const { return pending_.load(); }
  uint32_t takeFailures() { return failures_.exchange(0); }

 private:
  bool release() {
    uint32_t pending = pending_.load();
    while (pending != 0) {
      if (pending_.compare_exchange_weak(pending, pending - 1))
        return true;
    }
    return false;
  }
  std::atomic<uint32_t> pending_{0};
  std::atomic<uint32_t> failures_{0};
};
