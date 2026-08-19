#include "WiFiCameraDetector.h"

#include "configs.h"

#if defined(MARAUDER_MINI_V3) && defined(HAS_SCREEN) && \
    defined(HAS_BUTTONS) && (U_BTN >= 0) && (D_BTN >= 0) && \
    (L_BTN >= 0) && (R_BTN >= 0) && (C_BTN >= 0)

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

namespace WiFiCameraDetector {
namespace {

// This is a receive-only detector. Camera fingerprints were independently
// checked against the IEEE public OUI registry; the feature selection was
// inspired by the MIT-licensed nyanBOX project.
// https://github.com/jbohack/nyanBOX
constexpr uint8_t MAX_RESULTS = 32;
constexpr uint8_t MAX_EUFY_NETWORKS = 8;
constexpr uint8_t VISIBLE_ROWS = 6;
constexpr uint16_t CHANNEL_DWELL_MS = 350;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t ROW_START_Y = 29;
constexpr int16_t ROW_HEIGHT = 14;

enum class CameraVendor : uint8_t {
  Ring,
  Blink,
  Nest,
  Arlo,
  Wyze,
  Reolink,
  Hikvision,
  Dahua,
  Axis,
  Amcrest,
  Eufy,
  Unknown
};

struct CameraOui {
  uint32_t prefix;
  CameraVendor vendor;
};

struct Detection {
  char address[18];
  char vendor[12];
  char ssid[33];
  char evidence[14];
  uint8_t addressBytes[6];
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t channel;
  uint8_t confidence;
  bool hasBssid;
  uint32_t lastSeen;
};

enum DetectionConfidence : uint8_t {
  CONFIDENCE_LOW = 1,
  CONFIDENCE_MEDIUM = 2,
  CONFIDENCE_HIGH = 3
};

// Only vendor-specific camera prefixes are included. Broad consumer-device
// vendors are intentionally omitted because an OUI alone would create many
// false camera matches.
constexpr CameraOui CAMERA_OUIS[] = {
  {0x54E019, CameraVendor::Ring}, {0x9873C4, CameraVendor::Ring},
  {0x5C475E, CameraVendor::Ring}, {0x9C7613, CameraVendor::Ring},
  {0x343EA4, CameraVendor::Ring}, {0x187F88, CameraVendor::Ring},
  {0x649A63, CameraVendor::Ring}, {0x90486C, CameraVendor::Ring},
  {0x000B7F, CameraVendor::Ring}, {0x74AB93, CameraVendor::Blink},
  {0x641666, CameraVendor::Nest}, {0x18B430, CameraVendor::Nest},
  {0xFC9C98, CameraVendor::Arlo}, {0xA41162, CameraVendor::Arlo},
  {0xD03F27, CameraVendor::Wyze}, {0x80482C, CameraVendor::Wyze},
  {0x7C78B2, CameraVendor::Wyze}, {0x2CAA8E, CameraVendor::Wyze},
  {0xEC71DB, CameraVendor::Reolink},

  {0x0C75D2, CameraVendor::Hikvision}, {0x548C81, CameraVendor::Hikvision},
  {0x244845, CameraVendor::Hikvision}, {0xECC89C, CameraVendor::Hikvision},
  {0x8CE748, CameraVendor::Hikvision}, {0x2428FD, CameraVendor::Hikvision},
  {0xACB92F, CameraVendor::Hikvision}, {0xD4E853, CameraVendor::Hikvision},
  {0x240F9B, CameraVendor::Hikvision}, {0xC06DED, CameraVendor::Hikvision},
  {0x2432AE, CameraVendor::Hikvision}, {0xE0BAAD, CameraVendor::Hikvision},
  {0xE0CA3C, CameraVendor::Hikvision}, {0xDC07F8, CameraVendor::Hikvision},
  {0x64DB8B, CameraVendor::Hikvision}, {0x94E1AC, CameraVendor::Hikvision},
  {0x5803FB, CameraVendor::Hikvision}, {0x4447CC, CameraVendor::Hikvision},
  {0x98DF82, CameraVendor::Hikvision}, {0xC056E3, CameraVendor::Hikvision},
  {0xBCAD28, CameraVendor::Hikvision}, {0x80F5AE, CameraVendor::Hikvision},
  {0xBC2978, CameraVendor::Hikvision}, {0xA42902, CameraVendor::Hikvision},
  {0x188025, CameraVendor::Hikvision}, {0x08CC81, CameraVendor::Hikvision},
  {0xA41437, CameraVendor::Hikvision}, {0xF84DFC, CameraVendor::Hikvision},
  {0x849A40, CameraVendor::Hikvision}, {0xC0517E, CameraVendor::Hikvision},
  {0x2CA59C, CameraVendor::Hikvision}, {0x40ACBF, CameraVendor::Hikvision},
  {0x98F112, CameraVendor::Hikvision}, {0x989DE5, CameraVendor::Hikvision},
  {0x3C1BF8, CameraVendor::Hikvision}, {0x2857BE, CameraVendor::Hikvision},
  {0xACCB51, CameraVendor::Hikvision}, {0x5850ED, CameraVendor::Hikvision},
  {0x1012FB, CameraVendor::Hikvision}, {0x4CF5DC, CameraVendor::Hikvision},
  {0x988B0A, CameraVendor::Hikvision}, {0x807C62, CameraVendor::Hikvision},
  {0xBC9B5E, CameraVendor::Hikvision}, {0x80BEAF, CameraVendor::Hikvision},
  {0x040312, CameraVendor::Hikvision}, {0x1868CB, CameraVendor::Hikvision},
  {0xBCBAC2, CameraVendor::Hikvision}, {0xECA971, CameraVendor::Hikvision},
  {0x4C62DF, CameraVendor::Hikvision}, {0x4419B6, CameraVendor::Hikvision},
  {0xBC5E33, CameraVendor::Hikvision}, {0xFC9FFD, CameraVendor::Hikvision},
  {0xE8A0ED, CameraVendor::Hikvision}, {0x5C345B, CameraVendor::Hikvision},
  {0x4CBD8F, CameraVendor::Hikvision}, {0x24B105, CameraVendor::Hikvision},
  {0x340962, CameraVendor::Hikvision}, {0xA4A459, CameraVendor::Hikvision},
  {0x80489F, CameraVendor::Hikvision}, {0xE0DF13, CameraVendor::Hikvision},
  {0xDCD26A, CameraVendor::Hikvision}, {0x50E538, CameraVendor::Hikvision},
  {0xC42F90, CameraVendor::Hikvision}, {0x54C415, CameraVendor::Hikvision},
  {0xB4A382, CameraVendor::Hikvision}, {0x686DBC, CameraVendor::Hikvision},
  {0xA4D5C2, CameraVendor::Hikvision}, {0xA0FF0C, CameraVendor::Hikvision},
  {0x085411, CameraVendor::Hikvision}, {0x743FC2, CameraVendor::Hikvision},
  {0x08A189, CameraVendor::Hikvision}, {0x44A642, CameraVendor::Hikvision},

  {0x74C929, CameraVendor::Dahua}, {0x6C1C71, CameraVendor::Dahua},
  {0x08EDED, CameraVendor::Dahua}, {0xE02EFE, CameraVendor::Dahua},
  {0x5CF51A, CameraVendor::Dahua}, {0xFC5F49, CameraVendor::Dahua},
  {0x24526A, CameraVendor::Dahua}, {0x9002A9, CameraVendor::Dahua},
  {0xE0508B, CameraVendor::Dahua}, {0xC4AAC4, CameraVendor::Dahua},
  {0xA0BD1D, CameraVendor::Dahua}, {0x9C1463, CameraVendor::Dahua},
  {0xBC325F, CameraVendor::Dahua}, {0x38AF29, CameraVendor::Dahua},
  {0xF8CE07, CameraVendor::Dahua}, {0x8CE9B4, CameraVendor::Dahua},
  {0x14A78B, CameraVendor::Dahua}, {0x4C11BF, CameraVendor::Dahua},
  {0x64FD29, CameraVendor::Dahua}, {0x98F9CC, CameraVendor::Dahua},
  {0xD4430E, CameraVendor::Dahua}, {0xF4B1C2, CameraVendor::Dahua},
  {0x3CE36B, CameraVendor::Dahua}, {0xFCB69D, CameraVendor::Dahua},
  {0xE4246C, CameraVendor::Dahua}, {0xC0395A, CameraVendor::Dahua},
  {0xB44C3B, CameraVendor::Dahua}, {0x3CEF8C, CameraVendor::Dahua},
  {0x30DDAA, CameraVendor::Dahua},

  {0xB8A44F, CameraVendor::Axis}, {0x00408C, CameraVendor::Axis},
  {0xE82725, CameraVendor::Axis}, {0xACCC8E, CameraVendor::Axis},
  {0x00651E, CameraVendor::Amcrest}, {0x9C8ECD, CameraVendor::Amcrest},
  {0xA06032, CameraVendor::Amcrest},

  // Eufy is an Anker brand. These five MA-L assignments are registered by
  // IEEE to Smart Innovation LLC and are used by Eufy cameras/HomeBase units.
  // An OUI-only result remains a candidate because other Eufy product classes
  // can share the same assignments. A hidden AP or a client linked to one is
  // reported with stronger evidence below.
  {0x8C8580, CameraVendor::Eufy}, {0x0417B6, CameraVendor::Eufy},
  {0x102CB1, CameraVendor::Eufy}, {0x90BFD9, CameraVendor::Eufy},
  {0x2C8D48, CameraVendor::Eufy}
};

Detection results[MAX_RESULTS]{};
// UI rendering and target selection are synchronous on the Arduino loop task.
// Reuse one static snapshot so nested draw calls do not place two 3.2 KB
// Detection arrays on the comparatively small loop-task stack.
Detection uiSnapshot[MAX_RESULTS]{};
uint8_t resultCount = 0;
uint8_t eufyNetworks[MAX_EUFY_NETWORKS][6]{};
uint8_t eufyNetworkCount = 0;
volatile uint32_t resultRevision = 0;
volatile uint8_t scanChannel = 1;
portMUX_TYPE resultMux = portMUX_INITIALIZER_UNLOCKED;

const char* vendorName(CameraVendor vendor) {
  switch (vendor) {
    case CameraVendor::Ring: return "Ring";
    case CameraVendor::Blink: return "Blink";
    case CameraVendor::Nest: return "Nest";
    case CameraVendor::Arlo: return "Arlo";
    case CameraVendor::Wyze: return "Wyze";
    case CameraVendor::Reolink: return "Reolink";
    case CameraVendor::Hikvision: return "Hikvision";
    case CameraVendor::Dahua: return "Dahua";
    case CameraVendor::Axis: return "Axis";
    case CameraVendor::Amcrest: return "Amcrest";
    case CameraVendor::Eufy: return "Eufy";
    default: return "Camera";
  }
}

CameraVendor vendorFromMac(const uint8_t* address) {
  if ((address[0] & 0x01) != 0)
    return CameraVendor::Unknown;
  const uint32_t prefix = (static_cast<uint32_t>(address[0]) << 16) |
                          (static_cast<uint32_t>(address[1]) << 8) |
                          address[2];
  for (const CameraOui& entry : CAMERA_OUIS) {
    if (entry.prefix == prefix)
      return entry.vendor;
  }
  return CameraVendor::Unknown;
}

CameraVendor vendorFromSsid(const char* ssid) {
  char lower[33]{};
  size_t index = 0;
  for (; index < sizeof(lower) - 1 && ssid[index] != '\0'; index++)
    lower[index] = tolower(static_cast<unsigned char>(ssid[index]));

  if (strstr(lower, "ring")) return CameraVendor::Ring;
  if (strstr(lower, "blink")) return CameraVendor::Blink;
  if (strstr(lower, "nest")) return CameraVendor::Nest;
  if (strstr(lower, "arlo")) return CameraVendor::Arlo;
  if (strstr(lower, "wyze")) return CameraVendor::Wyze;
  if (strstr(lower, "reolink")) return CameraVendor::Reolink;
  if (strstr(lower, "hikvision")) return CameraVendor::Hikvision;
  if (strstr(lower, "dahua")) return CameraVendor::Dahua;
  if (strstr(lower, "amcrest")) return CameraVendor::Amcrest;
  if (strstr(lower, "eufy")) return CameraVendor::Eufy;
  if (strstr(lower, "axis")) return CameraVendor::Axis;
  return CameraVendor::Unknown;
}

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

void formatAddress(const uint8_t* address, char* output, size_t outputSize) {
  snprintf(output, outputSize, "%02X:%02X:%02X:%02X:%02X:%02X",
           address[0], address[1], address[2], address[3], address[4],
           address[5]);
}

bool validUnicastAddress(const uint8_t* address);

void updateResult(const uint8_t* address, CameraVendor vendor,
                  const char* ssid, const char* evidence, int8_t rssi,
                  uint8_t channel, uint8_t confidence,
                  const uint8_t* bssid = nullptr) {
  char formattedAddress[18]{};
  formatAddress(address, formattedAddress, sizeof(formattedAddress));

  portENTER_CRITICAL(&resultMux);
  uint8_t index = resultCount;
  for (uint8_t current = 0; current < resultCount; current++) {
    if (memcmp(results[current].address, formattedAddress,
               sizeof(results[current].address)) == 0) {
      index = current;
      break;
    }
  }
  if (index == resultCount && resultCount < MAX_RESULTS)
    resultCount++;
  if (index < MAX_RESULTS) {
    strlcpy(results[index].address, formattedAddress,
            sizeof(results[index].address));
    memcpy(results[index].addressBytes, address,
           sizeof(results[index].addressBytes));
    if (bssid && validUnicastAddress(bssid)) {
      memcpy(results[index].bssid, bssid, sizeof(results[index].bssid));
      results[index].hasBssid = true;
    }
    if (confidence >= results[index].confidence) {
      strlcpy(results[index].vendor, vendorName(vendor),
              sizeof(results[index].vendor));
      strlcpy(results[index].evidence, evidence,
              sizeof(results[index].evidence));
      results[index].confidence = confidence;
    }
    if (ssid && ssid[0] != '\0')
      strlcpy(results[index].ssid, ssid, sizeof(results[index].ssid));
    results[index].rssi = rssi;
    results[index].channel = channel;
    results[index].lastSeen = millis();
    resultRevision++;
  }
  portEXIT_CRITICAL(&resultMux);
}

bool validUnicastAddress(const uint8_t* address) {
  if ((address[0] & 0x01) != 0)
    return false;
  bool allZero = true;
  for (uint8_t index = 0; index < 6; index++) {
    if (address[index] != 0) {
      allZero = false;
      break;
    }
  }
  return !allZero;
}

bool knownEufyNetwork(const uint8_t* bssid) {
  for (uint8_t index = 0; index < eufyNetworkCount; index++) {
    if (memcmp(eufyNetworks[index], bssid, 6) == 0)
      return true;
  }
  return false;
}

void rememberEufyNetwork(const uint8_t* bssid) {
  if (!validUnicastAddress(bssid) || knownEufyNetwork(bssid) ||
      eufyNetworkCount >= MAX_EUFY_NETWORKS)
    return;
  memcpy(eufyNetworks[eufyNetworkCount++], bssid, 6);
}

bool readSsid(const uint8_t* payload, uint16_t length, char* ssid,
              size_t ssidSize, bool* hidden) {
  // Beacon and probe response fixed fields end at byte 36.
  *hidden = false;
  uint16_t offset = 36;
  while (offset + 2 <= length) {
    const uint8_t id = payload[offset];
    const uint8_t fieldLength = payload[offset + 1];
    offset += 2;
    if (offset + fieldLength > length)
      return false;
    if (id == 0) {
      const size_t copied = min(static_cast<size_t>(fieldLength), ssidSize - 1);
      memcpy(ssid, payload + offset, copied);
      ssid[copied] = '\0';
      bool onlyNulls = copied > 0;
      for (size_t index = 0; index < copied; index++) {
        if (ssid[index] != '\0') {
          onlyNulls = false;
          break;
        }
      }
      *hidden = copied == 0 || onlyNulls;
      if (*hidden)
        ssid[0] = '\0';
      return true;
    }
    offset += fieldLength;
  }
  return false;
}

void correlateEufyClient(const uint8_t* payload, uint16_t length,
                         uint8_t frameType, uint16_t frameControl,
                         int8_t rssi, uint8_t channel) {
  if (length < 24)
    return;

  const uint8_t* bssid = nullptr;
  const uint8_t* station = nullptr;
  if (frameType == 0) {
    // Management frames use Addr2 as transmitter and Addr3 as BSSID.
    bssid = payload + 16;
    station = payload + 10;
    // For AP-to-client responses Addr2 is the BSSID and Addr1 is the client.
    if (memcmp(station, bssid, 6) == 0 && validUnicastAddress(payload + 4) &&
        memcmp(payload + 4, bssid, 6) != 0)
      station = payload + 4;
  }
  else if (frameType == 2) {
    const bool toDs = (frameControl & 0x0100) != 0;
    const bool fromDs = (frameControl & 0x0200) != 0;
    if (toDs && !fromDs) {
      bssid = payload + 4;
      station = payload + 10;
    }
    else if (!toDs && fromDs) {
      bssid = payload + 10;
      station = payload + 4;
    }
  }

  if (bssid && station && knownEufyNetwork(bssid) &&
      memcmp(station, bssid, 6) != 0 && validUnicastAddress(station)) {
    updateResult(station, CameraVendor::Eufy, "<hidden>",
                 "HomeBase link", rssi, channel, CONFIDENCE_HIGH, bssid);
  }
}

void promiscuousCallback(void* buffer, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)
    return;

