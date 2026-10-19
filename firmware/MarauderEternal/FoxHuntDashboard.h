#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "FoxHunt.h"

namespace marauder::fox {
template <typename Canvas>
void draw(Canvas& canvas, const Target& target, uint32_t now, bool preview = false) {
  canvas.setFreeFont(nullptr); canvas.setTextWrap(false); canvas.setTextSize(1);
  if (preview) {
    canvas.fillScreen(TFT_BLACK); canvas.fillRect(0, 0, TFT_WIDTH, 16, TFT_NAVY);
    canvas.setTextColor(TFT_CYAN, TFT_NAVY); canvas.setTextDatum(TC_DATUM);
    canvas.drawString("TEST FOX HUNT", TFT_WIDTH / 2, 4, 1); canvas.setTextDatum(TL_DATUM);
  }
  canvas.fillRect(0, 16, TFT_WIDTH, TFT_HEIGHT - 16, TFT_BLACK);
  const bool live = target.fresh(now);
  canvas.setTextColor(live ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
  canvas.drawString(!target.active ? "NO TARGET" : live ? "LIVE" : target.seen ? "SIGNAL LOST" : "WAITING FOR RF", 2, 18, 1);
  const String name = target.value.name;
  canvas.setTextColor(TFT_CYAN, TFT_BLACK);
  canvas.drawString(name.substring(0, 20), 2, 32, 1);
  canvas.drawString(name.substring(20, 40), 2, 42, 1);
  canvas.setTextColor(live ? TFT_YELLOW : TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawCentreString(live ? String(target.value.rssi) + " dBm" : "-- dBm", TFT_WIDTH / 2, 58, 2);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("Hits " + String(target.hits) + (target.value.bluetooth ? " BLE" : " CH" + String(target.value.channel)), 2, 84, 1);
  const int signal = target.value.rssi < -100 ? -100 : target.value.rssi > -20 ? -20 : target.value.rssi;
  const int width = live ? (signal + 100) * (TFT_WIDTH - 4) / 80 : 0;
  canvas.drawRect(1, 98, TFT_WIDTH - 2, 12, TFT_DARKGREY);
  if (width) canvas.fillRect(2, 99, width, 10, TFT_CYAN);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("Center: exit", 2, TFT_HEIGHT - 9, 1);
}
} // namespace marauder::fox
