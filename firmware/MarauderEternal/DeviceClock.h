#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "UtcTime.h"

class DeviceClock {
 public:
  void begin();
  bool syncFromGps(const marauder::clock::UtcDateTime& value);
  bool setManual(const marauder::clock::UtcDateTime& value);
  bool nowUtc(marauder::clock::UtcDateTime& value) const;
  bool hasAuthoritativeTime() const;

 private:
  bool apply(const marauder::clock::UtcDateTime& value, bool authoritative,
             bool persist);
  bool buildTime(marauder::clock::UtcDateTime& value) const;
  void persistIfNeeded(int64_t epoch, bool force);

  Preferences preferences_;
  bool preferences_open_ = false;
  bool authoritative_ = false;
  uint32_t last_gps_sync_ms_ = 0;
  int64_t last_persisted_epoch_ = 0;
};
