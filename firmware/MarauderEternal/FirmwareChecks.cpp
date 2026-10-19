#include "FirmwareChecks.h"
#include "settings.h"
#include "GpsTrackerStats.h"
#include "FoxHunt.h"
#include "TrackerDiagnostics.h"
#include "WiFiScan.h"
#include "ScreenMirror.h"
#ifdef HAS_SCREEN
#include "MenuFunctions.h"
#endif

namespace {
constexpr const char* kWifiFixture = "/verify-wifi.json";
bool check(bool passed, const char* label) {
  Serial.printf("CHECK %s: %s\n", passed ? "PASS" : "FAIL", label);
  return passed;
}
void cleanWiFiFixture() {
  SPIFFS.remove(kWifiFixture);
  SPIFFS.remove(String(kWifiFixture) + ".tmp");
  SPIFFS.remove(String(kWifiFixture) + ".backup");
  SPIFFS.remove(String(kWifiFixture) + ".corrupt");
}
}

void runWiFiChecks(const String& phase) {
  bool ok = true;
  if (phase != "check") {
    cleanWiFiFixture();
    DynamicJsonDocument legacy(1024);
    legacy["Settings"][0]["name"] = "ClientSSID";
    legacy["Settings"][0]["value"] = "ETERNAL-VERIFY-A";
    legacy["Settings"][1]["name"] = "ClientPW";
    legacy["Settings"][1]["value"] = "fixture-password";
    legacy["Settings"][2]["name"] = "KeepCustom";
    legacy["Settings"][2]["value"] = "unchanged";
    File f = SPIFFS.open(kWifiFixture, FILE_WRITE);
    ok &= check(f && serializeJson(legacy, f) > 0 && !f.getWriteError(), "legacy fixture written");
    f.close();
    Settings fixture(kWifiFixture);
    ok &= check(fixture.begin() && fixture.getSavedWifiCount() == 1, "legacy migration");
    ok &= check(fixture.loadSetting<String>("KeepCustom") == "unchanged", "custom settings retained");
    for (unsigned i = 0; i < 4; ++i)
      ok &= check(fixture.saveWifiCredential("ETERNAL-VERIFY-" + String(i), i == 0 ? "" : "fixture-password") == saved_wifi::Saved, "append profile");
    ok &= check(fixture.saveWifiCredential("ETERNAL-VERIFY-FULL", "fixture-password") == saved_wifi::Full && fixture.getSavedWifiCount() == 5, "full list never evicts");
    ok &= check(fixture.saveWifiCredential("ETERNAL-VERIFY-A", "updated-password") == saved_wifi::Updated && fixture.getSavedWifiCount() == 5, "duplicate update");
    ok &= check(fixture.saveWifiCredential("ETERNAL-VERIFY-REPLACE", "fixture-password", 4) == saved_wifi::Saved, "explicit replacement");
    ok &= check(fixture.saveWifiCredential("invalid", "short") == saved_wifi::Error, "invalid credentials rejected");
    ok &= check(fixture.markSavedWifiSuccessful(2), "successful profile promoted");
    if (phase == "seed") {
      Serial.println(ok ? "SELFTEST wifi SEED PASS" : "SELFTEST wifi FAIL");
      return;
    }
  }
  Settings reopened(kWifiFixture);
  ok &= check(reopened.begin() && reopened.getSavedWifiCount() == 5, "profiles persist after reopen/reboot");
  String ssid, password;
  ok &= check(reopened.loadSavedWifiCredential(0, ssid, password) &&
              ssid == reopened.loadSetting<String>("ClientSSID") &&
              password == reopened.loadSetting<String>("ClientPW"), "legacy cache matches first profile");
  ok &= check(!reopened.removeSavedWifiCredential(5), "invalid delete rejected");
  for (unsigned i = 0; i < 5; ++i)
    ok &= check(reopened.removeSavedWifiCredential(0), "delete profile");
  ok &= check(reopened.getSavedWifiCount() == 0 && reopened.loadSetting<String>("ClientSSID") == "" && reopened.loadSetting<String>("ClientPW") == "", "last delete clears legacy credentials");
  cleanWiFiFixture();
  Serial.println(ok ? "SELFTEST wifi PASS" : "SELFTEST wifi FAIL");
}

