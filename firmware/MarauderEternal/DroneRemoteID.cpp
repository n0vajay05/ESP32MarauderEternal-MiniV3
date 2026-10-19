/*
 * Passive Open Drone ID receiver for the Marauder Mini V3.
 *
 * Feature selection and transport handling were adapted from nyanBOX:
 * https://github.com/jbohack/nyanBOX (Copyright 2025 jbohack, MIT)
 *
 * Message decoding is derived from opendroneid-core-c:
 * https://github.com/opendroneid/opendroneid-core-c (Apache-2.0)
 *
 * Modifications include a fixed-memory result store, NimBLE-Arduino 2.x
 * transport, 128x128 joystick UI, channel hopping, and stricter frame bounds.
 * SPDX-License-Identifier: MIT AND Apache-2.0
 */

#include "DroneRemoteID.h"
#include "DroneTracking.h"
#include "DroneExport.h"

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
#include "GpsInterface.h"
#include "StoragePaths.h"
#include <SD.h>
#include <memory>
#include "SPIFFS.h"

extern GpsInterface gps_obj;

extern Display display_obj;
extern Switches u_btn;
extern Switches d_btn;
extern Switches l_btn;
extern Switches r_btn;
extern Switches c_btn;

namespace DroneRemoteID {
namespace {

constexpr uint8_t MAX_RESULTS = 16;
constexpr uint8_t VISIBLE_ROWS = 6;
constexpr uint8_t ODID_MESSAGE_SIZE = 25;
constexpr uint16_t CHANNEL_DWELL_MS = 350;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t ROW_START_Y = 29;
constexpr int16_t ROW_HEIGHT = 14;
constexpr float INVALID_ALTITUDE = -1000.0f;

enum MessageType : uint8_t {
  BASIC_ID = 0,
  LOCATION = 1,
  AUTH = 2,
  SELF_ID = 3,
  SYSTEM = 4,
  OPERATOR_ID = 5,
  PACKED = 0x0F
};

enum Method : uint8_t {
  METHOD_WIFI = 1,
  METHOD_BLE = 2
};

using Drone = drone_tracking::Record;
Drone results[MAX_RESULTS]{};
// Foreground-only snapshot: do not put the enlarged 16-record store on the
// Arduino loop stack, and do not allocate/free it at each screen refresh.
Drone uiSnapshot[MAX_RESULTS]{};
unsigned resultCount = 0;
uint32_t nextGeneration = 0;
drone_tracking::Export exportLog;
String lastExportBase;
bool lastExportOk = false;
Drone previewRecord;
bool previewValid = false;
volatile uint32_t resultRevision = 0;
volatile uint8_t scanChannel = 1;
portMUX_TYPE resultMux = portMUX_INITIALIZER_UNLOCKED;

const uint8_t NAN_DESTINATION[] = {0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00};
const uint8_t NAN_SERVICE_ID[] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};

bool buttonDown(Switches& button) {
  return button.isDown();
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

void resetResults() {
  portENTER_CRITICAL(&resultMux);
  for (auto& result : results) result = Drone();
  resultCount = 0;
  resultRevision++;
  portEXIT_CRITICAL(&resultMux);
}

void formatAddress(const uint8_t* address, char* output, size_t outputSize) {
  snprintf(output, outputSize, "%02X:%02X:%02X:%02X:%02X:%02X",
           address[0], address[1], address[2], address[3], address[4],
           address[5]);
}

uint16_t readLeUint16(const uint8_t* value) {
  return static_cast<uint16_t>(value[0]) |
         (static_cast<uint16_t>(value[1]) << 8);
}

void decodeMessage(const char* address, int8_t rssi, uint8_t channel,
                   Method method, const uint8_t* message, size_t length);

void decodePacked(const char* address, int8_t rssi, uint8_t channel,
                  Method method, const uint8_t* message, size_t length) {
  if (length < 3 || message[1] != ODID_MESSAGE_SIZE || !message[2] ||
      message[2] > 9 || 3U + size_t(message[2]) * ODID_MESSAGE_SIZE > length ||
      (message[0] & 15) > 2) return;
  // Reject a malformed pack before retaining any of its component messages.
  for (unsigned i = 0; i < message[2]; ++i)
    if (!drone_tracking::validMessage(message + 3 + i * 25, 25)) return;
  for (unsigned i = 0; i < message[2]; ++i)
    decodeMessage(address, rssi, channel, method, message + 3 + i * 25, 25);
}

void decodeMessage(const char* address, int8_t rssi, uint8_t channel,
                   Method method, const uint8_t* message, size_t length) {
  if (!message || !length) return;
  if ((message[0] >> 4) == PACKED) {
    decodePacked(address, rssi, channel, method, message, length); return;
  }
  portENTER_CRITICAL(&resultMux);
  if (drone_tracking::observe(results, resultCount, MAX_RESULTS, address, rssi,
      channel, method, message, length, millis(), nextGeneration)) ++resultRevision;
  portEXIT_CRITICAL(&resultMux);
}

void decodeWifiBeacon(const wifi_promiscuous_pkt_t* packet,
                      const uint8_t* payload, uint16_t length) {
  char address[18]{};
  formatAddress(payload + 10, address, sizeof(address));
  uint16_t offset = 36;
  while (offset + 2 <= length) {
    const uint8_t id = payload[offset];
    const uint8_t fieldLength = payload[offset + 1];
    const uint16_t fieldEnd = offset + 2 + fieldLength;
    if (fieldEnd > length)
      return;
    if (id == 0xDD && fieldLength >= 30) {
      const uint8_t* oui = payload + offset + 2;
      const bool knownOui =
          (oui[0] == 0x90 && oui[1] == 0x3A && oui[2] == 0xE6) ||
          (oui[0] == 0xFA && oui[1] == 0x0B && oui[2] == 0xBC);
      if (knownOui) {
        // OUI, application byte and counter precede the encoded message.
        decodeMessage(address, packet->rx_ctrl.rssi, packet->rx_ctrl.channel,
                      METHOD_WIFI, payload + offset + 7, fieldLength - 5);
      }
    }
    offset = fieldEnd;
  }
}

void decodeWifiNan(const wifi_promiscuous_pkt_t* packet,
                   const uint8_t* payload, uint16_t length) {
  if (length < 44 || memcmp(payload + 4, NAN_DESTINATION,
                            sizeof(NAN_DESTINATION)) != 0)
    return;
  // Public action, vendor-specific action, Wi-Fi Alliance OUI, NAN type.
  if (payload[24] != 0x04 || payload[25] != 0x09 ||
      payload[26] != 0x50 || payload[27] != 0x6F ||
      payload[28] != 0x9A || payload[29] != 0x13)
    return;

  char address[18]{};
  formatAddress(payload + 10, address, sizeof(address));
  for (uint16_t offset = 30; offset + 13 <= length; offset++) {
    if (payload[offset] != 0x03 ||
        memcmp(payload + offset + 3, NAN_SERVICE_ID,
               sizeof(NAN_SERVICE_ID)) != 0)
      continue;
    const uint16_t attributeLength = readLeUint16(payload + offset + 1);
    const uint16_t attributeEnd = offset + 3 + attributeLength;
    if (attributeEnd > length || attributeLength < 10)
      return;
    const uint8_t infoLength = payload[offset + 12];
    if (offset + 13 + infoLength > attributeEnd || infoLength < 4)
      return;
    // Service Info is: counter, then an OpenDroneID MessagePack.
    decodeMessage(address, packet->rx_ctrl.rssi, packet->rx_ctrl.channel,
                  METHOD_WIFI, payload + offset + 14, infoLength - 1);
    return;
  }
}

void wifiCallback(void* buffer, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT)
    return;
  const auto* packet = static_cast<wifi_promiscuous_pkt_t*>(buffer);
  const uint8_t* payload = packet->payload;
  const uint16_t length = packet->rx_ctrl.sig_len;
  if (length < 24)
    return;
  const uint8_t frameType = (payload[0] >> 2) & 0x03;
  const uint8_t subtype = (payload[0] >> 4) & 0x0F;
  if (frameType != 0)
    return;
  if (subtype == 8 && length >= 38)
    decodeWifiBeacon(packet, payload, length);
  else if (subtype == 13)
    decodeWifiNan(packet, payload, length);
}

void decodeBleAdvertisement(const NimBLEAdvertisedDevice* device) {
  const std::vector<uint8_t>& payload = device->getPayload();
  size_t offset = 0;
  while (offset < payload.size()) {
    const uint8_t fieldLength = payload[offset];
    if (fieldLength == 0)
      break;
    const size_t fieldEnd = offset + fieldLength + 1;
    if (fieldEnd > payload.size())
      return;
    if (fieldLength >= 30 && payload[offset + 1] == 0x16 &&
        payload[offset + 2] == 0xFA && payload[offset + 3] == 0xFF &&
        payload[offset + 4] == 0x0D) {
      const std::string address = device->getAddress().toString();
      decodeMessage(address.c_str(), device->getRSSI(), 0, METHOD_BLE,
                    payload.data() + offset + 6, fieldLength - 5);
      return;
    }
    offset = fieldEnd;
  }
}

class RemoteIdCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    decodeBleAdvertisement(device);
  }
};

