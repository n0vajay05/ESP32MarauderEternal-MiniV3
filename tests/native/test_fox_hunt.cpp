#include "FoxHunt.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
  using namespace marauder::fox;
  Candidate item; item.channel = 36; item.lastSeen = 1000;
  Options options; options.band = Band::GHz5; options.recent = true;
  assert(matches(item, options, 31000));
  assert(!matches(item, options, 31001));
  item.channel = 6; assert(!matches(item, options, 2000));
  options.band = Band::GHz24; assert(matches(item, options, 2000));
  item.channel = 0; assert(!matches(item, options, 2000));
  item.channel = 6; item.lastSeen = 0; assert(!matches(item, options, 2000));
  item.lastSeen = UINT32_MAX - 99; assert(matches(item, options, 100));
  item.bluetooth = true; item.channel = 0; assert(matches(item, options, 100));

  Candidate top[40]; unsigned count = 0;
  for (unsigned i = 0; i < 100; ++i) {
    Candidate c; c.source = i; c.rssi = -120 + i;
    insertTop(top, count, 40, c, Sort::Signal);
  }
  assert(count == 40 && top[0].source == 99 && top[39].source == 60);
  Candidate a, b; strcpy(a.name, "zulu"); strcpy(b.name, "Alpha");
  assert(before(b,a,Sort::Name) && !before(a,b,Sort::Name));
  a.channel = 0; b.channel = 36; assert(before(b,a,Sort::Channel));
  a.channel = 6; assert(before(a,b,Sort::Channel));
  a = b; a.source = 1; b.source = 2; assert(before(a,b,Sort::Signal));

  Target target; Candidate wifi; wifi.mac[0] = 2; wifi.channel = 6;
  target.select(wifi, 100); uint8_t other[6] = {4};
  assert(!target.fresh(100));
  assert(!target.observeWifi(other, -40, 6, 1000));
  assert(!target.observeWifi(wifi.mac, 127, 6, 1000));
  assert(target.observeWifi(wifi.mac, -50, 36, 1000));
  assert(target.fresh(2500) && !target.fresh(2501));
  assert(target.value.channel == 36 && target.hits == 1);
  assert(!target.observeBle(wifi.mac, nullptr, 0, -40, 1100));
  target.select(wifi, UINT32_MAX - 100);
  assert(target.observeWifi(wifi.mac, -50, 6, UINT32_MAX - 50) && target.fresh(100));

  uint8_t ad[27] = {26,0xff,0x4c,0,2,0x15};
  for (unsigned i = 0; i < 16; ++i) ad[i+6] = i;
  ad[22] = 1; ad[24] = 2; ad[26] = 0xc5;
  Candidate beacon; beacon.kind = Kind::IBeacon; beacon.bluetooth = true;
  assert(marauder::parseIBeacon(ad, sizeof(ad), beacon.beacon));
  target.select(beacon, 0);
  assert(target.observeBle(other, ad, sizeof(ad), -40, 100));
  uint8_t rotated[6] = {6}; ad[26] = 0xb0; // Calibrated power is not identity.
  assert(target.observeBle(rotated, ad, sizeof(ad), -45, 200));
  assert(!memcmp(target.value.mac, rotated, 6) && target.hits == 2 && target.value.channel == 0);
  ++ad[6]; assert(!target.observeBle(rotated, ad, sizeof(ad), -40, 300)); --ad[6];
  ++ad[22]; assert(!target.observeBle(rotated, ad, sizeof(ad), -40, 300)); --ad[22];
  ++ad[24]; assert(!target.observeBle(rotated, ad, sizeof(ad), -40, 300)); --ad[24];
  assert(!target.observeBle(rotated, ad, sizeof(ad)-1, -40, 300));
  assert(!target.observeBle(rotated, ad, sizeof(ad), -128, 300));
  beacon.kind = Kind::Ble; memcpy(beacon.mac, other, 6); target.select(beacon, 0);
  assert(!target.observeBle(rotated, ad, sizeof(ad), -40, 300));
  assert(target.observeBle(other, nullptr, 0, -40, 300));
  std::cout << "Fox combined filters, bounded sorting, exact targets, stale signals and rotating iBeacon identity passed\n";
}