void runGeofenceChecks(const String& phase) {
  constexpr const char* path = "/verify-geofence.json";
  bool ok = true;
  geofences::Config fences[geofences::kMax];
  if (phase != "check") {
    SPIFFS.remove(path);
    Settings fixture(path);
    ok &= check(fixture.begin(), "geofence fixture created");
    for (unsigned i = 0; i < saved_wifi::kMaxProfiles; ++i) {
      char ssid[33], password[65];
      memset(ssid, 'A' + i, 32); ssid[32] = 0;
      memset(password, 'a', 64); password[64] = 0;
      ok &= check(fixture.saveWifiCredential(ssid, password) == saved_wifi::Saved, "maximum-size WiFi regression fixture");
    }
    for (unsigned i = 0; i < geofences::kMax; ++i) {
      auto& fence = fences[i]; fence.enabled = true;
      memset(fence.name, 'F' + i, 32); fence.name[32] = 0;
      fence.latitude = i * 10;
      ok &= check(fixture.saveGeofence(i, fence), "persist geofence slot");
    }
    auto invalid = fences[0]; invalid.radiusMiles = NAN;
    ok &= check(!fixture.saveGeofence(0, invalid) && !fixture.saveGeofence(5, fences[0]), "invalid radius and slot rejected");
    if (phase == "seed") {
      Serial.println(ok ? "SELFTEST geofence SEED PASS" : "SELFTEST geofence FAIL"); return;
    }
  }
  Settings reopened(path);
  ok &= check(reopened.begin(), "geofence fixture reopened");
  for (unsigned i = 0; i < geofences::kMax; ++i)
    ok &= check(reopened.loadGeofence(i, fences[i]) && fences[i].latitude == i * 10, "slot persists across reopen/reboot");
  ok &= check(reopened.getSavedWifiCount() == 5, "all saved WiFi profiles retained");
  ok &= check(reopened.loadGeofences(fences), "bulk geofence cache loaded");
  geofences::Gate gate;
  ok &= check(gate.update(fences, false, 0, 0) == geofences::State::WaitingForFix && gate.paused(), "no fix pauses collection");
  ok &= check(gate.update(fences, true, 0, 0) == geofences::State::Inside && gate.active == 0, "inside pauses collection");
  constexpr double degPerMile = 180.0 / (3.14159265358979323846 * 3958.7613);
  ok &= check(gate.update(fences, true, 0, .103 * degPerMile) == geofences::State::Inside, "boundary margin avoids flapping");
  ok &= check(gate.update(fences, true, 0, .12 * degPerMile) == geofences::State::Clear && !gate.paused(), "exit resumes collection");
  ok &= check(gate.update(fences, false, 0, 2) == geofences::State::WaitingForFix, "lost fix pauses collection");
  ok &= check(gate.update(fences, true, 0, 2) == geofences::State::Clear, "fresh outside fix resumes");
  ok &= check(reopened.clearGeofence(2) && !reopened.loadGeofence(2, fences[2]) && reopened.loadGeofence(4, fences[4]), "clear preserves other slots");
  SPIFFS.remove(path); SPIFFS.remove(String(path) + ".tmp"); SPIFFS.remove(String(path) + ".backup");
  Serial.println(ok ? "SELFTEST geofence PASS" : "SELFTEST geofence FAIL");
}

