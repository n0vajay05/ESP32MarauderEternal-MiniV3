#pragma once
#include <cstdint>

namespace marauder::tracker {
enum class Stage : uint8_t { None, Target, Client, Connect, Services, Characteristics, Ready, Sound };
enum class Result : uint8_t { None, NoTarget, InvalidTarget, ClientFailed, ConnectFailed, Disconnected,
  NoServices, NoSoundService, NoCharacteristic, NotWritable, Ready, SoundSent, SoundFailed };
struct Report {
  Stage stage = Stage::None;
  Result result = Result::None;
  char address[18] = {};
  uint8_t addressType = 0;
  int error = 0, profile = -1, rssi = -128;
  uint32_t scanAge = UINT32_MAX, connectMs = 0, elapsedMs = 0;
  unsigned services = 0, characteristics = 0;
  bool testOnly = true, linkEstablished = false, advertisementKnown = false;
  bool advertisedConnectable = false, cleanupPending = false;
};
inline const char* stageName(Stage s) {
  switch (s) {
    case Stage::None: return "Not run";
    case Stage::Target: return "Target";
    case Stage::Client: return "Client init";
    case Stage::Connect: return "BLE connection";
    case Stage::Services: return "GATT services";
    case Stage::Characteristics: return "Sound characteristic";
    case Stage::Ready: return "Sound service ready";
    case Stage::Sound: return "Sound command";
  }
  return "Unknown";
}
inline const char* resultName(Result r) {
  switch (r) {
    case Result::None: return "Not run";
    case Result::NoTarget: return "No target selected";
    case Result::InvalidTarget: return "Invalid address";
    case Result::ClientFailed: return "Client init failed";
    case Result::ConnectFailed: return "Connection failed";
    case Result::Disconnected: return "Link dropped";
    case Result::NoServices: return "No GATT response";
    case Result::NoSoundService: return "No sound service";
    case Result::NoCharacteristic: return "Char unavailable";
    case Result::NotWritable: return "Char not writable";
    case Result::Ready: return "Connection test OK";
    case Result::SoundSent: return "Sound write accepted";
    case Result::SoundFailed: return "Sound command failed";
  }
  return "Unknown";
}
// NimBLE host/ATT/HCI error codes from the pinned 2.3.8 library. Keep the raw
// value too: a timeout does not prove owner proximity or an address rotation.
inline const char* errorName(int e) {
  switch (e) {
    case 0: return "No stack detail";
    case 2: return "Already in progress";
    case 3: return "Invalid argument";
    case 6: return "Out of memory";
    case 7: return "Not connected";
    case 13: return "Operation timeout";
    case 15: return "Stack busy";
    case 19: return "Controller timeout";
    case 21: return "No identity address";
    case 22: return "Host not synced";
    case 23: case 0x105: case 0x205: return "Authentication needed";
    case 24: case 0x108: return "Authorization needed";
    case 25: case 0x10f: return "Encryption needed";
    case 0x208: return "Link timeout";
    case 0x213: return "Peer disconnected";
    case 0x216: return "Local disconnect";
    case 0x23e: return "Link not established";
    default: return "See raw error code";
  }
}
inline const char* profileName(int profile) {
  return profile == 0 ? "AirTag" : profile == 1 ? "FMNA" : profile == 2 ? "DULT" : "Unconfirmed";
}
template <typename Inspect, typename Send, typename Cleanup>
bool run(Report& report, bool testOnly, Inspect inspect, Send send, Cleanup cleanup) {
  report.testOnly = testOnly;
  const int profile = inspect();
  bool ok = false;
  if (profile >= 0) {
    report.profile = profile;
    report.stage = testOnly ? Stage::Ready : Stage::Sound;
    // The connection-test branch never invokes a sound write or subscription.
    ok = testOnly || send(profile);
    report.result = testOnly ? Result::Ready : ok ? Result::SoundSent : Result::SoundFailed;
  }
  report.cleanupPending = !cleanup();
  return ok;
}
} // namespace marauder::tracker