  const auto* packet = static_cast<wifi_promiscuous_pkt_t*>(buffer);
  const uint8_t* payload = packet->payload;
  const uint16_t length = packet->rx_ctrl.sig_len;
  if (length < 24)
    return;

  const uint16_t frameControl = payload[0] |
      (static_cast<uint16_t>(payload[1]) << 8);
  const uint8_t frameType = (frameControl >> 2) & 0x03;
  const uint8_t subtype = (frameControl >> 4) & 0x0F;
  const uint8_t* address1 = payload + 4;
  const uint8_t* transmitter = payload + 10;  // Address 2
  const uint8_t* address3 = payload + 16;
  const uint8_t* bssid = nullptr;
  const uint8_t* station = nullptr;

  if (frameType == 0) {
    bssid = address3;
    if (memcmp(transmitter, bssid, 6) != 0)
      station = transmitter;
    else if (validUnicastAddress(address1) && memcmp(address1, bssid, 6) != 0)
      station = address1;
  }
  else if (frameType == 2) {
    const bool toDs = (frameControl & 0x0100) != 0;
    const bool fromDs = (frameControl & 0x0200) != 0;
    if (toDs && !fromDs) {
      bssid = address1;
      station = transmitter;
    }
    else if (!toDs && fromDs) {
      bssid = transmitter;
      station = address1;
    }
  }

