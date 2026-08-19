/*
 * ESP32-C5 Wi-Fi/BLE activity scanner and baseline anomaly detector.
 *
 * The UI concept is inspired by nyanBOX's Scanner and Jam Detector. nyanBOX
 * measures raw 2.4 GHz energy with external nRF24 radios; this adaptation can
 * only count frames and advertisements decoded by the ESP32-C5. It therefore
 * labels possible saturation/interference as an anomaly, never proof of a
 * jammer.
 * SPDX-License-Identifier: MIT
 */

#include "WirelessActivityTools.h"

#include "configs.h"

#if defined(MARAUDER_MINI_V3) && defined(HAS_BT) && \
    defined(HAS_NIMBLE_2) && defined(HAS_SCREEN) && \
    defined(HAS_BUTTONS) && (U_BTN >= 0) && (D_BTN >= 0) && \
    (L_BTN >= 0) && (R_BTN >= 0) && (C_BTN >= 0)

#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "Display.h"
#include "Switches.h"

extern Display display_obj;
extern Switches u_btn;
extern Switches d_btn;
extern Switches l_btn;
extern Switches r_btn;
extern Switches c_btn;

namespace WirelessActivityTools {
namespace {

constexpr uint8_t CHANNEL_COUNT = 11;
constexpr uint16_t CHANNEL_DWELL_MS = 500;
constexpr uint8_t BASELINE_SWEEPS = 3;

struct ChannelSample {
  uint16_t frames;
  int8_t averageRssi;
  int8_t noiseFloor;
};

volatile uint32_t dwellFrames = 0;
volatile int32_t dwellRssiSum = 0;
volatile int32_t dwellNoiseSum = 0;
volatile uint32_t dwellNoiseSamples = 0;
volatile uint32_t bleAdvertisements = 0;
volatile uint8_t activeChannel = 1;
portMUX_TYPE activityMux = portMUX_INITIALIZER_UNLOCKED;

bool buttonDown(Switches& button) {
  const bool level = digitalRead(button.getPin());
  return button.getPullup() ? level == LOW : level == HIGH;
}

void releaseButton(Switches& button) {
  while (buttonDown(button)) {
    button.justPressed();
    delay(5);
  }
  button.justPressed();
}

void releaseControls() {
  releaseButton(u_btn);
  releaseButton(d_btn);
  releaseButton(l_btn);
  releaseButton(r_btn);
  releaseButton(c_btn);
}

void wifiCallback(void* buffer, wifi_promiscuous_pkt_type_t type) {
  if (!buffer || type == WIFI_PKT_MISC)
    return;
  const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
  portENTER_CRITICAL_ISR(&activityMux);
  dwellFrames++;
  dwellRssiSum += packet->rx_ctrl.rssi;
  const int8_t noise = packet->rx_ctrl.noise_floor;
  if (noise <= -20 && noise >= -127) {
    dwellNoiseSum += noise;
    dwellNoiseSamples++;
  }
  portEXIT_CRITICAL_ISR(&activityMux);
}

class ActivityCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice*) override {
    portENTER_CRITICAL(&activityMux);
    bleAdvertisements++;
    portEXIT_CRITICAL(&activityMux);
  }
};

ActivityCallbacks activityCallbacks;

ChannelSample finishDwell() {
  ChannelSample sample{};
  portENTER_CRITICAL(&activityMux);
  const uint32_t frames = dwellFrames;
  sample.frames = min<uint32_t>(frames, UINT16_MAX);
  sample.averageRssi = frames ? dwellRssiSum / static_cast<int32_t>(frames) : -127;
  sample.noiseFloor = dwellNoiseSamples ?
      dwellNoiseSum / static_cast<int32_t>(dwellNoiseSamples) : -127;
  dwellFrames = 0;
  dwellRssiSum = 0;
  dwellNoiseSum = 0;
  dwellNoiseSamples = 0;
  portEXIT_CRITICAL(&activityMux);
  return sample;
}

