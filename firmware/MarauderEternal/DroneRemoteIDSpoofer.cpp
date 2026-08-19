/*
 * Bounded captured Open Drone ID broadcaster for authorized lab testing.
 *
 * Transport/message handling is adapted from nyanBOX (MIT) and
 * opendroneid-core-c (Apache-2.0). This version only replays a drone explicitly
 * selected from the most recent passive Remote ID scan, uses a moderate update
 * interval, and stops automatically after 60 seconds.
 * SPDX-License-Identifier: MIT AND Apache-2.0
 */

#include "DroneRemoteIDSpoofer.h"

#include "configs.h"

#if defined(MARAUDER_MINI_V3) && defined(HAS_BT) && \
    defined(HAS_NIMBLE_2) && defined(HAS_SCREEN) && \
    defined(HAS_BUTTONS) && (C_BTN >= 0)

#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "Display.h"
#include "Switches.h"

extern Display display_obj;
extern Switches c_btn;

namespace DroneRemoteIDSpoofer {
namespace {

constexpr uint8_t DEFAULT_WIFI_CHANNEL = 6;
constexpr uint16_t MESSAGE_INTERVAL_MS = 250;
constexpr uint32_t MAX_RUNTIME_MS = 60000;

DroneRemoteID::CapturedDrone selectedTarget{};
bool targetSelected = false;

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

void putLe16(uint8_t* output, uint16_t value) {
  output[0] = value & 0xFF;
  output[1] = value >> 8;
}

bool parseAddress(const char* address, uint8_t output[6]) {
  if (!address)
    return false;
  unsigned values[6]{};
  if (sscanf(address, "%02x:%02x:%02x:%02x:%02x:%02x",
             &values[0], &values[1], &values[2], &values[3], &values[4],
             &values[5]) != 6)
    return false;
  for (uint8_t index = 0; index < 6; index++)
    output[index] = values[index];
  return true;
}

uint8_t capturedMessageTypes(uint8_t output[DroneRemoteID::CAPTURED_MESSAGE_TYPES]) {
  uint8_t count = 0;
  for (uint8_t type = 0; type < DroneRemoteID::CAPTURED_MESSAGE_TYPES; type++) {
    if (selectedTarget.messageMask & (1 << type))
      output[count++] = type;
  }
  return count;
}

bool updateBleAdvertisement(
    NimBLEAdvertising* advertising,
    const uint8_t message[DroneRemoteID::CAPTURED_MESSAGE_SIZE],
    uint8_t counter) {
  uint8_t raw[31]{};
  raw[0] = 30;
  raw[1] = 0x16;                      // Service Data, 16-bit UUID.
  raw[2] = 0xFA;
  raw[3] = 0xFF;                      // Open Drone ID UUID 0xFFFA.
  raw[4] = 0x0D;
  raw[5] = counter;
  memcpy(raw + 6, message, DroneRemoteID::CAPTURED_MESSAGE_SIZE);

  if (advertising->isAdvertising())
    advertising->stop();
  advertising->reset();
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  advertising->setDiscoverableMode(BLE_GAP_DISC_MODE_NON);
  advertising->enableScanResponse(false);
  advertising->setAdvertisingInterval(320);      // 200 ms.
  NimBLEAdvertisementData data;
  if (!data.addData(raw, sizeof(raw)) ||
      !advertising->setAdvertisementData(data))
    return false;
  return advertising->start();
}

bool sendWifiBeacon(
    const uint8_t source[6], uint8_t channel,
    const uint8_t message[DroneRemoteID::CAPTURED_MESSAGE_SIZE],
    uint8_t counter, uint16_t sequence) {
  uint8_t frame[83]{};
  frame[0] = 0x80;                    // Beacon.
  memset(frame + 4, 0xFF, 6);
  memcpy(frame + 10, source, 6);
  memcpy(frame + 16, source, 6);
  putLe16(frame + 22, (sequence & 0x0FFF) << 4);
  const uint64_t timestamp = esp_timer_get_time();
  memcpy(frame + 24, &timestamp, sizeof(timestamp));
  putLe16(frame + 32, 100);
  frame[34] = 0x01;
  frame[35] = 0x04;
  frame[36] = 0x00;                   // Empty SSID.
  frame[37] = 0x00;
  const uint8_t rates[] = {0x01, 0x08, 0x82, 0x84, 0x8B, 0x96,
                           0x0C, 0x18, 0x30, 0x48};
  memcpy(frame + 38, rates, sizeof(rates));
  frame[48] = 0x03;
  frame[49] = 0x01;
  frame[50] = channel;
  frame[51] = 0xDD;                   // Vendor-specific Remote ID IE.
  frame[52] = 30;
  frame[53] = 0xFA;
  frame[54] = 0x0B;
  frame[55] = 0xBC;
  frame[56] = 0x0D;
  frame[57] = counter;
  memcpy(frame + 58, message, DroneRemoteID::CAPTURED_MESSAGE_SIZE);
  return esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false) == ESP_OK;
}

void drawStatus(uint32_t started, uint32_t blePackets, uint32_t wifiPackets,
                uint8_t wifiChannel, uint8_t messageCount, bool bleOkay,
                bool wifiOkay, bool exactBleAddress, bool exactWifiAddress) {
  const uint32_t elapsed = millis() - started;
  const uint32_t remaining = elapsed >= MAX_RUNTIME_MS ? 0 :
      (MAX_RUNTIME_MS - elapsed + 999) / 1000;
  display_obj.tft.fillScreen(TFT_BLACK);
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, 16, TFT_RED);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_RED);
  display_obj.tft.drawString("AUTHORIZED DRONE SPOOF", TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  display_obj.tft.setViewport(2, 19, TFT_WIDTH - 4, 14);
  display_obj.tft.setCursor(0, 2);
  display_obj.tft.print(selectedTarget.id);
  display_obj.tft.resetViewport();
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(String("Captured types: ") + messageCount,
                             4, 36, 1);
  display_obj.tft.drawString(String("BLE: ") + (bleOkay ? "on " : "failed ") +
      blePackets + (exactBleAddress ? " exact" : " local"), 4, 50, 1);
  display_obj.tft.drawString(String("WiFi: ") + (wifiOkay ? "ch" : "failed ") +
      (wifiOkay ? String(wifiChannel) : String("")) + " " + wifiPackets +
      (exactWifiAddress ? " exact" : " local"), 4, 64, 1);
  display_obj.tft.setViewport(4, 78, TFT_WIDTH - 8, 13);
  display_obj.tft.setCursor(0, 2);
  display_obj.tft.print(selectedTarget.address);
  display_obj.tft.resetViewport();
  display_obj.tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  display_obj.tft.drawString(String("Auto stop: ") + remaining + "s", 4, 94, 1);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Center: stop now"));
}