  const CameraVendor ouiVendor = vendorFromMac(transmitter);
  if (ouiVendor != CameraVendor::Unknown) {
    updateResult(transmitter, ouiVendor, nullptr,
                 ouiVendor == CameraVendor::Eufy ? "Eufy OUI" : "IEEE OUI",
                 packet->rx_ctrl.rssi, packet->rx_ctrl.channel,
                 CONFIDENCE_MEDIUM, bssid);
  }

  // Downlink data and management responses place a client in Address 1, so
  // checking only the transmitter would miss a quiet camera receiving data.
  if (station && memcmp(station, transmitter, 6) != 0) {
    const CameraVendor stationVendor = vendorFromMac(station);
    if (stationVendor != CameraVendor::Unknown) {
      updateResult(station, stationVendor, nullptr,
                   stationVendor == CameraVendor::Eufy ? "Eufy OUI" : "IEEE OUI",
                   packet->rx_ctrl.rssi, packet->rx_ctrl.channel,
                   CONFIDENCE_MEDIUM, bssid);
    }
  }

  // SSIDs are only present in beacon (8) and probe-response (5) frames.
  if (frameType == 0 && (subtype == 8 || subtype == 5) && length >= 38) {
    char ssid[33]{};
    bool hidden = false;
    if (readSsid(payload, length, ssid, sizeof(ssid), &hidden)) {
      const uint8_t* bssid = payload + 16;
      const CameraVendor bssidVendor = vendorFromMac(bssid);
      const CameraVendor ssidVendor = hidden ? CameraVendor::Unknown :
          vendorFromSsid(ssid);
      if (hidden && (ouiVendor == CameraVendor::Eufy ||
                     bssidVendor == CameraVendor::Eufy)) {
        rememberEufyNetwork(bssid);
        updateResult(bssid, CameraVendor::Eufy, "<hidden>", "Hidden+OUI",
                     packet->rx_ctrl.rssi, packet->rx_ctrl.channel,
                     CONFIDENCE_HIGH, bssid);
      }
      if (ssidVendor != CameraVendor::Unknown) {
        updateResult(bssid, ssidVendor, ssid, "SSID pattern",
                     packet->rx_ctrl.rssi, packet->rx_ctrl.channel,
                     CONFIDENCE_MEDIUM, bssid);
      }
    }
  }

