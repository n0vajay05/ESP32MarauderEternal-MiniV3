#include "BLEDeviceDetectors.h"

#include "configs.h"

#if defined(HAS_BT) && defined(HAS_NIMBLE_2) && defined(HAS_SCREEN) && \
    defined(HAS_BUTTONS) && (U_BTN >= 0) && (D_BTN >= 0) && \
    (L_BTN >= 0) && (R_BTN >= 0) && (C_BTN >= 0)

#include <NimBLEDevice.h>

#include "Display.h"
#include "Switches.h"

extern Display display_obj;
extern Switches u_btn;
extern Switches d_btn;
extern Switches l_btn;
extern Switches r_btn;
extern Switches c_btn;

namespace BLEDeviceDetectors {
namespace {

// Detection identifiers and behavior were adapted from the MIT-licensed
// nyanBOX project. The scanner/UI are original Mini V3 implementations.
// https://github.com/jbohack/nyanBOX
constexpr uint8_t MAX_RESULTS = 32;
constexpr uint8_t VISIBLE_ROWS = 6;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t ROW_START_Y = 29;
constexpr int16_t ROW_HEIGHT = 14;

struct Detection {
  char address[18];
  char name[25];
  char detail[37];
  int8_t rssi;
  uint32_t lastSeen;
};

Detection results[MAX_RESULTS]{};
uint8_t resultCount = 0;
volatile uint32_t resultRevision = 0;
portMUX_TYPE resultMux = portMUX_INITIALIZER_UNLOCKED;
DetectorType activeDetector = DetectorType::Meshtastic;

const char* detectorTitle(DetectorType detector) {
  switch (detector) {
    case DetectorType::Meshtastic: return "MESHTASTIC";
    case DetectorType::MeshCore: return "MESHCORE";
    case DetectorType::SmartTag: return "SMARTTAG";
    case DetectorType::Tile: return "TILE";
    case DetectorType::Axon: return "AXON";
    case DetectorType::IBeacon: return "IBEACON";
    case DetectorType::NyanBox: return "NYANBOX";
  }
  return "BLE DETECTOR";
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

bool startsWithIgnoreCase(const char* value, const char* prefix) {
  while (*prefix) {
    if (tolower(static_cast<unsigned char>(*value++)) !=
        tolower(static_cast<unsigned char>(*prefix++)))
      return false;
  }
  return true;
}

bool copyManufacturerData(const NimBLEAdvertisedDevice* device,
                          uint8_t* output, size_t outputSize,
                          size_t& copied) {
  copied = 0;
  if (!device->haveManufacturerData())
    return false;
  const std::string data = device->getManufacturerData();
  copied = min(data.size(), outputSize);
  if (copied > 0)
    memcpy(output, data.data(), copied);
  return copied > 0;
}

bool matchesDetector(const NimBLEAdvertisedDevice* device,
                     char* detail, size_t detailSize) {
  static const NimBLEUUID meshtasticUuid(
      "6ba1b218-15a8-461f-9fa8-5dcae273eafd");
  static const NimBLEUUID meshCoreUuid(
      "6e400001-b5a3-f393-e0a9-e50e24dcca9e");
  static const NimBLEUUID smartTagUuid(static_cast<uint16_t>(0xFD5A));
  static const NimBLEUUID tileUuid1(static_cast<uint16_t>(0xFEED));
  static const NimBLEUUID tileUuid2(static_cast<uint16_t>(0xFEEC));
  static const NimBLEUUID nyanBoxUuid(
      "6e79616e-424f-582d-7365-727669636521");

  detail[0] = '\0';
  switch (activeDetector) {
    case DetectorType::Meshtastic:
      if (!device->isAdvertisingService(meshtasticUuid)) return false;
      strlcpy(detail, "Service: Meshtastic", detailSize);
      return true;

    case DetectorType::MeshCore:
      if (!device->isAdvertisingService(meshCoreUuid) &&
          !startsWithIgnoreCase(device->getName().c_str(), "MeshCore-"))
        return false;
      strlcpy(detail, "MeshCore UUID/name", detailSize);
      return true;

    case DetectorType::SmartTag:
      if (!device->isAdvertisingService(smartTagUuid)) return false;
      strlcpy(detail, "Service UUID: FD5A", detailSize);
      return true;

    case DetectorType::Tile:
      if (!device->isAdvertisingService(tileUuid1) &&
          !device->isAdvertisingService(tileUuid2))
        return false;
      strlcpy(detail, "Service: FEED/FEEC", detailSize);
      return true;

    case DetectorType::Axon: {
      const String address = device->getAddress().toString().c_str();
      if (!startsWithIgnoreCase(address.c_str(), "00:25:df"))
        return false;
      strlcpy(detail, "Axon IEEE OUI match", detailSize);
      return true;
    }

    case DetectorType::IBeacon: {
      uint8_t data[31]{};
      size_t length = 0;
      if (!copyManufacturerData(device, data, sizeof(data), length) ||
          length < 25 || data[0] != 0x4C || data[1] != 0x00 ||
          data[2] != 0x02 || data[3] != 0x15)
        return false;
      const uint16_t major = (static_cast<uint16_t>(data[20]) << 8) | data[21];
      const uint16_t minor = (static_cast<uint16_t>(data[22]) << 8) | data[23];
      snprintf(detail, detailSize, "Major:%u Minor:%u", major, minor);
      return true;
    }

    case DetectorType::NyanBox: {
      if (!device->isAdvertisingService(nyanBoxUuid)) return false;
      uint8_t data[31]{};
      size_t length = 0;
      if (copyManufacturerData(device, data, sizeof(data), length) &&
          length >= 8 && data[0] == 0xFF && data[1] == 0xFF) {
        const uint16_t level = (static_cast<uint16_t>(data[2]) << 8) | data[3];
        const uint32_t version = (static_cast<uint32_t>(data[4]) << 24) |
                                 (static_cast<uint32_t>(data[5]) << 16) |
                                 (static_cast<uint32_t>(data[6]) << 8) | data[7];
        snprintf(detail, detailSize, "Level:%u v%lu.%02lu.%02lu", level,
                 static_cast<unsigned long>(version / 10000),
                 static_cast<unsigned long>((version / 100) % 100),
                 static_cast<unsigned long>(version % 100));
      }
      else {
        strlcpy(detail, "nyanBOX service", detailSize);
      }
      return true;
    }
  }
  return false;
}

void updateResult(const NimBLEAdvertisedDevice* device,
                  const char* detail) {
  const String address = device->getAddress().toString().c_str();
  const String advertisedName = device->getName().c_str();
  char name[25]{};
  if (advertisedName.length() > 0)
    strlcpy(name, advertisedName.c_str(), sizeof(name));
  else
    strlcpy(name, detectorTitle(activeDetector), sizeof(name));

  portENTER_CRITICAL(&resultMux);
  uint8_t index = resultCount;
  for (uint8_t current = 0; current < resultCount; current++) {
    if (strncmp(results[current].address, address.c_str(),
                sizeof(results[current].address)) == 0) {
      index = current;
      break;
    }
  }
  if (index == resultCount && resultCount < MAX_RESULTS)
    resultCount++;
  if (index < MAX_RESULTS) {
    strlcpy(results[index].address, address.c_str(),
            sizeof(results[index].address));
    strlcpy(results[index].name, name, sizeof(results[index].name));
    strlcpy(results[index].detail, detail, sizeof(results[index].detail));
    results[index].rssi = device->getRSSI();
    results[index].lastSeen = millis();
    resultRevision++;
  }
  portEXIT_CRITICAL(&resultMux);

  if (index < MAX_RESULTS) {
    Serial.printf("[BLE %s] %d dBm %s %s %s\n",
                  detectorTitle(activeDetector), device->getRSSI(),
                  address.c_str(), name, detail);
  }
}

class DetectorCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    char detail[37]{};
    if (matchesDetector(device, detail, sizeof(detail)))
      updateResult(device, detail);
  }
};

DetectorCallbacks detectorCallbacks;

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
  resultRevision++;
  portEXIT_CRITICAL(&resultMux);
}