void runGpsTrackerChecks() {
  bool ok = true;
  marauder::GpsTrackerStats stats; stats.reset(0);
  ok &= check(stats.update(40000000, -74000000, 1, 1000), "GPS first trusted sample");
  for (unsigned i = 1; i <= 12; ++i)
    ok &= stats.update(40000000 + i * 10, -74000000, 1, 1000 + i * 1000);
  ok &= check(stats.distanceMeters() > 12 && stats.distanceMeters() < 14, "slow walking accumulates beyond jitter floor");
  const double distance = stats.distanceMeters();
  ok &= check(!stats.update(41000000, -74000000, 1, 14000), "unrealistic GPS jump rejected");
  ok &= check(stats.update(40000120, -74000000, 1, 15000) && stats.distanceMeters() == distance, "outlier never poisons reference");
  stats.loseFix();
  ok &= check(stats.speedMetersPerSecond() == 0 && !stats.currentTrusted(), "fix loss clears current speed");
  ok &= check(stats.update(41000000, -74000000, 1, 16000) && stats.startsSegment() && stats.distanceMeters() == distance, "reacquisition starts a segment without trip jump");
  ok &= check(!stats.update(41000000, -74000000, 1, 16000), "same receiver sample not counted twice");
  stats.queuedPoint();
  ok &= check(stats.queuedPoints() == 1 && stats.trustedSamples() > 1, "queued points distinct from trusted samples");
  stats.stop(17000);
  ok &= check(stats.elapsedMs(50000) == 17000 && !stats.update(41000001, -74000000, 1, 18000), "stopped session freezes elapsed time");
  marauder::clock::UtcDateTime utc;
  ok &= check(marauder::parseGpsUtc("2026-10-19 12:34:56", utc) && !marauder::parseGpsUtc("2026-02-30 12:34:56", utc), "GPS UTC validation");
  marauder::parseGpsUtc("2026-10-19 12:34:56", utc);
  std::string xml = "<?xml version=\"1.0\"?><gpx version=\"1.1\" creator=\"Marauder GPS check\" xmlns=\"http://www.topografix.com/GPX/1/1\"><trk><name>Fixture</name><trkseg>";
  xml += marauder::gpsPoint(40000000,-74000000,125.5f,&utc);
  xml += marauder::gpsPoint(40000120,-74000000,126,nullptr);
  xml += marauder::gpsPoint(41000000,-74000000,NAN,&utc,false,true);
  xml += "</trkseg></trk></gpx>\n";
  File file = SPIFFS.open("/gpscheck.gpx", FILE_WRITE);
  ok &= check(file && file.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()) == xml.size(), "GPS fixture file written");
  file.flush(); file.close(); file = SPIFFS.open("/gpscheck.gpx", FILE_READ);
  ok &= check(file && file.size() == xml.size(), "GPS fixture readback size");
  Serial.println(F("GPS-CHECK-GPX-BEGIN"));
  uint8_t bytes[128];
  while (file && file.available()) { const size_t n = file.read(bytes, sizeof(bytes)); if (!n) break; Serial.write(bytes, n); }
  file.close(); Serial.println(F("GPS-CHECK-GPX-END")); SPIFFS.remove("/gpscheck.gpx");
  Serial.println(ok ? F("SELFTEST gpstracker PASS") : F("SELFTEST gpstracker FAIL"));
}

void runFoxChecks() {
  using namespace marauder::fox;
  bool ok = true;
  Candidate item; item.channel = 36; item.lastSeen = 1000;
  Options options; options.band = Band::GHz5; options.recent = true;
  ok &= check(matches(item, options, 31000) && !matches(item, options, 31001), "combined band and 30-second boundary");
  item.channel = 6;
  ok &= check(!matches(item, options, 2000), "other band excluded");
  item.lastSeen = 0; options.band = Band::Any;
  ok &= check(!matches(item, options, 2000), "imported unknown age excluded from recent");
  item.lastSeen = UINT32_MAX - 99;
  ok &= check(matches(item, options, 100), "recent age survives millis wrap");
  Candidate sorted[3]; unsigned count = 0;
  for (unsigned i = 0; i < 10; ++i) { item.source = i; item.rssi = -100 + i; insertTop(sorted, count, 3, item, Sort::Signal); }
  ok &= check(count == 3 && sorted[0].source == 9 && sorted[2].source == 7, "bounded strongest-target ordering");
  Target target; item = Candidate(); item.mac[0] = 2; item.channel = 6;
  target.select(item, 0); uint8_t other[6] = {4};
  ok &= check(!target.fresh(100) && !target.observeWifi(other,-40,6,100), "unrelated packets cannot light target");
  ok &= check(target.observeWifi(item.mac,-40,6,1000) && target.fresh(2500) && !target.fresh(2501), "RF expires after reception stops");
  uint8_t ad[27] = {26,0xff,0x4c,0,2,0x15}; ad[22] = 1; ad[24] = 2; ad[26] = 0xc5;
  item.bluetooth = true; item.kind = Kind::IBeacon; item.channel = 0;
  ok &= check(marauder::parseIBeacon(ad,sizeof(ad),item.beacon), "iBeacon target fixture decoded");
  target.select(item,0);
  ok &= check(target.observeBle(other,ad,sizeof(ad),-40,100) && !memcmp(target.value.mac,other,6), "iBeacon identity follows rotating address");
  ++ad[24];
  ok &= check(!target.observeBle(other,ad,sizeof(ad),-40,200) && target.hits == 1 && target.value.channel == 0, "different beacon cannot update target");
  Serial.println(ok ? F("SELFTEST fox PASS") : F("SELFTEST fox FAIL"));
}

