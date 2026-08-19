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

namespace DroneRemoteID {
namespace {

constexpr uint8_t MAX_RESULTS = 16;
constexpr uint8_t VISIBLE_ROWS = 6;
constexpr uint8_t ODID_MESSAGE_SIZE = 25;
constexpr uint8_t ODID_ID_SIZE = 20;
constexpr uint8_t ODID_DESCRIPTION_SIZE = 23;
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

struct Drone {
  char address[18];
  char id[ODID_ID_SIZE + 1];
  char operatorId[ODID_ID_SIZE + 1];
  char description[ODID_DESCRIPTION_SIZE + 1];
  double latitude;
  double longitude;
  double operatorLatitude;
  double operatorLongitude;
  float altitude;
  float height;
  float speed;
  float direction;
  float operatorAltitude;
  uint32_t lastSeen;
  int8_t rssi;
  uint8_t channel;
  uint8_t uaType;
  uint8_t idType;
  uint8_t status;
  uint8_t messagesSeen;
  uint8_t methods;
  uint8_t rawMessageMask;
  uint8_t rawMessages[CAPTURED_MESSAGE_TYPES][CAPTURED_MESSAGE_SIZE];
};

Drone results[MAX_RESULTS]{};
uint8_t resultCount = 0;
volatile uint32_t resultRevision = 0;
volatile uint8_t scanChannel = 1;
portMUX_TYPE resultMux = portMUX_INITIALIZER_UNLOCKED;

const uint8_t NAN_DESTINATION[] = {0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00};
const uint8_t NAN_SERVICE_ID[] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};

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

void resetResults() {
  portENTER_CRITICAL(&resultMux);
  memset(results, 0, sizeof(results));
  resultCount = 0;
  resultRevision++;
  portEXIT_CRITICAL(&resultMux);
}

void formatAddress(const uint8_t* address, char* output, size_t outputSize) {
  snprintf(output, outputSize, "%02X:%02X:%02X:%02X:%02X:%02X",
           address[0], address[1], address[2], address[3], address[4],
           address[5]);
}

void copyPrintable(char* destination, size_t destinationSize,
                   const uint8_t* source, size_t sourceSize) {
  memset(destination, 0, destinationSize);
  const size_t copied = min(sourceSize, destinationSize - 1);
  for (size_t index = 0; index < copied; index++) {
    if (source[index] == 0)
      break;
    if (source[index] < 32 || source[index] > 126)
      break;
    destination[index] = static_cast<char>(source[index]);
  }
}

int32_t readLeInt32(const uint8_t* value) {
  return static_cast<int32_t>(
      static_cast<uint32_t>(value[0]) |
      (static_cast<uint32_t>(value[1]) << 8) |
      (static_cast<uint32_t>(value[2]) << 16) |
      (static_cast<uint32_t>(value[3]) << 24));
}

uint16_t readLeUint16(const uint8_t* value) {
  return static_cast<uint16_t>(value[0]) |
         (static_cast<uint16_t>(value[1]) << 8);
}

float decodeAltitude(uint16_t encoded) {
  return encoded == 0xFFFF ? INVALID_ALTITUDE :
      static_cast<float>(encoded) * 0.5f - 1000.0f;
}

Drone* findOrCreateLocked(const char* address, int8_t rssi,
                          uint8_t channel, Method method) {
  uint8_t index = resultCount;
  for (uint8_t current = 0; current < resultCount; current++) {
    if (strncmp(results[current].address, address,
                sizeof(results[current].address)) == 0) {
      index = current;
      break;
    }
  }
  if (index == resultCount && resultCount < MAX_RESULTS) {
    resultCount++;
    Drone& created = results[index];
    memset(&created, 0, sizeof(created));
    strlcpy(created.address, address, sizeof(created.address));
    snprintf(created.id, sizeof(created.id), "RID-%c%c%c%c",
             address[12], address[13], address[15], address[16]);
    created.altitude = INVALID_ALTITUDE;
    created.height = INVALID_ALTITUDE;
    created.operatorAltitude = INVALID_ALTITUDE;
    created.speed = 255.0f;
    created.direction = 361.0f;
  }
  if (index >= MAX_RESULTS)
    return nullptr;

  Drone& drone = results[index];
  drone.rssi = rssi;
  if (channel != 0)
    drone.channel = channel;
  drone.lastSeen = millis();
  drone.methods |= method;
  return &drone;
}

void decodeMessage(const char* address, int8_t rssi, uint8_t channel,
                   Method method, const uint8_t* message, size_t length);

void decodePacked(const char* address, int8_t rssi, uint8_t channel,
                  Method method, const uint8_t* message, size_t length) {
  if (length < 3 || message[1] != ODID_MESSAGE_SIZE)
    return;
  const uint8_t available = (length - 3) / ODID_MESSAGE_SIZE;
  const uint8_t count = min(message[2], available);
  for (uint8_t index = 0; index < count; index++) {
    decodeMessage(address, rssi, channel, method,
                  message + 3 + index * ODID_MESSAGE_SIZE,
                  ODID_MESSAGE_SIZE);
  }
}

void decodeMessage(const char* address, int8_t rssi, uint8_t channel,
                   Method method, const uint8_t* message, size_t length) {
  if (!message || length < ODID_MESSAGE_SIZE)
    return;
  const uint8_t type = (message[0] >> 4) & 0x0F;
  if (type == PACKED) {
    decodePacked(address, rssi, channel, method, message, length);
    return;
  }
  if (type > OPERATOR_ID && type != AUTH)
    return;

  portENTER_CRITICAL(&resultMux);
  Drone* drone = findOrCreateLocked(address, rssi, channel, method);
  if (!drone) {
    portEXIT_CRITICAL(&resultMux);
    return;
  }

  if (type < CAPTURED_MESSAGE_TYPES) {
    memcpy(drone->rawMessages[type], message, CAPTURED_MESSAGE_SIZE);
    drone->rawMessageMask |= 1 << type;
  }

  switch (type) {
    case BASIC_ID:
      drone->idType = (message[1] >> 4) & 0x0F;
      drone->uaType = message[1] & 0x0F;
      copyPrintable(drone->id, sizeof(drone->id), message + 2,
                    ODID_ID_SIZE);
      if (drone->id[0] == '\0')
        snprintf(drone->id, sizeof(drone->id), "RID-%c%c%c%c",
                 address[12], address[13], address[15], address[16]);
      drone->messagesSeen |= 1 << BASIC_ID;
      break;

    case LOCATION: {
      drone->status = (message[1] >> 4) & 0x0F;
      const bool speedMultiplier = (message[1] & 0x01) != 0;
      const bool eastWest = (message[1] & 0x02) != 0;
      drone->direction = message[2] + (eastWest ? 180.0f : 0.0f);
      drone->speed = message[3] == 0xFF ? 255.0f :
          (speedMultiplier ? message[3] * 0.75f + 63.75f :
                             message[3] * 0.25f);
      drone->latitude = readLeInt32(message + 5) / 10000000.0;
      drone->longitude = readLeInt32(message + 9) / 10000000.0;
      drone->altitude = decodeAltitude(readLeUint16(message + 15));
      drone->height = decodeAltitude(readLeUint16(message + 17));
      drone->messagesSeen |= 1 << LOCATION;
      break;
    }

    case AUTH:
      drone->messagesSeen |= 1 << AUTH;
      break;

    case SELF_ID:
      copyPrintable(drone->description, sizeof(drone->description),
                    message + 2, ODID_DESCRIPTION_SIZE);
      drone->messagesSeen |= 1 << SELF_ID;
      break;

    case SYSTEM:
      drone->operatorLatitude = readLeInt32(message + 2) / 10000000.0;
      drone->operatorLongitude = readLeInt32(message + 6) / 10000000.0;
      drone->operatorAltitude = decodeAltitude(readLeUint16(message + 18));
      drone->messagesSeen |= 1 << SYSTEM;
      break;

    case OPERATOR_ID:
      copyPrintable(drone->operatorId, sizeof(drone->operatorId),
                    message + 2, ODID_ID_SIZE);
      drone->messagesSeen |= 1 << OPERATOR_ID;
      break;
  }
  resultRevision++;
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

void drawHeader() {
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_NAVY);
  display_obj.tft.drawString("DRONE REMOTE ID", TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
}

void drawList(uint8_t selected) {
  Drone snapshot[MAX_RESULTS]{};
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
    display_obj.tft.printf("%d %s", snapshot[index].rssi,
                           snapshot[index].id);
    display_obj.tft.resetViewport();
  }
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Center: exit"));
}

void drawDetail(uint8_t selected, uint8_t page) {
  Drone snapshot[MAX_RESULTS]{};
  const uint8_t count = copyResults(snapshot);
  if (count == 0 || selected >= count) {
    drawList(0);
    return;
  }
  const Drone& drone = snapshot[selected];
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextSize(1);

  if (page == 0) {
    display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    display_obj.tft.drawString(drone.id, 2, 20, 1);
    display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
    display_obj.tft.drawString(drone.address, 2, 34, 1);
    display_obj.tft.drawString(String("Type: ") + uaTypeName(drone.uaType),
                               2, 48, 1);
    display_obj.tft.drawString(String("State: ") + statusName(drone.status),
                               2, 62, 1);
    display_obj.tft.drawString(String(drone.rssi) + "dBm " +
                               methodName(drone.methods), 2, 76, 1);
    display_obj.tft.drawString(String("Seen ") +
        ((millis() - drone.lastSeen) / 1000) + "s ago", 2, 90, 1);
  }
  else if (page == 1) {
    display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    display_obj.tft.drawString("Aircraft location", 2, 20, 1);
    display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
    if (drone.messagesSeen & (1 << LOCATION)) {
      display_obj.tft.drawString("Lat: " + String(drone.latitude, 6), 2, 34, 1);
      display_obj.tft.drawString("Lon: " + String(drone.longitude, 6), 2, 48, 1);
      display_obj.tft.drawString("Alt: " + String(drone.altitude, 1) + "m",
                                 2, 62, 1);
      display_obj.tft.drawString("Spd: " + String(drone.speed, 1) + "m/s",
                                 2, 76, 1);
      display_obj.tft.drawString("Dir: " + String(drone.direction, 0) + "deg",
                                 2, 90, 1);
    }
    else {
      display_obj.tft.drawString("Not observed", 2, 40, 1);
    }
  }
  else if (page == 2) {
    display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    display_obj.tft.drawString("Operator", 2, 20, 1);
    display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
    display_obj.tft.setViewport(2, 34, TFT_WIDTH - 4, 13);
    display_obj.tft.setCursor(0, 2);
    display_obj.tft.print(drone.operatorId[0] ? drone.operatorId : "ID not observed");
    display_obj.tft.resetViewport();
    if (drone.messagesSeen & (1 << SYSTEM)) {
      display_obj.tft.drawString("Lat: " + String(drone.operatorLatitude, 6),
                                 2, 50, 1);
      display_obj.tft.drawString("Lon: " + String(drone.operatorLongitude, 6),
                                 2, 64, 1);
      display_obj.tft.drawString("Alt: " + String(drone.operatorAltitude, 1) +
                                 "m", 2, 78, 1);
    }
    else {
      display_obj.tft.drawString("Location not observed", 2, 54, 1);
    }
  }
  else {
    display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    display_obj.tft.drawString("Messages / description", 2, 20, 1);
    display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
    char messageFlags[16]{};
    snprintf(messageFlags, sizeof(messageFlags), "B%c L%c A%c S%c Y%c O%c",
             (drone.messagesSeen & (1 << BASIC_ID)) ? '+' : '-',
             (drone.messagesSeen & (1 << LOCATION)) ? '+' : '-',
             (drone.messagesSeen & (1 << AUTH)) ? '+' : '-',
             (drone.messagesSeen & (1 << SELF_ID)) ? '+' : '-',
             (drone.messagesSeen & (1 << SYSTEM)) ? '+' : '-',
             (drone.messagesSeen & (1 << OPERATOR_ID)) ? '+' : '-');
    display_obj.tft.drawString(messageFlags, 2, 36, 1);
    display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
    display_obj.tft.setTextWrap(true);
    display_obj.tft.setCursor(2, 52);
    display_obj.tft.print(drone.description[0] ? drone.description :
                          "Description not observed");
    display_obj.tft.setTextWrap(false);
  }

  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("L:list R:next C:exit"));
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
  Drone snapshot[MAX_RESULTS]{};
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

void run() {
  releaseControls();
  resetResults();

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

  while (true) {
    if (c_btn.justPressed())
      break;

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
      }
      else {
        detailPage = (detailPage + 1) % 4;
      }
      redraw = true;
    }
    if (l_btn.justPressed() && detailView) {
      detailView = false;
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
  releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

}  // namespace DroneRemoteID

#else

namespace DroneRemoteID {
void run() {}
bool selectCapturedForSpoof(CapturedDrone&) { return false; }
}

#endif
