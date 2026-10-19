#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cctype>
#include "IBeacon.h"

namespace marauder::fox {
enum class Kind : uint8_t { Ap, Station, Pineapple, MultiSSID, Ble, FindMy, Flipper, IBeacon, Meta, Flock };
enum class Sort : uint8_t { Signal, Name, Channel };
enum class Band : uint8_t { Any, GHz24, GHz5 };
constexpr uint8_t kMeta = 1, kFlock = 2, kFlipper = 4;
struct Options { Sort sort = Sort::Signal; Band band = Band::Any; bool recent = false; };
struct Candidate {
  Kind kind = Kind::Ap;
  unsigned source = 0;
  uint8_t mac[6] = {};
  char name[41] = {};
  int16_t rssi = -128;
  uint8_t channel = 0;
  uint32_t lastSeen = 0;
  bool bluetooth = false;
  IBeaconPayload beacon;
};
inline bool matches(const Candidate& item, const Options& options, uint32_t now) {
  if (options.recent && (!item.lastSeen || uint32_t(now - item.lastSeen) > 30000)) return false;
  if (!item.bluetooth) {
    if (options.band == Band::GHz24 && (item.channel < 1 || item.channel > 14)) return false;
    if (options.band == Band::GHz5 && (item.channel < 32 || item.channel > 196)) return false;
  }
  return true;
}
inline int compareName(const char* a, const char* b) {
  while (*a && *b) {
    const int d = std::tolower(static_cast<unsigned char>(*a++)) - std::tolower(static_cast<unsigned char>(*b++));
    if (d) return d;
  }
  return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}
inline bool before(const Candidate& a, const Candidate& b, Sort mode) {
  if (mode == Sort::Signal && a.rssi != b.rssi) return a.rssi > b.rssi;
  if (mode == Sort::Channel && a.channel != b.channel)
    return (a.channel ? a.channel : 255) < (b.channel ? b.channel : 255);
  const int names = compareName(a.name, b.name);
  if (names) return names < 0;
  const int addresses = memcmp(a.mac, b.mac, 6);
  if (addresses) return addresses < 0;
  return a.source < b.source;
}
// Retain the best screen-sized subset without allocating a second discovery
// list. Sorting/filtering never modifies the underlying AP/BLE/station data.
inline void insertTop(Candidate* list, unsigned& count, unsigned capacity,
                       const Candidate& item, Sort mode) {
  if (!list || !capacity || count > capacity) return;
  unsigned at = 0;
  while (at < count && !before(item, list[at], mode)) ++at;
  if (at >= capacity) return;
  const unsigned end = count < capacity ? count++ : capacity - 1;
  for (unsigned i = end; i > at; --i) list[i] = list[i - 1];
  list[at] = item;
}
struct Target {
  Candidate value;
  uint32_t started = 0, lastSeen = 0, hits = 0;
  bool active = false, seen = false;
  void select(const Candidate& candidate, uint32_t now) {
    *this = Target(); value = candidate; started = now; active = true;
  }
  bool fresh(uint32_t now) const { return active && seen && uint32_t(now - lastSeen) <= 1500; }
  bool observeWifi(const uint8_t* mac, int8_t rssi, uint8_t channel, uint32_t now) {
    if (!active || value.bluetooth || !mac || rssi > 0 || rssi < -127 || memcmp(mac, value.mac, 6)) return false;
    value.rssi = rssi; if (channel) value.channel = channel;
    seen = true; lastSeen = now; ++hits; return true;
  }
  bool observeBle(const uint8_t* mac, const uint8_t* data, size_t length, int8_t rssi, uint32_t now) {
    if (!active || !value.bluetooth || !mac || rssi > 0 || rssi < -127) return false;
    if (value.kind == Kind::IBeacon) {
      IBeaconPayload beacon;
      if (!parseIBeacon(data, length, beacon) || !sameIBeacon(beacon, value.beacon)) return false;
      memcpy(value.mac, mac, 6); // A rotating address does not change the target.
    } else if (memcmp(mac, value.mac, 6)) return false;
    value.rssi = rssi; seen = true; lastSeen = now; ++hits; return true;
  }
};
} // namespace marauder::fox
