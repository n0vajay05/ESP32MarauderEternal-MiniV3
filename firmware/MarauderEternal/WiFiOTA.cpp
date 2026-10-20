#include "WiFiOTA.h"
#include "OtaPolicy.h"
#include "OtaTransport.h"
#include "OtaImageWriter.h"
#include "UpdateKeyboard.h"
#include "FirmwareMetadata.h"
#include "MenuFunctions.h"
#include "CommandLine.h"
#include "DeviceClock.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_wifi.h"
#include "mbedtls/sha256.h"
#include <Preferences.h>
#include <memory>
#include <vector>

extern Display display_obj;
extern MenuFunctions menu_function_obj;
extern WiFiScan wifi_scan_obj;
extern CommandLine cli_obj;
#ifdef HAS_GPS
extern GpsInterface gps_obj;
#endif

// The Arduino core otherwise accepts a new image before setup has finished.
// Keep rollback pending until the complete device setup has succeeded.
extern "C" bool verifyRollbackLater() { return true; }

namespace marauder { namespace ota {
namespace {
bool session = false, ui = false, cancelRequest = false, serverReady = false;
bool profileSaved = false;
const char* profileStorage = "eternal-ota";
String ssid, password;
Network network;
Release release;
constexpr uint16_t kSurface = 0x18e3, kAccent = 0x733f;
uint8_t repeatDirection = 0;
uint32_t repeatAt = 0;
uint32_t lastCheckMs = 0;
const char* serverStage = "Idle";
const char* serverError = "None";
int serverCode = 0, serverStatus = 0;
const char* phase = "Idle";

struct PsramAllocator {
  void* allocate(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void* value) { free(value); }
  void* reallocate(void* value, size_t size) { return heap_caps_realloc(value, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
using Document = BasicJsonDocument<PsramAllocator>;

// A single atomic NVS blob prevents mismatched SSID/password after power loss.
struct Profile {
  uint32_t magic = 0x45544f41;
  uint8_t schema = 1, manual = 0;
  char ssid[33]{}, password[65]{};
  uint32_t fields[5]{};
};
bool loadProfile(String& name, String& secret, Network& config, const char* storage = "eternal-ota", bool* forgotten = nullptr) {
  if (forgotten) *forgotten = false;
  Preferences prefs;
  if (!prefs.begin(storage, true)) return false;
  Profile value;
  const bool loaded = prefs.getBytesLength("profile") == sizeof(value) && prefs.getBytes("profile", &value, sizeof(value)) == sizeof(value);
  prefs.end();
  if (!loaded || value.magic != 0x45544f41 || value.schema != 1 || value.manual > 1 ||
      !memchr(value.ssid, 0, sizeof(value.ssid)) ||
      !memchr(value.password, 0, sizeof(value.password)) || !validPassword(value.password)) return false;
  if (!value.ssid[0]) {
    bool cleared = !value.password[0] && !value.manual;
    for (uint32_t field : value.fields) cleared &= !field;
    if (forgotten) *forgotten = cleared;
    return false;
  }
  config.manual = value.manual; config.ip = value.fields[0]; config.mask = value.fields[1];
  config.gateway = value.fields[2]; config.dns1 = value.fields[3]; config.dns2 = value.fields[4];
  if (!config.valid()) return false;
  name = value.ssid; secret = value.password; return true;
}
bool writeProfile(const String& name, const String& secret, const Network& config, const char* storage) {
  if (!name.length() || name.length() > 32 || !validPassword(secret.c_str()) || !config.valid()) return false;
  Profile value{};
  memcpy(value.ssid, name.c_str(), name.length()); memcpy(value.password, secret.c_str(), secret.length());
  value.manual = config.manual;
  value.fields[0] = config.ip; value.fields[1] = config.mask; value.fields[2] = config.gateway;
  value.fields[3] = config.dns1; value.fields[4] = config.dns2;
  Preferences prefs; if (!prefs.begin(storage, false)) return false;
  const bool saved = prefs.putBytes("profile", &value, sizeof(value)) == sizeof(value); prefs.end(); return saved;
}
bool saveProfile() { return writeProfile(ssid, password, network, profileStorage); }
bool forgetProfile(const char* storage) {
  // An atomic empty profile also records an explicit forget, so first-use
  // import cannot silently reconnect a network from the general WiFi profiles.
  const Profile cleared{};
  Preferences prefs; if (!prefs.begin(storage, false)) return false;
  const bool saved = prefs.putBytes("profile", &cleared, sizeof(cleared)) == sizeof(cleared);
  prefs.end(); return saved;
}
void pump() {
  static bool pumping = false;
  if (pumping) return;
  pumping = true;
  cli_obj.main(millis()); // runCommand restricts commands during the update session.
  #ifdef HAS_GPS
    gps_obj.main();
  #endif
  pumping = false;
}
bool cancelled() { return !session || cancelRequest; }
void resetButtons() {
  repeatDirection = 0;
  u_btn.justPressed(); d_btn.justPressed(); l_btn.justPressed(); r_btn.justPressed(); c_btn.justPressed();
}
enum Key { None, Up, Down, Left, Right, Center };
Key key() {
  pump();
  const bool up = u_btn.justPressed(), down = d_btn.justPressed(), left = l_btn.justPressed();
  const bool right = r_btn.justPressed(), center = c_btn.justPressed();
  if (center) return Center;
  Key direction = up ? Up : down ? Down : left ? Left : right ? Right : None;
  const bool levels[] = {false, u_btn.isDown(), d_btn.isDown(), l_btn.isDown(), r_btn.isDown()};
  unsigned count = 0; Key held = None;
  for (unsigned i = 1; i <= 4; ++i) if (levels[i]) { held = Key(i); ++count; }
  if (count != 1) repeatDirection = 0;
  else if (held != repeatDirection) { repeatDirection = held; repeatAt = millis() + 450; }
  else if (int32_t(millis() - repeatAt) >= 0) { direction = held; repeatAt = millis() + 120; }
  return direction;
}
void screen(const char* title) {
  auto& tft = display_obj.tft;
  tft.setFreeFont(nullptr); tft.setTextDatum(TL_DATUM); tft.setTextSize(1);
  tft.setTextWrap(false); tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 128, 16, kAccent); tft.setTextColor(TFT_WHITE, kAccent);
  tft.drawString(title, 3, 4, 1);
}
void progress(const char* title, const String& text, unsigned percent = 0) {
  screen(title); auto& tft = display_obj.tft;
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextWrap(true); tft.setCursor(3, 29); tft.print(text);
  tft.setTextWrap(false);
  if (percent) { tft.drawRect(4, 94, 120, 10, kSurface); tft.fillRect(5, 95, 118 * std::min(percent, 100u) / 100, 8, TFT_GREEN); }
}
void message(const char* title, const String& text) {
  progress(title, text); display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.drawString("C / Left: Back", 4, 116, 1);
  resetButtons();
  while (session) { const Key value = key(); if (value == Center || value == Left) break; delay(3); }
}
void drawWifiName(int offset) {
  auto& tft = display_obj.tft;
  tft.setViewport(3, 14, 122, 8);
  tft.fillRect(0, 0, 122, 8, kAccent); tft.setTextColor(TFT_WHITE, kAccent);
  tft.drawString(WiFi.SSID(), -offset, 0, 1);
  tft.resetViewport();
}
void drawWifiBanner() {
  auto& tft = display_obj.tft;
  tft.fillRect(0, 0, 128, 26, kAccent); tft.setTextColor(TFT_WHITE, kAccent);
  if (WiFi.status() == WL_CONNECTED) {
    tft.drawString("Connected:", 3, 3, 1); drawWifiName(0);
  } else {
    tft.drawString("Please Connect", 3, 3, 1);
    tft.drawString("To Update", 3, 14, 1);
  }
}
void drawList(const char* title, const std::vector<String>& rows, unsigned selected, bool wifiMenu = false) {
  screen(title); auto& tft = display_obj.tft;
  if (wifiMenu) drawWifiBanner();
  const unsigned visible = wifiMenu ? 6 : 5;
  const unsigned start = selected / visible * visible;
  for (unsigned row = start; row < rows.size() && row < start + visible; ++row) {
    const int y = (wifiMenu ? 30 : 20) + int(row - start) * (wifiMenu ? 13 : 18);
    const uint16_t color = row == selected ? kAccent : kSurface;
    tft.fillRoundRect(2, y, 124, wifiMenu ? 12 : 16, 3, color); tft.setTextColor(TFT_WHITE, color);
    const int newline = rows[row].indexOf('\n');
    if (newline >= 0) {
      tft.drawString(rows[row].substring(0, newline), 5, y, 1);
      tft.drawString(rows[row].substring(newline + 1), 5, y + 8, 1);
    } else tft.drawString(rows[row].substring(0, 20), 5, y + (wifiMenu ? 2 : 4), 1);
  }
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK); tft.drawString("C: Select  L: Back", 3, 116, 1);
}
int choose(const char* title, const std::vector<String>& rows, bool wifiMenu = false) {
  unsigned selected = 0; drawList(title, rows, selected, wifiMenu); resetButtons();
  const bool connected = WiFi.status() == WL_CONNECTED;
  const int scrollLimit = wifiMenu && connected ? std::max(0, int(display_obj.tft.textWidth(WiFi.SSID(), 1)) - 122) : 0;
  int offset = 0, direction = 1;
  uint32_t nextScroll = millis() + 900;
  while (session) {
    const Key value = key();
    if (wifiMenu && connected != (WiFi.status() == WL_CONNECTED)) return -1;
    if (value == Left) return 0;
    if (value == Center || value == Right) return selected;
    if (value == Up || value == Down) {
      selected = (selected + rows.size() + (value == Up ? -1 : 1)) % rows.size();
      drawList(title, rows, selected, wifiMenu);
      offset = 0; direction = 1; nextScroll = millis() + 900;
    } else if (scrollLimit && int32_t(millis() - nextScroll) >= 0) {
      offset += direction; drawWifiName(offset);
      if (offset == scrollLimit || !offset) { direction = -direction; nextScroll = millis() + 900; }
      else nextScroll = millis() + 120;
    }
    delay(3);
  }
  return 0;
}
bool edit(const char* title, String& text, bool secret, size_t maximum, bool numeric = false) {
  Keyboard keyboard(numeric); keyboard.value = text.c_str(); keyboard.maximum = maximum;
  const auto draw = [&]() {
    screen(title); auto& tft = display_obj.tft;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    String visible = secret ? String("") : String(keyboard.value.c_str());
    if (secret) { visible = ""; for (size_t i = 0; i < keyboard.value.size(); ++i) visible += '*'; }
    if (visible.length() > 19) visible = visible.substring(visible.length() - 19);
    tft.drawString(visible, 3, 21, 1);
    tft.drawString(String(keyboard.value.size()) + "/" + String(maximum), 92, 31, 1);
    const unsigned cols = keyboard.columns(), width = 120 / cols;
    const char* keys = keyboard.keys();
    for (unsigned i = 0; keys[i]; ++i) {
      const int x = 4 + (i % cols) * width, y = 40 + (i / cols) * (numeric ? 17 : 12);
      const uint16_t color = !keyboard.actions && keyboard.selected == i ? kAccent : kSurface;
      tft.fillRect(x, y, width - 2, numeric ? 15 : 10, color); tft.setTextColor(TFT_WHITE, color);
      const String label = keys[i] == ' ' ? "Sp" : String(keys[i]);
      tft.drawCentreString(label, x + (width - 2) / 2, y + 1, 1);
    }
    const char* labels[] = {"Mode", "Del", "Back", "Enter"};
    const unsigned count = keyboard.actionCount();
    for (unsigned i = 0; i < count; ++i) {
      const uint16_t color = keyboard.actions && keyboard.selected == i ? kAccent : kSurface;
      const int width = 128 / count;
      tft.fillRect(i * width, 112, width - 1, 16, color); tft.setTextColor(TFT_WHITE, color);
      tft.drawCentreString(labels[i + (numeric ? 1 : 0)], i * width + width / 2, 116, 1);
    }
  };
  draw(); resetButtons();
  while (session) {
    const Key value = key();
    if (value == Up || value == Down) keyboard.move(0, value == Up ? -1 : 1);
    else if (value == Left || value == Right) keyboard.move(value == Left ? -1 : 1, 0);
    else if (value == Center) {
      const int result = keyboard.choose();
      if (result < 0) return false;
      if (result > 0) { text = keyboard.value.c_str(); return true; }
    }
    if (value != None) draw();
    delay(3);
  }
  return false;
}
IPAddress address(uint32_t value) { return IPAddress(value >> 24, value >> 16, value >> 8, value); }
uint32_t number(IPAddress value) { return uint32_t(value[0]) << 24 | uint32_t(value[1]) << 16 | uint32_t(value[2]) << 8 | value[3]; }
Network received() {
  Network result;
  result.ip = number(WiFi.localIP()); result.mask = number(WiFi.subnetMask()); result.gateway = number(WiFi.gatewayIP());
  result.dns1 = number(WiFi.dnsIP(0)); result.dns2 = number(WiFi.dnsIP(1)); return result;
}
bool checkServer() {
  struct Timing { uint32_t start = millis(); ~Timing() { lastCheckMs = millis() - start; } } timing;
  cancelRequest = false; serverReady = false; phase = "Checking update server";
  progress("WiFi Update", "Checking update server\n15 second timeout");
  HttpStream http(pump, cancelled);
  serverStage = "Connecting"; serverError = "None"; serverCode = serverStatus = 0;
  if (!http.get(kApi, kServerTimeoutMs, 65536)) {
    serverStage = http.stage(); serverError = http.error(); serverCode = http.transportError(); serverStatus = http.statusCode();
    return false;
  }
  serverStatus = http.statusCode();
  Document doc(16384); StaticJsonDocument<512> filter;
  filter["tag_name"] = true; filter["draft"] = true; filter["prerelease"] = true;
  for (const char* field : {"name", "browser_download_url", "size", "digest"}) filter["assets"][0][field] = true;
  const DeserializationError jsonError = deserializeJson(doc, http, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(8));
  if (jsonError) { serverStage = "JSON"; serverError = jsonError.c_str(); return false; }
  if (!http.finishJson()) { serverStage = "Body"; serverError = http.error(); return false; }
  if (!readRelease(doc.as<JsonVariantConst>(), release)) { serverStage = "Release"; serverError = "Invalid release assets"; return false; }
  serverStage = "Complete";
  serverReady = true; phase = "Connected"; return true;
}
void disconnectNetwork() {
  serverReady = false; release = {};
  WiFi.setAutoReconnect(false);
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(false, true);
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
    WiFi.disconnect(true, true); WiFi.mode(WIFI_OFF);
  }
}
void forgetNetwork() {
  if (!forgetProfile(profileStorage)) { message("Forget Network", "Could not clear\nsaved network"); return; }
  disconnectNetwork(); ssid = ""; password = ""; network = {}; profileSaved = false;
  phase = "Network forgotten"; serverStage = "Idle"; serverError = "None";
  serverCode = serverStatus = 0; lastCheckMs = 0;
}
bool connectNetwork(const String& name, const String& secret, const Network& config) {
  if (!name.length() || name.length() > 32 || !validPassword(secret.c_str()) || !config.valid()) {
    message("Network", "Invalid SSID, password\nor network settings"); return false;
  }
  disconnectNetwork(); cancelRequest = false; phase = "Connecting WiFi";
  progress("WiFi Update", "Connecting to\n" + name);
  WiFi.persistent(false); WiFi.setAutoReconnect(false); WiFi.mode(WIFI_STA);
  const bool configured = config.manual ? WiFi.config(address(config.ip), address(config.gateway), address(config.mask), address(config.dns1), address(config.dns2))
                                        : WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  if (!configured) { disconnectNetwork(); message("Network", "Could not configure\nnetwork"); return false; }
  WiFi.begin(name.c_str(), secret.c_str());
  const uint32_t start = millis();
  while (session && remaining(start, millis(), 15000) && WiFi.status() != WL_CONNECTED) {
    pump(); delay(5);
  }
  if (!session || WiFi.status() != WL_CONNECTED || !number(WiFi.localIP())) {
    disconnectNetwork(); phase = "WiFi connection failed";
    message("WiFi Update", "Failed to connect to\nWiFi network"); return false;
  }
  ssid = name; password = secret; network = config; profileSaved = saveProfile();
  if (!checkServer()) {
    phase = "Update server unreachable";
    message("WiFi Update", "Failed to connect to update server"); return false;
  }
  return true;
}
void networkInfo() {
  if (WiFi.status() != WL_CONNECTED) { message("Network Info", "Not connected"); return; }
  Network draft = received(); draft.manual = network.manual;
  const char* labels[] = {"IP Address", "Subnet Mask", "Gateway", "Primary DNS", "Secondary DNS"};
  while (session) {
    uint32_t* fields[] = {&draft.ip, &draft.mask, &draft.gateway, &draft.dns1, &draft.dns2};
    std::vector<String> rows{"Back"};
    for (unsigned i = 0; i < 5; ++i) rows.push_back(String(labels[i]) + "\n" + address(*fields[i]).toString());
    rows.push_back("Use DHCP"); rows.push_back("Apply Changes");
    // Each field opens with the received/current address prefilled on a number pad.
    const int selected = choose("Network Info", rows);
    if (!selected) return;
    if (selected >= 1 && selected <= 5) {
      String text = address(*fields[selected - 1]).toString();
      if (edit(labels[selected - 1], text, false, 15, true)) {
        uint32_t parsed;
        if (!ipv4(text.c_str(), parsed)) message("Network Info", "Invalid IPv4 address");
        else { *fields[selected - 1] = parsed; draft.manual = true; }
      }
    } else if (selected == 6) {
      if (connectNetwork(ssid, password, Network{})) draft = received();
      draft.manual = false;
    } else if (selected == 7) {
      if (!draft.valid()) { message("Network Info", "Check IP, subnet,\ngateway and DNS"); continue; }
      if (connectNetwork(ssid, password, draft)) { draft = received(); draft.manual = network.manual; }
    }
  }
}
void manualNetwork() {
  String name = ssid, secret;
  if (!edit("Enter SSID", name, false, 32) || !name.length()) return;
  if (name == ssid) secret = password;
  if (!edit("WiFi Password", secret, true, 64)) return;
  const Network config = name == ssid ? network : Network{};
  connectNetwork(name, secret, config);
}
void scanNetworks() {
  phase = "Scanning SSIDs"; progress("Scan SSIDs", "Scanning networks...");
  if (WiFi.getMode() == WIFI_OFF) { WiFi.persistent(false); WiFi.setAutoReconnect(false); WiFi.mode(WIFI_STA); }
  WiFi.scanDelete(); WiFi.scanNetworks(true, true, false, 120);
  const uint32_t start = millis();
  int found = WIFI_SCAN_RUNNING;
  while (session && remaining(start, millis(), 12000) && (found = WiFi.scanComplete()) == WIFI_SCAN_RUNNING) { pump(); delay(5); }
  if (found < 0) { esp_wifi_scan_stop(); WiFi.scanDelete(); message("Scan SSIDs", "No networks found"); return; }
  struct Found { String name; int32_t rssi = -200; bool open = false; } entries[24];
  unsigned count = 0;
  for (int i = 0; i < found; ++i) {
    const String name = WiFi.SSID(i); if (!name.length() || name.length() > 32) continue;
    unsigned slot = count;
    for (unsigned j = 0; j < count; ++j) if (entries[j].name == name) { slot = j; break; }
    if (slot == count && count == 24) {
      slot = 0; for (unsigned j = 1; j < count; ++j) if (entries[j].rssi < entries[slot].rssi) slot = j;
    }
    if (slot < count && entries[slot].rssi >= WiFi.RSSI(i)) continue;
    entries[slot] = {name, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN};
    if (slot == count) ++count;
  }
  WiFi.scanDelete();
  std::sort(entries, entries + count, [](const Found& a, const Found& b) { return a.rssi > b.rssi; });
  std::vector<String> rows{"Back"};
  for (unsigned i = 0; i < count; ++i) {
    const String& name = entries[i].name;
    rows.push_back(name.length() > 20 ? name.substring(0, 20) + "\n" + name.substring(20) : name);
  }
  if (!count) { message("Scan SSIDs", "No visible SSIDs found"); return; }
  const int selected = choose("Scan SSIDs", rows); if (!selected) return;
  const auto selectedNetwork = entries[selected - 1];
  String secret = selectedNetwork.name == ssid ? password : "";
  if (!selectedNetwork.open && !edit("WiFi Password", secret, true, 64)) return;
  if (selectedNetwork.open) secret = "";
  const Network config = selectedNetwork.name == ssid ? network : Network{};
  connectNetwork(selectedNetwork.name, secret, config);
}

bool install(const Image& image) {
  cancelRequest = false; phase = "Downloading update"; progress("Installing", "Downloading firmware");
  HttpStream http(pump, cancelled);
  if (!http.get(image.url, 180000, image.bytes) ||
      (http.contentLength() >= 0 && http.contentLength() != image.bytes)) return false;
  std::unique_ptr<uint8_t, decltype(&free)> buffer(static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), free);
  ImageWriter writer;
  if (!buffer || !writer.begin(image)) return false;
  uint32_t written = 0, lastDraw = 0;
  while (written < image.bytes && session && !cancelRequest) {
    const size_t requested = std::min(size_t(4096), size_t(image.bytes - written));
    if (http.readBytes(reinterpret_cast<char*>(buffer.get()), requested) != requested || !writer.push(buffer.get(), requested)) return false;
    written += requested;
    if (millis() - lastDraw >= 300) { progress("Installing", "Writing firmware\nKeep power connected", written * 100u / image.bytes); lastDraw = millis(); }
    pump();
  }
  if (written != image.bytes || http.read() >= 0 || !http.complete() || !writer.finish(true)) return false;
  progress("Update complete", "Verified firmware\nRebooting...");
  disconnectNetwork(); delay(500); ESP.restart(); return true;
}
bool fetchImage(Image& image) {
  phase = "Checking release"; progress("WiFi Update", "Reading release info");
  {
    HttpStream http(pump, cancelled);
    if (!http.get(release.manifestUrl, 30000, 16384)) {
      serverStage = http.stage(); serverError = http.error(); serverCode = http.transportError(); serverStatus = http.statusCode(); return false;
    }
    Document doc(8192);
    if (deserializeJson(doc, http, DeserializationOption::NestingLimit(8)) || !http.finishJson() || !readManifest(doc.as<JsonVariantConst>(), release, image)) {
      serverStage = "Manifest"; serverError = "Invalid or incompatible manifest"; return false;
    }
  }
  return true;
}
void checkUpdate() {
  if (WiFi.status() != WL_CONNECTED) { message("WiFi Update", "Connect to WiFi first"); return; }
  if (!checkServer()) { message("WiFi Update", "Failed to connect to update server"); return; }
  Image image;
  if (!fetchImage(image)) { message("WiFi Update", "Invalid or unavailable\nrelease manifest"); return; }
  Version offered, current;
  if (!offered.parse(image.version.c_str()) || !current.parse(MARAUDER_VERSION)) { message("WiFi Update", "Invalid firmware version"); return; }
  if (offered.compare(current) <= 0) { phase = "Up to date"; message("WiFi Update", "Up to date\n" + String(MARAUDER_VERSION)); return; }
  phase = "Update available";
  if (choose("Update available", {"Back", "Install " + String(image.version.c_str())}) != 1) return;
  if (!install(image)) { phase = "Update failed"; message("Update failed", "Update not installed\nCurrent firmware kept"); }
}
}

bool active() { return session; }
bool uiActive() { return ui; }
void beginSession() {
  if (session) return;
  wifi_scan_obj.resetStandaloneWiFiState();
  WiFi.persistent(false); WiFi.setAutoReconnect(false); WiFi.mode(WIFI_OFF);
  session = true; cancelRequest = false; serverReady = false; phase = "Idle";
  ssid = ""; password = ""; network = {};
  bool forgotten = false;
  profileSaved = loadProfile(ssid, password, network, profileStorage, &forgotten);
  // Reuse the user's last successful general WiFi profile on first use. OTA
  // thereafter keeps its own last network/static settings without replacing
  // or evicting entries in the general five-profile list.
  if (!profileSaved && !forgotten && settings_obj.getSavedWifiCount())
    settings_obj.loadSavedWifiCredential(0, ssid, password);
}
void endSession() {
  cancelRequest = true; disconnectNetwork(); session = false;
  password = ""; ssid = ""; network = {}; phase = "Idle";
}
static void runWifiMenu(bool reconnectOnEntry) {
  if (ui) return;
  beginSession(); ui = true;
  if (reconnectOnEntry && ssid.length() && WiFi.status() != WL_CONNECTED) connectNetwork(ssid, password, network);
  while (session) {
    std::vector<String> rows{"Back", "Scan SSIDs", "Add SSID Manually", "Network Info"};
    if (WiFi.status() == WL_CONNECTED) rows.push_back("Check For Updates");
    else if (ssid.length()) rows.push_back("Reconnect Last WiFi");
    const int forgetIndex = rows.size(); rows.push_back("Forget Network");
    const int selected = choose("Update Over WiFi", rows, true);
    if (selected < 0) continue;
    if (!selected) break;
    if (selected == 1) scanNetworks();
    else if (selected == 2) manualNetwork();
    else if (selected == 3) networkInfo();
    else if (selected == forgetIndex) forgetNetwork();
    else if (WiFi.status() == WL_CONNECTED) checkUpdate();
    else connectNetwork(ssid, password, network);
  }
  ui = false; resetButtons();
}
void run() { runWifiMenu(true); }
void runMenuCheck() {
  if (session || ui) { Serial.println(F("Leave Update Firmware before menu checks")); return; }
  const char* original = profileStorage; profileStorage = "ota-menu-check";
  bool ok = writeProfile("ETERNAL-OTA-FIXTURE", "fixture-password", Network{}, profileStorage);
  if (ok) runWifiMenu(false); // No RF connection; real saved credentials stay in their own namespace.
  endSession(); ui = false;
  String name, secret; Network config; bool forgotten = false;
  ok = ok && !loadProfile(name, secret, config, profileStorage, &forgotten) && forgotten && WiFi.getMode() == WIFI_OFF;
  beginSession(); // An explicitly forgotten fixture must stay forgotten on re-entry.
  ok = ok && !ssid.length() && !password.length() && !profileSaved;
  endSession();
  Preferences prefs; if (prefs.begin(profileStorage, false)) { prefs.clear(); prefs.end(); }
  profileStorage = original; menu_function_obj.exitFirmwareUpdate();
  Serial.printf("SELFTEST ota menu %s (forget, cleared credentials, re-entry, WiFi off)\n", ok ? "PASS" : "FAIL");
}
void printStatus() {
  const esp_partition_t* running = esp_ota_get_running_partition(); esp_ota_img_states_t state;
  const int bootState = running && esp_ota_get_state_partition(running, &state) == ESP_OK ? int(state) : -1;
  Serial.printf("@WIFIOTA:{\"active\":%s,\"ui\":%s,\"connected\":%s,\"serverReady\":%s,\"profileSaved\":%s,\"wifiMode\":%u,\"autoReconnect\":%s,\"phase\":\"%s\",\"serverStage\":\"%s\",\"serverError\":\"%s\",\"serverCode\":%d,\"serverStatus\":%d,\"serverCheckMs\":%lu,\"runningOffset\":%lu,\"bootState\":%d,\"savedWiFiProfiles\":%u,\"heap\":%lu}\n",
    session ? "true" : "false", ui ? "true" : "false", WiFi.status() == WL_CONNECTED ? "true" : "false",
    serverReady ? "true" : "false", profileSaved ? "true" : "false", unsigned(WiFi.getMode()), WiFi.getAutoReconnect() ? "true" : "false", phase,
    serverStage, serverError, serverCode, serverStatus, static_cast<unsigned long>(lastCheckMs), static_cast<unsigned long>(running ? running->address : 0), bootState,
    unsigned(settings_obj.getSavedWifiCount()), static_cast<unsigned long>(ESP.getFreeHeap()));
}
void confirmBoot() {
  const esp_partition_t* running = esp_ota_get_running_partition(); esp_ota_img_states_t state;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) return;
  const bool healthy = ESP.getFreeHeap() >= 16384 && ESP.getPsramSize() > 0 &&
      display_obj.tft.width() == 128 && display_obj.tft.height() == 128 && MarauderFirmware::validMetadata(MarauderFirmware::currentMetadata());
  if (healthy && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) Serial.println(F("OTA: startup verified"));
  else { Serial.println(F("OTA: startup failed; rolling back")); esp_ota_mark_app_invalid_rollback_and_reboot(); }
}
void runProfileChecks(bool seed) {
  const char* storage = "ota-verify";
  const Network fixture{true, 0xc0a80a14, 0xffffff00, 0xc0a80a01, 0x01010101, 0};
  bool ok;
  if (seed) {
    ok = writeProfile("ETERNAL-OTA-FIXTURE", "fixture-password", fixture, storage);
    ok = ok && !writeProfile("", "fixture-password", fixture, storage);
  } else {
    String name, secret; Network loaded;
    ok = loadProfile(name, secret, loaded, storage) && name == "ETERNAL-OTA-FIXTURE" && secret == "fixture-password" &&
        loaded.manual && loaded.ip == fixture.ip && loaded.mask == fixture.mask && loaded.gateway == fixture.gateway &&
        loaded.dns1 == fixture.dns1 && !loaded.dns2;
    Preferences prefs; if (prefs.begin(storage, false)) { prefs.clear(); prefs.end(); }
  }
  Serial.printf("SELFTEST ota profile %s %s\n", seed ? "SEED" : "CHECK", ok ? "PASS" : "FAIL");
}
void runDownloadCheck() {
  if (session) { Serial.println(F("Leave Update Firmware before the download check")); return; }
  beginSession(); ui = true;
  bool ok = ssid.length() && connectNetwork(ssid, password, network);
  Image image;
  ok = ok && fetchImage(image);
  uint32_t bytes = 0;
  if (ok) {
    phase = "Verifying download"; progress("Verify download", "Reading release image\nNo firmware installed");
    HttpStream http(pump, cancelled);
    std::unique_ptr<uint8_t, decltype(&free)> buffer(static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), free);
    mbedtls_sha256_context hash; mbedtls_sha256_init(&hash);
    MarauderFirmware::MetadataScanner scanner;
    ok = buffer && http.get(image.url, 180000, image.bytes) &&
        (http.contentLength() < 0 || http.contentLength() == image.bytes) && mbedtls_sha256_starts(&hash, 0) == 0;
    while (ok && bytes < image.bytes) {
      const size_t size = std::min(size_t(4096), size_t(image.bytes - bytes));
      ok = http.readBytes(reinterpret_cast<char*>(buffer.get()), size) == size && mbedtls_sha256_update(&hash, buffer.get(), size) == 0;
      if (ok) { for (size_t i = 0; i < size; ++i) scanner.push(buffer.get()[i]); bytes += size; }
      pump();
    }
    uint8_t digest[32]{}; char hex[65]{};
    ok = ok && http.read() < 0 && http.complete() && mbedtls_sha256_finish(&hash, digest) == 0;
    for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    mbedtls_sha256_free(&hash);
    ok = ok && image.sha256 == hex && scanner.found() &&
        MarauderFirmware::metadataMatches(scanner.metadata(), MarauderFirmware::currentMetadata()) && "v" + image.version == scanner.metadata().version;
    if (!ok) { serverStage = http.stage(); serverError = http.error(); serverCode = http.transportError(); serverStatus = http.statusCode(); }
  }
  endSession(); ui = false; menu_function_obj.exitFirmwareUpdate();
  Serial.printf("SELFTEST ota download %s (%lu bytes; no flash writes)\n", ok ? "PASS" : "FAIL", static_cast<unsigned long>(bytes));
}
void runKeypadCheck() {
  if (session) return;
  session = ui = true;
  String text = "192.168.10.20";
  const bool accepted = edit("TEST IPv4 keypad", text, false, 15, true);
  uint32_t parsed;
  const bool ok = accepted && ipv4(text.c_str(), parsed) && parsed == 0xc0a80a14;
  endSession(); ui = false;
  menu_function_obj.exitFirmwareUpdate();
  Serial.printf("SELFTEST ota keypad %s\n", ok ? "PASS" : "FAIL");
}
void runImageChecks(bool reboot) {
  if (session) { Serial.println(F("Leave Update Firmware before isolated image checks")); return; }
  const esp_partition_t* source = esp_ota_get_running_partition();
  const esp_partition_t* originalBoot = esp_ota_get_boot_partition();
  esp_partition_pos_t position{}; esp_image_metadata_t metadata{};
  if (!source) { Serial.println(F("SELFTEST ota image FAIL: no running partition")); return; }
  position.offset = source->address; position.size = source->size;
  if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) != ESP_OK || metadata.image_len < 1024 || metadata.image_len > kSlotBytes) {
    Serial.println(F("SELFTEST ota image FAIL: source image")); return;
  }
  std::unique_ptr<uint8_t, decltype(&free)> buffer(static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), free);
  if (!buffer) { Serial.println(F("SELFTEST ota image FAIL: buffer")); return; }
  Image image; image.version = MARAUDER_VERSION + 1; image.bytes = metadata.image_len;
  mbedtls_sha256_context hash; mbedtls_sha256_init(&hash);
  bool ok = mbedtls_sha256_starts(&hash, 0) == 0;
  for (uint32_t offset = 0; ok && offset < image.bytes; offset += 4096) {
    const size_t size = std::min(size_t(4096), size_t(image.bytes - offset));
    ok = esp_partition_read(source, offset, buffer.get(), size) == ESP_OK && mbedtls_sha256_update(&hash, buffer.get(), size) == 0;
    delay(1);
  }
  uint8_t digest[32]{}; char hex[65]{};
  ok = ok && mbedtls_sha256_finish(&hash, digest) == 0; mbedtls_sha256_free(&hash);
  for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  image.sha256 = hex;
  if (!ok) { Serial.println(F("SELFTEST ota image FAIL: source digest")); return; }
  const auto copy = [&](const Image& expected, bool activate) {
    ImageWriter writer;
    if (!writer.begin(expected)) return false;
    for (uint32_t offset = 0; offset < image.bytes; offset += 4096) {
      const size_t size = std::min(size_t(4096), size_t(image.bytes - offset));
      if (esp_partition_read(source, offset, buffer.get(), size) != ESP_OK || !writer.push(buffer.get(), size)) return false;
      delay(1);
    }
    return writer.finish(activate);
  };
  // First prove a bad expected hash never changes boot selection. Source bytes
  // are copied solely from this running image; no unsigned external fixture.
  Image wrong = image; wrong.sha256[0] = wrong.sha256[0] == 'a' ? 'b' : 'a';
  ok = !copy(wrong, true) && esp_ota_get_boot_partition() == originalBoot;
  Serial.printf("CHECK %s: bad digest never activates inactive slot\n", ok ? "PASS" : "FAIL");
  ok = ok && copy(image, reboot);
  if (!reboot) ok = ok && esp_ota_get_boot_partition() == originalBoot;
  Serial.printf("SELFTEST ota image %s (%lu bytes, activate=%s)\n", ok ? "PASS" : "FAIL", static_cast<unsigned long>(image.bytes), reboot ? "true" : "false");
  if (ok && reboot) { Serial.flush(); delay(200); ESP.restart(); }
}
void runChecks() {
  bool ok = true; unsigned count = 0;
  const auto check = [&](bool condition, const char* label) { ++count; ok &= condition; Serial.printf("CHECK %s: %s\n", condition ? "PASS" : "FAIL", label); };
  Version a, b;
  check(a.parse("v1.16.2") && b.parse("1.16.10") && b.compare(a) > 0, "numeric update versions");
  check(!a.parse("1.16.2-rc1") && !a.parse("1.2"), "malformed/pre-release versions rejected");
  check(allowedUrl(kApi) && !allowedUrl("https://api.github.com/repos/other/repo/releases/latest"), "only configured repository API");
  check(allowedUrl(assetUrl("FirmwareUpdate1.16.2", "manifest.json")), "repository release asset path");
  check(!allowedUrl("http://github.com/file") && !allowedUrl("https://github.com.evil.test/file"), "TLS and hostname restrictions");
  check(!allowedUrl("https://release-assets.githubusercontent.com/github-production-release-asset/1/x", true), "other repository CDN assets rejected");
  check(remaining(UINT32_MAX - 10, 4, 15000) == 14985 && !remaining(0, 15000, 15000), "15 second deadline and clock rollover");
  Keyboard keyboard; keyboard.value = "test"; keyboard.actions = true; keyboard.selected = 3;
  check(keyboard.choose() == 1 && keyboard.value == "test", "keyboard Enter bottom right");
  keyboard.selected = 2; check(keyboard.choose() == -1, "keyboard cancel");
  keyboard.selected = 1; keyboard.choose(); check(keyboard.value == "tes", "keyboard delete");
  keyboard.selected = 0; keyboard.choose(); check(keyboard.page == 1 && !keyboard.actions, "keyboard case/number/symbol pages");
  uint32_t parsed;
  check(ipv4("192.168.10.20", parsed) && parsed == 0xc0a80a14 && !ipv4("256.1.1.1", parsed), "IPv4 number pad validation");
  check(validPassword("") && validPassword("fixture-password") && !validPassword("short"), "open networks and WPA passwords");
  check(!session && WiFi.getMode() == WIFI_OFF, "isolated checks leave updater and WiFi inactive");
  check(checkTransportFixtures(), "TLS PSRAM/restoration, HTTP framing and truncation rejection");
  Serial.printf("SELFTEST ota %s (%u checks)\n", ok ? "PASS" : "FAIL", count);
}
} }