  correlateEufyClient(payload, length, frameType, frameControl,
                      packet->rx_ctrl.rssi, packet->rx_ctrl.channel);
}

uint8_t copyResults(Detection* output) {
  portENTER_CRITICAL(&resultMux);
  const uint8_t count = resultCount;
  memcpy(output, results, sizeof(Detection) * count);
  portEXIT_CRITICAL(&resultMux);
  return count;
}

void resetResults() {
  portENTER_CRITICAL(&resultMux);
  memset(results, 0, sizeof(results));
  resultCount = 0;
  memset(eufyNetworks, 0, sizeof(eufyNetworks));
  eufyNetworkCount = 0;
  resultRevision++;
  portEXIT_CRITICAL(&resultMux);
}

void drawHeader() {
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_NAVY);
  display_obj.tft.drawString("CAMERA CANDIDATES", TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
}

void drawList(uint8_t selected) {
  const uint8_t count = copyResults(uiSnapshot);
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
    const char confidence = uiSnapshot[index].confidence >= CONFIDENCE_HIGH ?
        'H' : (uiSnapshot[index].confidence >= CONFIDENCE_MEDIUM ? 'M' : 'L');
    display_obj.tft.printf("%d %s [%c]", uiSnapshot[index].rssi,
                           uiSnapshot[index].vendor, confidence);
    display_obj.tft.resetViewport();
  }
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Center: exit"));
}

