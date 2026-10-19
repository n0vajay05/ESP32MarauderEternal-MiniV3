#pragma once

#include <ArduinoJson.h>
#include <cstring>
#include <string>

// Shared by the settings store and native/device regression checks.
namespace saved_wifi {
constexpr unsigned kMaxProfiles = 5;
constexpr const char* kKey = "SavedWiFi";
enum Result { Saved, Updated, Full, Error };
struct Profile { char ssid[33] = {}; char password[65] = {}; };

inline JsonObject setting(JsonDocument& doc, const char* key) {
  for (JsonObject item : doc["Settings"].as<JsonArray>())
    if (strcmp(item["name"] | "", key) == 0) return item;
  return JsonObject();
}
inline JsonArray profiles(JsonDocument& doc) {
  return setting(doc, kKey)["value"].as<JsonArray>();
}
inline bool valid(const char* ssid, const char* password) {
  if (!ssid || !password || strlen(ssid) == 0 || strlen(ssid) > 32) return false;
  const size_t length = strlen(password);
  if (length == 0 || (length >= 8 && length <= 63)) return true;
  if (length != 64) return false;
  for (unsigned i = 0; i < 64; ++i)
    if (!((password[i] >= '0' && password[i] <= '9') ||
          (password[i] >= 'a' && password[i] <= 'f') ||
          (password[i] >= 'A' && password[i] <= 'F'))) return false;
  return true;
}
inline bool read(JsonDocument& doc, unsigned index, Profile& out) {
  out = Profile();
  JsonArray list = profiles(doc);
  if (index >= list.size() || index >= kMaxProfiles) return false;
  const char* ssid = list[index]["ssid"] | "";
  const char* password = list[index]["password"] | "";
  if (!valid(ssid, password)) return false;
  strcpy(out.ssid, ssid); strcpy(out.password, password);
  return true;
}
inline void append(JsonArray list, const Profile& profile) {
  JsonObject item = list.createNestedObject();
  // ArduinoJson owns copies; callers' temporary credentials never dangle.
  item["ssid"] = std::string(profile.ssid);
  item["password"] = std::string(profile.password);
}
inline void legacy(JsonDocument& doc, const Profile& profile) {
  setting(doc, "ClientSSID")["value"] = std::string(profile.ssid);
  setting(doc, "ClientPW")["value"] = std::string(profile.password);
}
inline bool migrate(JsonDocument& doc) {
  if (!setting(doc, kKey).isNull()) return false;
  Profile old;
  const char* ssid = setting(doc, "ClientSSID")["value"] | "";
  const char* password = setting(doc, "ClientPW")["value"] | "";
  const bool have_old = valid(ssid, password);
  if (have_old) { strcpy(old.ssid, ssid); strcpy(old.password, password); }
  JsonObject item = doc["Settings"].as<JsonArray>().createNestedObject();
  item["name"] = kKey; item["type"] = "wifi_list";
  JsonArray list = item.createNestedArray("value");
  item["range"]["min"] = 0; item["range"]["max"] = kMaxProfiles;
  if (have_old) append(list, old);
  return true;
}
inline Result save(JsonDocument& doc, const char* ssid, const char* password,
                   int replace = -1) {
  if (!valid(ssid, password)) return Error;
  migrate(doc);
  Profile old[kMaxProfiles];
  if (profiles(doc).isNull()) return Error;
  const unsigned count = profiles(doc).size();
  if (count > kMaxProfiles) return Error;
  int target = -1;
  for (unsigned i = 0; i < count; ++i) {
    if (!read(doc, i, old[i])) return Error;
    if (strcmp(old[i].ssid, ssid) == 0) target = i;
  }
  const bool updating = target >= 0;
  if (target < 0 && replace >= 0) {
    if (static_cast<unsigned>(replace) >= count) return Error;
    target = replace;
  }
  if (target < 0 && count == kMaxProfiles) return Full;
  Profile next;
  strcpy(next.ssid, ssid); strcpy(next.password, password);
  JsonArray list = profiles(doc); list.clear();
  append(list, next);
  for (unsigned i = 0; i < count; ++i)
    if (static_cast<int>(i) != target) append(list, old[i]);
  legacy(doc, next);
  return doc.overflowed() ? Error : (updating ? Updated : Saved);
}
inline bool remove(JsonDocument& doc, unsigned index) {
  JsonArray list = profiles(doc);
  if (index >= list.size() || index >= kMaxProfiles) return false;
  list.remove(index);
  Profile first;
  if (list.size() && !read(doc, 0, first)) return false;
  legacy(doc, first);
  return !doc.overflowed();
}
// Visible networks first, strongest RSSI first; preserve recency for ties and
// unseen/hidden networks. Each index occurs exactly once.
inline void rank(unsigned count, const bool* visible, const int* rssi,
                 unsigned* order) {
  for (unsigned i = 0; i < count; ++i) {
    unsigned at = i;
    while (at && ((visible[i] && !visible[order[at - 1]]) ||
           (visible[i] == visible[order[at - 1]] && visible[i] &&
            rssi[i] > rssi[order[at - 1]]))) {
      order[at] = order[at - 1]; --at;
    }
    order[at] = i;
  }
}
}  // namespace saved_wifi