RemoteIdCallbacks remoteIdCallbacks;

uint8_t copyResults(Drone* output) {
  portENTER_CRITICAL(&resultMux);
  const uint8_t count = resultCount;
  memcpy(output, results, sizeof(Drone) * count);
  portEXIT_CRITICAL(&resultMux);
  return count;
}

const char* uaTypeName(uint8_t type) {
  switch (type) {
    case 1: return "Aeroplane";
    case 2: return "Multi/Heli";
    case 3: return "Gyroplane";
    case 4: return "Hybrid";
    case 5: return "Ornithopter";
    case 6: return "Glider";
    case 7: return "Kite";
    case 8: return "Balloon";
    case 9: return "Captive balloon";
    case 10: return "Airship";
    case 12: return "Rocket";
    case 15: return "Other";
    default: return "Unknown";
  }
}

const char* statusName(uint8_t status) {
  switch (status) {
    case 1: return "Ground";
    case 2: return "Airborne";
    case 3: return "EMERGENCY";
    case 4: return "RID failure";
    default: return "Undeclared";
  }
}

String methodName(uint8_t methods) {
  if (methods == (METHOD_WIFI | METHOD_BLE)) return "WiFi+BLE";
  if (methods & METHOD_WIFI) return "WiFi";
  return "BLE";
}

