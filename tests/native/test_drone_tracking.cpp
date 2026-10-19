#include "DroneTracking.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

using namespace drone_tracking;
void le32(uint8_t* p, int32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint32_t(value) >> (i * 8);
}
int main() {
  uint8_t basic[25] = {0x02, 0x12}; memcpy(basic + 2, "TEST&\"DRONE", 11);
  uint8_t location[25] = {0x12, 0x20, 90, 255};
  le32(location + 5, 400000000); le32(location + 9, -740000000);
  Record records[3]; unsigned count = 0; uint32_t generation = 0;
  Record* r = observe(records, count, 3, "02:00:00:00:00:01", -30, 6, 1, basic, 25, 100, generation);
  assert(r && count == 1 && r->firstSeen == 100 && r->rawMessageMask == 1);
  assert(apply(*r, location, 25, 200));
  assert(r->locationValid && r->speed == 63.75f && r->altitude == -1000);
  assert(gpxPoint(r->history[0]).find("<ele>") == std::string::npos);
  location[1] |= 1; assert(apply(*r, location, 25, 300)); assert(r->speed == 255);
  location[15] = 0xFF; location[16] = 0xFF;
  assert(apply(*r, location, 25, 400) && r->altitude == 31767.5f);
  assert(gpxPoint(r->history[r->historyCount - 1]).find("31767.5") != std::string::npos);
  const uint32_t revision = r->locationRevision;
  assert(apply(*r, location, 25, 500) && r->locationRevision == revision);
  for (unsigned i = 1; i <= 15; ++i) {
    le32(location + 5, 400000000 + i * 100); assert(apply(*r, location, 25, 500 + i * 100));
  }
  assert(r->historyCount == 10 && r->history[0].latitudeE7 == 400000600);
  Record invalid = *r;
  le32(location + 5, 910000000); assert(apply(invalid, location, 25, 2500));
  assert(!invalid.locationValid);
  assert(csvRow(invalid, 2500).find("91.0000000") == std::string::npos);
  assert(!validMessage(nullptr, 25) && !validMessage(basic, 24));
  uint8_t unsupported[25] = {0x03}; assert(!validMessage(unsupported, 25));
  assert(csvField("a,\"b\n") == "\"a,\"\"b \"");
  assert(csvField("=1+2") == "\"'=1+2\"");
  assert(xmlText("<&\"'>") == "&lt;&amp;&quot;&apos;&gt;");
  lifecycle(*r, r->lastSeen + kLostMs); lifecycle(*r, r->lastSeen + kLostMs + 1);
  assert(r->lost && r->lostCount == 1);
  r = observe(records, count, 3, "02:00:00:00:00:02", -50, 0, 2, basic, 25, 40000, generation);
  assert(count == 1 && r->methods == 3 && r->reacquiredCount == 1);
  assert(!strcmp(r->wifiAddress, "02:00:00:00:00:01"));
  assert(!strcmp(r->bleAddress, "02:00:00:00:00:02"));
  // Location-before-Basic-ID on a rotating address merges with the stable ID.
  le32(location + 5, 400000123);
  const uint32_t beforeMerge = r->messageCount;
  assert(observe(records, count, 3, "02:00:00:00:00:03", -60, 0, 2, location, 25, 41000, generation));
  assert(count == 2);
  r = observe(records, count, 3, "02:00:00:00:00:03", -60, 0, 2, basic, 25, 42000, generation);
  assert(r && count == 1 && std::abs(r->latitude - 40.0000123) < 1e-9);
  assert(r->messageCount == beforeMerge + 2);
  uint8_t registration[25] = {0x02, 0x22}; memcpy(registration + 2, "REGISTRATION", 12);
  assert(apply(*r, registration, 25, 43000) && r->idType == 1 && !strcmp(r->id, "TEST&\"DRONE"));
  uint8_t other[25] = {0x02, 0x12}; memcpy(other + 2, "OTHER", 5);
  assert(observe(records, count, 3, "02:00:00:00:00:04", -70, 1, 1, other, 25, 43000, generation));
  other[2] = 'X';
  assert(observe(records, count, 3, "02:00:00:00:00:05", -70, 1, 1, other, 25, 44000, generation));
  other[2] = 'Z';
  assert(!observe(records, count, 3, "02:00:00:00:00:06", -70, 1, 1, other, 25, 45000, generation));
  records[0].pinned = true;
  assert(observe(records, count, 3, "02:00:00:00:00:06", -70, 1, 1, other, 25, 170000, generation));
  assert(records[0].pinned && !strcmp(records[0].id, "TEST&\"DRONE"));
  assert(count == 3);
  Record aliases[1]; unsigned aliasCount = 0;
  assert(observe(aliases, aliasCount, 1, "02:aa:bb:00:00:01", -30, 6, 1, basic, 25, 100, generation));
  assert(observe(aliases, aliasCount, 1, "02:AA:BB:00:00:01", -30, 6, 1, location, 25, 200, generation));
  assert(aliasCount == 1 && aliases[0].messageCount == 2);
  uint8_t auth[25] = {0x22, 0x30, 2, 40}; le32(auth + 4, 12345);
  assert(apply(aliases[0], auth, 25, 300));
  assert(aliases[0].authenticationType == 3 && aliases[0].authenticationLastPage == 2 && aliases[0].authenticationLength == 40 && aliases[0].authenticationTimestamp == 12345);
  auth[2] = 16; assert(!apply(aliases[0], auth, 25, 400));
  Record wrap; wrap.lastSeen = UINT32_MAX - 1000;
  lifecycle(wrap, 2000); assert(!wrap.lost);
  lifecycle(wrap, 30000); assert(wrap.lost);
  assert(std::abs(distanceMeters(0, 0, 0, .001) - 111.195) < .1);
  assert(distanceMeters(0, 179.999, 0, -179.999) < 223);
  assert(std::abs(bearingDegrees(0, 0, 0, 1) - 90) < .001);
  assert(std::isfinite(distanceMeters(90, 180, -90, 0)));
  std::cout << "Remote ID sentinels, identity merge, lifecycle, history and export escaping passed; record=" << sizeof(Record) << " bytes\n";
}
