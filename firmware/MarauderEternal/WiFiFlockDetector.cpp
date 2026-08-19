/*
 * Passive Flock/Raven detector research reviewed from:
 * https://github.com/colonelpanichacks/flock-you
 * https://github.com/NSM-Barii/flock-back
 * https://github.com/zmattmanz/flock-detection
 * https://github.com/ReconGrunt/FlipDeFlock
 * https://github.com/garrettwise814/FlipDeFlock
 * https://github.com/rbarriaultjr/flock-detection
 * https://github.com/yetisoldier/CYD-Flock-You
 *
 * Only target-specific indicators are retained here. Unverified bulk OUI
 * seeds and prefixes retracted upstream for false positives are excluded.
 * The complete flock-you MIT license is retained in
 * wireless-tools/FLOCK_YOU_LICENSE.txt.
 */

#include "WiFiFlockDetector.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "configs.h"

#if defined(MARAUDER_MINI_V3) && defined(HAS_SCREEN) && \
    defined(HAS_BUTTONS) && (U_BTN >= 0) && (D_BTN >= 0) && \
    (L_BTN >= 0) && (R_BTN >= 0) && (C_BTN >= 0)
  #include "Display.h"
  #include "Switches.h"

  extern Display display_obj;
  extern Switches u_btn;
  extern Switches d_btn;
  extern Switches l_btn;
  extern Switches r_btn;
  extern Switches c_btn;
#endif