template <typename Canvas>
void drawHeaderTo(Canvas& canvas, bool preview = false) {
  canvas.fillRect(0, 0, TFT_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  canvas.setTextDatum(TC_DATUM);
  canvas.setTextSize(1);
  canvas.setTextColor(TFT_CYAN, TFT_NAVY);
  canvas.drawString(preview ? "TEST REMOTE ID" : "DRONE REMOTE ID", TFT_WIDTH / 2, 4, 1);
  canvas.setTextDatum(TL_DATUM);
}

void drawHeader() { drawHeaderTo(display_obj.tft); }

void drawList(uint8_t selected) {
  Drone* snapshot = uiSnapshot;
  const uint8_t count = copyResults(snapshot);
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader();
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(2, 18);
  display_obj.tft.printf("Found:%u Ch:%u R:detail", count, scanChannel);

  uint8_t start = 0;
  if (selected >= VISIBLE_ROWS)
    start = selected - VISIBLE_ROWS + 1;
  for (uint8_t row = 0; row < VISIBLE_ROWS; row++) {
    const uint8_t index = start + row;
    if (index >= count) break;
    const int16_t y = ROW_START_Y + row * ROW_HEIGHT;
    const bool highlighted = index == selected;
    const uint16_t background = highlighted ? TFT_CYAN : TFT_BLACK;
    const uint16_t foreground = highlighted ? TFT_BLACK : TFT_CYAN;
    display_obj.tft.fillRect(0, y, TFT_WIDTH, ROW_HEIGHT - 1, background);
    display_obj.tft.setTextColor(foreground, background);
    display_obj.tft.setViewport(2, y, TFT_WIDTH - 4, ROW_HEIGHT - 1);
    display_obj.tft.setCursor(0, 2);
    display_obj.tft.printf("%c %d %s", snapshot[index].lost ? 'L' : 'A', snapshot[index].rssi,
                           snapshot[index].id);
    display_obj.tft.resetViewport();
  }
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Center: exit"));
}

template <typename Canvas>
void drawDetailTo(Canvas& canvas, const Drone& drone, uint8_t page, bool preview = false) {
  canvas.fillScreen(TFT_BLACK);
  drawHeaderTo(canvas, preview);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextSize(1);

  if (page == 0) {
    canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    canvas.drawString(drone.id, 2, 20, 1);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.drawString(drone.address, 2, 34, 1);
    canvas.drawString(String("Type: ") + uaTypeName(drone.uaType),
                               2, 48, 1);
    canvas.drawString(String("State: ") + statusName(drone.status),
                               2, 62, 1);
    canvas.drawString(String(drone.rssi) + "dBm " +
                               methodName(drone.methods), 2, 76, 1);
    canvas.drawString(String("Seen ") +
        ((millis() - drone.lastSeen) / 1000) + "s ago", 2, 90, 1);
  }
  else if (page == 1) {
    canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    canvas.drawString("Aircraft location", 2, 20, 1);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    if (drone.locationValid && uint32_t(millis() - drone.locationSeen) < drone_tracking::kLostMs) {
      canvas.drawString("Lat: " + String(drone.latitude, 6), 2, 34, 1);
      canvas.drawString("Lon: " + String(drone.longitude, 6), 2, 48, 1);
      canvas.drawString("Alt: " + (drone.altitude > INVALID_ALTITUDE ? String(drone.altitude, 1) + "m" : "unknown"),
                                 2, 62, 1);
      canvas.drawString("Spd: " + (drone.speed <= 254.25f ? String(drone.speed, 1) + "m/s" : "unknown"),
                                 2, 76, 1);
      canvas.drawString("Dir: " + (drone.direction <= 360 ? String(drone.direction, 0) + "deg" : "unknown"),
                                 2, 90, 1);
    }
    else {
      canvas.drawString("No fresh position", 2, 40, 1);
    }
  }
  else if (page == 2) {
    canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    canvas.drawString("Operator", 2, 20, 1);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setViewport(2, 34, TFT_WIDTH - 4, 13);
    canvas.setCursor(0, 2);
    canvas.print(drone.operatorId[0] ? drone.operatorId : "ID not observed");
    canvas.resetViewport();
    if (drone.operatorLocationValid && uint32_t(millis() - drone.systemSeen) < drone_tracking::kLostMs) {
      canvas.drawString("Lat: " + String(drone.operatorLatitude, 6),
                                 2, 50, 1);
      canvas.drawString("Lon: " + String(drone.operatorLongitude, 6),
                                 2, 64, 1);
      canvas.drawString("Alt: " + (drone.operatorAltitude > INVALID_ALTITUDE ? String(drone.operatorAltitude, 1) + "m" : "unknown"), 2, 78, 1);
    }
    else {
      canvas.drawString("Location not observed", 2, 54, 1);
    }
  }
  else if (page == 3) {
    canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    canvas.drawString("Messages / description", 2, 20, 1);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    char messageFlags[19]{};
    snprintf(messageFlags, sizeof(messageFlags), "B%c L%c A%c S%c Y%c O%c",
             (drone.messagesSeen & (1 << BASIC_ID)) ? '+' : '-',
             (drone.messagesSeen & (1 << LOCATION)) ? '+' : '-',
             (drone.messagesSeen & (1 << AUTH)) ? '+' : '-',
             (drone.messagesSeen & (1 << SELF_ID)) ? '+' : '-',
             (drone.messagesSeen & (1 << SYSTEM)) ? '+' : '-',
             (drone.messagesSeen & (1 << OPERATOR_ID)) ? '+' : '-');
    canvas.drawString(messageFlags, 2, 36, 1);
    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.setTextWrap(true);
    canvas.setCursor(2, 52);
    canvas.print(drone.description[0] ? drone.description :
                          "Description not observed");
    canvas.setTextWrap(false);
    if (drone.messagesSeen & (1 << AUTH)) {
      canvas.drawString("Auth type:" + String(drone.authenticationType) + " pg:" + String(drone.authenticationPage), 2, 90, 1);
      canvas.drawString("Pages:" + String(drone.authenticationLastPage + 1) + " len:" + String(drone.authenticationLength), 2, 104, 1);
    }
  }

  else if (page == 4) {
    canvas.setTextColor(drone.lost ? TFT_RED : TFT_GREEN, TFT_BLACK);
    canvas.drawString(drone.lost ? "LOST (30s timeout)" : "TRACKING", 2, 20, 1);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.drawString(String(drone.messageRate, 1) + " msg/s #" + String(drone.messageCount), 2, 34, 1);
    canvas.drawString("Lost:" + String(drone.lostCount) + " Back:" + String(drone.reacquiredCount), 2, 48, 1);
    if (gps_obj.getFixStatus() && gps_obj.getLastSentenceAgeMs() <= 5000 && drone.locationValid &&
        uint32_t(millis() - drone.locationSeen) < drone_tracking::kLostMs) {
      const double lat = gps_obj.getLatInt() / 1e6, lon = gps_obj.getLonInt() / 1e6;
      canvas.drawString(String(drone_tracking::distanceMeters(lat, lon, drone.latitude, drone.longitude), 0) + "m from receiver", 2, 62, 1);
      canvas.drawString("Bearing: " + String(drone_tracking::bearingDegrees(lat, lon, drone.latitude, drone.longitude), 0) + "deg", 2, 76, 1);
    } else canvas.drawString("Range: GPS/fix needed", 2, 68, 1);
    canvas.drawString("Trail points: " + String(drone.historyCount), 2, 96, 1);
  } else {
    canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    canvas.drawString("Movement trail", 2, 20, 1);
    if (!drone.historyCount) canvas.drawString("No positions yet", 2, 40, 1);
    else {
      const double lat = drone.history[0].latitudeE7 / 1e7;
      const double lon = drone.history[0].longitudeE7 / 1e7;
      double radius = 50;
      for (unsigned i = 0; i < drone.historyCount; ++i)
        radius = std::max(radius, drone_tracking::distanceMeters(lat, lon, drone.history[i].latitudeE7 / 1e7, drone.history[i].longitudeE7 / 1e7) * 1.15);
      const int cx = TFT_WIDTH / 2, cy = 71;
      canvas.drawRect(3, 33, TFT_WIDTH - 6, 76, TFT_DARKGREY);
      canvas.drawLine(cx, 33, cx, 108, TFT_DARKGREY);
      canvas.drawLine(3, cy, TFT_WIDTH - 4, cy, TFT_DARKGREY);
      int previousX = cx, previousY = cy;
      for (unsigned i = 0; i < drone.historyCount; ++i) {
        const double pointLat = drone.history[i].latitudeE7 / 1e7, pointLon = drone.history[i].longitudeE7 / 1e7;
        const double bearing = drone_tracking::bearingDegrees(lat, lon, pointLat, pointLon) * .017453292519943295;
        const double ratio = drone_tracking::distanceMeters(lat, lon, pointLat, pointLon) / radius;
        const int x = cx + int(sin(bearing) * ratio * (TFT_WIDTH / 2 - 7));
        const int y = cy - int(cos(bearing) * ratio * 34);
        if (i && uint32_t(drone.history[i].observedMs - drone.history[i - 1].observedMs) < drone_tracking::kLostMs)
          canvas.drawLine(previousX, previousY, x, y, TFT_CYAN);
        canvas.fillCircle(x, y, i + 1 == drone.historyCount ? 2 : 1, TFT_YELLOW);
        previousX = x; previousY = y;
      }
      canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      canvas.drawString("N up, " + String(radius, 0) + "m radius", 2, 110, 1);
    }
  }

  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.setCursor(2, TFT_HEIGHT - 9);
  canvas.print(F("L:list R:next C:exit"));
}

void drawDetail(uint8_t selected, uint8_t page) {
  const uint8_t count = copyResults(uiSnapshot);
  if (!count || selected >= count) { drawList(0); return; }
  drawDetailTo(display_obj.tft, uiSnapshot[selected], page);
}

void showError(const char* message) {
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader();
  display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
  display_obj.tft.setTextWrap(true);
  display_obj.tft.setCursor(4, 28);
  display_obj.tft.print(message);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(4, TFT_HEIGHT - 12);
  display_obj.tft.print(F("Center: exit"));
}

}  // namespace