void drawDetail(uint8_t selected) {
  const uint8_t count = copyResults(uiSnapshot);
  if (count == 0 || selected >= count) {
    drawList(0);
    return;
  }
  const Detection& item = uiSnapshot[selected];
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader();
  display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  display_obj.tft.drawString(item.vendor, 2, 20, 1);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(item.address, 2, 34, 1);
  display_obj.tft.drawString(String(item.rssi) + " dBm  Ch " +
      item.channel, 2, 48, 1);
  display_obj.tft.drawString(String("Seen ") +
      ((millis() - item.lastSeen) / 1000) + "s ago", 2, 62, 1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
  display_obj.tft.setCursor(2, 76);
  display_obj.tft.printf("%s: %s",
      item.confidence >= CONFIDENCE_HIGH ? "High" :
      (item.confidence >= CONFIDENCE_MEDIUM ? "Medium" : "Low"),
      item.evidence);
  display_obj.tft.setViewport(2, 89, TFT_WIDTH - 4, 13);
  display_obj.tft.setCursor(0, 2);
  display_obj.tft.print(item.ssid[0] ? item.ssid : "SSID not observed");
  display_obj.tft.resetViewport();
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Left:list Center:exit"));
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

esp_err_t startPassiveScan() {
  // Arduino's WiFi wrapper owns the IDF initialization lifecycle on this
  // target. No association is attempted; promiscuous receive is the only mode.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);

  wifi_promiscuous_filter_t filter{};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_DATA;
  esp_err_t error = esp_wifi_set_promiscuous_filter(&filter);
  if (error == ESP_OK)
    error = esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
  if (error == ESP_OK)
    error = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  if (error == ESP_OK)
    error = esp_wifi_set_promiscuous(true);
  return error;
}

void stopPassiveScan() {
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
}

void drawDeauthList(uint8_t selected) {
  const uint8_t count = copyResults(uiSnapshot);
  display_obj.tft.fillScreen(TFT_BLACK);
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, HEADER_HEIGHT, TFT_RED);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_RED);
  display_obj.tft.drawString("CAMERA DEAUTH", TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(2, 18);
  display_obj.tft.printf("Found:%u Ch:%u R:scope", count, scanChannel);

  uint8_t start = 0;
  if (selected >= VISIBLE_ROWS)
    start = selected - VISIBLE_ROWS + 1;
  for (uint8_t row = 0; row < VISIBLE_ROWS; row++) {
    const uint8_t index = start + row;
    if (index >= count) break;
    const int16_t y = ROW_START_Y + row * ROW_HEIGHT;
    const bool highlighted = index == selected;
    const uint16_t background = highlighted ? TFT_RED : TFT_BLACK;
    const uint16_t foreground = highlighted ? TFT_WHITE : TFT_RED;
    display_obj.tft.fillRect(0, y, TFT_WIDTH, ROW_HEIGHT - 1, background);
    display_obj.tft.setTextColor(foreground, background);
    display_obj.tft.setViewport(2, y, TFT_WIDTH - 4, ROW_HEIGHT - 1);
    display_obj.tft.setCursor(0, 2);
    const char confidence = uiSnapshot[index].confidence >= CONFIDENCE_HIGH ?
        'H' : (uiSnapshot[index].confidence >= CONFIDENCE_MEDIUM ? 'M' : 'L');
    display_obj.tft.printf("%d %s [%c]%s", uiSnapshot[index].rssi,
                           uiSnapshot[index].vendor, confidence,
                           uiSnapshot[index].hasBssid ? "" : " ?");
    display_obj.tft.resetViewport();
  }
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Center: exit"));
}

bool isWholeAp(const Detection& item) {
  return memcmp(item.addressBytes, item.bssid, 6) == 0;
}

bool sameLink(const DeauthLink& left, const DeauthLink& right) {
  return left.channel == right.channel &&
         memcmp(left.bssid, right.bssid, sizeof(left.bssid)) == 0 &&
         memcmp(left.destination, right.destination,
                sizeof(left.destination)) == 0;
}

bool addDetectionToPlan(const Detection& item, DeauthTarget& plan) {
  if (!item.hasBssid || item.channel < 1 || item.channel > 14 ||
      plan.count >= MAX_DEAUTH_TARGETS)
    return false;

  DeauthLink link{};
  memcpy(link.camera, item.addressBytes, sizeof(link.camera));
  memcpy(link.bssid, item.bssid, sizeof(link.bssid));
  if (isWholeAp(item))
    memset(link.destination, 0xFF, sizeof(link.destination));
  else
    memcpy(link.destination, item.addressBytes, sizeof(link.destination));
  link.channel = item.channel;

  for (uint8_t index = 0; index < plan.count; index++) {
    if (sameLink(plan.links[index], link))
      return false;
  }

  plan.links[plan.count++] = link;
  if (isWholeAp(item))
    plan.includesApScope = true;
  return true;
}

bool hasClientForBssid(const Detection* snapshot, uint8_t count,
                       const char* vendor, const uint8_t* bssid) {
  for (uint8_t index = 0; index < count; index++) {
    const Detection& item = snapshot[index];
    if (strcmp(item.vendor, vendor) == 0 && item.hasBssid &&
        item.channel >= 1 && item.channel <= 14 && !isWholeAp(item) &&
        memcmp(item.bssid, bssid, sizeof(item.bssid)) == 0)
      return true;
  }
  return false;
}

void buildDeauthPlan(const Detection* snapshot, uint8_t count,
                     const Detection& selected, bool sameBrand,
                     DeauthTarget& plan) {
  memset(&plan, 0, sizeof(plan));
  snprintf(plan.vendor, sizeof(plan.vendor), "%s", selected.vendor);
  plan.sameBrand = sameBrand;

  if (!sameBrand) {
    addDetectionToPlan(selected, plan);
    return;
  }

  for (uint8_t index = 0; index < count; index++) {
    const Detection& item = snapshot[index];
    if (strcmp(item.vendor, selected.vendor) != 0)
      continue;

    // If individual camera links were observed behind this AP, do not add a
    // redundant AP-wide broadcast target as well. Keep the AP target as a
    // fallback for brands/topologies where only the AP itself was observed.
    if (isWholeAp(item) &&
        hasClientForBssid(snapshot, count, selected.vendor, item.bssid))
      continue;
    addDetectionToPlan(item, plan);
  }
}

void buildApScopePlan(const Detection& selected, DeauthTarget& plan) {
  memset(&plan, 0, sizeof(plan));
  snprintf(plan.vendor, sizeof(plan.vendor), "%s", selected.vendor);
  memcpy(plan.links[0].camera, selected.addressBytes,
         sizeof(plan.links[0].camera));
  memcpy(plan.links[0].bssid, selected.bssid,
         sizeof(plan.links[0].bssid));
  memset(plan.links[0].destination, 0xFF,
         sizeof(plan.links[0].destination));
  plan.links[0].channel = selected.channel;
  plan.count = 1;
  plan.includesApScope = true;
  plan.apOnly = true;
}

void drawScopeOption(int16_t y, const String& label, bool selected,
                     bool enabled = true) {
  const uint16_t background = selected ? TFT_RED : TFT_BLACK;
  const uint16_t foreground = selected ? TFT_WHITE :
      (enabled ? TFT_RED : TFT_DARKGREY);
  display_obj.tft.fillRect(0, y, TFT_WIDTH, 13, background);
  display_obj.tft.setTextColor(foreground, background);
  display_obj.tft.drawString(label, 2, y + 2, 1);
}

void drawDeauthScope(const Detection& item, const DeauthTarget& brandPlan,
                     uint8_t scopeChoice, bool brandAvailable) {
  display_obj.tft.fillScreen(TFT_BLACK);
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, HEADER_HEIGHT, TFT_RED);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_RED);
  display_obj.tft.drawString("TARGET SCOPE", TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);

  String brandLine = String("All ") + item.vendor;
  if (brandAvailable)
    brandLine += String(" (") + brandPlan.count + ")";
  else
    brandLine += " unavailable";

  drawScopeOption(20, "This camera (1)", scopeChoice == 0);
  drawScopeOption(35, brandLine, scopeChoice == 1, brandAvailable);
  drawScopeOption(50, "This AP (all clients)", scopeChoice == 2);

  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(item.address, 2, 67, 1);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.drawString("Up/Down: scope", 2, 84, 1);
  display_obj.tft.drawString("Center: continue", 2, 98, 1);
  display_obj.tft.drawString("Left: cancel", 2, 112, 1);
}

