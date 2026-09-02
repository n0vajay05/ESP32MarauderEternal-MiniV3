#include "DeviceClock.h"

#include <sys/time.h>

namespace {
constexpr int64_t MINIMUM_SANE_EPOCH = 1577836800LL;  // 2020-01-01 UTC
constexpr int64_t MAXIMUM_SANE_EPOCH = 4102444799LL;  // 2099-12-31 UTC
constexpr uint32_t GPS_RESYNC_INTERVAL_MS = 60000;
constexpr int64_t PERSIST_INTERVAL_SECONDS = 6LL * 60LL * 60LL;

uint8_t buildMonth(const char* name) {
  static constexpr char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  for (uint8_t index = 0; index < 12; ++index) {
    if (strncmp(name, months + index * 3, 3) == 0)
      return index + 1;
  }
  return 0;
}
}  // namespace

void DeviceClock::begin() {
  preferences_open_ = preferences_.begin("device-clock", false);
  if (preferences_open_) {
    const uint64_t saved = preferences_.getULong64("last-utc", 0);
    if (saved >= static_cast<uint64_t>(MINIMUM_SANE_EPOCH) &&
        saved <= static_cast<uint64_t>(MAXIMUM_SANE_EPOCH)) {
      marauder::clock::UtcDateTime value{};
      if (marauder::clock::fromUnixSeconds(saved, value)) {
        apply(value, false, false);
        last_persisted_epoch_ = static_cast<int64_t>(saved);
        Serial.println(F("Clock: restored last known UTC; waiting for GPS or manual sync"));
        return;
      }
    }
  }

  marauder::clock::UtcDateTime compiled{};
  if (buildTime(compiled)) {
    apply(compiled, false, false);
    Serial.println(F("Clock: using firmware build UTC until GPS or manual sync"));
  }
}

bool DeviceClock::syncFromGps(const marauder::clock::UtcDateTime& value) {
  if (!marauder::clock::isValid(value))
    return false;
  const uint32_t now = millis();
  if (authoritative_ && last_gps_sync_ms_ != 0 &&
      now - last_gps_sync_ms_ < GPS_RESYNC_INTERVAL_MS)
    return true;
  if (!apply(value, true, false))
    return false;
  last_gps_sync_ms_ = now;

  int64_t epoch = 0;
  if (marauder::clock::toUnixSeconds(value, epoch))
    persistIfNeeded(epoch, false);
  return true;
}

bool DeviceClock::setManual(const marauder::clock::UtcDateTime& value) {
  return apply(value, true, true);
}

bool DeviceClock::nowUtc(marauder::clock::UtcDateTime& value) const {
  const time_t now = time(nullptr);
  return now >= MINIMUM_SANE_EPOCH &&
         marauder::clock::fromUnixSeconds(static_cast<int64_t>(now), value);
}

bool DeviceClock::hasAuthoritativeTime() const {
  return authoritative_;
}

bool DeviceClock::apply(const marauder::clock::UtcDateTime& value,
                        bool authoritative, bool persist) {
  int64_t epoch = 0;
  if (!marauder::clock::toUnixSeconds(value, epoch) ||
      epoch < MINIMUM_SANE_EPOCH || epoch > MAXIMUM_SANE_EPOCH)
    return false;

  timeval current_time{};
  current_time.tv_sec = static_cast<time_t>(epoch);
  current_time.tv_usec = 0;
  if (settimeofday(&current_time, nullptr) != 0)
    return false;

  authoritative_ = authoritative;
  if (persist)
    persistIfNeeded(epoch, true);
  return true;
}

void DeviceClock::persistIfNeeded(int64_t epoch, bool force) {
  if (!preferences_open_)
    return;
  const int64_t difference = epoch >= last_persisted_epoch_
                                 ? epoch - last_persisted_epoch_
                                 : last_persisted_epoch_ - epoch;
  if (!force && last_persisted_epoch_ != 0 &&
      difference < PERSIST_INTERVAL_SECONDS)
    return;
  if (preferences_.putULong64("last-utc", static_cast<uint64_t>(epoch)) ==
      sizeof(uint64_t))
    last_persisted_epoch_ = epoch;
}

bool DeviceClock::buildTime(marauder::clock::UtcDateTime& value) const {
  const char* date = __DATE__;  // "Mmm dd yyyy"
  const char* clock = __TIME__; // "hh:mm:ss"
  value.month = buildMonth(date);
  value.day = static_cast<uint8_t>(atoi(date + 4));
  value.year = static_cast<uint16_t>(atoi(date + 7));
  value.hour = static_cast<uint8_t>(atoi(clock));
  value.minute = static_cast<uint8_t>(atoi(clock + 3));
  value.second = static_cast<uint8_t>(atoi(clock + 6));
  return marauder::clock::isValid(value);
}