namespace WiFiFlockDetector {
namespace {

// Vetted Flock-associated prefixes. A shared silicon prefix is never enough on
// its own; it must be corroborated by probe behavior, a strict Flock SSID, or
// the full IE signature. F8:A2:D6 is intentionally absent because current
// FlipDeFlock research retracts it after a false hit on a Sony media player.
constexpr uint8_t FLOCK_OUIS[][3] = {
  {0x70, 0xC9, 0x4E}, {0x3C, 0x91, 0x80}, {0xD8, 0xF3, 0xBC},
  {0x80, 0x30, 0x49}, {0xB8, 0x35, 0x32}, {0x14, 0x5A, 0xFC},
  {0x74, 0x4C, 0xA1}, {0x08, 0x3A, 0x88}, {0x9C, 0x2F, 0x9D},
  {0xC0, 0x35, 0x32}, {0x94, 0x08, 0x53}, {0xE4, 0xAA, 0xEA},
  {0xF4, 0x6A, 0xDD}, {0x24, 0xB2, 0xB9}, {0x00, 0xF4, 0x8D},
  {0xD0, 0x39, 0x57}, {0xE8, 0xD0, 0xFC},
  {0xE0, 0x4F, 0x43}, {0xB8, 0x1E, 0xA4}, {0x70, 0x08, 0x94},
  {0x58, 0x8E, 0x81}, {0xEC, 0x1B, 0xBD}, {0x3C, 0x71, 0xBF},
  {0x58, 0x00, 0xE3}, {0x90, 0x35, 0xEA}, {0x5C, 0x93, 0xA2},
  {0x64, 0x6E, 0x69}, {0x48, 0x27, 0xEA}, {0xA4, 0xCF, 0x12},
  {0x82, 0x6B, 0xF2}, {0xB4, 0x1E, 0x52}
};

// SoundThinking (formerly ShotSpotter) is a separate acoustic-sensor class.
constexpr uint8_t SOUNDTHINKING_OUIS[][3] = {{0xD4, 0x11, 0xD6}};
constexpr uint8_t FLOCK_REGISTERED_OUI[3] = {0xB4, 0x1E, 0x52};

#ifdef HAS_DUAL_BAND
constexpr uint8_t FLOCK_SCAN_CHANNELS[] = {
  161, 157, 153, 149, 48, 44, 40, 36, 11, 6, 1,
  161, 157, 153, 149, 48, 44, 40, 36, 11, 6, 1,
  177, 173, 169, 165, 161, 157, 153, 149,
  144, 140, 136, 132, 128, 124, 120, 116, 112, 100,
  64, 60, 56, 52, 48, 44, 40, 36,
  14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1
};
#else
constexpr uint8_t FLOCK_SCAN_CHANNELS[] = {
  11, 6, 1, 11, 6, 1,
  14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1
};
#endif

constexpr char FLOCK_PROBE_IE_SIG[] =
    "2,12,127,221:506f9a16030103,45,191,221:0050f208000000";
constexpr char FLOCK_LITEON_IE_PREFIX[] = "221:506f9a16030103";
constexpr uint8_t IE_SSID = 0;
constexpr uint8_t IE_VENDOR = 221;
constexpr uint8_t PHANTOM_SKIP_CAP = 16;
constexpr uint8_t TLV_RESYNC_MAX = 64;

template <size_t Count>
bool IRAM_ATTR matchesOui(const uint8_t* mac,
                          const uint8_t (&ouis)[Count][3]) {
  if (!mac || (mac[0] & 0x01) != 0)
    return false;
  for (const auto& oui : ouis) {
    if (mac[0] == oui[0] && mac[1] == oui[1] && mac[2] == oui[2])
      return true;
  }
  return false;
}

bool IRAM_ATTR matchesFlockOui(const uint8_t* mac) {
  return matchesOui(mac, FLOCK_OUIS);
}

bool IRAM_ATTR matchesSoundThinkingOui(const uint8_t* mac) {
  return matchesOui(mac, SOUNDTHINKING_OUIS);
}

bool IRAM_ATTR matchesRegisteredFlockOui(const uint8_t* mac) {
  return mac && memcmp(mac, FLOCK_REGISTERED_OUI, sizeof(FLOCK_REGISTERED_OUI)) == 0;
}

// Returns 1 for wildcard SSID, 0 for a directed probe, and -1 if no valid
// SSID information element was found.
int IRAM_ATTR wildcardProbeSsid(const uint8_t* body, int length) {
  if (!body || length < 2)
    return -1;
  while (length >= 2) {
    const uint8_t id = body[0];
    const uint8_t fieldLength = body[1];
    if (static_cast<int>(fieldLength) + 2 > length)
      break;
    if (id == IE_SSID)
      return fieldLength == 0 ? 1 : 0;
    body += fieldLength + 2;
    length -= fieldLength + 2;
  }
  return -1;
}

int IRAM_ATTR readSsid(const uint8_t* body, int length, char* output,
                       size_t capacity, bool* allNull = nullptr) {
  if (allNull)
    *allNull = false;
  if (!body || length < 2 || !output || capacity < 2)
    return -1;

  while (length >= 2) {
    const uint8_t id = body[0];
    const int fieldLength = body[1];
    if (fieldLength + 2 > length)
      return -1;
    if (id == IE_SSID) {
      if (fieldLength > 32)
        return -1;
      const size_t copyLength = min(static_cast<size_t>(fieldLength), capacity - 1);
      bool onlyNulls = fieldLength > 0;
      for (int index = 0; index < fieldLength; index++) {
        if (body[index + 2] != 0)
          onlyNulls = false;
        if (static_cast<size_t>(index) < copyLength)
          output[index] = static_cast<char>(body[index + 2]);
      }
      output[copyLength] = '\0';
      if (allNull)
        *allNull = fieldLength == 0 || onlyNulls;
      return fieldLength;
    }
    body += fieldLength + 2;
    length -= fieldLength + 2;
  }
  return -1;
}

char IRAM_ATTR lowerAscii(char value) {
  return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

bool IRAM_ATTR equalsIgnoreCase(const char* value, int length,
                                const char* expected) {
  if (!value || !expected || length < 0 ||
      static_cast<size_t>(length) != strlen(expected)) {
    return false;
  }
  for (int index = 0; index < length; index++) {
    if (lowerAscii(value[index]) != lowerAscii(expected[index]))
      return false;
  }
  return true;
}

bool IRAM_ATTR containsIgnoreCase(const char* value, int length,
                                  const char* needle) {
  if (!value || !needle || length <= 0)
    return false;
  const int needleLength = strlen(needle);
  if (needleLength == 0 || needleLength > length)
    return false;
  for (int start = 0; start <= length - needleLength; start++) {
    bool equal = true;
    for (int index = 0; index < needleLength; index++) {
      if (lowerAscii(value[start + index]) != lowerAscii(needle[index])) {
        equal = false;
        break;
      }
    }
    if (equal)
      return true;
  }
  return false;
}

bool IRAM_ATTR strictProvisioningSsid(const char* ssid, int length) {
  if (!ssid || length != 12)
    return false;
  const char prefix[] = "flock-";
  for (int index = 0; index < 6; index++) {
    if (lowerAscii(ssid[index]) != prefix[index])
      return false;
  }
  for (int index = 6; index < 12; index++) {
    const char value = lowerAscii(ssid[index]);
    if (!((value >= '0' && value <= '9') ||
          (value >= 'a' && value <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool IRAM_ATTR flockSsidIndicator(const char* ssid, int length) {
  return strictProvisioningSsid(ssid, length) ||
      equalsIgnoreCase(ssid, length, "test_flck") ||
      containsIgnoreCase(ssid, length, "flock") ||
      containsIgnoreCase(ssid, length, "flck") ||
      containsIgnoreCase(ssid, length, "penguin") ||
      containsIgnoreCase(ssid, length, "pigvision") ||
      containsIgnoreCase(ssid, length, "fs ext battery");
}

bool IRAM_ATTR unicastAddress(const uint8_t* mac) {
  return mac && (mac[0] & 0x01) == 0;
}

void IRAM_ATTR setMatch(Match& match, const uint8_t* mac,
                        const wifi_promiscuous_pkt_t* packet,
                        DeviceClass deviceClass, Confidence confidence,
                        const char* method) {
  memcpy(match.mac, mac, sizeof(match.mac));
  match.rssi = packet->rx_ctrl.rssi;
  match.channel = packet->rx_ctrl.channel;
  match.deviceClass = deviceClass;
  match.confidence = confidence;
  match.method = method;
}

void IRAM_ATTR hexBytes(char* destination, const uint8_t* bytes, int count) {
  static const char HEX_DIGITS[] = "0123456789abcdef";
  for (int index = 0; index < count; index++) {
    destination[index * 2] = HEX_DIGITS[bytes[index] >> 4];
    destination[index * 2 + 1] = HEX_DIGITS[bytes[index] & 0x0F];
  }
}

bool IRAM_ATTR liteOnVendorAt(const uint8_t* ies, int length, int position) {
  return position + 9 <= length && ies[position] == IE_VENDOR &&
      ies[position + 1] == 7 && ies[position + 2] == 0x50 &&
      ies[position + 3] == 0x6F && ies[position + 4] == 0x9A;
}

bool IRAM_ATTR phantomLiteOnAhead(const uint8_t* ies, int length,
                                  int position) {
  int end = position + 2 + 32;
  if (end > length - 1)
    end = length - 1;
  for (int current = position + 2; current < end; current++) {
    if (liteOnVendorAt(ies, length, current))
      return true;
  }
  return false;
}

bool IRAM_ATTR isPhantomOverflow(const uint8_t* ies, int length, uint8_t id,
                                 int fieldLength, int position) {
  if (position + 2 + fieldLength <= length)
    return false;
  if (fieldLength > 200)
    return true;
  return id == 64 && fieldLength == 128 &&
      phantomLiteOnAhead(ies, length, position);
}

int IRAM_ATTR resyncTlv(const uint8_t* ies, int length, int start) {
  int end = start + TLV_RESYNC_MAX;
  if (end > length - 1)
    end = length - 1;
  for (int current = start; current < end; current++) {
    const int fieldLength = static_cast<int>(ies[current + 1]);
    if (fieldLength <= 200 && current + 2 + fieldLength <= length)
      return current;
  }
  return -1;
}

bool IRAM_ATTR appendSignaturePart(char* output, size_t capacity,
                                   size_t* position, const char* part) {
  const size_t partLength = strlen(part);
  if (*position != 0) {
    if (*position + 1 >= capacity)
      return false;
    output[(*position)++] = ',';
  }
  if (*position + partLength >= capacity)
    return false;
  memcpy(output + *position, part, partLength);
  *position += partLength;
  output[*position] = '\0';
  return true;
}

bool IRAM_ATTR appendTag(char* output, size_t capacity, size_t* position,
                         uint8_t id) {
  char tag[8];
  snprintf(tag, sizeof(tag), "%u", static_cast<unsigned>(id));
  return appendSignaturePart(output, capacity, position, tag);
}

bool IRAM_ATTR appendVendor(char* output, size_t capacity, size_t* position,
                            const uint8_t* body, int fieldLength) {
  char vendor[24];
  const int take = fieldLength < 8 ? fieldLength : 8;
  vendor[0] = '2';
  vendor[1] = '2';
  vendor[2] = '1';
  vendor[3] = ':';
  hexBytes(vendor + 4, body, take);
  vendor[4 + take * 2] = '\0';
  return appendSignaturePart(output, capacity, position, vendor);
}

bool IRAM_ATTR buildSignatureFromIes(const uint8_t* ies, int length,
                                     char* output, size_t capacity,
                                     bool* complete) {
  if (!ies || length < 2 || !output || capacity < 2)
    return false;

  size_t outputPosition = 0;
  output[0] = '\0';
  int position = 0;
  uint8_t phantomSkips = 0;
  while (position + 2 <= length) {
    const uint8_t id = ies[position];
    const int fieldLength = static_cast<int>(ies[position + 1]);
    if (position + 2 + fieldLength > length) {
      if (phantomSkips < PHANTOM_SKIP_CAP &&
          isPhantomOverflow(ies, length, id, fieldLength, position)) {
        phantomSkips++;
        position += 2;
        continue;
      }
      const int next = resyncTlv(ies, length, position);
      if (next > position) {
        position = next;
        continue;
      }
      return false;
    }

    position += 2;
    if (id == IE_SSID) {
      if (fieldLength == 0) {
        while (position + 2 <= length && ies[position] == 0 &&
               ies[position + 1] == 0) {
          position += 2;
        }
      }
      else {
        position += fieldLength;
      }
      continue;
    }

    const bool appended = id == IE_VENDOR && fieldLength >= 4 ?
        appendVendor(output, capacity, &outputPosition, ies + position,
                     fieldLength) :
        appendTag(output, capacity, &outputPosition, id);
    if (!appended)
      return false;
    position += fieldLength;
  }

  if (complete)
    *complete = position == length;
  return outputPosition > 0;
}

void IRAM_ATTR canonicalizeSignature(char* signature, size_t capacity) {
  if (!signature || capacity < 8)
    return;
  if (strncmp(signature, "2,12,127,", 9) == 0 &&
      strstr(signature, FLOCK_LITEON_IE_PREFIX) != nullptr) {
    return;
  }
  const char* anchor = strstr(signature, FLOCK_LITEON_IE_PREFIX);
  if (!anchor)
    return;

  char normalized[128];
  const int written = snprintf(normalized, sizeof(normalized),
                               "2,12,127,%s", anchor);
  if (written > 0 && static_cast<size_t>(written) < capacity)
    memcpy(signature, normalized, static_cast<size_t>(written) + 1);
}

bool IRAM_ATTR chooseSignature(const char* first, bool firstComplete,
                               const char* second, bool secondComplete,
                               char* output, size_t capacity) {
  if (!first[0] && !second[0])
    return false;
  const char* selected = first;
  if (!first[0])
    selected = second;
  else if (second[0]) {
    if (!firstComplete && secondComplete)
      selected = second;
    else if (firstComplete == secondComplete && strlen(second) > strlen(first))
      selected = second;
  }
  strncpy(output, selected, capacity - 1);
  output[capacity - 1] = '\0';
  return true;
}

bool IRAM_ATTR buildProbeSignature(const uint8_t* body, int length,
                                   char* output, size_t capacity) {
  if (!body || length < 2 || !output || capacity < 16)
    return false;

  char first[128]{};
  char second[128]{};
  bool firstComplete = false;
  bool secondComplete = false;
  const bool firstOk = buildSignatureFromIes(
      body, length, first, sizeof(first), &firstComplete);
  bool secondOk = false;
  if (body[0] == 0 && body[1] == 0) {
    secondOk = buildSignatureFromIes(body + 2, length - 2, second,
                                     sizeof(second), &secondComplete);
  }

  char selected[128]{};
  if (!chooseSignature(firstOk ? first : "", firstComplete,
                       secondOk ? second : "", secondComplete, selected,
                       sizeof(selected))) {
    return false;
  }
  canonicalizeSignature(selected, sizeof(selected));
  strncpy(output, selected, capacity - 1);
  output[capacity - 1] = '\0';
  return output[0] != '\0';
}

bool IRAM_ATTR hasPrimaryProbeSignature(const uint8_t* body, int length) {
  char signature[128];
  if (buildProbeSignature(body, length, signature, sizeof(signature)) &&
      strcmp(signature, FLOCK_PROBE_IE_SIG) == 0) {
    return true;
  }
  return length > 4 &&
      buildProbeSignature(body, length - 4, signature, sizeof(signature)) &&
      strcmp(signature, FLOCK_PROBE_IE_SIG) == 0;
}

#if defined(MARAUDER_MINI_V3) && defined(HAS_SCREEN) && \
    defined(HAS_BUTTONS) && (U_BTN >= 0) && (D_BTN >= 0) && \
    (L_BTN >= 0) && (R_BTN >= 0) && (C_BTN >= 0)

constexpr uint8_t MAX_RESULTS = 32;
constexpr uint8_t VISIBLE_ROWS = 6;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t ROW_START_Y = 29;
constexpr int16_t ROW_HEIGHT = 14;

struct Detection {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t channel;
  uint16_t count;
  uint32_t lastSeen;
  DeviceClass deviceClass;
  Confidence confidence;
  const char* method;
};

Detection results[MAX_RESULTS]{};
Detection snapshot[MAX_RESULTS]{};
volatile uint8_t resultCount = 0;
volatile uint32_t resultRevision = 0;
volatile uint8_t scanChannel = 11;
portMUX_TYPE resultMux = portMUX_INITIALIZER_UNLOCKED;

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

void formatMac(const uint8_t* mac, char* output, size_t size) {
  snprintf(output, size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

void updateResult(const Match& match) {
  portENTER_CRITICAL(&resultMux);
  uint8_t index = resultCount;
  for (uint8_t current = 0; current < resultCount; current++) {
    if (memcmp(results[current].mac, match.mac, sizeof(match.mac)) == 0) {
      index = current;
      break;
    }
  }
  if (index == resultCount && resultCount < MAX_RESULTS)
    resultCount++;
  if (index < MAX_RESULTS) {
    Detection& result = results[index];
    memcpy(result.mac, match.mac, sizeof(result.mac));
    result.rssi = match.rssi;
    result.channel = match.channel;
    result.deviceClass = match.deviceClass;
    result.confidence = match.confidence;
    result.method = match.method;
    result.lastSeen = millis();
    if (result.count < UINT16_MAX)
      result.count++;
    resultRevision++;
  }
  portEXIT_CRITICAL(&resultMux);
}

void promiscuousCallback(void* buffer, wifi_promiscuous_pkt_type_t type) {
  Match match{};
  if (matchPacket(static_cast<wifi_promiscuous_pkt_t*>(buffer), type, match))
    updateResult(match);
}

uint8_t copyResults() {
  portENTER_CRITICAL(&resultMux);
  const uint8_t count = resultCount;
  memcpy(snapshot, results, count * sizeof(Detection));
  portEXIT_CRITICAL(&resultMux);
  return count;
}

void resetResults() {
  portENTER_CRITICAL(&resultMux);
  memset(results, 0, sizeof(results));
  resultCount = 0;
  resultRevision++;
  portEXIT_CRITICAL(&resultMux);
}

void drawHeader() {
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, HEADER_HEIGHT, TFT_ORANGE);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_BLACK, TFT_ORANGE);
  display_obj.tft.drawString("DETECT FLOCK", TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
}

void drawList(uint8_t selected) {
  const uint8_t count = copyResults();
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
    if (index >= count)
      break;
    const int16_t y = ROW_START_Y + row * ROW_HEIGHT;
    const bool highlighted = index == selected;
    const uint16_t background = highlighted ? TFT_ORANGE : TFT_BLACK;
    const uint16_t foreground = highlighted ? TFT_BLACK : TFT_ORANGE;
    char mac[18];
    formatMac(snapshot[index].mac, mac, sizeof(mac));
    display_obj.tft.fillRect(0, y, TFT_WIDTH, ROW_HEIGHT - 1, background);
    display_obj.tft.setTextColor(foreground, background);
    display_obj.tft.setViewport(2, y, TFT_WIDTH - 4, ROW_HEIGHT - 1);
    display_obj.tft.setCursor(0, 2);
    const char* marker = snapshot[index].deviceClass == DeviceClass::SoundThinking
                             ? "ST"
                             : (snapshot[index].deviceClass == DeviceClass::Raven
                                    ? "RV"
                                    : "FL");
    display_obj.tft.printf("%s %d %s", marker, snapshot[index].rssi, mac);
    display_obj.tft.resetViewport();
  }
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Center: exit"));
}

void drawDetail(uint8_t selected) {
  const uint8_t count = copyResults();
  if (count == 0 || selected >= count) {
    drawList(0);
    return;
  }

  char mac[18];
  formatMac(snapshot[selected].mac, mac, sizeof(mac));
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader();
  display_obj.tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  display_obj.tft.drawString(
      String(classLabel(snapshot[selected].deviceClass)) + " / " +
          confidenceLabel(snapshot[selected].confidence),
      2, 20, 1);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(mac, 2, 35, 1);
  display_obj.tft.drawString(String(snapshot[selected].rssi) + " dBm  Ch " +
      snapshot[selected].channel, 2, 49, 1);
  display_obj.tft.drawString(String("Hits: ") + snapshot[selected].count, 2,
                             63, 1);
  display_obj.tft.drawString(String("Seen ") +
      ((millis() - snapshot[selected].lastSeen) / 1000) + "s ago", 2, 77, 1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
  display_obj.tft.drawString(snapshot[selected].method, 2, 92, 1);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Left:list Center:exit"));
}

void showError() {
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader();
  display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
  display_obj.tft.setTextWrap(true);
  display_obj.tft.setCursor(4, 30);
  display_obj.tft.print(F("Passive Wi-Fi scan could not start."));
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(4, TFT_HEIGHT - 12);
  display_obj.tft.print(F("Center: exit"));
}

esp_err_t startPassiveScan() {
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
  size_t channelCount = 0;
  const uint8_t* channels = scanChannelPlan(channelCount);
  if (error == ESP_OK && channelCount > 0)
    error = esp_wifi_set_channel(channels[0], WIFI_SECOND_CHAN_NONE);
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

#endif

}  // namespace

bool IRAM_ATTR matchPacket(const wifi_promiscuous_pkt_t* packet,
                           wifi_promiscuous_pkt_type_t type, Match& match) {
  if (!packet || (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) ||
      packet->rx_ctrl.sig_len < 24) {
    return false;
  }

  const uint8_t* payload = packet->payload;
  const uint8_t frameType = (payload[0] >> 2) & 0x03;
  const uint8_t subtype = (payload[0] >> 4) & 0x0F;
  const uint8_t* receiver = payload + 4;
  const uint8_t* transmitter = payload + 10;
  const uint8_t* bssid = payload + 16;

  // SoundThinking/ShotSpotter uses a separate, dedicated vendor prefix and is
  // never folded into the ALPR class.
  if (matchesSoundThinkingOui(transmitter)) {
    setMatch(match, transmitter, packet, DeviceClass::SoundThinking,
             Confidence::High, "soundthinking_oui_tx");
    return true;
  }
  if (unicastAddress(receiver) && matchesSoundThinkingOui(receiver)) {
    setMatch(match, receiver, packet, DeviceClass::SoundThinking,
             Confidence::Medium, "soundthinking_oui_rx");
    return true;
  }

  const bool transmitterFlock = matchesFlockOui(transmitter);
  const bool receiverFlock = unicastAddress(receiver) && matchesFlockOui(receiver);
  const bool bssidFlock = type == WIFI_PKT_MGMT && matchesFlockOui(bssid);

  // Flock's registered OUI is useful on non-management traffic too. Shared
  // silicon-vendor prefixes still require management/probe corroboration.
  if (type == WIFI_PKT_DATA || frameType != 0) {
    if (matchesRegisteredFlockOui(transmitter)) {
      setMatch(match, transmitter, packet, DeviceClass::FlockAlpr,
               Confidence::Medium, "flock_registered_oui_tx");
      return true;
    }
    if (receiverFlock) {
      setMatch(match, receiver, packet, DeviceClass::FlockAlpr,
               Confidence::Low, "flock_receiver_oui");
      return true;
    }
    return false;
  }

  const bool probeRequest = subtype == 4;
  const bool beaconOrResponse = subtype == 8 || subtype == 5;
  const uint8_t* informationElements = nullptr;
  int informationLength = 0;
  if (probeRequest) {
    informationElements = payload + 24;
    informationLength = static_cast<int>(packet->rx_ctrl.sig_len) - 24;
  }
  else if (beaconOrResponse && packet->rx_ctrl.sig_len >= 38) {
    informationElements = payload + 36;
    informationLength = static_cast<int>(packet->rx_ctrl.sig_len) - 36;
  }

  char ssid[33]{};
  bool hiddenSsid = false;
  int ssidLength = -1;
  if (informationElements && informationLength >= 2) {
    ssidLength = readSsid(informationElements, informationLength, ssid,
                          sizeof(ssid), &hiddenSsid);
    if (ssidLength < 0 && informationLength > 4) {
      ssidLength = readSsid(informationElements, informationLength - 4, ssid,
                            sizeof(ssid), &hiddenSsid);
    }
  }

  const bool confirmedSsid = ssidLength > 0 &&
      (strictProvisioningSsid(ssid, ssidLength) ||
       equalsIgnoreCase(ssid, ssidLength, "test_flck"));
  const bool ssidIndicator = ssidLength > 0 &&
      flockSsidIndicator(ssid, ssidLength);

  if (probeRequest) {
    int wildcard = wildcardProbeSsid(informationElements, informationLength);
    if (wildcard < 0 && informationLength > 4) {
      wildcard = wildcardProbeSsid(informationElements,
                                   informationLength - 4);
    }

    // The complete ordered IE fingerprint is MAC-independent, so it can catch
    // field units that rotate locally administered transmitter addresses.
    if (wildcard == 1 &&
        hasPrimaryProbeSignature(informationElements, informationLength)) {
      setMatch(match, transmitter, packet, DeviceClass::FlockAlpr,
               Confidence::High, "wifi_probe_ie_signature");
      return true;
    }
    if (confirmedSsid) {
      setMatch(match, transmitter, packet, DeviceClass::FlockAlpr,
               Confidence::High, "wifi_provisioning_ssid");
      return true;
    }
    if (transmitterFlock && wildcard == 1) {
      setMatch(match, transmitter, packet, DeviceClass::FlockAlpr,
               Confidence::Medium, "wifi_oui_wildcard_probe");
      return true;
    }
    if (transmitterFlock && ssidIndicator) {
      setMatch(match, transmitter, packet, DeviceClass::FlockAlpr,
               Confidence::Medium, "wifi_oui_ssid");
      return true;
    }
    if (matchesRegisteredFlockOui(transmitter)) {
      setMatch(match, transmitter, packet, DeviceClass::FlockAlpr,
               Confidence::Medium, "flock_registered_oui_probe");
      return true;
    }
  }

  if (beaconOrResponse) {
    if (confirmedSsid) {
      setMatch(match, transmitter, packet, DeviceClass::FlockAlpr,
               Confidence::High, "wifi_provisioning_ssid");
      return true;
    }
    if (transmitterFlock && ssidIndicator) {
      setMatch(match, transmitter, packet, DeviceClass::FlockAlpr,
               Confidence::Medium, "wifi_oui_ssid");
      return true;
    }
    if (transmitterFlock && hiddenSsid) {
      setMatch(match, transmitter, packet, DeviceClass::FlockAlpr,
               Confidence::Low, "wifi_hidden_ssid_oui");
      return true;
    }
  }

  if (receiverFlock) {
    setMatch(match, receiver, packet, DeviceClass::FlockAlpr,
             Confidence::Low, "wifi_receiver_oui");
    return true;
  }
  if (bssidFlock) {
    setMatch(match, bssid, packet, DeviceClass::FlockAlpr,
             Confidence::Low, "wifi_bssid_oui");
    return true;
  }

  return false;
}

const char* classLabel(DeviceClass deviceClass) {
  switch (deviceClass) {
    case DeviceClass::Raven: return "Raven";
    case DeviceClass::SoundThinking: return "ShotSpotter";
    default: return "Flock ALPR";
  }
}

const char* confidenceLabel(Confidence confidence) {
  switch (confidence) {
    case Confidence::High: return "high";
    case Confidence::Medium: return "medium";
    default: return "low";
  }
}

const uint8_t* scanChannelPlan(size_t& count) {
  count = sizeof(FLOCK_SCAN_CHANNELS) / sizeof(FLOCK_SCAN_CHANNELS[0]);
  return FLOCK_SCAN_CHANNELS;
}

uint16_t channelDwellMs(uint8_t channel) {
  return channel == 1 || channel == 6 || channel == 11 ? 500 : 200;
}

void run() {
#if defined(MARAUDER_MINI_V3) && defined(HAS_SCREEN) && \
    defined(HAS_BUTTONS) && (U_BTN >= 0) && (D_BTN >= 0) && \
    (L_BTN >= 0) && (R_BTN >= 0) && (C_BTN >= 0)
  releaseControls();
  resetResults();
  size_t channelCount = 0;
  const uint8_t* channels = scanChannelPlan(channelCount);
  scanChannel = channelCount > 0 ? channels[0] : 1;
  const esp_err_t error = startPassiveScan();

  uint8_t selected = 0;
  uint8_t channelIndex = 0;
  bool detailView = false;
  uint32_t drawnRevision = UINT32_MAX;
  uint32_t nextRefresh = 0;
  uint32_t nextChannelHop = millis() + channelDwellMs(scanChannel);

  if (error != ESP_OK)
    showError();

  while (true) {
    if (c_btn.justPressed())
      break;

    if (error == ESP_OK &&
        static_cast<int32_t>(millis() - nextChannelHop) >= 0) {
      channelIndex = channelCount > 0 ? (channelIndex + 1) % channelCount : 0;
      scanChannel = channelCount > 0 ? channels[channelIndex] : 1;
      esp_wifi_set_channel(scanChannel, WIFI_SECOND_CHAN_NONE);
      nextChannelHop = millis() + channelDwellMs(scanChannel);
    }

    uint8_t count = 0;
    uint32_t revision = 0;
    portENTER_CRITICAL(&resultMux);
    count = resultCount;
    revision = resultRevision;
    portEXIT_CRITICAL(&resultMux);
    if (count == 0)
      selected = 0;
    else if (selected >= count)
      selected = count - 1;

    bool redraw = revision != drawnRevision;
    if (u_btn.justPressed() && count > 0) {
      selected = (selected + count - 1) % count;
      redraw = true;
    }
    if (d_btn.justPressed() && count > 0) {
      selected = (selected + 1) % count;
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
      nextRefresh = millis() + (detailView ? 1000 : 200);
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
#endif
}

}  // namespace WiFiFlockDetector