bool selectCapturedForSpoof(CapturedDrone& target) {
  releaseControls();
  Drone* snapshot = uiSnapshot;
  const uint8_t count = copyResults(snapshot);
  if (count == 0) {
    showError("No captured drones. Run Drone Remote ID first.");
    while (!c_btn.justPressed() && !l_btn.justPressed())
      delay(5);
    releaseControls();
    return false;
  }

  uint8_t selected = 0;
  bool redraw = true;
  while (true) {
    if (l_btn.justPressed()) {
      releaseControls();
      return false;
    }
    if (u_btn.justPressed() && selected > 0) {
      selected--;
      redraw = true;
    }
    if (d_btn.justPressed() && selected + 1 < count) {
      selected++;
      redraw = true;
    }
    if (c_btn.justPressed()) {
      const Drone& source = snapshot[selected];
      memset(&target, 0, sizeof(target));
      strlcpy(target.address, source.address, sizeof(target.address));
      strlcpy(target.id, source.id, sizeof(target.id));
      memcpy(target.messages, source.rawMessages, sizeof(target.messages));
      target.lastSeen = source.lastSeen;
      target.rssi = source.rssi;
      target.channel = source.channel;
      target.methods = source.methods;
      target.messageMask = source.rawMessageMask;
      releaseControls();
      return target.messageMask != 0;
    }

    if (redraw) {
      display_obj.tft.fillScreen(TFT_BLACK);
      display_obj.tft.fillRect(0, 0, TFT_WIDTH, HEADER_HEIGHT, TFT_NAVY);
      display_obj.tft.setTextDatum(TC_DATUM);
      display_obj.tft.setTextSize(1);
      display_obj.tft.setTextColor(TFT_ORANGE, TFT_NAVY);
      display_obj.tft.drawString("SELECT DRONE TO SPOOF", TFT_WIDTH / 2, 4, 1);
      display_obj.tft.setTextDatum(TL_DATUM);
      display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
      display_obj.tft.setCursor(2, 18);
      display_obj.tft.printf("Captured:%u", count);

      const uint8_t start = selected >= VISIBLE_ROWS ?
          selected - VISIBLE_ROWS + 1 : 0;
      for (uint8_t row = 0; row < VISIBLE_ROWS; row++) {
        const uint8_t index = start + row;
        if (index >= count)
          break;
        const int16_t y = ROW_START_Y + row * ROW_HEIGHT;
        const bool highlighted = index == selected;
        const uint16_t background = highlighted ? TFT_ORANGE : TFT_BLACK;
        const uint16_t foreground = highlighted ? TFT_BLACK : TFT_ORANGE;
        const char method = snapshot[index].methods == (METHOD_WIFI | METHOD_BLE) ?
            '+' : (snapshot[index].methods & METHOD_WIFI ? 'W' : 'B');
        display_obj.tft.fillRect(0, y, TFT_WIDTH, ROW_HEIGHT - 1, background);
        display_obj.tft.setTextColor(foreground, background);
        display_obj.tft.setViewport(2, y, TFT_WIDTH - 4, ROW_HEIGHT - 1);
        display_obj.tft.setCursor(0, 2);
        display_obj.tft.printf("%c %d %s", method, snapshot[index].rssi,
                               snapshot[index].id);
        display_obj.tft.resetViewport();
      }
      display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
      display_obj.tft.print(F("C:select  L:cancel"));
      redraw = false;
    }
    delay(5);
  }
}

