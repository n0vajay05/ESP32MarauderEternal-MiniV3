#include "GpsTrackerStats.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

int main() {
  using namespace marauder;
  GpsTrackerStats stats; stats.reset(0);
  assert(stats.update(40000000, -74000000, 1, 1000));
  // Walking 1.1 m per second must accumulate past the 3 m jitter floor.
  for (unsigned i = 1; i <= 12; ++i)
    assert(stats.update(40000000 + i * 10, -74000000, 1, 1000 + i * 1000));
  assert(stats.distanceMeters() > 12 && stats.distanceMeters() < 14);
  assert(stats.speedMetersPerSecond() > 1 && stats.speedMetersPerSecond() < 1.2);
  const uint32_t samples = stats.trustedSamples();
  assert(!stats.update(40000120, -74000000, 1, 13000));
  assert(stats.trustedSamples() == samples);
  const double distance = stats.distanceMeters();
  for (unsigned i = 1; i <= 5; ++i) assert(stats.update(40000120, -74000000, 1, 13000 + i * 1000));
  assert(stats.distanceMeters() == distance && stats.speedMetersPerSecond() == 0);
  assert(!stats.update(41000000, -74000000, 1, 19000));
  assert(!stats.update(41000000, -74000000, 1, 19000));
  assert(stats.rejectedSamples() == 1); // One receiver sample, not two polls.
  assert(stats.update(40000120, -74000000, 1, 20000));
  assert(stats.distanceMeters() == distance); // Bad jump never poisons the anchor.
  stats.loseFix();
  assert(stats.speedMetersPerSecond() == 0 && !stats.currentTrusted());
  assert(stats.update(41000000, -74000000, 1, 21000) && stats.startsSegment());
  assert(stats.distanceMeters() == distance); // No route through an unknown gap.
  assert(!stats.update(91000000, 0, 1, 22000));
  assert(!stats.update(41000000, 0, std::numeric_limits<float>::quiet_NaN(), 23000));
  stats.queuedPoint(); assert(stats.queuedPoints() == 1 && stats.trustedSamples() > 1);
  assert(GpsTrackerStats::coordinates(0, 0));
  assert(std::isfinite(GpsTrackerStats::distanceBetweenMeters(90000000,180000000,-90000000,0)));
  assert(GpsTrackerStats::distanceBetweenMeters(0,179999999,0,-179999999) < 1);
  stats.reset(UINT32_MAX - 500);
  assert(stats.update(0, 0, 1, UINT32_MAX - 400));
  assert(stats.update(0, 10, 1, 600));
  assert(stats.elapsedMs(600) == 1101);
  stats.stop(900); assert(stats.elapsedMs(9000) == 1401);
  assert(!stats.update(0,20,1,1000));
  clock::UtcDateTime time;
  assert(parseGpsUtc("2026-10-19 12:34:56", time));
  assert(!parseGpsUtc("2026-02-30 12:34:56", time));
  assert(!parseGpsUtc("2026-10-19 12:34:56Z", time));
  assert(!parseGpsUtc("", time));
  assert(parseGpsUtc("2026-10-19 12:34:56", time));
  const auto point = gpsPoint(40000000,-74000000,12.5,&time);
  assert(point.find("2026-10-19T12:34:56Z") != std::string::npos);
  assert(point.find("40.000000") != std::string::npos);
  assert(gpsPoint(0,0,NAN,nullptr).find("<time>") == std::string::npos);
  assert(gpsPoint(0,0,NAN,nullptr).find("<ele>") == std::string::npos);
  assert(gpsPoint(0,0,1,nullptr,false,true).find("</trkseg><trkseg>") == 0);
  assert(gpsPoint(91000000,0,1,nullptr).empty());
  std::cout << "GPS walking, jitter, outliers, gaps, counters, wraparound and GPX fields passed\n";
}