void drawError(const char* message) {
  display_obj.tft.fillScreen(TFT_BLACK);
  display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
  display_obj.tft.setTextWrap(true);
  display_obj.tft.setCursor(4, 20);
  display_obj.tft.print(message);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(4, TFT_HEIGHT - 12);
  display_obj.tft.print(F("Center: return"));
}

}  // namespace

bool selectTarget(const DroneRemoteID::CapturedDrone& target) {
  if (target.messageMask == 0)
    return false;
  selectedTarget = target;
  targetSelected = true;
  return true;
}

void run() {
  releaseButton(c_btn);
  if (!targetSelected || selectedTarget.messageMask == 0) {
    drawError("No captured target selected. Run Drone Remote ID first.");
    while (!c_btn.justPressed())
      delay(5);
    releaseButton(c_btn);
    return;
  }
  if (NimBLEDevice::isInitialized())
    NimBLEDevice::deinit(true);

  uint8_t messageTypes[DroneRemoteID::CAPTURED_MESSAGE_TYPES]{};
  const uint8_t messageCount = capturedMessageTypes(messageTypes);
  if (messageCount == 0) {
    drawError("The selected entry has no replayable Remote ID messages.");
    while (!c_btn.justPressed())
      delay(5);
    releaseButton(c_btn);
    return;
  }

  uint8_t capturedAddress[6]{};
  const bool parsedAddress = parseAddress(selectedTarget.address, capturedAddress);
  const bool exactWifiAddress = parsedAddress &&
      (selectedTarget.methods & DroneRemoteID::CAPTURE_METHOD_WIFI);
  uint8_t wifiSource[6]{};
  uint8_t bleAddress[6]{};
  if (exactWifiAddress) {
    memcpy(wifiSource, capturedAddress, sizeof(wifiSource));
    wifiSource[0] &= 0xFE;             // 802.11 source must be unicast.
  }
  else {
    for (uint8_t index = 0; index < 6; index++)
      wifiSource[index] = esp_random();
    wifiSource[0] = (wifiSource[0] & 0xFC) | 0x02;
  }

  if (parsedAddress)
    memcpy(bleAddress, capturedAddress, sizeof(bleAddress));
  else
    for (uint8_t index = 0; index < 6; index++)
      bleAddress[index] = esp_random();

  const NimBLEAddress capturedRandom(bleAddress, BLE_ADDR_RANDOM);
  const bool exactBleAddress = parsedAddress &&
      (selectedTarget.methods & DroneRemoteID::CAPTURE_METHOD_BLE) &&
      capturedRandom.isStatic();
  if (!exactBleAddress) {
    for (uint8_t index = 0; index < 6; index++)
      bleAddress[index] = esp_random();
    bleAddress[0] = (bleAddress[0] & 0x3F) | 0xC0;
  }

  const uint8_t wifiChannel = selectedTarget.channel >= 1 &&
      selectedTarget.channel <= 11 ? selectedTarget.channel : DEFAULT_WIFI_CHANNEL;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  bool wifiOkay = esp_wifi_set_channel(wifiChannel,
                                       WIFI_SECOND_CHAN_NONE) == ESP_OK;

  NimBLEDevice::init("Marauder-RID-Replay");
  const NimBLEAddress randomAddress(bleAddress, BLE_ADDR_RANDOM);
  bool bleOkay = randomAddress.isStatic() &&
      NimBLEDevice::setOwnAddr(randomAddress) &&
      NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  const uint8_t* firstMessage = selectedTarget.messages[messageTypes[0]];
  if (bleOkay)
    bleOkay = updateBleAdvertisement(advertising, firstMessage, 0);

  if (!bleOkay && !wifiOkay) {
    drawError("BLE and Wi-Fi captured-data broadcasts could not start.");
    while (!c_btn.justPressed())
      delay(5);
  }
  else {
    const uint32_t started = millis();
    uint32_t nextMessage = started;
    uint32_t nextDraw = 0;
    uint32_t blePackets = bleOkay ? 1 : 0;
    uint32_t wifiPackets = 0;
    uint8_t messageIndex = 0;
    uint8_t counter = 0;
    uint16_t sequence = 0;
    Serial.printf("[Drone Spoof] replaying captured RID %s (%u message types); maximum 60 seconds\n",
                  selectedTarget.id, messageCount);
    while (millis() - started < MAX_RUNTIME_MS) {
      if (c_btn.justPressed())
        break;
      const uint32_t now = millis();
      if (static_cast<int32_t>(now - nextMessage) >= 0) {
        const uint8_t* message = selectedTarget.messages[messageTypes[messageIndex]];
        if (bleOkay) {
          if (updateBleAdvertisement(advertising, message, counter))
            blePackets++;
          else
            bleOkay = false;
        }
        if (wifiOkay) {
          if (sendWifiBeacon(wifiSource, wifiChannel, message, counter,
                             sequence++))
            wifiPackets++;
          else
            wifiOkay = false;
        }
        messageIndex = (messageIndex + 1) % messageCount;
        counter++;
        nextMessage = now + MESSAGE_INTERVAL_MS;
      }
      if (static_cast<int32_t>(now - nextDraw) >= 0) {
        drawStatus(started, blePackets, wifiPackets, wifiChannel, messageCount,
                   bleOkay, wifiOkay, exactBleAddress, exactWifiAddress);
        nextDraw = now + 500;
      }
      delay(5);
    }
    Serial.printf("[Drone Spoof] stopped; BLE updates=%lu Wi-Fi frames=%lu\n",
                  static_cast<unsigned long>(blePackets),
                  static_cast<unsigned long>(wifiPackets));
  }

  if (advertising->isAdvertising())
    advertising->stop();
  NimBLEDevice::deinit(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  releaseButton(c_btn);
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

}  // namespace DroneRemoteIDSpoofer

#else

namespace DroneRemoteIDSpoofer {
bool selectTarget(const DroneRemoteID::CapturedDrone&) { return false; }
void run() {}
}

#endif
