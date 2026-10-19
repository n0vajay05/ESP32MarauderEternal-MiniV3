#pragma once
#include <ArduinoJson.h>
#include <cmath>
#include <cstring>
#include <string>

namespace geofences {
constexpr unsigned kMax = 5;
struct Config {
  bool enabled = false;
  char name[33] = {};
  double latitude = 0, longitude = 0;
  float radiusMiles = 0.1f;
};
inline bool coordinates(double lat, double lon) {
  return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 &&
         lat <= 90 && lon >= -180 && lon <= 180;
}
inline bool valid(const Config& fence) {
  return fence.enabled && fence.name[0] &&
         memchr(fence.name, '\0', sizeof(fence.name)) &&
         coordinates(fence.latitude, fence.longitude) &&
         std::isfinite(fence.radiusMiles) && fence.radiusMiles >= 0.1f &&
         fence.radiusMiles <= 1.0f;
}
inline double distanceMiles(double lat1, double lon1, double lat2, double lon2) {
  constexpr double radians = 0.017453292519943295;
  const double a = sin((lat2 - lat1) * radians / 2);
  const double b = sin((lon2 - lon1) * radians / 2);
  double h = a * a + cos(lat1 * radians) * cos(lat2 * radians) * b * b;
  h = h < 0 ? 0 : (h > 1 ? 1 : h); // Rounding at antipodal coordinates.
  return 3958.7613 * 2 * atan2(sqrt(h), sqrt(1 - h));
}
enum class State { Clear, Inside, WaitingForFix };
class Gate {
 public:
  State state = State::Clear;
  int active = -1;
  bool paused() const { return state != State::Clear; }
  State update(const Config* fences, bool fresh_fix, double lat, double lon) {
    bool configured = false;
    for (unsigned i = 0; i < kMax; ++i) configured |= valid(fences[i]);
    if (!configured) { active = -1; return state = State::Clear; }
    if (!fresh_fix || !coordinates(lat, lon)) return state = State::WaitingForFix;
    // Exit only after another ten metres; entry always uses the exact radius.
    if (active >= 0 && active < static_cast<int>(kMax) && valid(fences[active]) &&
        distanceMiles(lat, lon, fences[active].latitude, fences[active].longitude) <=
            fences[active].radiusMiles + 0.006213712)
      return state = State::Inside;
    active = -1;
    for (unsigned i = 0; i < kMax; ++i)
      if (valid(fences[i]) && distanceMiles(lat, lon, fences[i].latitude,
          fences[i].longitude) <= fences[i].radiusMiles) {
        active = i; return state = State::Inside;
      }
    return state = State::Clear;
  }
};
inline JsonObject setting(JsonDocument& doc) {
  for (JsonObject item : doc["Settings"].as<JsonArray>())
    if (strcmp(item["name"] | "", "Geofences") == 0) return item;
  return JsonObject();
}
inline bool migrate(JsonDocument& doc) {
  if (!setting(doc).isNull()) return false;
  JsonObject item = doc["Settings"].as<JsonArray>().createNestedObject();
  item["name"] = "Geofences"; item["type"] = "geofence_list";
  item.createNestedArray("value");
  item["range"]["min"] = 0; item["range"]["max"] = kMax;
  return true;
}
inline bool load(JsonDocument& doc, unsigned index, Config& fence) {
  fence = Config();
  if (index >= kMax) return false;
  for (JsonObject entry : setting(doc)["value"].as<JsonArray>()) {
    if ((entry["slot"] | 255U) != index) continue;
    const char* name = entry["name"] | "";
    if (strlen(name) > 32) return false;
    strcpy(fence.name, name); fence.enabled = entry["enabled"] | false;
    fence.latitude = entry["lat"] | 0.0; fence.longitude = entry["lon"] | 0.0;
    fence.radiusMiles = entry["radius_mi"] | 0.1f;
    if (valid(fence)) return true;
    fence = Config(); return false;
  }
  return false;
}
inline bool save(JsonDocument& doc, unsigned index, const Config& fence) {
  if (index >= kMax || !valid(fence)) return false;
  migrate(doc);
  JsonArray list = setting(doc)["value"].as<JsonArray>();
  if (list.isNull()) return false;
  JsonObject target;
  for (JsonObject item : list)
    if ((item["slot"] | 255U) == index) target = item;
  if (target.isNull()) target = list.createNestedObject();
  target["slot"] = index; target["enabled"] = true;
  target["name"] = std::string(fence.name);
  target["lat"] = fence.latitude; target["lon"] = fence.longitude;
  target["radius_mi"] = fence.radiusMiles;
  return !doc.overflowed();
}
inline bool remove(JsonDocument& doc, unsigned index) {
  if (index >= kMax) return false;
  JsonArray list = setting(doc)["value"].as<JsonArray>();
  for (unsigned i = 0; i < list.size(); ++i)
    if ((list[i]["slot"] | 255U) == index) { list.remove(i); return true; }
  return true;
}
} // namespace geofences
