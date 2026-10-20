#include "OtaPolicy.h"
#include "UpdateKeyboard.h"
#include <cassert>
#include <cstdio>
#include <set>

using namespace marauder::ota;
int main() {
  Version old, next;
  assert(old.parse("v1.9.99") && next.parse("1.16.2") && next.compare(old) > 0);
  assert(old.parse("1.16.2") && next.parse("1.16.10") && next.compare(old) > 0);
  for (const char* invalid : {"", "1.2", "1.2.3.4", "1.2.3beta", "1.-2.3", "1.2.3-rc1", "1000000.2.3", "v"}) assert(!next.parse(invalid));
  assert(remaining(UINT32_MAX - 10, 4, 15000) == 14985);
  assert(remaining(100, 15099, 15000) == 1 && remaining(100, 15100, 15000) == 0);

  assert(allowedUrl(kApi));
  assert(allowedUrl(assetUrl("FirmwareUpdate1.16.2", "manifest.json")));
  for (const char* url : {
       "http://api.github.com/repos/n0vajay05/ESP32MarauderEternal-MiniV3/releases/latest",
       "https://api.github.com/repos/attacker/ESP32MarauderEternal-MiniV3/releases/latest",
       "https://github.com/n0vajay05/another/releases/download/v1/manifest.json",
       "https://github.com.evil.test/n0vajay05/ESP32MarauderEternal-MiniV3/releases/download/v1/manifest.json",
       "https://github.com@evil.test/n0vajay05/ESP32MarauderEternal-MiniV3/releases/download/v1/manifest.json",
       "https://github.com:443/n0vajay05/ESP32MarauderEternal-MiniV3/releases/download/v1/manifest.json",
       "https://github.com/n0vajay05/ESP32MarauderEternal-MiniV3/releases/download/../manifest.json",
       "https://github.com/n0vajay05/ESP32MarauderEternal-MiniV3/releases/download/v1/%2e%2e/file",
       "https://github.com/n0vajay05/ESP32MarauderEternal-MiniV3/releases/download/v1/file\r\nHost: evil",
       "https://raw.githubusercontent.com/n0vajay05/ESP32MarauderEternal-MiniV3/main/manifest.json",
       "https://release-assets.githubusercontent.com/github-production-release-asset/13399689480/a",
       "https://release-assets.githubusercontent.com/github-production-release-asset/1339968948/../evil",
       "https://release-assets.githubusercontent.com/github-production-release-asset/1339968948/a#fragment"}) assert(!allowedUrl(url, true));
  const std::string cdn = "https://release-assets.githubusercontent.com/github-production-release-asset/1339968948/abcd-1234?sig=abc%3D&download=1";
  assert(allowedUrl(cdn, true) && !allowedUrl(cdn));

  uint32_t ip;
  assert(ipv4("192.168.10.20", ip) && ip == 0xc0a80a14);
  for (const char* invalid : {"", "1.2.3", "1.2.3.4.5", "256.0.0.1", "1.2.3.-1", "1.2.3.4x", " 1.2.3.4", "1..3.4"}) assert(!ipv4(invalid, ip));
  Network network{true, 0xc0a80114, 0xffffff00, 0xc0a80101, 0x01010101, 0};
  assert(network.valid());
  for (uint32_t mask : {0u, 0xff00ff00u, 0xffffffffu, 0xfffffffeu}) { auto changed = network; changed.mask = mask; assert(!changed.valid()); }
  for (uint32_t gateway : {0u, 0xc0a80201u, 0xc0a80114u, 0xc0a80100u, 0xc0a801ffu}) { auto changed = network; changed.gateway = gateway; assert(!changed.valid()); }
  assert(validPassword("") && validPassword("password") && validPassword(std::string(63, 'x')) && validPassword(std::string(64, 'a')));
  assert(!validPassword("short") && !validPassword(std::string(64, 'g')) && !validPassword(std::string(65, 'a')));

  DynamicJsonDocument releaseJson(8192);
  releaseJson["tag_name"] = "FirmwareUpdate1.16.2"; releaseJson["draft"] = false; releaseJson["prerelease"] = false;
  auto assets = releaseJson.createNestedArray("assets");
  auto manifestAsset = assets.createNestedObject(); manifestAsset["name"] = "manifest.json";
  manifestAsset["browser_download_url"] = assetUrl("FirmwareUpdate1.16.2", "manifest.json");
  auto app = assets.createNestedObject();
  const std::string file = "Marauder_Eternal_1.16.2_MiniV3_ESP32-C5.app.bin";
  app["name"] = file; app["size"] = 2639280; app["browser_download_url"] = assetUrl("FirmwareUpdate1.16.2", file);
  app["digest"] = "sha256:" + std::string(64, 'a');
  Release release;
  assert(readRelease(releaseJson.as<JsonVariantConst>(), release));
  releaseJson["draft"] = true; assert(!readRelease(releaseJson.as<JsonVariantConst>(), release)); releaseJson["draft"] = false;
  app["browser_download_url"] = "https://evil.test/firmware.bin"; assert(!readRelease(releaseJson.as<JsonVariantConst>(), release));
  app["browser_download_url"] = assetUrl("FirmwareUpdate1.16.2", file);
  assert(readRelease(releaseJson.as<JsonVariantConst>(), release));

  DynamicJsonDocument manifest(4096);
  manifest["product"] = "ESP32 Marauder Eternal"; manifest["hardware"] = "Marauder Mini V3"; manifest["chip"] = "ESP32-C5";
  manifest["flash_size"] = "8MB"; manifest["psram"] = true; manifest["partition_layout"] = "mini-v3-c5-8m-ota-v1"; manifest["version"] = "1.16.2";
  auto image = manifest["images"].createNestedObject(); image["file"] = file; image["bytes"] = 2639280; image["sha256"] = std::string(64, 'a');
  Image candidate;
  assert(readManifest(manifest.as<JsonVariantConst>(), release, candidate));
  manifest["chip"] = "ESP32-S3"; assert(!readManifest(manifest.as<JsonVariantConst>(), release, candidate)); manifest["chip"] = "ESP32-C5";
  image["bytes"] = kSlotBytes + 1; assert(!readManifest(manifest.as<JsonVariantConst>(), release, candidate)); image["bytes"] = 2639280;
  image["sha256"] = std::string(64, 'b'); assert(!readManifest(manifest.as<JsonVariantConst>(), release, candidate)); image["sha256"] = std::string(64, 'a');
  manifest["version"] = "1.16.3"; assert(!readManifest(manifest.as<JsonVariantConst>(), release, candidate)); manifest["version"] = "1.16.2";

  Keyboard keyboard;
  std::set<char> available;
  for (unsigned page = 0; page < 4; ++page) { keyboard.page = page; for (const char* c = keyboard.keys(); *c; ++c) available.insert(*c); }
  for (char c = 32; c < 127; ++c) assert(available.count(c));
  keyboard.page = 0; keyboard.maximum = 2;
  keyboard.choose(); keyboard.choose(); keyboard.choose(); assert(keyboard.value == "aa");
  keyboard.move(0, -1); assert(keyboard.actions && keyboard.selected == 0); keyboard.choose(); assert(keyboard.page == 1 && !keyboard.actions);
  keyboard.actions = true; keyboard.selected = 1; keyboard.choose(); assert(keyboard.value == "a");
  keyboard.selected = 2; assert(keyboard.choose() == -1); keyboard.selected = 3; assert(keyboard.choose() == 1);
  Keyboard pad(true); assert(pad.columns() == 3 && pad.actionCount() == 3);
  for (int i = 0; i < 200; ++i) { pad.move(i & 1 ? -1 : 1, i & 2 ? -1 : 1); assert(pad.selected < (pad.actions ? pad.actionCount() : strlen(pad.keys()))); }
  std::puts("OTA policy: numeric versions, deadlines, repository/redirect restrictions, release integrity, static IPv4 and full joystick keyboard passed");
}