void run(uint32_t durationMs) {
  if (!durationMs) releaseControls();
  resetResults();
  lastExportOk = false;
  lastExportBase = "";
  #ifdef HAS_SD
    SD.mkdir(marauder::storage::LOGS_DIR);
    for (unsigned i = 1; i <= 9999; ++i) {
      const String base = String(marauder::storage::LOGS_DIR) + "/remoteid_" + String(i);
      if (!SD.exists(base + ".csv") && !SD.exists(base + ".gpx") && !SD.exists(base + ".part")) {
        if (exportLog.begin(SD, base)) lastExportBase = base;
        break;
      }
    }
    if (lastExportBase.length()) Serial.println("Remote ID logging: " + lastExportBase);
    else Serial.println(F("Remote ID: SD logging unavailable; retaining on-device results"));
  #endif

  if (NimBLEDevice::isInitialized())
    NimBLEDevice::deinit(true);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);

  wifi_promiscuous_filter_t filter{};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_err_t wifiError = esp_wifi_set_promiscuous_filter(&filter);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_promiscuous_rx_cb(wifiCallback);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_promiscuous(true);
  scanChannel = 1;

  NimBLEDevice::init("Marauder-RemoteID");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&remoteIdCallbacks, true);
  scan->setActiveScan(false);
  scan->setInterval(50);
  scan->setWindow(40);
  scan->setMaxResults(0);
  const bool bleScanning = scan->start(0, false, true);

  const bool usable = wifiError == ESP_OK || bleScanning;
  if (!usable)
    showError("Wi-Fi and BLE scans could not start.");

  uint8_t selected = 0;
  bool detailView = false;
  uint8_t detailPage = 0;
  uint32_t drawnRevision = UINT32_MAX;
  uint32_t nextRefresh = 0;
  uint32_t nextChannelHop = millis() + CHANNEL_DWELL_MS;
  const uint32_t startedMs = millis();
  uint32_t nextLog = startedMs;
  char selectedId[21] = {}, selectedAddress[18] = {};

  Serial.printf("Remote ID radios: WiFi=%s BLE=%s\n", wifiError == ESP_OK ? "ready" : "failed", bleScanning ? "ready" : "failed");
  while (true) {
    gps_obj.main();
    const uint32_t now = millis();
    if (c_btn.justPressed() || (durationMs && uint32_t(now - startedMs) >= durationMs)) break;
    portENTER_CRITICAL(&resultMux);
    for (unsigned i = 0; i < resultCount; ++i) drone_tracking::lifecycle(results[i], now);
    portEXIT_CRITICAL(&resultMux);
    if (exportLog.active() && static_cast<int32_t>(now - nextLog) >= 0) {
      nextLog = now + 1000;
      const uint8_t logCount = copyResults(uiSnapshot);
      for (unsigned i = 0; i < logCount; ++i)
        if (!exportLog.sample(uiSnapshot[i], now)) break;
    }

    if (wifiError == ESP_OK &&
        static_cast<int32_t>(millis() - nextChannelHop) >= 0) {
      scanChannel = (scanChannel % 11) + 1;
      esp_wifi_set_channel(scanChannel, WIFI_SECOND_CHAN_NONE);
      nextChannelHop = millis() + CHANNEL_DWELL_MS;
    }

    uint8_t count = 0;
    portENTER_CRITICAL(&resultMux);
    count = resultCount;
    const uint32_t revision = resultRevision;
    portEXIT_CRITICAL(&resultMux);

    if (count == 0)
      selected = 0;
    else if (selected >= count)
      selected = count - 1;

    if (detailView && (selectedId[0] || selectedAddress[0])) {
      portENTER_CRITICAL(&resultMux);
      for (unsigned i = 0; i < resultCount; ++i)
        if ((selectedId[0] && strcmp(results[i].id, selectedId) == 0) ||
            strcmp(results[i].address, selectedAddress) == 0 ||
            strcmp(results[i].wifiAddress, selectedAddress) == 0 ||
            strcmp(results[i].bleAddress, selectedAddress) == 0) { selected = i; break; }
      if (selected < resultCount) {
        strlcpy(selectedId, results[selected].id, sizeof(selectedId));
        strlcpy(selectedAddress, results[selected].address, sizeof(selectedAddress));
      }
      portEXIT_CRITICAL(&resultMux);
    }
    bool redraw = revision != drawnRevision;
    if (!detailView && u_btn.justPressed() && selected > 0) {
      selected--;
      redraw = true;
    }
    if (!detailView && d_btn.justPressed() && selected + 1 < count) {
      selected++;
      redraw = true;
    }
    if (r_btn.justPressed() && count > 0) {
      if (!detailView) {
        detailView = true;
        detailPage = 0;
        portENTER_CRITICAL(&resultMux);
        strlcpy(selectedId, results[selected].id, sizeof(selectedId));
        strlcpy(selectedAddress, results[selected].address, sizeof(selectedAddress));
        for (unsigned i = 0; i < resultCount; ++i) results[i].pinned = i == selected;
        portEXIT_CRITICAL(&resultMux);
      }
      else {
        detailPage = (detailPage + 1) % 6;
      }
      redraw = true;
    }
    if (l_btn.justPressed() && detailView) {
      detailView = false;
      portENTER_CRITICAL(&resultMux);
      for (unsigned i = 0; i < resultCount; ++i) results[i].pinned = false;
      portEXIT_CRITICAL(&resultMux);
      redraw = true;
    }
    if (static_cast<int32_t>(millis() - nextRefresh) >= 0) {
      nextRefresh = millis() + (detailView ? 1000 : CHANNEL_DWELL_MS);
      redraw = true;
    }

    if (usable && redraw) {
      if (detailView)
        drawDetail(selected, detailPage);
      else
        drawList(selected);
      drawnRevision = revision;
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
  if (exportLog.active()) {
    const uint8_t count = copyResults(uiSnapshot);
    for (unsigned i = 0; i < count; ++i) exportLog.sample(uiSnapshot[i], millis());
    lastExportOk = exportLog.finish();
    Serial.println(lastExportOk ? "Remote ID CSV/GPX saved: " + lastExportBase : "Remote ID export failed; partial files retained: " + lastExportBase);
  }
  if (!durationMs) releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

void printRetained() {
  const uint8_t count = copyResults(uiSnapshot);
  Serial.printf("Remote ID retained: %u/16\n", count);
  for (unsigned i = 0; i < count; ++i) {
    const auto& r = uiSnapshot[i];
    Serial.printf("%u: %s [%s] %ddBm, %lu messages, lost=%lu back=%lu\n", i,
        r.id, r.lost ? "lost" : "active", r.rssi, static_cast<unsigned long>(r.messageCount),
        static_cast<unsigned long>(r.lostCount), static_cast<unsigned long>(r.reacquiredCount));
  }
  if (lastExportBase.length()) Serial.println("Remote ID exports: " + lastExportBase + (lastExportOk ? " [complete]" : " [partial]"));
}
bool exportRetained() {
  if (lastExportOk) { Serial.println("Remote ID exports: " + lastExportBase + ".csv and .gpx"); return true; }
  const uint8_t count = copyResults(uiSnapshot);
  if (!count) { Serial.println(F("No Remote ID results to export")); return false; }
  #ifdef HAS_SD
    SD.mkdir(marauder::storage::LOGS_DIR);
    for (unsigned i = 1; i <= 9999; ++i) {
      const String base = String(marauder::storage::LOGS_DIR) + "/remoteid_" + String(i);
      if (SD.exists(base + ".csv") || SD.exists(base + ".gpx") || SD.exists(base + ".part")) continue;
      if (!exportLog.begin(SD, base)) return false;
      lastExportBase = base;
      bool ok = true;
      for (unsigned record = 0; record < count && ok; ++record) ok = exportLog.sample(uiSnapshot[record], millis());
      lastExportOk = exportLog.finish() && ok;
      Serial.println(lastExportOk ? "Remote ID exports: " + base : "Remote ID export incomplete: " + base);
      return lastExportOk;
    }
  #endif
  return false;
}
bool renderPreview(TFT_eSprite& frame, uint8_t page) {
  if (!previewValid || page > 5) return false;
  drawDetailTo(frame, previewRecord, page, true);
  return true;
}
void runChecks() {
  bool ok = true;
  const auto check = [&ok](bool passed, const char* label) {
    Serial.printf("CHECK %s: %s\n", passed ? "PASS" : "FAIL", label);
    ok &= passed; return passed;
  };
  std::unique_ptr<Drone[]> fixture(new (std::nothrow) Drone[3]);
  std::unique_ptr<drone_tracking::Export> writer(new (std::nothrow) drone_tracking::Export);
  if (!check(fixture && writer, "Remote ID fixture memory")) { Serial.println(F("SELFTEST remoteid FAIL")); return; }
  constexpr const char* base = "/ridcheck";
  const auto cleanup = []() {
    SPIFFS.remove("/ridcheck.csv"); SPIFFS.remove("/ridcheck.gpx"); SPIFFS.remove("/ridcheck.part");
    for (unsigned i = 0; i < 16; ++i) SPIFFS.remove(String("/ridcheck.t") + String(i));
  };
  cleanup();
  unsigned count = 0; uint32_t generation = 0;
  uint8_t basic[25] = {0x02, 0x12}; memcpy(basic + 2, "TEST&\"DRONE", 11);
  uint8_t location[25] = {0x12, 0x20, 90, 255};
  const auto put32 = [](uint8_t* p, int32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = uint32_t(n) >> (i * 8); };
  put32(location + 5, 400000000); put32(location + 9, -740000000);
  const auto observe = [&](const char* mac, const uint8_t* message, uint32_t when, uint8_t method = 1) {
    return drone_tracking::observe(fixture.get(), count, 3, mac, -42, method == 1 ? 6 : 0,
                                  method, message, 25, when, generation);
  };
  Drone* record = observe("02:00:00:00:00:01", basic, 1000);
  if (!check(record, "synthetic Basic ID decoded")) { cleanup(); Serial.println(F("SELFTEST remoteid FAIL")); return; }
  check(writer->begin(SPIFFS, base), "export fixture created");
  if (writer->active()) check(writer->sample(*record, 1000), "CSV identity row");
  record = observe("02:00:00:00:00:01", location, 2000);
  check(record && record->locationValid && record->speed == 63.75f && record->altitude == -1000, "valid speed and unknown altitude decoded");
  if (record && writer->active()) check(writer->sample(*record, 2000), "first GPX point");
  put32(location + 5, 400001000); location[15] = 0x98; location[16] = 8; // 100 m.
  record = observe("02:00:00:00:00:01", location, 3000);
  if (record && writer->active()) check(writer->sample(*record, 3000), "second GPX point");
  drone_tracking::lifecycle(*record, 34000);
  check(record->lost && record->lostCount == 1, "lost transition");
  if (writer->active()) check(writer->sample(*record, 34000), "CSV lost event");
  record = observe("02:00:00:00:00:02", basic, 35000, 2);
  check(record && count == 1 && record->methods == 3 && record->reacquiredCount == 1, "cross-radio identity merge and reacquisition");
  put32(location + 5, 400002000); record = observe("02:00:00:00:00:02", location, 35001, 2);
  if (record && writer->active()) check(writer->sample(*record, 36000), "GPX segment after reacquisition");
  check(writer->finish(), "CSV and GPX committed");
  check(!writer->begin(SPIFFS, base), "existing export never overwritten");
  if (record) {
    check(record->rawMessageMask == 3 && !memcmp(record->rawMessages[1], location, 25), "retained raw messages preserved");
    previewRecord = *record;
    const uint32_t offset = millis() - previewRecord.lastSeen;
    previewRecord.firstSeen += offset; previewRecord.lastSeen += offset; previewRecord.locationSeen += offset;
    for (unsigned i = 0; i < previewRecord.historyCount; ++i) previewRecord.history[i].observedMs += offset;
    previewValid = true;
    uint8_t invalid[25] = {0x12}; put32(invalid + 5, 910000000);
    check(drone_tracking::apply(*record, invalid, 25, 37000) && !record->locationValid, "invalid coordinates excluded");
  }
  for (const char* extension : {"csv", "gpx"}) {
    File file = SPIFFS.open(String(base) + "." + extension, FILE_READ);
    check(file && file.size() > 0, "export readback");
    Serial.printf("RID-CHECK-%s-BEGIN\n", extension);
    uint8_t buffer[128];
    while (file && file.available()) {
      const size_t bytes = file.read(buffer, sizeof(buffer));
      if (!bytes) { check(false, "export readback progress"); break; }
      Serial.write(buffer, bytes);
    }
    file.close(); Serial.printf("RID-CHECK-%s-END\n", extension);
  }
  cleanup();
  Serial.println(ok ? F("SELFTEST remoteid PASS") : F("SELFTEST remoteid FAIL"));
}

}  // namespace DroneRemoteID

#else

namespace DroneRemoteID {
void run(uint32_t) {}
void printRetained() {}
bool exportRetained() { return false; }
void runChecks() {}
bool renderPreview(TFT_eSprite&, uint8_t) { return false; }
bool selectCapturedForSpoof(CapturedDrone&) { return false; }
}

#endif