void drawDeauthConfirm(const DeauthTarget& plan) {
  display_obj.tft.fillScreen(TFT_BLACK);
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, HEADER_HEIGHT, TFT_RED);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_RED);
  display_obj.tft.drawString("AUTHORIZED TEST?", TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  display_obj.tft.drawString(plan.vendor, 2, 22, 1);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(String("Targets: ") + plan.count, 2, 36, 1);
  display_obj.tft.drawString(plan.apOnly ? "Scope: this AP" :
                             (plan.sameBrand ? "Scope: same brand" :
                              "Scope: one camera"), 2, 50, 1);
  display_obj.tft.drawString(plan.apOnly ? "Affects ALL AP clients" :
                             (plan.includesApScope ? "Includes AP scope" :
                              "Client links only"), 2, 64, 1);
  display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
  display_obj.tft.drawString("Runs until manual exit", 2, 81, 1);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.drawString("Center:start", 2, 99, 1);
  display_obj.tft.drawString("Left:cancel", 2, 113, 1);
}

void drawDeauthPreflight(const DeauthTarget& plan, uint8_t selected) {
  if (plan.count == 0)
    return;
  if (selected >= plan.count)
    selected = plan.count - 1;

  char camera[18]{};
  char bssid[18]{};
  formatAddress(plan.links[selected].camera, camera, sizeof(camera));
  formatAddress(plan.links[selected].bssid, bssid, sizeof(bssid));

  display_obj.tft.fillScreen(TFT_BLACK);
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, HEADER_HEIGHT, TFT_RED);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_RED);
  display_obj.tft.drawString(
      String("PREFLIGHT ") + (selected + 1) + "/" + plan.count,
      TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(String("CAM ") + camera, 2, 20, 1);
  display_obj.tft.drawString(String("AP  ") + bssid, 2, 34, 1);
  display_obj.tft.drawString(
      String("CH ") + plan.links[selected].channel +
      (plan.apOnly ? "  AP broadcast" : "  client link"), 2, 48, 1);
  display_obj.tft.drawString("TX OK:0  FAIL:0", 2, 62, 1);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.drawString("OK = driver accepted", 2, 78, 1);
  display_obj.tft.drawString("Up/Down:browse C:auth", 2, 98, 1);
  display_obj.tft.drawString("Left:scope", 2, 112, 1);
}

}  // namespace

