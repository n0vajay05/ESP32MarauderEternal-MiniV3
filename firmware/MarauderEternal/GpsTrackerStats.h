#pragma once
#include <cstdint>
#include <string>
#include "UtcTime.h"

namespace marauder {
class GpsTrackerStats {
 public:
  void reset(uint32_t now);
  // True means a trusted sample can be logged. Jitter does not move the trip
  // anchor, so successive short walking steps eventually accumulate distance.
  bool update(int32_t latitudeE6, int32_t longitudeE6, float accuracy, uint32_t stamp);
  void loseFix();
  void stop(uint32_t now) { if (!stopped_) { stopped_ = true; stopped_at_ = now; speed_ = 0; } }
  void queuedPoint() { ++queued_; }
  uint32_t elapsedMs(uint32_t now) const { return (stopped_ ? stopped_at_ : now) - started_; }
  double distanceMeters() const { return distance_; }
  float speedMetersPerSecond() const { return speed_; }
  uint32_t trustedSamples() const { return trusted_; }
  uint32_t rejectedSamples() const { return rejected_; }
  uint32_t queuedPoints() const { return queued_; }
  bool currentTrusted() const { return current_trusted_; }
  bool startsSegment() const { return new_segment_; }
  static bool coordinates(int32_t lat, int32_t lon);
  static double distanceBetweenMeters(int32_t latA, int32_t lonA, int32_t latB, int32_t lonB);
 private:
  uint32_t stopped_at_ = 0;
  bool stopped_ = false;
  uint32_t observed_time_ = 0;
  bool observed_ = false;
  uint32_t started_ = 0, sample_time_ = 0, anchor_time_ = 0, motion_time_ = 0;
  uint32_t trusted_ = 0, rejected_ = 0, queued_ = 0;
  int32_t anchor_lat_ = 0, anchor_lon_ = 0, sample_lat_ = 0, sample_lon_ = 0;
  float anchor_accuracy_ = 0, speed_ = 0;
  double distance_ = 0;
  bool have_sample_ = false, anchor_ = false, current_trusted_ = false, new_segment_ = false;
};
bool parseGpsUtc(const char* text, clock::UtcDateTime& time);
std::string gpsPoint(int32_t lat, int32_t lon, float altitude,
                     const clock::UtcDateTime* time, bool poi = false, bool segment = false);
} // namespace marauder