void drawHeader() {
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_NAVY);
  display_obj.tft.drawString(detectorTitle(activeDetector), TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
}

void drawList(uint8_t selected) {
  Detection snapshot[MAX_RESULTS]{};
  const uint8_t count = copyResults(snapshot);
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader();
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(2, 18);
  display_obj.tft.printf("Found:%u  R:detail", count);

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
                           snapshot[index].name);
    display_obj.tft.resetViewport();
  }
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Center: exit"));
}

void drawDetail(uint8_t selected) {
  Detection snapshot[MAX_RESULTS]{};
  const uint8_t count = copyResults(snapshot);
  if (count == 0 || selected >= count) {
    drawList(0);
    return;
  }
  const Detection& item = snapshot[selected];
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  display_obj.tft.drawString(item.name, 2, 20, 1);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(item.address, 2, 34, 1);
  display_obj.tft.drawString(String(item.rssi) + " dBm", 2, 48, 1);
  display_obj.tft.drawString(String("Seen ") +
      ((millis() - item.lastSeen) / 1000) + "s ago", 2, 62, 1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
  display_obj.tft.setTextWrap(true);
  display_obj.tft.setCursor(2, 77);
  display_obj.tft.print(item.detail);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Left: list  Center: exit"));
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

void run(DetectorType detector) {
  releaseControls();
  activeDetector = detector;
  resetResults();

  if (NimBLEDevice::isInitialized())
    NimBLEDevice::deinit(true);
  NimBLEDevice::init("Marauder-Detector");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&detectorCallbacks, true);
  scan->setActiveScan(true);
  scan->setInterval(50);
  scan->setWindow(30);
  scan->setMaxResults(0);

  const bool scanning = scan->start(0, false, true);
  uint8_t selected = 0;
  bool detailView = false;
  uint32_t drawnRevision = UINT32_MAX;
  uint32_t nextAgeRefresh = 0;
  if (!scanning)
    showError("BLE scan could not start.");

  while (true) {
    if (c_btn.justPressed())
      break;

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
    if (detailView && static_cast<int32_t>(millis() - nextAgeRefresh) >= 0) {
      nextAgeRefresh = millis() + 1000;
      redraw = true;
    }

    if (scanning && redraw) {
      if (detailView)
        drawDetail(selected);
      else
        drawList(selected);
      drawnRevision = revision;
    }
    delay(5);
  }

  if (scan->isScanning())
    scan->stop();
  delay(20);
  scan->setScanCallbacks(nullptr);
  scan->clearResults();
  NimBLEDevice::deinit(true);
  releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

}  // namespace BLEDeviceDetectors

#else

namespace BLEDeviceDetectors {
void run(DetectorType) {}
}

#endif
