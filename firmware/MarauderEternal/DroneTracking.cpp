#include "DroneTracking.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace drone_tracking {
namespace {
int32_t i32(const uint8_t* p) {
  return static_cast<int32_t>(uint32_t(p[0]) | uint32_t(p[1]) << 8 |
                             uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
}
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
void text(char* out, size_t capacity, const uint8_t* source, size_t length) {
  size_t n = 0;
  while (n + 1 < capacity && n < length && source[n] >= 32 && source[n] <= 126) {
    out[n] = char(source[n]); ++n;
  }
  out[n] = 0;
}
bool stable(const Record& r) { return (r.idType == 1 || r.idType == 2) && r.id[0]; }
void observeTime(Record& r, uint32_t now) {
  if (r.lost) { r.lost = false; ++r.reacquiredCount; r.lastReacquired = now; }
  r.lastSeen = now; ++r.messageCount; ++r.rateMessages;
  const uint32_t elapsed = now - r.rateStarted;
  if (elapsed >= 1000) {
    r.messageRate = r.rateMessages * 1000.0f / elapsed;
    r.rateStarted = now; r.rateMessages = 0;
  }
}
}
bool coordinates(double lat, double lon) {
  return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 &&
         lon >= -180 && lon <= 180 && (lat != 0 || lon != 0);
}
bool validMessage(const uint8_t* m, size_t length) {
  if (!m || length < 25 || (m[0] >> 4) > 5 || (m[0] & 15) > 2) return false;
  // Version 0 is ASTM F3411-19; retain compatibility with deployed receivers.
  return (m[0] >> 4) != 2 || (m[1] & 15) != 0 || m[2] <= 15;
}
void lifecycle(Record& r, uint32_t now) {
  if (!r.lost && uint32_t(now - r.lastSeen) >= kLostMs) {
    r.lost = true; ++r.lostCount; r.lastLost = now; r.messageRate = 0;
  }
}
bool apply(Record& r, const uint8_t* m, size_t length, uint32_t now, bool countReception) {
  if (!validMessage(m, length)) return false;
  const uint8_t type = m[0] >> 4;
  if (type == 0) {
    const uint8_t candidate = m[1] >> 4;
    // Keep a serial number when a transmitter alternates multiple Basic IDs.
    if (!(r.idType == 1 && candidate != 1)) {
      char id[21] = {}; text(id, sizeof(id), m + 2, 20);
      if (id[0]) { strcpy(r.id, id); r.idType = candidate; }
    }
    r.uaType = m[1] & 15;
  } else if (type == 1) {
    r.status = m[1] >> 4;
    r.direction = m[2] + ((m[1] & 2) ? 180.0f : 0.0f);
    r.speed = (m[1] & 1) ? m[3] * .75f + 63.75f : m[3] * .25f;
    r.verticalSpeed = static_cast<int8_t>(m[4]) * .5f;
    r.latitude = i32(m + 5) / 1e7; r.longitude = i32(m + 9) / 1e7;
    r.altitude = u16(m + 15) * .5f - 1000;
    r.height = u16(m + 17) * .5f - 1000;
    r.locationValid = coordinates(r.latitude, r.longitude);
    r.locationSeen = now;
    if (r.locationValid) {
      Point p; p.latitudeE7 = i32(m + 5); p.longitudeE7 = i32(m + 9);
      p.altitude = r.altitude; p.observedMs = now;
      const bool moved = !r.historyCount ||
          p.latitudeE7 != r.history[r.historyCount - 1].latitudeE7 ||
          p.longitudeE7 != r.history[r.historyCount - 1].longitudeE7 ||
          p.altitude != r.history[r.historyCount - 1].altitude;
      if (moved) {
        if (r.historyCount == kHistory) {
          for (unsigned i = 1; i < kHistory; ++i) r.history[i - 1] = r.history[i];
          --r.historyCount;
        }
        r.history[r.historyCount++] = p;
      }
      if (moved) ++r.locationRevision;
    }
  } else if (type == 2) {
    r.authenticationType = m[1] >> 4; r.authenticationPage = m[1] & 15;
    if (!r.authenticationPage) {
      r.authenticationLastPage = m[2]; r.authenticationLength = m[3];
      r.authenticationTimestamp = static_cast<uint32_t>(i32(m + 4));
    }
  } else if (type == 3) text(r.description, sizeof(r.description), m + 2, 23);
  else if (type == 4) {
    r.systemSeen = now;
    r.operatorLatitude = i32(m + 2) / 1e7; r.operatorLongitude = i32(m + 6) / 1e7;
    r.operatorAltitude = u16(m + 18) * .5f - 1000;
    r.operatorLocationValid = coordinates(r.operatorLatitude, r.operatorLongitude);
  } else if (type == 5) text(r.operatorId, sizeof(r.operatorId), m + 2, 20);
  memcpy(r.rawMessages[type], m, 25); r.rawMessageMask |= 1U << type;
  r.messagesSeen |= 1U << type; r.messageSeen[type] = now;
  if (countReception) observeTime(r, now);
  return true;
}
bool sameIdentity(const Record& a, const Record& b) {
  return stable(a) && stable(b) && a.idType == b.idType && strcmp(a.id, b.id) == 0;
}
Record* observe(Record* records, unsigned& count, unsigned capacity,
                const char* address, int8_t rssi, uint8_t channel,
                uint8_t method, const uint8_t* message, size_t length,
                uint32_t now, uint32_t& nextGeneration) {
  if (!records || !capacity || count > capacity || !address || strlen(address) != 17 ||
      !validMessage(message, length)) return nullptr;
  char normalized[18];
  for (unsigned i = 0; i < 17; ++i) {
    char c = address[i];
    if (i % 3 == 2) { if (c != ':') return nullptr; }
    else {
      if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return nullptr;
    }
    normalized[i] = c;
  }
  normalized[17] = 0; address = normalized;
  unsigned index = count;
  for (unsigned i = 0; i < count; ++i)
    if (!strcmp(records[i].address, address) || !strcmp(records[i].wifiAddress, address) ||
        !strcmp(records[i].bleAddress, address)) { index = i; break; }
  Record candidate;
  if ((message[0] >> 4) == 0) apply(candidate, message, length, now);
  unsigned identity = count;
  for (unsigned i = 0; i < count; ++i)
    if (sameIdentity(records[i], candidate)) { identity = i; break; }
  if (identity < count && identity != index) {
    if (index < count) {
      // The new address may have emitted Location before its Basic ID.
      const Record& partial = records[index];
      Record& known = records[identity];
      known.methods |= partial.methods; known.pinned |= partial.pinned;
      if (partial.wifiAddress[0]) strcpy(known.wifiAddress, partial.wifiAddress);
      if (partial.bleAddress[0]) strcpy(known.bleAddress, partial.bleAddress);
      for (unsigned type = 1; type < 6; ++type)
        if ((partial.rawMessageMask & (1U << type)) &&
            (!(known.rawMessageMask & (1U << type)) ||
             uint32_t(now - partial.messageSeen[type]) < uint32_t(now - known.messageSeen[type])))
          apply(known, partial.rawMessages[type], 25, partial.messageSeen[type], false);
      known.messageCount += partial.messageCount;
      if (uint32_t(now - partial.firstSeen) > uint32_t(now - known.firstSeen)) known.firstSeen = partial.firstSeen;
      if (uint32_t(now - partial.lastSeen) < uint32_t(now - known.lastSeen)) known.lastSeen = partial.lastSeen;
      for (unsigned i = index + 1; i < count; ++i) records[i - 1] = records[i];
      --count; records[count] = Record();
      if (identity > index) --identity;
    }
    index = identity;
  }
  if (index == count) {
    if (count < capacity) ++count;
    else {
      uint32_t oldest = 0;
      for (unsigned i = 0; i < count; ++i) {
        const uint32_t age = now - records[i].lastSeen;
        if (!records[i].pinned && age >= kReuseMs && age > oldest) { oldest = age; index = i; }
      }
      if (!oldest) return nullptr; // Never evict a live retained target.
    }
    records[index] = Record();
    records[index].firstSeen = now; records[index].rateStarted = now;
    records[index].generation = ++nextGeneration;
    snprintf(records[index].id, sizeof(records[index].id), "RID-%.2s%.2s", address + 12, address + 15);
  }
  Record& record = records[index];
  lifecycle(record, now);
  if (!apply(record, message, length, now)) return nullptr;
  strcpy(record.address, address);
  strcpy(method == 1 ? record.wifiAddress : record.bleAddress, address);
  record.rssi = rssi; if (channel) record.channel = channel;
  record.methods |= method;
  return &record;
}
double distanceMeters(double a, double b, double c, double d) {
  constexpr double rad = .017453292519943295;
  const double s = sin((c - a) * rad / 2), t = sin((d - b) * rad / 2);
  const double h = std::max(0.0, std::min(1.0, s * s + cos(a * rad) * cos(c * rad) * t * t));
  return 12742000 * atan2(sqrt(h), sqrt(1 - h));
}
double bearingDegrees(double a, double b, double c, double d) {
  constexpr double rad = .017453292519943295;
  const double y = sin((d - b) * rad) * cos(c * rad);
  const double x = cos(a * rad) * sin(c * rad) - sin(a * rad) * cos(c * rad) * cos((d - b) * rad);
  return fmod(atan2(y, x) / rad + 360, 360);
}
std::string csvField(const char* text) {
  std::string out = "\"";
  const char* first = text;
  while (first && *first == ' ') ++first;
  if (first && (*first == '=' || *first == '+' || *first == '-' || *first == '@')) out += '\'';
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); p && *p; ++p) {
    if (*p == '"') out += '"';
    out += *p >= 32 && *p < 127 ? char(*p) : ' ';
  }
  return out + '"';
}
std::string xmlText(const char* text) {
  std::string out;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); p && *p; ++p) {
    switch (*p) {
      case '&': out += "&amp;"; break; case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break; case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: if (*p >= 32 && *p < 127) out += char(*p);
    }
  }
  return out;
}
std::string csvHeader() {
  return "observed_ms,uas_id,address,operator_id,description,transports,channel,rssi,messages,msg_per_second,state,lost,reacquired,first_seen_ms,last_seen_ms,location_ms,latitude,longitude,altitude_m,height_m,speed_mps,direction_deg,operator_latitude,operator_longitude,operator_altitude_m,last_lost_ms,last_reacquired_ms,authentication_type,authentication_page,authentication_last_page,authentication_length,authentication_timestamp\n";
}
std::string csvRow(const Record& r, uint32_t now) {
  char b[256];
  std::string out = std::to_string(now) + ',' + csvField(r.id) + ',' + csvField(r.address) + ',' +
                    csvField(r.operatorId) + ',' + csvField(r.description) + ',';
  snprintf(b, sizeof(b), "%u,%u,%d,%lu,%.2f,%s,%lu,%lu,%lu,%lu,%lu,", r.methods, r.channel, r.rssi,
      static_cast<unsigned long>(r.messageCount), r.messageRate, r.lost ? "lost" : "active",
      static_cast<unsigned long>(r.lostCount), static_cast<unsigned long>(r.reacquiredCount),
      static_cast<unsigned long>(r.firstSeen), static_cast<unsigned long>(r.lastSeen), static_cast<unsigned long>(r.locationSeen));
  out += b;
  const bool fresh = r.locationValid && uint32_t(now - r.locationSeen) < kLostMs;
  const auto number = [&out, &b](bool valid, double value, int precision) {
    if (valid) { snprintf(b, sizeof(b), "%.*f", precision, value); out += b; }
    out += ',';
  };
  number(fresh, r.latitude, 7); number(fresh, r.longitude, 7);
  number(fresh && r.altitude > kUnknownAltitude, r.altitude, 1);
  number(fresh && r.height > kUnknownAltitude, r.height, 1);
  number(fresh && r.speed <= 254.25f, r.speed, 2);
  number(fresh && r.direction <= 360, r.direction, 0);
  const bool operatorFresh = r.operatorLocationValid && uint32_t(now - r.systemSeen) < kLostMs;
  number(operatorFresh, r.operatorLatitude, 7);
  number(operatorFresh, r.operatorLongitude, 7);
  if (operatorFresh && r.operatorAltitude > kUnknownAltitude) {
    snprintf(b, sizeof(b), "%.1f", r.operatorAltitude); out += b;
  }
  snprintf(b, sizeof(b), ",%lu,%lu,", static_cast<unsigned long>(r.lastLost), static_cast<unsigned long>(r.lastReacquired));
  out += b;
  if (r.messagesSeen & 4) {
    snprintf(b, sizeof(b), "%u,%u,%u,%u,%lu", r.authenticationType, r.authenticationPage,
        r.authenticationLastPage, r.authenticationLength, static_cast<unsigned long>(r.authenticationTimestamp));
    out += b;
  } else out += ",,,,";
  return out + '\n';
}
std::string gpxPoint(const Point& p) {
  if (!coordinates(p.latitudeE7 / 1e7, p.longitudeE7 / 1e7)) return "";
  char b[180];
  snprintf(b, sizeof(b), "<trkpt lat=\"%.7f\" lon=\"%.7f\">", p.latitudeE7 / 1e7, p.longitudeE7 / 1e7);
  std::string out = b;
  if (p.altitude > kUnknownAltitude) {
    snprintf(b, sizeof(b), "<ele>%.1f</ele>", p.altitude); out += b;
  }
  // Device uptime is explicit; never present it as a fabricated UTC timestamp.
  snprintf(b, sizeof(b), "<extensions><observed_ms xmlns=\"urn:marauder:remoteid\">%lu</observed_ms></extensions></trkpt>\n", static_cast<unsigned long>(p.observedMs));
  return out + b;
}
} // namespace drone_tracking