uint32_t copyBleCount() {
  portENTER_CRITICAL(&activityMux);
  const uint32_t count = bleAdvertisements;
  portEXIT_CRITICAL(&activityMux);
  return count;
}

void resetCounters() {
  portENTER_CRITICAL(&activityMux);
  dwellFrames = 0;
  dwellRssiSum = 0;
  dwellNoiseSum = 0;
  dwellNoiseSamples = 0;
  bleAdvertisements = 0;
  portEXIT_CRITICAL(&activityMux);
}

void drawHeader(const char* title, uint16_t color) {
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, 16, TFT_NAVY);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(color, TFT_NAVY);
  display_obj.tft.drawString(title, TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
}

void drawScanner(const ChannelSample samples[CHANNEL_COUNT], uint32_t bleRate) {
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader("C5 WIFI/BLE SCANNER", TFT_CYAN);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(2, 18);
  display_obj.tft.printf("BLE:%lu/s  Ch:%u", static_cast<unsigned long>(bleRate),
                         activeChannel);

  uint16_t maximum = 1;
  for (uint8_t index = 0; index < CHANNEL_COUNT; index++)
    maximum = max(maximum, samples[index].frames);
  for (uint8_t index = 0; index < CHANNEL_COUNT; index++) {
    const int16_t x = 3 + index * 11;
    const int16_t height = min<int16_t>(67,
        static_cast<int32_t>(samples[index].frames) * 67 / maximum);
    const uint16_t color = (index + 1 == activeChannel) ? TFT_YELLOW : TFT_CYAN;
    display_obj.tft.drawRect(x, 32, 8, 69, TFT_DARKGREY);
    if (height > 0)
      display_obj.tft.fillRect(x + 1, 100 - height, 6, height, color);
    display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display_obj.tft.setCursor(x, 103);
    display_obj.tft.print(index + 1);
  }
  const ChannelSample& current = samples[(activeChannel + CHANNEL_COUNT - 2) % CHANNEL_COUNT];
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, 114);
  if (current.noiseFloor > -127)
    display_obj.tft.printf("N:%ddBm  Center:exit", current.noiseFloor);
  else
    display_obj.tft.print(F("Decoded frames  C:exit"));
}

void drawBaseline(uint8_t sweeps, uint32_t elapsed) {
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader("JAM DETECTOR", TFT_ORANGE);
  display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  display_obj.tft.drawString("Measuring baseline", 4, 25, 1);
  const uint8_t progress = min<uint32_t>(100,
      (elapsed * 100) / (CHANNEL_COUNT * CHANNEL_DWELL_MS * BASELINE_SWEEPS));
  display_obj.tft.drawRect(4, 47, TFT_WIDTH - 8, 13, TFT_DARKGREY);
  display_obj.tft.fillRect(6, 49, (TFT_WIDTH - 12) * progress / 100, 9,
                           TFT_CYAN);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(String(progress) + "%  sweep " +
      String(sweeps + 1) + "/" + String(BASELINE_SWEEPS), 4, 68, 1);
  display_obj.tft.setTextWrap(true);
  display_obj.tft.setCursor(4, 84);
  display_obj.tft.print(F("Keep the local radio environment steady."));
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Center: exit"));
}

