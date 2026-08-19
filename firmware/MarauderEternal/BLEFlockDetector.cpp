#include "BLEFlockDetector.h"

#include <cstring>
#include <strings.h>

namespace BLEFlockDetector {
namespace {

const uint8_t kFlockPrefixes[][3] = {
    {0x70, 0xc9, 0x4e}, {0x3c, 0x91, 0x80}, {0xd8, 0xf3, 0xbc},
    {0x80, 0x30, 0x49}, {0xb8, 0x35, 0x32}, {0x14, 0x5a, 0xfc},
    {0x74, 0x4c, 0xa1}, {0x08, 0x3a, 0x88}, {0x9c, 0x2f, 0x9d},
    {0xc0, 0x35, 0x32}, {0x94, 0x08, 0x53}, {0xe4, 0xaa, 0xea},
    {0xf4, 0x6a, 0xdd}, {0x24, 0xb2, 0xb9}, {0x00, 0xf4, 0x8d},
    {0xd0, 0x39, 0x57}, {0xe8, 0xd0, 0xfc}, {0xe0, 0x4f, 0x43},
    {0xb8, 0x1e, 0xa4}, {0x70, 0x08, 0x94}, {0x58, 0x8e, 0x81},
    {0xec, 0x1b, 0xbd}, {0x3c, 0x71, 0xbf}, {0x58, 0x00, 0xe3},
    {0x90, 0x35, 0xea}, {0x5c, 0x93, 0xa2}, {0x64, 0x6e, 0x69},
    {0x48, 0x27, 0xea}, {0xa4, 0xcf, 0x12}, {0x82, 0x6b, 0xf2},
    {0xb4, 0x1e, 0x52},
};

const uint8_t kSoundThinkingPrefixes[][3] = {{0xd4, 0x11, 0xd6}};

const char* const kRavenServiceUuids[] = {
    "0000180a-0000-1000-8000-00805f9b34fb",
    "00003100-0000-1000-8000-00805f9b34fb",
    "00003200-0000-1000-8000-00805f9b34fb",
    "00003300-0000-1000-8000-00805f9b34fb",
    "00003400-0000-1000-8000-00805f9b34fb",
    "00003500-0000-1000-8000-00805f9b34fb",
    "00001809-0000-1000-8000-00805f9b34fb",
    "00001819-0000-1000-8000-00805f9b34fb",
};

const uint16_t kRavenShortUuids[] = {
    0x180a, 0x3100, 0x3200, 0x3300, 0x3400, 0x3500, 0x1809, 0x1819,
};

template <size_t Count>
bool prefixMatches(const uint8_t mac[6], const uint8_t (&prefixes)[Count][3]) {
  if (mac == nullptr) return false;
  for (size_t index = 0; index < Count; index++) {
    if (memcmp(mac, prefixes[index], 3) == 0) return true;
  }
  return false;
}

void extractXuntongSerial(const uint8_t* data, size_t length, String& serial) {
  serial = "";
  bool started = false;
  for (size_t index = 0; index < length; index++) {
    const char value = static_cast<char>(data[index]);
    if (!started) {
      if (value == 'T' && index + 1 < length && data[index + 1] == 'N') {
        serial = "TN";
        started = true;
        index++;
      }
    } else if (value >= '0' && value <= '9') {
      serial += value;
    } else if (value != ' ' && value != '#' && value != '-') {
      break;
    }
  }
}

bool findXuntongManufacturerData(const uint8_t* payload, size_t length,
                                String& serial) {
  if (payload == nullptr) return false;

  // BLE advertising data is a sequence of length/type/value structures.
  for (size_t offset = 0; offset < length;) {
    const size_t fieldLength = payload[offset];
    if (fieldLength == 0) {
      offset++;
      continue;
    }
    if (offset + fieldLength >= length) break;

    const uint8_t type = payload[offset + 1];
    if (type == 0xff && fieldLength >= 3 &&
        payload[offset + 2] == 0xc8 && payload[offset + 3] == 0x09) {
      extractXuntongSerial(payload + offset + 4, fieldLength - 3, serial);
      return true;
    }
    offset += fieldLength + 1;
  }
  return false;
}

bool allDigits(const String& value, size_t start = 0) {
  if (value.length() <= start) return false;
  for (size_t index = start; index < value.length(); index++) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  return true;
}

bool specificNameMatches(const String& name) {
  String lowerName = name;
  lowerName.toLowerCase();
  if (lowerName == "fs ext battery") return true;
  if (lowerName.length() == 18 && lowerName.startsWith("penguin-") &&
      allDigits(lowerName, 8)) return true;
  return lowerName.startsWith("pigvision") || lowerName == "flockcam";
}

}  // namespace

Match classify(const uint8_t mac[6], const String& name,
               const uint8_t* payload, size_t payloadLength) {
  Match match;

  if (prefixMatches(mac, kSoundThinkingPrefixes)) {
    match.detected = true;
    match.isSoundThinking = true;
    match.method = "soundthinking_oui";
  } else if (findXuntongManufacturerData(payload, payloadLength,
                                         match.serial)) {
    match.detected = true;
    match.method = "ble_mfr_0x09c8";
  } else if (specificNameMatches(name)) {
    match.detected = true;
    match.method = "flock_device_name";
  } else if (name.length() == 10 && allDigits(name) &&
             prefixMatches(mac, kFlockPrefixes)) {
    match.detected = true;
    match.highConfidence = false;
    match.method = "numeric_name_plus_oui";
  } else if (mac && mac[0] == 0xb4 && mac[1] == 0x1e && mac[2] == 0x52) {
    match.detected = true;
    match.method = "flock_registered_oui";
  }

  return match;
}

void considerServiceUuid(Match& match, const char* uuid) {
  if (uuid == nullptr || uuid[0] == '\0') return;

  int ravenIndex = -1;
  for (size_t index = 0;
       index < sizeof(kRavenServiceUuids) / sizeof(kRavenServiceUuids[0]);
       index++) {
    char compactUuid[7];
    snprintf(compactUuid, sizeof(compactUuid), "0x%04x",
             kRavenShortUuids[index]);
    if (strcasecmp(uuid, kRavenServiceUuids[index]) == 0 ||
        strcasecmp(uuid, compactUuid) == 0 ||
        strcasecmp(uuid, compactUuid + 2) == 0) {
      ravenIndex = static_cast<int>(index);
      break;
    }
  }
  if (ravenIndex < 0) return;

  const uint16_t shortUuid = kRavenShortUuids[ravenIndex];
  if (shortUuid == 0x3100)
    match.ravenHasNewGps = true;
  else if (shortUuid == 0x1809)
    match.ravenHasOldHealth = true;
  else if (shortUuid == 0x1819)
    match.ravenHasOldLocation = true;
  else if (shortUuid == 0x3200)
    match.ravenHasPower = true;
  else if (shortUuid == 0x3300)
    match.ravenHasNetwork = true;
  else if (shortUuid == 0x3400)
    match.ravenHasUpload = true;
  else if (shortUuid == 0x3500)
    match.ravenHasError = true;

  // 0x180A, 0x1809, and 0x1819 are standard Bluetooth SIG services used by
  // many unrelated consumer and medical devices. They may support firmware
  // estimation, but never identify Raven equipment by themselves. Only the
  // proprietary Raven range 0x3100-0x3500 can trigger this evidence class.
  const bool proprietaryRavenUuid = shortUuid >= 0x3100 && shortUuid <= 0x3500;
  if (proprietaryRavenUuid) {
    match.detected = true;
    match.isRaven = true;
    match.matchedRavenUuid = shortUuid;
    match.method = "raven_uuid";
  }
}

const char* ravenFirmware(const Match& match) {
  if (!match.isRaven) return "";
  if (match.ravenHasUpload || match.ravenHasError) return "1.3.x";
  if (match.ravenHasNewGps && match.ravenHasPower &&
      match.ravenHasNetwork) return "1.2.x";
  if (match.ravenHasOldHealth || match.ravenHasOldLocation) return "1.1.x";
  return "?";
}

const char* classLabel(const Match& match) {
  if (match.isRaven) return "Raven";
  if (match.isSoundThinking) return "ShotSpotter";
  return "Flock ALPR";
}

}  // namespace BLEFlockDetector
