#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace marauder {
struct IBeaconPayload {
  uint8_t uuid[16] = {};
  uint16_t major = 0, minor = 0;
  int8_t measured_power = 0;
};
struct IBeaconObservation {
  IBeaconPayload beacon;
  char address[18] = {};
  int8_t rssi = 0;
  uint32_t first_seen = 0, last_seen = 0, advertisements = 0;
};
inline bool sameIBeacon(const IBeaconPayload& a, const IBeaconPayload& b) {
  return a.major == b.major && a.minor == b.minor && !memcmp(a.uuid, b.uuid, 16);
}
inline int retainIBeacon(IBeaconObservation* records, unsigned& count, unsigned capacity,
                         const IBeaconPayload& beacon, const char* address,
                         int8_t rssi, uint32_t now) {
  if (!records || !capacity || count > capacity || !address || strlen(address) != 17) return -1;
  unsigned slot = count;
  for (unsigned i = 0; i < count; ++i)
    if (sameIBeacon(records[i].beacon, beacon)) { slot = i; break; }
  if (slot == count) {
    if (count >= capacity) return -1; // Keep retained identities stable until the next scan.
    ++count;
    records[slot] = IBeaconObservation(); records[slot].first_seen = now;
  }
  auto& record = records[slot];
  record.beacon = beacon; strcpy(record.address, address); record.rssi = rssi;
  record.last_seen = now;
  if (record.advertisements != UINT32_MAX) ++record.advertisements;
  return static_cast<int>(slot);
}
inline bool parseIBeaconManufacturer(const uint8_t* data, size_t length, IBeaconPayload& out) {
  if (!data || length < 25 || data[0] != 0x4c || data[1] != 0 ||
      data[2] != 2 || data[3] != 0x15) return false;
  IBeaconPayload parsed;
  memcpy(parsed.uuid, data + 4, 16);
  parsed.major = uint16_t(data[20]) << 8 | data[21];
  parsed.minor = uint16_t(data[22]) << 8 | data[23];
  parsed.measured_power = static_cast<int8_t>(data[24]);
  out = parsed;
  return true;
}
inline bool parseIBeacon(const uint8_t* payload, size_t length, IBeaconPayload& out) {
  if (!payload) return false;
  // A signature inside a name/service field is not an iBeacon. Walk bounded
  // AD structures, including advertisements with several manufacturer fields.
  IBeaconPayload found;
  bool matched = false;
  for (size_t offset = 0; offset < length;) {
    const size_t bytes = payload[offset];
    if (!bytes) break;
    if (bytes + 1 > length - offset) return false;
    if (payload[offset + 1] == 0xff && !matched)
      matched = parseIBeaconManufacturer(payload + offset + 2, bytes - 1, found);
    offset += bytes + 1;
  }
  if (matched) out = found;
  return matched;
}
inline bool formatIBeaconUuid(const IBeaconPayload& beacon, char* out, size_t size) {
  if (!out || size < 37) return false;
  const auto* u = beacon.uuid;
  snprintf(out, size, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
      u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
  return true;
}
} // namespace marauder