void drawDetector(const ChannelSample samples[CHANNEL_COUNT],
                  const uint16_t baselineFrames[CHANNEL_COUNT],
                  const int8_t baselineNoise[CHANNEL_COUNT],
                  uint32_t bleRate, uint32_t baselineBleRate) {
  uint8_t worstChannel = 1;
  uint16_t worstScore = 0;
  int16_t noiseDelta = 0;
  bool activityAlert = false;
  bool noiseAlert = false;
  for (uint8_t index = 0; index < CHANNEL_COUNT; index++) {
    const uint16_t base = max<uint16_t>(1, baselineFrames[index]);
    const uint16_t ratio = min<uint32_t>(999,
        static_cast<uint32_t>(samples[index].frames) * 100 / base);
    int16_t delta = 0;
    if (samples[index].noiseFloor > -127 && baselineNoise[index] > -127)
      delta = samples[index].noiseFloor - baselineNoise[index];
    const uint16_t score = ratio + max<int16_t>(0, delta) * 20;
    if (score >= worstScore) {
      worstScore = score;
      worstChannel = index + 1;
      noiseDelta = delta;
    }
  }
  const uint8_t index = worstChannel - 1;
  activityAlert = samples[index].frames > baselineFrames[index] * 3 + 8;
  noiseAlert = noiseDelta >= 8 && samples[index].frames >= 3;
  const bool bleAlert = bleRate > baselineBleRate * 3 + 5;
  const bool alert = activityAlert || noiseAlert || bleAlert;

  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader("JAM DETECTOR", alert ? TFT_RED : TFT_GREEN);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextColor(alert ? TFT_RED : TFT_GREEN, TFT_BLACK);
  display_obj.tft.setTextSize(2);
  display_obj.tft.drawString(alert ? "ANOMALY" : "NORMAL", TFT_WIDTH / 2, 22, 1);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(String("WiFi ch ") + worstChannel + ": " +
      samples[index].frames + " / " + baselineFrames[index], 4, 48, 1);
  display_obj.tft.drawString(String("Noise delta: ") + noiseDelta + " dB",
                             4, 62, 1);
  display_obj.tft.drawString(String("BLE ads/s: ") + bleRate + " / " +
      baselineBleRate, 4, 76, 1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
  const char* reason = noiseAlert ? "Decoded-frame noise rise" :
      (activityAlert ? "WiFi activity saturation" :
       (bleAlert ? "BLE activity saturation" : "No baseline deviation"));
  display_obj.tft.drawString(reason, 4, 91, 1);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("R:recalibrate C:exit"));
}

void showError(const char* message) {
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader("WIRELESS ACTIVITY", TFT_RED);
  display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
  display_obj.tft.setTextWrap(true);
  display_obj.tft.setCursor(4, 28);
  display_obj.tft.print(message);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(4, TFT_HEIGHT - 12);
  display_obj.tft.print(F("Center: exit"));
}

