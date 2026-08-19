#include "BLESecurityTools.h"

#if defined(HAS_BT) && defined(HAS_NIMBLE_2)

#include <NimBLEDevice.h>
#include <esp_mac.h>

#ifdef HAS_SCREEN
  #include "Display.h"
#endif
#ifdef HAS_BUTTONS
  #include "Switches.h"
#endif

#include "BLECompanyIdentifiers.h"

#ifdef HAS_SCREEN
extern Display display_obj;
#endif
#if defined(HAS_BUTTONS) && (C_BTN >= 0)
extern Switches c_btn;
#endif
#if defined(HAS_BUTTONS) && (U_BTN >= 0)
extern Switches u_btn;
#endif
#if defined(HAS_BUTTONS) && (D_BTN >= 0)
extern Switches d_btn;
#endif

namespace BLESecurityTools {
namespace {

struct Target {
  bool valid;
  uint8_t mac[6];
  uint8_t addressType;
  uint16_t companyId;
  uint16_t appearance;
  bool hasCompanyId;
  bool hasAppearance;
  bool connectable;
  bool scannable;
  bool advertisementTruncated;
  char name[48];
  char advertisedServices[192];
  uint8_t advertisementData[31];
  uint8_t advertisementLength;
  uint8_t scanResponseData[31];
  uint8_t scanResponseLength;
};

struct AdvertisementVariant {
  uint8_t advertisementData[31];
  uint8_t advertisementLength;
  uint8_t scanResponseData[31];
  uint8_t scanResponseLength;
  bool scannable;
};

struct GattDisplayItem {
  bool service;
  uint8_t properties;
  char uuid[37];
  char parentServiceUuid[37];
};

constexpr uint32_t DEVICE_SPOOF_VARIANT_DWELL_MS = 1000;
constexpr size_t DEVICE_SPOOF_MINIMUM_FREE_HEAP = 64 * 1024;
constexpr size_t GATT_RESULTS_MINIMUM_FREE_HEAP = 48 * 1024;
constexpr size_t GATT_DISPLAY_BLOCK_ITEMS = 32;
constexpr uint16_t GATT_PAGE_REPEAT_DELAY_MS = 450;
constexpr uint16_t GATT_PAGE_REPEAT_INTERVAL_MS = 120;

struct GattDisplayBlock {
  GattDisplayBlock* next;
  size_t used;
  GattDisplayItem items[GATT_DISPLAY_BLOCK_ITEMS];
};

class GattDisplayResults {
 public:
  GattDisplayResults() = default;
  GattDisplayResults(const GattDisplayResults&) = delete;
  GattDisplayResults& operator=(const GattDisplayResults&) = delete;

  ~GattDisplayResults() {
    GattDisplayBlock* block = first_;
    while (block) {
      GattDisplayBlock* next = block->next;
      free(block);
      block = next;
    }
  }

  bool append(const GattDisplayItem& item) {
    if (!last_ || last_->used == GATT_DISPLAY_BLOCK_ITEMS) {
      GattDisplayBlock* block = nullptr;
#ifdef HAS_PSRAM
      if (ESP.getFreePsram() > sizeof(GattDisplayBlock) + 4096)
        block = static_cast<GattDisplayBlock*>(ps_malloc(sizeof(GattDisplayBlock)));
#endif
      if (!block && ESP.getFreeHeap() > GATT_RESULTS_MINIMUM_FREE_HEAP + sizeof(GattDisplayBlock))
        block = static_cast<GattDisplayBlock*>(malloc(sizeof(GattDisplayBlock)));
      if (!block)
        return false;

      memset(block, 0, sizeof(GattDisplayBlock));
      if (last_)
        last_->next = block;
      else
        first_ = block;
      last_ = block;
    }

    last_->items[last_->used++] = item;
    itemCount_++;
    return true;
  }

  bool empty() const {
    return itemCount_ == 0;
  }

  size_t size() const {
    return itemCount_;
  }

  const GattDisplayItem& operator[](size_t index) const {
    const GattDisplayBlock* block = first_;
    while (block && index >= block->used) {
      index -= block->used;
      block = block->next;
    }
    if (block)
      return block->items[index];
    static const GattDisplayItem emptyItem{};
    return emptyItem;
  }