void run() {
  releaseControls();
  resetResults();
  const esp_err_t error = startPassiveScan();

  uint8_t selected = 0;
  bool detailView = false;
  uint32_t drawnRevision = UINT32_MAX;
  uint32_t nextRefresh = 0;
  uint32_t nextChannelHop = millis() + CHANNEL_DWELL_MS;
  scanChannel = 1;

  if (error != ESP_OK)
    showError("Passive Wi-Fi scan could not start.");

  while (true) {
    if (c_btn.justPressed())
      break;

    if (error == ESP_OK &&
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
    if (u_btn.justPressed() && selected > 0) {
      selected--;
      redraw = true;
    }
    if (d_btn.justPressed() && selected + 1 < count) {
      selected++;
      redraw = true;
    }
    if (r_btn.justPressed() && count > 0) {
      detailView = true;
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

    if (error == ESP_OK && redraw) {
      if (detailView)
        drawDetail(selected);
      else
        drawList(selected);
      drawnRevision = revision;
    }
    delay(5);
  }

  stopPassiveScan();
  releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

bool selectDeauthTarget(DeauthTarget& target) {
  memset(&target, 0, sizeof(target));
  releaseControls();
  resetResults();
  const esp_err_t error = startPassiveScan();

  uint8_t selected = 0;
  bool choosingScope = false;
  bool preflighting = false;
  bool confirming = false;
  uint8_t scopeChoice = 0;
  uint8_t preflightIndex = 0;
  bool brandAvailable = false;
  static Detection pending{};
  static DeauthTarget individualPlan{};
  static DeauthTarget brandPlan{};
  memset(&pending, 0, sizeof(pending));
  memset(&individualPlan, 0, sizeof(individualPlan));
  memset(&brandPlan, 0, sizeof(brandPlan));
  uint32_t drawnRevision = UINT32_MAX;
  uint32_t nextRefresh = 0;
  uint32_t nextChannelHop = millis() + CHANNEL_DWELL_MS;
  scanChannel = 1;
  bool accepted = false;

  if (error != ESP_OK)
    showError("Passive camera scan could not start.");

  while (true) {
    if (confirming) {
      if (l_btn.justPressed()) {
        confirming = false;
        preflighting = true;
        drawDeauthPreflight(target, preflightIndex);
        releaseControls();
      }
      else if (c_btn.justPressed()) {
        accepted = true;
        break;
      }
      delay(5);
      continue;
    }

    if (preflighting) {
      const bool upPressed = u_btn.justPressed();
      const bool downPressed = d_btn.justPressed();
      if (l_btn.justPressed()) {
        preflighting = false;
        choosingScope = true;
        drawDeauthScope(pending, brandPlan, scopeChoice, brandAvailable);
        releaseControls();
      }
      else if ((upPressed || downPressed) && target.count > 1) {
        if (upPressed)
          preflightIndex = preflightIndex == 0 ? target.count - 1 :
              preflightIndex - 1;
        else
          preflightIndex = (preflightIndex + 1) % target.count;
        drawDeauthPreflight(target, preflightIndex);
        releaseControls();
      }
      else if (c_btn.justPressed()) {
        preflighting = false;
        confirming = true;
        drawDeauthConfirm(target);
        releaseControls();
      }
      delay(5);
      continue;
    }

    if (choosingScope) {
      const bool upPressed = u_btn.justPressed();
      const bool downPressed = d_btn.justPressed();
      if (l_btn.justPressed()) {
        choosingScope = false;
        drawnRevision = UINT32_MAX;
        releaseControls();
      }
      else if (upPressed || downPressed) {
        const int8_t direction = upPressed ? -1 : 1;
        do {
          scopeChoice = (scopeChoice + direction + 3) % 3;
        } while (scopeChoice == 1 && !brandAvailable);
        drawDeauthScope(pending, brandPlan, scopeChoice, brandAvailable);
        releaseControls();
      }
      else if (c_btn.justPressed()) {
        if (scopeChoice == 2)
          buildApScopePlan(pending, target);
        else
          target = scopeChoice == 1 ? brandPlan : individualPlan;
        choosingScope = false;
        preflighting = true;
        preflightIndex = 0;
        drawDeauthPreflight(target, preflightIndex);
        releaseControls();
      }
      delay(5);
      continue;
    }

    if (c_btn.justPressed())
      break;

    if (error == ESP_OK &&
        static_cast<int32_t>(millis() - nextChannelHop) >= 0) {
      scanChannel = (scanChannel % 11) + 1;
      esp_wifi_set_channel(scanChannel, WIFI_SECOND_CHAN_NONE);
      nextChannelHop = millis() + CHANNEL_DWELL_MS;
    }

    const uint8_t count = copyResults(uiSnapshot);
    uint32_t revision = 0;
    portENTER_CRITICAL(&resultMux);
    revision = resultRevision;
    portEXIT_CRITICAL(&resultMux);

    if (count == 0)
      selected = 0;
    else if (selected >= count)
      selected = count - 1;

    bool redraw = revision != drawnRevision;
    if (u_btn.justPressed() && selected > 0) {
      selected--;
      redraw = true;
    }
    if (d_btn.justPressed() && selected + 1 < count) {
      selected++;
      redraw = true;
    }
    if (r_btn.justPressed() && count > 0) {
      if (uiSnapshot[selected].hasBssid && uiSnapshot[selected].channel >= 1 &&
          uiSnapshot[selected].channel <= 14) {
        pending = uiSnapshot[selected];
        buildDeauthPlan(uiSnapshot, count, pending, false, individualPlan);
        brandAvailable = strcmp(pending.vendor, "Camera") != 0;
        if (brandAvailable)
          buildDeauthPlan(uiSnapshot, count, pending, true, brandPlan);
        else
          memset(&brandPlan, 0, sizeof(brandPlan));
        brandAvailable = brandAvailable && brandPlan.count > 0;
        scopeChoice = 0;
        choosingScope = true;
        drawDeauthScope(pending, brandPlan, scopeChoice, brandAvailable);
        releaseControls();
      }
      else {
        showError("No AP link observed yet. Generate camera traffic and retry.");
        delay(1200);
        redraw = true;
      }
    }
    if (static_cast<int32_t>(millis() - nextRefresh) >= 0) {
      nextRefresh = millis() + CHANNEL_DWELL_MS;
      redraw = true;
    }
    if (error == ESP_OK && redraw && !confirming && !choosingScope) {
      drawDeauthList(selected);
      drawnRevision = revision;
    }
    delay(5);
  }

  stopPassiveScan();
  releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
  return accepted;
}

}  // namespace WiFiCameraDetector

#else

namespace WiFiCameraDetector {
void run() {}
bool selectDeauthTarget(DeauthTarget&) { return false; }
}

#endif