void runTool(bool detector) {
  releaseControls();
  resetCounters();

  if (NimBLEDevice::isInitialized())
    NimBLEDevice::deinit(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);

  wifi_promiscuous_filter_t filter{};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
  esp_err_t wifiError = esp_wifi_set_promiscuous_filter(&filter);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_promiscuous_rx_cb(wifiCallback);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_promiscuous(true);
  activeChannel = 1;

  NimBLEDevice::init("Marauder-Activity");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&activityCallbacks, true);
  scan->setActiveScan(false);
  scan->setInterval(50);
  scan->setWindow(35);
  scan->setMaxResults(0);
  const bool bleScanning = scan->start(0, false, true);
  const bool usable = wifiError == ESP_OK || bleScanning;
  if (!usable)
    showError("Wi-Fi and BLE receivers could not start.");

  ChannelSample samples[CHANNEL_COUNT]{};
  uint32_t baselineFrameSums[CHANNEL_COUNT]{};
  int32_t baselineNoiseSums[CHANNEL_COUNT]{};
  uint8_t baselineNoiseSamples[CHANNEL_COUNT]{};
  uint16_t baselineFrames[CHANNEL_COUNT]{};
  int8_t baselineNoise[CHANNEL_COUNT];
  memset(baselineNoise, -127, sizeof(baselineNoise));
  uint8_t baselineSweeps = 0;
  bool baselineReady = !detector;
  uint32_t baselineStarted = millis();
  uint32_t baselineBleStart = copyBleCount();
  uint32_t baselineBleRate = 0;
  uint32_t lastBleCount = 0;
  uint32_t bleRate = 0;
  uint32_t lastBleAt = millis();
  uint32_t nextHop = millis() + CHANNEL_DWELL_MS;
  uint32_t nextDraw = 0;

  while (true) {
    if (c_btn.justPressed())
      break;
    if (detector && baselineReady && r_btn.justPressed()) {
      memset(samples, 0, sizeof(samples));
      memset(baselineFrameSums, 0, sizeof(baselineFrameSums));
      memset(baselineNoiseSums, 0, sizeof(baselineNoiseSums));
      memset(baselineNoiseSamples, 0, sizeof(baselineNoiseSamples));
      memset(baselineFrames, 0, sizeof(baselineFrames));
      memset(baselineNoise, -127, sizeof(baselineNoise));
      baselineSweeps = 0;
      baselineReady = false;
      baselineBleRate = 0;
      baselineStarted = millis();
      baselineBleStart = copyBleCount();
      lastBleCount = baselineBleStart;
    }

    const uint32_t now = millis();
    if (wifiError == ESP_OK && static_cast<int32_t>(now - nextHop) >= 0) {
      const uint8_t completed = activeChannel - 1;
      samples[completed] = finishDwell();
      if (detector && !baselineReady) {
        baselineFrameSums[completed] += samples[completed].frames;
        if (samples[completed].noiseFloor > -127) {
          baselineNoiseSums[completed] += samples[completed].noiseFloor;
          baselineNoiseSamples[completed]++;
        }
      }
      activeChannel = (activeChannel % CHANNEL_COUNT) + 1;
      if (activeChannel == 1 && detector && !baselineReady) {
        baselineSweeps++;
        if (baselineSweeps >= BASELINE_SWEEPS) {
          for (uint8_t channel = 0; channel < CHANNEL_COUNT; channel++) {
            baselineFrames[channel] = baselineFrameSums[channel] / BASELINE_SWEEPS;
            if (baselineNoiseSamples[channel])
              baselineNoise[channel] = baselineNoiseSums[channel] /
                                       baselineNoiseSamples[channel];
          }
          const uint32_t elapsedSeconds = max<uint32_t>(1,
              (now - baselineStarted) / 1000);
          baselineBleRate = (copyBleCount() - baselineBleStart) / elapsedSeconds;
          baselineReady = true;
        }
      }
      else if (detector && baselineReady) {
        const bool activityAlert = samples[completed].frames >
            baselineFrames[completed] * 3 + 8;
        const bool noiseAlert = samples[completed].noiseFloor > -127 &&
            baselineNoise[completed] > -127 &&
            samples[completed].noiseFloor - baselineNoise[completed] >= 8;
        if (!activityAlert && !noiseAlert) {
          baselineFrames[completed] =
              (baselineFrames[completed] * 15 + samples[completed].frames) / 16;
          if (samples[completed].noiseFloor > -127)
            baselineNoise[completed] =
                (baselineNoise[completed] * 15 + samples[completed].noiseFloor) / 16;
        }
      }
      esp_wifi_set_channel(activeChannel, WIFI_SECOND_CHAN_NONE);
      nextHop = now + CHANNEL_DWELL_MS;
    }

    if (now - lastBleAt >= 1000) {
      const uint32_t current = copyBleCount();
      bleRate = (current - lastBleCount) * 1000 / max<uint32_t>(1, now - lastBleAt);
      lastBleCount = current;
      lastBleAt = now;
    }
    if (usable && static_cast<int32_t>(now - nextDraw) >= 0) {
      nextDraw = now + 500;
      if (detector && !baselineReady)
        drawBaseline(baselineSweeps, now - baselineStarted);
      else if (detector)
        drawDetector(samples, baselineFrames, baselineNoise, bleRate,
                     baselineBleRate);
      else
        drawScanner(samples, bleRate);
    }
    delay(5);
  }

  if (scan->isScanning())
    scan->stop();
  scan->setScanCallbacks(nullptr);
  scan->clearResults();
  NimBLEDevice::deinit(true);
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

}  // namespace

void runScanner() { runTool(false); }
void runJamDetector() { runTool(true); }

}  // namespace WirelessActivityTools

#else

namespace WirelessActivityTools {
void runScanner() {}
void runJamDetector() {}
}

#endif