 private:
  GattDisplayBlock* first_ = nullptr;
  GattDisplayBlock* last_ = nullptr;
  size_t itemCount_ = 0;
};

Target target{};

enum GattProperty : uint8_t {
  GATT_PROPERTY_READ = 1 << 0,
  GATT_PROPERTY_WRITE = 1 << 1,
  GATT_PROPERTY_NOTIFY = 1 << 2,
  GATT_PROPERTY_INDICATE = 1 << 3,
};

void copyText(char* destination, size_t capacity, const String& source) {
  if (capacity == 0)
    return;
  source.toCharArray(destination, capacity);
  destination[capacity - 1] = '\0';
}

String targetAddress() {
  if (!target.valid)
    return "none";
  return String(NimBLEAddress(target.mac, target.addressType).toString().c_str());
}

const char* addressTypeName(uint8_t type) {
  switch (type) {
    case BLE_ADDR_PUBLIC: return "public";
    case BLE_ADDR_RANDOM: return "random";
    case BLE_ADDR_PUBLIC_ID: return "public identity";
    case BLE_ADDR_RANDOM_ID: return "random identity";
    default: return "unknown";
  }
}

String bytesAsHex(const uint8_t* bytes, size_t length) {
  String output;
  output.reserve(length * 2);
  for (size_t index = 0; index < length; index++) {
    char byteText[3];
    snprintf(byteText, sizeof(byteText), "%02X", bytes[index]);
    output += byteText;
  }
  return output;
}

String targetVendorName() {
  const char* company = target.hasCompanyId ? BLECompanyIdentifiers::lookup(target.companyId) : nullptr;
  return company && company[0] ? String(company) : String(F("UnknownVendor"));
}

#ifdef HAS_SD
String singleLineText(const char* value) {
  String text(value ? value : "");
  text.replace('\r', ' ');
  text.replace('\n', ' ');
  return text;
}

String filenameToken(const String& value, size_t maximumLength) {
  String token;
  token.reserve(maximumLength);
  for (size_t index = 0; index < value.length() && token.length() < maximumLength; index++) {
    const char character = value[index];
    const bool alphaNumeric = (character >= 'a' && character <= 'z') ||
                              (character >= 'A' && character <= 'Z') ||
                              (character >= '0' && character <= '9');
    if (alphaNumeric) {
      token += character;
    }
    else if (token.length() && token[token.length() - 1] != '_') {
      token += '_';
    }
  }
  while (token.endsWith("_"))
    token.remove(token.length() - 1);
  return token.length() ? token : String(F("UnknownVendor"));
}

bool openGattServiceLog(File& logFile, String& logPath) {
  if (!sd_obj.supported)
    return false;

  String addressToken = targetAddress();
  addressToken.toUpperCase();
  addressToken.replace(':', '-');
  const String basePath = String(F("/BLE_GATT_ADV_")) +
                          filenameToken(targetVendorName(), 28) + "_" + addressToken;
  for (uint16_t index = 0; index < 10000; index++) {
    logPath = basePath;
    if (index)
      logPath += String("_") + index;
    logPath += F(".log");
    if (SD.exists(logPath))
      continue;
    logFile = SD.open(logPath, FILE_WRITE);
    return static_cast<bool>(logFile);
  }
  logPath = "";
  return false;
}

void writeGattLogHeader(File& logFile) {
  const char* company = target.hasCompanyId ? BLECompanyIdentifiers::lookup(target.companyId) : nullptr;
  logFile.println(F("# ESP32 Marauder BLE advertisement and GATT enumeration"));
  logFile.print(F("firmware="));
  logFile.println(MARAUDER_VERSION);
  logFile.print(F("uptime_ms="));
  logFile.println(millis());
  logFile.print(F("target_name="));
  logFile.println(singleLineText(target.name));
  logFile.print(F("target_vendor="));
  logFile.println(targetVendorName());
  logFile.print(F("target_address="));
  logFile.println(targetAddress());
  logFile.print(F("address_type="));
  logFile.println(addressTypeName(target.addressType));
  logFile.print(F("connectable="));
  logFile.println(target.connectable ? F("yes") : F("no"));
  logFile.print(F("scannable="));
  logFile.println(target.scannable ? F("yes") : F("no"));
  if (target.hasCompanyId) {
    char identifier[7];
    snprintf(identifier, sizeof(identifier), "0x%04X", target.companyId);
    logFile.print(F("company_id="));
    logFile.println(identifier);
    logFile.print(F("manufacturer="));
    logFile.println(company ? company : "unassigned");
  }
  if (target.hasAppearance) {
    char appearance[7];
    snprintf(appearance, sizeof(appearance), "0x%04X", target.appearance);
    logFile.print(F("appearance="));
    logFile.println(appearance);
  }
  logFile.print(F("advertised_service_uuids="));
  logFile.println(target.advertisedServices);
  logFile.print(F("advertisement_data="));
  logFile.println(bytesAsHex(target.advertisementData, target.advertisementLength));
  logFile.print(F("scan_response_data="));
  logFile.println(bytesAsHex(target.scanResponseData, target.scanResponseLength));
  logFile.print(F("advertisement_truncated="));
  logFile.println(target.advertisementTruncated ? F("yes") : F("no"));
  logFile.println();
  logFile.println(F("[Connected GATT Enumeration]"));
}
#endif

int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool decodeHex(const String& text, uint8_t* output, size_t capacity, uint8_t& length) {
  if ((text.length() & 1) != 0 || text.length() / 2 > capacity)
    return false;
  length = text.length() / 2;
  for (size_t index = 0; index < length; index++) {
    const int high = hexNibble(text[index * 2]);
    const int low = hexNibble(text[index * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    output[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool variantsEqual(const AdvertisementVariant& first, const AdvertisementVariant& second) {
  return first.advertisementLength == second.advertisementLength &&
         first.scanResponseLength == second.scanResponseLength &&
         first.scannable == second.scannable &&
         memcmp(first.advertisementData, second.advertisementData, first.advertisementLength) == 0 &&
         memcmp(first.scanResponseData, second.scanResponseData, first.scanResponseLength) == 0;
}

bool addUniqueVariant(std::vector<AdvertisementVariant>& variants,
                      const AdvertisementVariant& candidate,
                      bool& memoryLimited) {
  for (const AdvertisementVariant& existing : variants) {
    if (variantsEqual(existing, candidate))
      return true;
  }
  size_t allocationHeadroom = sizeof(AdvertisementVariant) * 4;
  if (variants.size() == variants.capacity()) {
    const size_t nextCapacity = variants.capacity() ? variants.capacity() * 2 : 1;
    allocationHeadroom += nextCapacity * sizeof(AdvertisementVariant);
  }
  if (ESP.getFreeHeap() < DEVICE_SPOOF_MINIMUM_FREE_HEAP + allocationHeadroom) {
    memoryLimited = true;
    return false;
  }
  variants.push_back(candidate);
  return true;
}

bool parseCaptureVariant(const String& line,
                         const String& selectedAddress,
                         AdvertisementVariant& variant,
                         bool& unsupportedLength) {
  String fields[9];
  int cursor = 0;
  for (uint8_t field = 0; field < 9; field++) {
    int comma = line.indexOf(',', cursor);
    if (comma < 0) {
      if (field != 8)
        return false;
      comma = line.length();
    }
    fields[field] = line.substring(cursor, comma);
    cursor = comma + 1;
  }
  fields[2].trim();
  if (!fields[2].equalsIgnoreCase(selectedAddress))
    return false;

  const unsigned long statedAdvertisementLength = strtoul(fields[6].c_str(), nullptr, 10);
  if (statedAdvertisementLength > sizeof(variant.advertisementData) ||
      fields[7].length() != statedAdvertisementLength * 2 ||
      fields[8].length() / 2 > sizeof(variant.scanResponseData)) {
    unsupportedLength = true;
    return false;
  }

  memset(&variant, 0, sizeof(variant));
  if (!decodeHex(fields[7], variant.advertisementData, sizeof(variant.advertisementData),
                 variant.advertisementLength) ||
      !decodeHex(fields[8], variant.scanResponseData, sizeof(variant.scanResponseData),
                 variant.scanResponseLength))
    return false;
  variant.scannable = fields[5] == "1" || variant.scanResponseLength > 0;
  return variant.advertisementLength > 0;
}

void loadAdvertisementVariants(std::vector<AdvertisementVariant>& variants,
                               bool& memoryLimited,
                               bool& unsupportedLength,
                               uint16_t& captureFilesRead) {
  AdvertisementVariant selected{};
  selected.advertisementLength = target.advertisementLength;
  selected.scanResponseLength = target.scanResponseLength;
  selected.scannable = target.scannable || target.scanResponseLength > 0;
  memcpy(selected.advertisementData, target.advertisementData, selected.advertisementLength);
  memcpy(selected.scanResponseData, target.scanResponseData, selected.scanResponseLength);
  addUniqueVariant(variants, selected, memoryLimited);

#ifdef HAS_SD
  if (!sd_obj.supported)
    return;

  File root = SD.open("/");
  if (!root)
    return;
  while (true) {
    File entry = root.openNextFile();
    if (!entry)
      break;
    String fileName = entry.name();
    const int slash = fileName.lastIndexOf('/');
    if (slash >= 0)
      fileName = fileName.substring(slash + 1);
    if (entry.isDirectory() || !fileName.startsWith("ble_advertisements_") || !fileName.endsWith(".log")) {
      entry.close();
      continue;
    }

    captureFilesRead++;
    while (entry.available() && !memoryLimited) {
      const String line = entry.readStringUntil('\n');
      AdvertisementVariant captured{};
      if (parseCaptureVariant(line, targetAddress(), captured, unsupportedLength))
        addUniqueVariant(variants, captured, memoryLimited);
    }
    entry.close();
    if (memoryLimited)
      break;
  }
  root.close();
#endif
}

bool configureExactAddress(uint8_t originalBtAddress[6], bool& publicAddressOverridden, String& error) {
  if (esp_read_mac(originalBtAddress, ESP_MAC_BT) != ESP_OK) {
    error = "Could not read local BT address";
    return false;
  }

  if (NimBLEDevice::isInitialized())
    NimBLEDevice::deinit(true);

  const bool usePublic = target.addressType == BLE_ADDR_PUBLIC || target.addressType == BLE_ADDR_PUBLIC_ID;
  const bool useRandom = target.addressType == BLE_ADDR_RANDOM || target.addressType == BLE_ADDR_RANDOM_ID;
  if (!usePublic && !useRandom) {
    error = "Unsupported target address type";
    return false;
  }

  if (usePublic) {
    if (esp_iface_mac_addr_set(target.mac, ESP_MAC_BT) != ESP_OK) {
      error = "Could not set exact public address";
      return false;
    }
    publicAddressOverridden = true;
    NimBLEDevice::init(target.name);
    if (!NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC)) {
      error = "Could not select public address";
      return false;
    }
  } else {
    const NimBLEAddress randomAddress(target.mac, BLE_ADDR_RANDOM);
    if (!randomAddress.isStatic()) {
      error = "Private address is not a static identity";
      return false;
    }
    NimBLEDevice::init(target.name);
    if (!NimBLEDevice::setOwnAddr(randomAddress) || !NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM)) {
      error = "Could not set exact random-static address";
      return false;
    }
  }

  const String activeAddress = NimBLEDevice::getAddress().toString().c_str();
  if (!activeAddress.equalsIgnoreCase(targetAddress())) {
    error = String("Address verify failed: ") + activeAddress;
    return false;
  }
  return true;
}

void restoreLocalAddress(const uint8_t originalBtAddress[6], bool publicAddressOverridden) {
  if (NimBLEDevice::isInitialized())
    NimBLEDevice::deinit(true);
  if (publicAddressOverridden)
    esp_iface_mac_addr_set(originalBtAddress, ESP_MAC_BT);
}

bool startVariant(NimBLEAdvertising* advertising, const AdvertisementVariant& variant) {
  if (advertising->isAdvertising())
    advertising->stop();
  advertising->reset();
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  advertising->setDiscoverableMode(variant.scannable ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON);
  advertising->enableScanResponse(variant.scannable && variant.scanResponseLength > 0);
  advertising->setAdvertisingInterval(160);  // 100 ms; each variant is broadcast repeatedly.

  NimBLEAdvertisementData advertisement;
  NimBLEAdvertisementData scanResponse;
  if (!advertisement.addData(variant.advertisementData, variant.advertisementLength) ||
      !advertising->setAdvertisementData(advertisement))
    return false;
  if (variant.scanResponseLength &&
      (!scanResponse.addData(variant.scanResponseData, variant.scanResponseLength) ||
       !advertising->setScanResponseData(scanResponse)))
    return false;
  return advertising->start();
}

void showStatus(const char* title, const String& first = "", const String& second = "", const String& third = "") {
  Serial.println();
  Serial.println(String("[BLE Discovery] ") + title);
  if (first.length()) Serial.println(first);
  if (second.length()) Serial.println(second);
  if (third.length()) Serial.println(third);
#ifdef HAS_SCREEN
  display_obj.clearScreen();
  display_obj.tft.setTextWrap(true, false);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
  display_obj.tft.setCursor(3, 3);
  display_obj.tft.println(title);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (first.length()) display_obj.tft.println(first);
  if (second.length()) display_obj.tft.println(second);
  if (third.length()) display_obj.tft.println(third);
#endif
}

void releaseCenterButton() {
#if defined(HAS_BUTTONS) && (C_BTN >= 0)
  while (c_btn.getPullup() ? digitalRead(c_btn.getPin()) == LOW : digitalRead(c_btn.getPin()) == HIGH) {
    c_btn.justPressed();
    delay(10);
  }
  c_btn.justPressed();
#endif
}

void releaseNavigationButtons() {
  releaseCenterButton();
#if defined(HAS_BUTTONS) && (U_BTN >= 0)
  while (u_btn.getPullup() ? digitalRead(u_btn.getPin()) == LOW : digitalRead(u_btn.getPin()) == HIGH) {
    u_btn.justPressed();
    delay(10);
  }
  u_btn.justPressed();
#endif
#if defined(HAS_BUTTONS) && (D_BTN >= 0)
  while (d_btn.getPullup() ? digitalRead(d_btn.getPin()) == LOW : digitalRead(d_btn.getPin()) == HIGH) {
    d_btn.justPressed();
    delay(10);
  }
  d_btn.justPressed();
#endif
}

bool centerPressed() {
#if defined(HAS_BUTTONS) && (C_BTN >= 0)
  return c_btn.justPressed();
#else
  return false;
#endif
}

bool upPressed() {
#if defined(HAS_BUTTONS) && (U_BTN >= 0)
  return u_btn.justPressed();
#else
  return false;
#endif
}

bool downPressed() {
#if defined(HAS_BUTTONS) && (D_BTN >= 0)
  return d_btn.justPressed();
#else
  return false;
#endif
}

void waitForReturn();

bool addGattDisplayItem(GattDisplayResults& results,
                        bool service,
                        const std::string& uuid,
                        const std::string& parentServiceUuid,
                        uint8_t properties,
                        bool& limited) {
  if (limited)
    return false;

  GattDisplayItem item{};
  item.service = service;
  item.properties = properties;
  snprintf(item.uuid, sizeof(item.uuid), "%s", uuid.c_str());
  snprintf(item.parentServiceUuid, sizeof(item.parentServiceUuid), "%s", parentServiceUuid.c_str());
  if (results.append(item))
    return true;
  limited = true;
  return false;
}

#ifdef HAS_SCREEN
void drawUuidLines(const char* uuid, int16_t y) {
  String text(uuid);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(3, y);
  display_obj.tft.println(text.substring(0, min(static_cast<int>(text.length()), 20)));
  if (text.length() > 20) {
    display_obj.tft.setCursor(3, y + 10);
    display_obj.tft.println(text.substring(20));
  }
}

void drawGattResult(const GattDisplayResults& results,
                    size_t index,
                    uint16_t serviceCount,
                    uint16_t characteristicCount,
                    bool limited,
                    size_t overallPage = 0,
                    size_t overallPageCount = 0) {
  const GattDisplayItem& item = results[index];
  display_obj.clearScreen();
  display_obj.tft.setTextWrap(false, false);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
  display_obj.tft.setCursor(3, 3);
  display_obj.tft.printf("GATT %u/%u%s", static_cast<unsigned>(index + 1),
                         static_cast<unsigned>(results.size()), limited ? "*" : "");
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(3, 13);
  if (overallPageCount) {
    display_obj.tft.printf("Page %u/%u S:%u C:%u",
                           static_cast<unsigned>(overallPage + 1),
                           static_cast<unsigned>(overallPageCount),
                           serviceCount, characteristicCount);
  }
  else {
    display_obj.tft.printf("Services:%u  Chars:%u", serviceCount, characteristicCount);
  }

  display_obj.tft.setTextColor(item.service ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  display_obj.tft.setCursor(3, 27);
  display_obj.tft.println(item.service ? "SERVICE UUID" : "CHARACTERISTIC UUID");
  drawUuidLines(item.uuid, 39);

  if (!item.service) {
    display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
    display_obj.tft.setCursor(3, 62);
    display_obj.tft.println("PARENT SERVICE");
    drawUuidLines(item.parentServiceUuid, 74);
    display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
    display_obj.tft.setCursor(3, 97);
    display_obj.tft.printf("Props: %c %c %c %c",
                           item.properties & GATT_PROPERTY_READ ? 'R' : '-',
                           item.properties & GATT_PROPERTY_WRITE ? 'W' : '-',
                           item.properties & GATT_PROPERTY_NOTIFY ? 'N' : '-',
                           item.properties & GATT_PROPERTY_INDICATE ? 'I' : '-');
  } else if (limited) {
    display_obj.tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    display_obj.tft.setCursor(3, 74);
    display_obj.tft.println("* result list limited");
  }

  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(3, 116);
  display_obj.tft.println("Up/Down  Center exit");
}
#endif

void showGattResults(const GattDisplayResults& results,
                     uint16_t serviceCount,
                     uint16_t characteristicCount,
                     bool limited) {
  if (results.empty()) {
    showStatus("BLE GATT Services",
               String(serviceCount) + " services",
               String(characteristicCount) + " characteristics",
               "No displayable UUIDs");
    waitForReturn();
    return;
  }

#ifdef HAS_SCREEN
  size_t index = 0;
  releaseNavigationButtons();
  drawGattResult(results, index, serviceCount, characteristicCount, limited);
#if defined(HAS_BUTTONS) && (C_BTN >= 0)
  while (true) {
    if (centerPressed())
      break;
    if (upPressed()) {
      index = index == 0 ? results.size() - 1 : index - 1;
      drawGattResult(results, index, serviceCount, characteristicCount, limited);
    }
    if (downPressed()) {
      index = (index + 1) % results.size();
      drawGattResult(results, index, serviceCount, characteristicCount, limited);
    }
    delay(20);
  }
  releaseNavigationButtons();
#else
  delay(3000);
#endif
#else
  (void)limited;
  delay(100);
#endif
}

constexpr size_t INFO_PAGE_CHARACTERS = 180;

size_t pagedTextPageCount(const String& text) {
  return max(static_cast<size_t>(1),
             (text.length() + INFO_PAGE_CHARACTERS - 1) / INFO_PAGE_CHARACTERS);
}

#ifdef HAS_SCREEN
void drawCombinedPageFrame(const char* title, size_t page, size_t pageCount) {
  display_obj.clearScreen();
  display_obj.tft.setTextWrap(false, false);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
  display_obj.tft.setCursor(3, 3);
  display_obj.tft.println(title);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(3, 13);
  display_obj.tft.printf("Page %u/%u", static_cast<unsigned>(page + 1),
                         static_cast<unsigned>(pageCount));
  display_obj.tft.setCursor(3, 116);
  display_obj.tft.println("Up/Down  Center exit");
}

void drawWrappedText(const String& text, size_t offset, int16_t y,
                     uint8_t maximumLines, uint16_t color = TFT_WHITE) {
  display_obj.tft.setTextColor(color, TFT_BLACK);
  for (uint8_t line = 0; line < maximumLines && offset < text.length(); line++) {
    const size_t remaining = text.length() - offset;
    const size_t count = min(static_cast<size_t>(20), remaining);
    display_obj.tft.setCursor(3, y + line * 10);
    display_obj.tft.println(text.substring(offset, offset + count));
    offset += count;
  }
}

void drawCombinedSummaryPage(size_t page,
                             size_t summaryPageCount,
                             size_t totalPageCount,
                             uint16_t serviceCount,
                             uint16_t characteristicCount,
                             bool resultsLimited,
                             const String& outcome,
                             const String& logStatus) {
  const String services = target.advertisedServices[0]
                            ? String(target.advertisedServices)
                            : String(F("None advertised"));
  const size_t servicePageCount = pagedTextPageCount(services);
  const size_t advertisementPage = 2 + servicePageCount;
  const size_t scanResponsePage = advertisementPage + 1;
  const size_t gattSummaryPage = scanResponsePage + 1;

  if (page == 0) {
    drawCombinedPageFrame("BLE Target", page, totalPageCount);
    display_obj.tft.setTextColor(TFT_GREEN, TFT_BLACK);
    display_obj.tft.setCursor(3, 25);
    display_obj.tft.println("VENDOR");
    drawWrappedText(targetVendorName(), 0, 35, 3);
    display_obj.tft.setTextColor(TFT_GREEN, TFT_BLACK);
    display_obj.tft.setCursor(3, 65);
    display_obj.tft.println("MAC");
    drawWrappedText(targetAddress(), 0, 75, 1);
    display_obj.tft.setTextColor(TFT_GREEN, TFT_BLACK);
    display_obj.tft.setCursor(3, 87);
    display_obj.tft.println("NAME");
    drawWrappedText(String(target.name).length() ? String(target.name) : String(F("Unnamed")),
                    0, 97, 2);
    return;
  }

  if (page == 1) {
    drawCombinedPageFrame("Advertisement", page, totalPageCount);
    display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
    display_obj.tft.setCursor(3, 25);
    display_obj.tft.printf("Type: %s", addressTypeName(target.addressType));
    display_obj.tft.setCursor(3, 35);
    display_obj.tft.printf("Connectable: %s", target.connectable ? "yes" : "no");
    display_obj.tft.setCursor(3, 45);
    display_obj.tft.printf("Scannable: %s", target.scannable ? "yes" : "no");
    display_obj.tft.setCursor(3, 55);
    if (target.hasCompanyId)
      display_obj.tft.printf("Company ID: 0x%04X", target.companyId);
    else
      display_obj.tft.print("Company ID: none");
    display_obj.tft.setCursor(3, 65);
    if (target.hasAppearance)
      display_obj.tft.printf("Appearance: 0x%04X", target.appearance);
    else
      display_obj.tft.print("Appearance: none");
    display_obj.tft.setCursor(3, 75);
    display_obj.tft.printf("Adv bytes: %u", target.advertisementLength);
    display_obj.tft.setCursor(3, 85);
    display_obj.tft.printf("Scan rsp bytes: %u", target.scanResponseLength);
    display_obj.tft.setCursor(3, 95);
    display_obj.tft.printf("Truncated: %s", target.advertisementTruncated ? "yes" : "no");
    return;
  }

  if (page < advertisementPage) {
    drawCombinedPageFrame("Advertised UUIDs", page, totalPageCount);
    const size_t servicesPage = page - 2;
    drawWrappedText(services, servicesPage * INFO_PAGE_CHARACTERS, 25, 9);
    return;
  }

  if (page == advertisementPage) {
    drawCombinedPageFrame("Advertisement Data", page, totalPageCount);
    const String data = target.advertisementLength
                          ? bytesAsHex(target.advertisementData, target.advertisementLength)
                          : String(F("None"));
    drawWrappedText(data, 0, 25, 9);
    return;
  }

  if (page == scanResponsePage) {
    drawCombinedPageFrame("Scan Response Data", page, totalPageCount);
    const String data = target.scanResponseLength
                          ? bytesAsHex(target.scanResponseData, target.scanResponseLength)
                          : String(F("None"));
    drawWrappedText(data, 0, 25, 9);
    return;
  }

  if (page == gattSummaryPage && page < summaryPageCount) {
    drawCombinedPageFrame("GATT Summary", page, totalPageCount);
    display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
    display_obj.tft.setCursor(3, 25);
    display_obj.tft.printf("Services: %u", serviceCount);
    display_obj.tft.setCursor(3, 35);
    display_obj.tft.printf("Characteristics: %u", characteristicCount);
    display_obj.tft.setCursor(3, 45);
    display_obj.tft.printf("GATT pages: %u%s",
                           static_cast<unsigned>(totalPageCount - summaryPageCount),
                           resultsLimited ? " partial" : "");
    display_obj.tft.setTextColor(TFT_GREEN, TFT_BLACK);
    display_obj.tft.setCursor(3, 57);
    display_obj.tft.println("RESULT");
    drawWrappedText(outcome, 0, 67, 2);
    display_obj.tft.setTextColor(TFT_GREEN, TFT_BLACK);
    display_obj.tft.setCursor(3, 89);
    display_obj.tft.println("LOG");
    drawWrappedText(logStatus, 0, 99, 1);
  }
}
#endif

void showCombinedResults(const GattDisplayResults& results,
                         uint16_t serviceCount,
                         uint16_t characteristicCount,
                         bool resultsLimited,
                         const String& outcome,
                         const String& logStatus) {
#ifdef HAS_SCREEN
  const String services = target.advertisedServices[0]
                            ? String(target.advertisedServices)
                            : String(F("None advertised"));
  const size_t summaryPageCount = 5 + pagedTextPageCount(services);
  const size_t totalPageCount = summaryPageCount + results.size();
  size_t page = 0;

  auto drawPage = [&]() {
    if (page < summaryPageCount) {
      drawCombinedSummaryPage(page, summaryPageCount, totalPageCount,
                              serviceCount, characteristicCount,
                              resultsLimited, outcome, logStatus);
    }
    else {
      const size_t resultIndex = page - summaryPageCount;
      drawGattResult(results, resultIndex, serviceCount, characteristicCount,
                     resultsLimited, page, totalPageCount);
    }
  };

  releaseNavigationButtons();
  drawPage();
#if defined(HAS_BUTTONS) && (C_BTN >= 0)
  uint8_t repeatDirection = 0;
  uint32_t repeatAt = 0;
  while (true) {
    if (centerPressed())
      break;

    bool navigateUp = upPressed();
    bool navigateDown = downPressed();
#if (U_BTN >= 0) && (D_BTN >= 0)
    const bool upDown = u_btn.getPullup() ? digitalRead(u_btn.getPin()) == LOW
                                         : digitalRead(u_btn.getPin()) == HIGH;
    const bool downDown = d_btn.getPullup() ? digitalRead(d_btn.getPin()) == LOW
                                           : digitalRead(d_btn.getPin()) == HIGH;
    const uint8_t currentDirection = upDown == downDown ? 0 : (upDown ? 1 : 2);
    const uint32_t now = millis();
    if (!currentDirection) {
      repeatDirection = 0;
    }
    else if (currentDirection != repeatDirection) {
      repeatDirection = currentDirection;
      repeatAt = now + GATT_PAGE_REPEAT_DELAY_MS;
    }
    else if (static_cast<int32_t>(now - repeatAt) >= 0) {
      navigateUp = currentDirection == 1;
      navigateDown = currentDirection == 2;
      repeatAt = now + GATT_PAGE_REPEAT_INTERVAL_MS;
    }
#endif

    if (navigateUp) {
      page = page == 0 ? totalPageCount - 1 : page - 1;
      drawPage();
    }
    else if (navigateDown) {
      page = (page + 1) % totalPageCount;
      drawPage();
    }
    delay(20);
  }
  releaseNavigationButtons();
#else
  delay(3000);
#endif
#else
  (void)results;
  (void)serviceCount;
  (void)characteristicCount;
  (void)resultsLimited;
  (void)outcome;
  (void)logStatus;
  delay(100);
#endif
}

void waitForReturn() {
#ifdef HAS_SCREEN
  display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  display_obj.tft.println("Center: return");
#endif
  releaseCenterButton();
#if defined(HAS_BUTTONS) && (C_BTN >= 0)
  while (!centerPressed())
    delay(20);
#else
  delay(2000);
#endif
}

void closeBLEClient(NimBLEClient*& client) {
  if (client) {
    if (client->isConnected())
      client->disconnect();
    NimBLEDevice::deleteClient(client);
    client = nullptr;
  }
  if (NimBLEDevice::isInitialized())
    NimBLEDevice::deinit(true);
}

bool targetReady(String& error, bool requireConnectable) {
  if (!target.valid) {
    error = "Select a scanned target first";
    return false;
  }
  if (requireConnectable && !target.connectable) {
    error = "Target did not advertise connectable";
    return false;
  }
  return true;
}

}  // namespace

void selectTarget(const BleDevice& device) {
  memset(&target, 0, sizeof(target));
  target.valid = true;
  memcpy(target.mac, device.mac, sizeof(target.mac));
  target.addressType = device.addressType;
  target.companyId = device.companyId;
  target.appearance = device.appearance;
  target.hasCompanyId = device.hasCompanyId;
  target.hasAppearance = device.hasAppearance;
  target.connectable = device.connectable;
  target.scannable = device.scannable;
  target.advertisementTruncated = device.advertisementTruncated;
  copyText(target.name, sizeof(target.name), device.name);
  copyText(target.advertisedServices, sizeof(target.advertisedServices), device.advertisedServices);
  target.advertisementLength = device.advertisementLength;
  target.scanResponseLength = device.scanResponseLength;
  memcpy(target.advertisementData, device.advertisementData, target.advertisementLength);
  memcpy(target.scanResponseData, device.scanResponseData, target.scanResponseLength);
  Serial.println(String("[BLE Discovery] selected ") + selectedTargetLabel());
}

bool hasTarget() {
  return target.valid;
}

const char* manufacturerName(const BleDevice& device) {
  return device.hasCompanyId ? BLECompanyIdentifiers::lookup(device.companyId) : nullptr;
}

String deviceDisplayLabel(const BleDevice& device) {
  const char* company = manufacturerName(device);
  String label = String(device.rssi) + " ";
  if (device.name.length() && device.name.indexOf(':') < 0)
    label += device.name;
  else if (company)
    label += company;
  else
    label += device.name;
  if (!device.connectable)
    label += " [NC]";
  return label;
}

String selectedTargetLabel() {
  if (!target.valid)
    return "No target selected";
  String label(target.name);
  if (!label.length())
    label = targetAddress();
  if (target.hasCompanyId) {
    const char* company = BLECompanyIdentifiers::lookup(target.companyId);
    if (company && label != company)
      label += String(" / ") + company;
  }
  return label;
}

void showAdvertisedInfo() {
  String error;
  if (!targetReady(error, false)) {
    showStatus("Advertised Services", error);
    waitForReturn();
    return;
  }

  const char* company = target.hasCompanyId ? BLECompanyIdentifiers::lookup(target.companyId) : nullptr;
  Serial.println(F("[BLE Advertisement]"));
  Serial.println(String("  name: ") + target.name);
  Serial.println(String("  address: ") + targetAddress());
  Serial.println(String("  address type: ") + addressTypeName(target.addressType));
  Serial.println(String("  connectable: ") + (target.connectable ? "yes" : "no"));
  Serial.println(String("  scannable: ") + (target.scannable ? "yes" : "no"));
  if (target.hasCompanyId) {
    Serial.printf("  company ID: 0x%04X\n", target.companyId);
    Serial.println(String("  company: ") + (company ? company : "unassigned"));
  }
  if (target.hasAppearance)
    Serial.printf("  appearance: 0x%04X\n", target.appearance);
  Serial.println(String("  advertised service UUIDs: ") +
                 (target.advertisedServices[0] ? target.advertisedServices : "none"));
  Serial.println(String("  advertisement data: ") + bytesAsHex(target.advertisementData, target.advertisementLength));
  Serial.println(String("  scan response data: ") + bytesAsHex(target.scanResponseData, target.scanResponseLength));
  if (target.advertisementTruncated)
    Serial.println(F("  warning: extended advertisement was truncated to the legacy 31+31 byte snapshot"));

  String companyText = company ? String(company) : String("Company not advertised");
  String servicesText = target.advertisedServices[0] ? String(target.advertisedServices) : String("No service UUIDs advertised");
  if (servicesText.length() > 65)
    servicesText = servicesText.substring(0, 65) + "...";
  showStatus("Advertised Services",
             targetAddress() + " (" + addressTypeName(target.addressType) + ")",
             companyText,
             servicesText);
  waitForReturn();
}

void inspectTarget() {
  String error;
  if (!targetReady(error, false)) {
    showStatus("GATT + Advertised", error);
    waitForReturn();
    return;
  }

  const char* company = target.hasCompanyId ? BLECompanyIdentifiers::lookup(target.companyId) : nullptr;
  Serial.println(F("[BLE GATT + Advertisement Enumeration]"));
  Serial.println(String("  vendor: ") + targetVendorName());
  Serial.println(String("  name: ") + target.name);
  Serial.println(String("  address: ") + targetAddress());
  Serial.println(String("  address type: ") + addressTypeName(target.addressType));
  Serial.println(String("  connectable: ") + (target.connectable ? "yes" : "no"));
  Serial.println(String("  scannable: ") + (target.scannable ? "yes" : "no"));
  if (target.hasCompanyId) {
    Serial.printf("  company ID: 0x%04X\n", target.companyId);
    Serial.println(String("  company: ") + (company ? company : "unassigned"));
  }
  if (target.hasAppearance)
    Serial.printf("  appearance: 0x%04X\n", target.appearance);
  Serial.println(String("  advertised service UUIDs: ") +
                 (target.advertisedServices[0] ? target.advertisedServices : "none"));
  Serial.println(String("  advertisement data: ") +
                 bytesAsHex(target.advertisementData, target.advertisementLength));
  Serial.println(String("  scan response data: ") +
                 bytesAsHex(target.scanResponseData, target.scanResponseLength));
  if (target.advertisementTruncated)
    Serial.println(F("  warning: extended advertisement was truncated to the legacy 31+31 byte snapshot"));

  releaseCenterButton();

  uint16_t serviceCount = 0;
  uint16_t characteristicCount = 0;
  bool resultsLimited = false;
  bool enumerationStopped = false;
  GattDisplayResults displayResults;
  String outcome;
  String logStatus = F("SD card unavailable");

#ifdef HAS_SD
  File gattLog;
  String gattLogPath;
  bool gattLogSaved = false;
  if (openGattServiceLog(gattLog, gattLogPath)) {
    writeGattLogHeader(gattLog);
    logStatus = gattLogPath;
    Serial.println(String(F("[BLE Enumeration] Saving combined results to ")) + gattLogPath);
  }
  else {
    logStatus = sd_obj.supported ? F("SD log create failed") : F("SD card unavailable");
    Serial.println(String(F("[BLE Enumeration] ")) + logStatus);
  }

  auto finishGattLog = [&]() {
    if (!gattLog)
      return;
    gattLog.println();
    gattLog.print(F("services_discovered="));
    gattLog.println(serviceCount);
    gattLog.print(F("characteristics_discovered="));
    gattLog.println(characteristicCount);
    gattLog.print(F("screen_result_pages="));
    gattLog.println(displayResults.size());
    gattLog.print(F("screen_results_complete="));
    gattLog.println(resultsLimited ? F("no") : F("yes"));
    gattLog.print(F("result="));
    gattLog.println(outcome);
    gattLog.flush();
    gattLog.close();
    gattLogSaved = true;
    Serial.println(String(F("[BLE Enumeration] Combined log saved: ")) + gattLogPath);
  };
#endif

  if (!target.connectable) {
    outcome = F("GATT skipped: not connectable");
    Serial.println(String(F("[GATT] ")) + outcome);
  }
  else {
    showStatus("GATT + Advertised", "Connecting...", selectedTargetLabel()
#ifdef HAS_SD
               , logStatus
#endif
    );
    NimBLEDevice::init("Marauder-Discovery");
    NimBLEDevice::setSecurityAuth(false, false, false);
    NimBLEClient* client = NimBLEDevice::createClient();
    if (!client) {
      outcome = F("error: cannot create BLE client");
      closeBLEClient(client);
    }
    else {
      client->setConnectTimeout(15000);
      if (!client->connect(NimBLEAddress(target.mac, target.addressType), true, false, true)) {
        outcome = String(F("connect failed: ")) + client->getLastError();
        closeBLEClient(client);
      }
      else {

#ifdef HAS_SD
        if (gattLog)
          gattLog.println(F("connection=connected"));
#endif

        const auto& services = client->getServices(true);
        for (NimBLERemoteService* service : services) {
          if (!service)
            continue;
          if (centerPressed()) {
            enumerationStopped = true;
            break;
          }
          serviceCount++;
          const std::string serviceUuid = service->getUUID().toString();
          Serial.printf("[GATT] Service %s\n", serviceUuid.c_str());
#ifdef HAS_SD
          if (gattLog) {
            gattLog.print(F("Service "));
            gattLog.println(serviceUuid.c_str());
          }
#endif
          addGattDisplayItem(displayResults, true, serviceUuid, "", 0, resultsLimited);
          const auto& characteristics = service->getCharacteristics(true);
          for (NimBLERemoteCharacteristic* characteristic : characteristics) {
            if (!characteristic)
              continue;
            if (centerPressed()) {
              enumerationStopped = true;
              break;
            }
            characteristicCount++;
            const std::string characteristicUuid = characteristic->getUUID().toString();
            uint8_t properties = 0;
            if (characteristic->canRead()) properties |= GATT_PROPERTY_READ;
            if (characteristic->canWrite()) properties |= GATT_PROPERTY_WRITE;
            if (characteristic->canNotify()) properties |= GATT_PROPERTY_NOTIFY;
            if (characteristic->canIndicate()) properties |= GATT_PROPERTY_INDICATE;
            Serial.printf("  Characteristic %s [%c%c%c%c]\n",
                          characteristicUuid.c_str(),
                          properties & GATT_PROPERTY_READ ? 'R' : '-',
                          properties & GATT_PROPERTY_WRITE ? 'W' : '-',
                          properties & GATT_PROPERTY_NOTIFY ? 'N' : '-',
                          properties & GATT_PROPERTY_INDICATE ? 'I' : '-');
#ifdef HAS_SD
            if (gattLog) {
              gattLog.print(F("  Characteristic "));
              gattLog.print(characteristicUuid.c_str());
              gattLog.printf(" [%c%c%c%c]\n",
                             properties & GATT_PROPERTY_READ ? 'R' : '-',
                             properties & GATT_PROPERTY_WRITE ? 'W' : '-',
                             properties & GATT_PROPERTY_NOTIFY ? 'N' : '-',
                             properties & GATT_PROPERTY_INDICATE ? 'I' : '-');
            }
#endif
            addGattDisplayItem(displayResults, false, characteristicUuid, serviceUuid,
                               properties, resultsLimited);
          }
          if (enumerationStopped)
            break;
        }
        closeBLEClient(client);
        outcome = enumerationStopped ? F("stopped by user") : F("complete");
      }
    }
  }

  Serial.println(String("[GATT] Display results: ") + displayResults.size() +
                 (resultsLimited ? " (limited)" : ""));
  if (enumerationStopped)
    Serial.println(F("[GATT] Enumeration stopped by user"));
  if (outcome.startsWith("error") || outcome.startsWith("connect failed"))
    Serial.println(String(F("[GATT] ")) + outcome);

#ifdef HAS_SD
  finishGattLog();
  if (gattLogSaved)
    logStatus = gattLogPath;
#endif
  showCombinedResults(displayResults, serviceCount, characteristicCount,
                      resultsLimited, outcome, logStatus);
}

void runDeviceSpoof() {
  String error;
  if (!targetReady(error, false)) {
    showStatus("Device Spoof", error);
    waitForReturn();
    return;
  }
  if (target.advertisementLength == 0) {
    showStatus("Device Spoof", "No advertisement snapshot");
    waitForReturn();
    return;
  }

  releaseCenterButton();
  showStatus("Device Spoof", "Loading captured variants...", targetAddress());
  std::vector<AdvertisementVariant> variants;
  variants.reserve(8);
  bool memoryLimited = false;
  bool unsupportedLength = target.advertisementTruncated;
  uint16_t captureFilesRead = 0;
  loadAdvertisementVariants(variants, memoryLimited, unsupportedLength, captureFilesRead);
  if (variants.empty()) {
    showStatus("Device Spoof", "No legacy advertisement data");
    waitForReturn();
    return;
  }

  uint8_t originalBtAddress[6]{};
  bool publicAddressOverridden = false;
  if (!configureExactAddress(originalBtAddress, publicAddressOverridden, error)) {
    restoreLocalAddress(originalBtAddress, publicAddressOverridden);
    showStatus("Device Spoof", error);
    waitForReturn();
    return;
  }

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  if (!startVariant(advertising, variants[0])) {
    restoreLocalAddress(originalBtAddress, publicAddressOverridden);
    showStatus("Device Spoof", "Advertising failed");
    waitForReturn();
    return;
  }

  const String localAddress = NimBLEDevice::getAddress().toString().c_str();
  Serial.println(String("[Device Spoof] exact address: ") + localAddress);
  Serial.println(String("[Device Spoof] unique legacy payload sets: ") + variants.size());
  Serial.println(String("[Device Spoof] capture logs read: ") + captureFilesRead);
  if (memoryLimited)
    Serial.println(F("[Device Spoof] stopped loading variants to preserve 64 KB free heap"));
  if (unsupportedLength)
    Serial.println(F("[Device Spoof] extended payloads beyond legacy 31+31 bytes were skipped"));
  Serial.println(F("[Device Spoof] non-connectable advertising only; no GATT, keys, pairing state, or IRK is cloned"));

  String variantText = String(variants.size()) + " payload set" + (variants.size() == 1 ? "" : "s");
  if (memoryLimited)
    variantText += " (memory limit)";
  showStatus("Device Spoof",
             String("Exact address ") + localAddress,
             variantText + "; non-connectable",
             "Center: stop");
  releaseCenterButton();
  size_t variantIndex = 0;
  uint32_t nextVariantAt = millis() + DEVICE_SPOOF_VARIANT_DWELL_MS;
  bool replayFailed = false;
  while (!centerPressed()) {
    if (variants.size() > 1 && static_cast<int32_t>(millis() - nextVariantAt) >= 0) {
      variantIndex = (variantIndex + 1) % variants.size();
      if (!startVariant(advertising, variants[variantIndex])) {
        replayFailed = true;
        break;
      }
      nextVariantAt = millis() + DEVICE_SPOOF_VARIANT_DWELL_MS;
    }
    delay(20);
  }
  if (advertising->isAdvertising())
    advertising->stop();
  restoreLocalAddress(originalBtAddress, publicAddressOverridden);
  if (replayFailed) {
    showStatus("Device Spoof", "Variant replay failed", "Local address restored");
    waitForReturn();
  }
}

}  // namespace BLESecurityTools

#else

namespace BLESecurityTools {
void selectTarget(const BleDevice&) {}
bool hasTarget() { return false; }
String selectedTargetLabel() { return "BLE discovery unavailable"; }
String deviceDisplayLabel(const BleDevice& device) { return device.name; }
const char* manufacturerName(const BleDevice&) { return nullptr; }
void showAdvertisedInfo() {}
void inspectTarget() {}
void runDeviceSpoof() {}
}  // namespace BLESecurityTools

#endif
