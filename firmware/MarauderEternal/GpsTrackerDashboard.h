#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "GpsTrackerStats.h"

namespace marauder {
struct GpsTrackerView {
  bool fix = false, receiver = false, logging = false;
  unsigned satellites = 0;
  int32_t latitudeE6 = 0, longitudeE6 = 0;
  float altitude = 0, accuracy = 0;
  uint32_t age = UINT32_MAX, baud = 0;
  char utc[20] = {};
};
template <typename Canvas>
void drawGpsTracker(Canvas& canvas, const GpsTrackerView& view,
                    const GpsTrackerStats& stats, uint32_t now,
                    unsigned page, bool preview = false) {
  canvas.setFreeFont(nullptr); canvas.setTextSize(1); canvas.setTextWrap(false);
  if (preview) {
    canvas.fillScreen(TFT_BLACK); canvas.fillRect(0, 0, TFT_WIDTH, 16, TFT_NAVY);
    canvas.setTextColor(TFT_CYAN, TFT_NAVY); canvas.setTextDatum(TC_DATUM);
    canvas.drawString("TEST GPS TRACKER", TFT_WIDTH / 2, 4, 1); canvas.setTextDatum(TL_DATUM);
  }
  canvas.fillRect(0, 16, TFT_WIDTH, TFT_HEIGHT - 16, TFT_BLACK);
  const bool trusted = view.fix && stats.currentTrusted();
  canvas.setTextColor(trusted ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
  canvas.drawString(page ? "GPS DETAILS  2/2" : "GPS TRIP     1/2", 2, 18, 1);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  const String fix = !view.receiver ? "NO NMEA" : !view.fix ? "NO FIX" : !stats.currentTrusted() ? "BAD FIX" : "FIX";
  if (!page) {
    canvas.drawString(fix + "  SAT:" + String(view.satellites), 2, 31, 1);
    const double distance = stats.distanceMeters();
    canvas.drawString("DIST " + (distance >= 1000 ? String(distance / 1000, 2) + "km" : String(distance, 1) + "m"), 2, 45, 1);
    canvas.drawString("SPD  " + String(trusted ? stats.speedMetersPerSecond() * 3.6f : 0, 1) + "km/h", 2, 57, 1);
    const uint32_t seconds = stats.elapsedMs(now) / 1000;
    char elapsed[24]; snprintf(elapsed, sizeof(elapsed), "TIME %02lu:%02lu:%02lu", static_cast<unsigned long>(seconds / 3600), static_cast<unsigned long>((seconds / 60) % 60), static_cast<unsigned long>(seconds % 60));
    canvas.drawString(elapsed, 2, 69, 1);
    canvas.drawString("FIXES " + String(stats.trustedSamples()) + " DROP " + String(stats.rejectedSamples()), 2, 81, 1);
    canvas.drawString("QUEUED PTS " + String(stats.queuedPoints()), 2, 93, 1);
    canvas.setTextColor(view.logging ? TFT_CYAN : TFT_LIGHTGREY, TFT_BLACK);
    canvas.drawString(view.logging ? "GPX: buffering" : "GPX: off/unavailable", 2, 105, 1);
  } else {
    canvas.drawString(trusted ? "LAT " + String(view.latitudeE6 / 1e6, 6) : "LAT --", 2, 31, 1);
    canvas.drawString(trusted ? "LON " + String(view.longitudeE6 / 1e6, 6) : "LON --", 2, 43, 1);
    canvas.drawString(trusted ? "ALT " + String(view.altitude, 1) + "m" : "ALT --", 2, 55, 1);
    canvas.drawString(trusted ? "ACC +/-" + String(view.accuracy, 1) + "m" : "ACC --", 2, 67, 1);
    canvas.drawString("NMEA " + String(view.baud) + " " + (view.age == UINT32_MAX ? "--" : String(view.age) + "ms"), 2, 79, 1);
    const String utc = view.utc;
    canvas.drawString(utc.length() == 19 ? "UTC " + utc.substring(0, 10) : "UTC unknown", 2, 91, 1);
    if (utc.length() == 19) canvas.drawString(utc.substring(11), 2, 103, 1);
  }
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("L/R:page C:exit", 2, TFT_HEIGHT - 9, 1);
}
} // namespace marauder
