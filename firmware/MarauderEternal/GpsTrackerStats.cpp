#include "GpsTrackerStats.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace marauder {
void GpsTrackerStats::reset(uint32_t now) { *this = GpsTrackerStats(); started_ = now; }
bool GpsTrackerStats::coordinates(int32_t lat, int32_t lon) {
  return lat >= -90000000 && lat <= 90000000 && lon >= -180000000 && lon <= 180000000;
}
double GpsTrackerStats::distanceBetweenMeters(int32_t a, int32_t b, int32_t c, int32_t d) {
  constexpr double rad = .017453292519943295;
  const double latA = a * 1e-6 * rad, latB = c * 1e-6 * rad;
  const double s = sin((latB - latA) / 2), t = sin((double(d) - b) * 1e-6 * rad / 2);
  const double h = std::max(0.0, std::min(1.0, s * s + cos(latA) * cos(latB) * t * t));
  return 12742000 * atan2(sqrt(h), sqrt(1 - h));
}
void GpsTrackerStats::loseFix() { anchor_ = false; current_trusted_ = false; speed_ = 0; }
bool GpsTrackerStats::update(int32_t lat, int32_t lon, float accuracy, uint32_t stamp) {
  if (stopped_ || (observed_ && stamp == observed_time_)) return false;
  observed_ = true; observed_time_ = stamp;
  if (!coordinates(lat, lon) || !std::isfinite(accuracy) || accuracy < 0) {
    ++rejected_; current_trusted_ = false; speed_ = 0; return false;
  }
  const uint32_t dt = stamp - sample_time_;
  if (have_sample_ && anchor_ && dt <= 5000 &&
      distanceBetweenMeters(sample_lat_, sample_lon_, lat, lon) > dt * .001 * 120.0) {
    ++rejected_; current_trusted_ = false; speed_ = 0; return false;
  }
  new_segment_ = have_sample_ && (!anchor_ || dt > 5000);
  if (!anchor_ || !have_sample_ || dt > 5000) {
    anchor_lat_ = lat; anchor_lon_ = lon; anchor_time_ = stamp;
    anchor_accuracy_ = accuracy; anchor_ = true; speed_ = 0;
  } else {
    const double step = distanceBetweenMeters(anchor_lat_, anchor_lon_, lat, lon);
    const double threshold = std::max(3.0, std::hypot(double(anchor_accuracy_), double(accuracy)));
    if (step >= threshold) {
      distance_ += step;
      const uint32_t moving_dt = stamp - anchor_time_;
      speed_ = moving_dt ? step / (moving_dt * .001) : 0;
      anchor_lat_ = lat; anchor_lon_ = lon; anchor_time_ = stamp;
      anchor_accuracy_ = accuracy; motion_time_ = stamp;
    } else if (uint32_t(stamp - motion_time_) > 3000) speed_ = 0;
  }
  sample_lat_ = lat; sample_lon_ = lon; sample_time_ = stamp;
  have_sample_ = true; current_trusted_ = true; ++trusted_;
  return true;
}
bool parseGpsUtc(const char* text, clock::UtcDateTime& time) {
  if (!text || strlen(text) != 19) return false;
  for (unsigned i = 0; i < 19; ++i) {
    const char separator = i == 4 || i == 7 ? '-' : i == 10 ? ' ' : i == 13 || i == 16 ? ':' : 0;
    if (separator ? text[i] != separator : (text[i] < '0' || text[i] > '9')) return false;
  }
  unsigned y,m,d,h,minute,s;
  if (sscanf(text, "%4u-%2u-%2u %2u:%2u:%2u", &y,&m,&d,&h,&minute,&s) != 6) return false;
  time = {uint16_t(y),uint8_t(m),uint8_t(d),uint8_t(h),uint8_t(minute),uint8_t(s)};
  return clock::isValid(time);
}
std::string gpsPoint(int32_t lat, int32_t lon, float altitude,
                     const clock::UtcDateTime* time, bool poi, bool segment) {
  if (!GpsTrackerStats::coordinates(lat, lon)) return "";
  char text[128];
  std::string out = segment && !poi ? "</trkseg><trkseg>\n" : "";
  snprintf(text, sizeof(text), "<%s lat=\"%.6f\" lon=\"%.6f\">", poi ? "wpt" : "trkpt", lat / 1e6, lon / 1e6);
  out += text;
  if (std::isfinite(altitude)) { snprintf(text, sizeof(text), "<ele>%.2f</ele>", altitude); out += text; }
  if (time && clock::isValid(*time)) {
    snprintf(text, sizeof(text), "<time>%04u-%02u-%02uT%02u:%02u:%02uZ</time>",
        time->year,time->month,time->day,time->hour,time->minute,time->second);
    out += text;
  }
  return out + (poi ? "</wpt>\n" : "</trkpt>\n");
}
} // namespace marauder
