#include "IBeacon.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
  using namespace marauder;
  uint8_t data[25] = {0x4c,0,2,0x15};
  for (unsigned i = 0; i < 16; ++i) data[4 + i] = i;
  data[20] = 0x12; data[21] = 0x34; data[22] = 0xab; data[23] = 0xcd; data[24] = 0xc5;
  IBeaconPayload beacon;
  assert(parseIBeaconManufacturer(data, sizeof(data), beacon));
  assert(beacon.major == 0x1234 && beacon.minor == 0xabcd && beacon.measured_power == -59);
  char uuid[37]; assert(formatIBeaconUuid(beacon, uuid, sizeof(uuid)));
  assert(!strcmp(uuid, "00010203-0405-0607-0809-0A0B0C0D0E0F"));
  assert(!formatIBeaconUuid(beacon, uuid, 36));
  uint8_t advertisement[30] = {2,1,6,26,0xff}; memcpy(advertisement + 5, data, 25);
  IBeaconPayload parsed;
  assert(parseIBeacon(advertisement, sizeof(advertisement), parsed) && sameIBeacon(beacon, parsed));
  for (size_t length = 0; length < sizeof(advertisement); ++length)
    assert(!parseIBeacon(advertisement, length, parsed));
  advertisement[4] = 0x09; assert(!parseIBeacon(advertisement, 30, parsed)); // Fake name signature.
  advertisement[4] = 0x16; assert(!parseIBeacon(advertisement, 30, parsed)); // Fake service signature.
  advertisement[4] = 0xff;
  uint8_t multi[35] = {4,0xff,0x12,0x34,0x56}; memcpy(multi + 5, advertisement, 30);
  assert(parseIBeacon(multi, sizeof(multi), parsed) && sameIBeacon(parsed, beacon));
  auto changedPower = beacon; changedPower.measured_power = -80;
  assert(sameIBeacon(changedPower, beacon));
  ++changedPower.major; assert(!sameIBeacon(changedPower, beacon));
  uint8_t trailing[32]; memcpy(trailing, advertisement, 30); trailing[30] = 5; trailing[31] = 0xff;
  assert(!parseIBeacon(trailing, sizeof(trailing), parsed)); // Whole malformed advertisement rejected.
  assert(!parseIBeacon(nullptr, 30, parsed));
  IBeaconObservation records[2]; unsigned count = 0;
  assert(retainIBeacon(records, count, 2, beacon, "02:00:00:00:00:01", -70, UINT32_MAX - 100) == 0);
  assert(retainIBeacon(records, count, 2, beacon, "02:00:00:00:00:02", -42, 50) == 0);
  assert(count == 1 && records[0].advertisements == 2 && records[0].rssi == -42);
  assert(!strcmp(records[0].address, "02:00:00:00:00:02"));
  ++beacon.major; assert(retainIBeacon(records, count, 2, beacon, "02:00:00:00:00:02", -42, 60) == 1);
  ++beacon.major; assert(retainIBeacon(records, count, 2, beacon, "02:00:00:00:00:03", -42, 70) == -1);
  assert(count == 2);
  assert(retainIBeacon(records, count, 2, records[0].beacon, "02:00:00:00:00:04", -60, 100) == 0);
  assert(records[0].advertisements == 3);
  std::cout << "iBeacon AD boundaries, false signatures, UUID, byte order, power and identity passed\n";
}