void runTrackerChecks() {
  using namespace marauder::tracker;
  bool ok = true; unsigned writes = 0, cleanups = 0;
  ScanTimer timer;
  ok &= check(!timer.expired(60000), "FindMy timer inactive before scan");
  timer.start(100);
  ok &= check(!timer.expired(60099) && timer.expired(60100), "FindMy timer stops at 60 seconds, never early");
  timer.cancel();
  ok &= check(!timer.expired(60101), "FindMy timer cancelled on early stop");
  timer.start(200000);
  ok &= check(!timer.expired(259999) && timer.expired(260000), "FindMy timer resets for each scan");
  timer.start(UINT32_MAX - 29999);
  ok &= check(!timer.expired(29999) && timer.expired(30000), "FindMy timer survives millis rollover");
  const auto send = [&](int) { ++writes; return true; };
  const auto cleanup = [&]() { ++cleanups; return true; };
  for (int profile=0;profile<3;++profile) {
    Report r;
    ok &= check(run(r,true,[&]() { r.linkEstablished=true; r.services=3; return profile; },send,cleanup) &&
        r.result==Result::Ready && r.profile==profile && writes==0, "connection-only path never sends sound for any tracker profile");
  }
  Report r;
  ok &= check(!run(r,true,[&]() { r.stage=Stage::Connect; r.result=Result::ConnectFailed; r.error=13; r.connectMs=15000; return -1; },send,cleanup) &&
      r.stage==Stage::Connect && r.result==Result::ConnectFailed && r.error==13 && r.connectMs==15000 && writes==0,
      "connection timeout stage and raw error retained after cleanup");
  ok &= check(!run(r,true,[&]() { r.stage=Stage::Services; r.result=Result::NoServices; r.error=0; return -1; },send,cleanup) &&
      r.result==Result::NoServices && r.stage==Stage::Services, "empty GATT response distinct from link failure");
  ok &= check(run(r,false,[](){return 0;},send,cleanup) && writes==1 && r.result==Result::SoundSent, "explicit sound path still dispatches write");
  ok &= check(run(r,true,[](){return 0;},send,[](){return false;}) && r.cleanupPending && writes==1, "pending cleanup shown independently of test outcome");
  ok &= check(cleanups==6 && !strcmp(errorName(13),"Operation timeout") && !strcmp(errorName(0x23e),"Link not established"), "cleanup count and NimBLE error labels");
  #ifdef HAS_NIMBLE_2
    extern WiFiScan wifi_scan_obj;
    extern LinkedList<AirTag>* airtags;
    LinkedList<AirTag> emptyFixture;
    auto* original=airtags; airtags=&emptyFixture;
    const bool attempted=wifi_scan_obj.executeFindMySound(false,true);
    airtags=original;
    ok &= check(!attempted && wifi_scan_obj.trackerReport().result==Result::NoTarget, "real entry point handles empty isolated target list without RF commands");
    #ifdef HAS_SCREEN
      extern MenuFunctions menu_function_obj;
      ok &= menu_function_obj.verifyFindMyMenus();
    #endif
  #endif
  Serial.println(ok ? F("SELFTEST findmy PASS") : F("SELFTEST findmy FAIL"));
}

void runMirrorChecks() {
  #ifdef HAS_SCREEN
    extern Display display_obj;
    extern MenuFunctions menu_function_obj;
    bool ok = check(marauder::mirror::verifyCapture(display_obj.tft), "mirror pixel, line, fill, sprite, image byte order and rotations");
    Serial.println(F("MIRROR BLOCKING BEGIN"));
    delay(6500); // Deliberately longer than the session lease; USB runs independently.
    Serial.println(F("MIRROR BLOCKING END"));
    display_obj.init(); menu_function_obj.changeMenu(menu_function_obj.current_menu, true);
    Serial.println(ok ? F("SELFTEST mirror PASS") : F("SELFTEST mirror FAIL"));
  #endif
}
