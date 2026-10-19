#include "settings.h"

// ---------------------------------------------------------------------------
// _buildCache — called once after json_settings_string is loaded/updated.
// Parses the JSON exactly once and fills every field of _cache.
// All loadSetting<T>() reads hit the cache; no heap is allocated on read.
// ---------------------------------------------------------------------------
void Settings::_buildCache() {
  DynamicJsonDocument json(JSON_SETTING_SIZE);

  if (deserializeJson(json, this->json_settings_string)) {
    Serial.println(F("_buildCache: could not parse json"));
    return;
  }

  for (int i = 0; i < (int)json["Settings"].size(); i++) {
    const char* name = json["Settings"][i]["name"] | "";

    if (strcmp(name, "ForcePMKID") == 0)
      _cache.ForcePMKID = json["Settings"][i]["value"].as<bool>();
    else if (strcmp(name, "ForceProbe") == 0)
      _cache.ForceProbe = json["Settings"][i]["value"].as<bool>();
    else if (strcmp(name, "SavePCAP") == 0)
      _cache.SavePCAP = json["Settings"][i]["value"].as<bool>();
    else if (strcmp(name, "EnableLED") == 0)
      _cache.EnableLED = json["Settings"][i]["value"].as<bool>();
    else if (strcmp(name, "EPDeauth") == 0)
      _cache.EPDeauth = json["Settings"][i]["value"].as<bool>();
    else if (strcmp(name, "ChanHop") == 0)
      _cache.ChanHop = json["Settings"][i]["value"].as<bool>();
    else if (strcmp(name, "ClientSSID") == 0)
      _cache.ClientSSID = json["Settings"][i]["value"].as<String>();
    else if (strcmp(name, "ClientPW") == 0)
      _cache.ClientPW = json["Settings"][i]["value"].as<String>();
    else if (strcmp(name, "wu") == 0)
      _cache.wu = json["Settings"][i]["value"].as<String>();
    else if (strcmp(name, "wt") == 0)
      _cache.wt = json["Settings"][i]["value"].as<String>();
    else if (strcmp(name, WDG_KEY_NAME) == 0)
      _cache.wdg_key = json["Settings"][i]["value"].as<String>();
  }
}

bool Settings::_commitDocument(DynamicJsonDocument& json,
                               String& serialized) {
  if (json.overflowed())
    return false;

  serialized = "";
  const size_t expected_bytes = measureJson(json);
  if (!expected_bytes || serializeJson(json, serialized) != expected_bytes)
    return false;

  SPIFFS.remove((String(path_) + ".tmp"));
  File temporary = SPIFFS.open((String(path_) + ".tmp"), FILE_WRITE);
  const size_t written = temporary ? serializeJson(json, temporary) : 0;
  if (temporary) temporary.flush(); // VFS size uses fstat; flush stdio first.
  const size_t file_bytes = temporary ? temporary.size() : 0;
  const bool complete = temporary && written == expected_bytes &&
                        !temporary.getWriteError() && file_bytes == expected_bytes;
  temporary.close();
  if (!complete) {
    Serial.printf("Settings write failed: %u/%u bytes, file size %u\n",
                  static_cast<unsigned>(written), static_cast<unsigned>(expected_bytes),
                  static_cast<unsigned>(file_bytes));
    SPIFFS.remove((String(path_) + ".tmp"));
    return false;
  }

  SPIFFS.remove((String(path_) + ".backup"));
  const bool had_settings = SPIFFS.exists(path_);
  if (had_settings &&
      !SPIFFS.rename(path_, (String(path_) + ".backup"))) {
    SPIFFS.remove((String(path_) + ".tmp"));
    return false;
  }
  if (!SPIFFS.rename((String(path_) + ".tmp"), path_)) {
    if (had_settings)
      SPIFFS.rename((String(path_) + ".backup"), path_);
    SPIFFS.remove((String(path_) + ".tmp"));
    return false;
  }
  SPIFFS.remove((String(path_) + ".backup"));
  return true;
}

// ---------------------------------------------------------------------------

String Settings::getSettingsString() { return this->json_settings_string; }

bool Settings::begin() {
  if (!SPIFFS.begin(strcmp(path_, "/settings.json") == 0 && FORMAT_SPIFFS_IF_FAILED)) {
    return false;
  }

  File settingsFile;
  if (!SPIFFS.exists(path_) && SPIFFS.exists(String(path_) + ".backup"))
    SPIFFS.rename(String(path_) + ".backup", path_);

  // SPIFFS.remove(path_); // NEED TO REMOVE THIS LINE

  if (SPIFFS.exists(path_)) {
    settingsFile = SPIFFS.open(path_, FILE_READ);

    if (!settingsFile) {
      settingsFile.close();
      if (this->createDefaultSettings(SPIFFS))
        return true;
      else
        return false;
    }
  } else {
    if (this->createDefaultSettings(SPIFFS))
      return true;
    else
      return false;
  }

  String json_string;
  DynamicJsonDocument jsonBuffer(JSON_SETTING_SIZE);
  DeserializationError error = deserializeJson(jsonBuffer, settingsFile);
  settingsFile.close();
  if (error) {
    Serial.println(error.f_str());
    if (error == DeserializationError::NoMemory) return false;
    SPIFFS.remove((String(path_) + ".corrupt"));
    SPIFFS.rename(path_, (String(path_) + ".corrupt"));
    return this->createDefaultSettings(SPIFFS);
  }

  JsonArray settings = jsonBuffer["Settings"].as<JsonArray>();
  if (settings.isNull()) {
    jsonBuffer.remove("Settings");
    settings = jsonBuffer.createNestedArray("Settings");
  }

  bool migrated = false;
  const auto contains = [&settings](const char* key) {
    for (JsonObject item : settings) {
      if (strcmp(item["name"] | "", key) == 0)
        return true;
    }
    return false;
  };
  const auto addBool = [&settings, &contains, &migrated](const char* key,
                                                         bool value) {
    if (contains(key))
      return;
    JsonObject item = settings.createNestedObject();
    item["name"] = key;
    item["type"] = "bool";
    item["value"] = value;
    item["range"]["min"] = false;
    item["range"]["max"] = true;
    migrated = true;
  };
  const auto addString = [&settings, &contains, &migrated](const char* key) {
    if (contains(key))
      return;
    JsonObject item = settings.createNestedObject();
    item["name"] = key;
    item["type"] = "String";
    item["value"] = "";
    item["range"]["min"] = "";
    item["range"]["max"] = "";
    migrated = true;
  };

  addBool("ForcePMKID", false);
  addBool("ForceProbe", false);
  addBool("SavePCAP", true);
  addBool("EnableLED", true);
  addBool("EPDeauth", false);
  addBool("ChanHop", false);
  addString("ClientSSID");
  addString("ClientPW");
  addString("wu");
  addString("wt");
  addString(WDG_KEY_NAME);

  migrated = saved_wifi::migrate(jsonBuffer) || migrated;
  migrated = geofences::migrate(jsonBuffer) || migrated;

  if (jsonBuffer.overflowed()) {
    Serial.println(F("Settings migration exceeded JSON capacity"));
    return false;
  }

  if (migrated) {
    if (!this->_commitDocument(jsonBuffer, json_string)) {
      Serial.println(F("Could not commit migrated settings"));
      return false;
    }
  }
  else if (serializeJson(jsonBuffer, json_string) == 0) {
    return false;
  }

  this->json_settings_string = json_string;

  // Populate the flat cache from the freshly loaded JSON.
  this->_buildCache();

  return true;
}

// ---------------------------------------------------------------------------
// loadSetting<T> — O(1), zero heap, reads straight from the cache struct.
// ---------------------------------------------------------------------------

template <typename T> T Settings::loadSetting(const char* key) {}

// Get type int settings
template <> int Settings::loadSetting<int>(const char* key) {
  // No int settings are defined currently; fall back to parsing if ever added.
  DynamicJsonDocument json(JSON_SETTING_SIZE);
  deserializeJson(json, this->json_settings_string);

  for (int i = 0; i < (int)json["Settings"].size(); i++) {
    const char* setting_name = json["Settings"][i]["name"] | "";

    if (strcmp(setting_name, key) == 0)
      return json["Settings"][i]["value"];
  }

  return 0;
}

// Get type String settings — read from cache, no JSON parse
template <> String Settings::loadSetting<String>(const char* key) {
  if (strcmp(key, "ClientSSID") == 0)
    return _cache.ClientSSID;
  if (strcmp(key, "ClientPW") == 0)
    return _cache.ClientPW;
    if (strcmp(key, "wu") == 0)
    return _cache.wu;
  if (strcmp(key, "wt") == 0)
    return _cache.wt;
  if (strcmp(key, WDG_KEY_NAME) == 0)
    return _cache.wdg_key;

  // Unknown String key: fall back to JSON so the setting can be auto-created.
  DynamicJsonDocument json(JSON_SETTING_SIZE);
  deserializeJson(json, this->json_settings_string);

  for (int i = 0; i < (int)json["Settings"].size(); i++) {
    const char* setting_name = json["Settings"][i]["name"] | "";

    if (strcmp(setting_name, key) == 0)
      return json["Settings"][i]["value"].as<String>();
  }

  // Serial.print("Did not find setting named ");
  // Serial.print(key);
  // Serial.println(". Creating...");

  if (this->createDefaultSettings(
          SPIFFS,
          true,
          json["Settings"].size(),
          "String",
          key))
    return "";

  return "";
}

// Get type bool settings — read from cache, no JSON parse
template <> bool Settings::loadSetting<bool>(const char* key) {
  if (strcmp(key, "ForcePMKID") == 0)
    return _cache.ForcePMKID;
  if (strcmp(key, "ForceProbe") == 0)
    return _cache.ForceProbe;
  if (strcmp(key, "SavePCAP") == 0)
    return _cache.SavePCAP;
  if (strcmp(key, "EnableLED") == 0)
    return _cache.EnableLED;
  if (strcmp(key, "EPDeauth") == 0)
    return _cache.EPDeauth;
  if (strcmp(key, "ChanHop") == 0)
    return _cache.ChanHop;

  // Unknown bool key: fall back to JSON so the setting can be auto-created.
  DynamicJsonDocument json(JSON_SETTING_SIZE);
  deserializeJson(json, this->json_settings_string);

  for (int i = 0; i < (int)json["Settings"].size(); i++) {
    const char* setting_name = json["Settings"][i]["name"] | "";

    if (strcmp(setting_name, key) == 0) {
      return json["Settings"][i]["value"].as<bool>();
    }
  }

  // Serial.print("Did not find setting named ");
  // Serial.print(key);
  // Serial.println(". Creating...");

  if (this->createDefaultSettings(SPIFFS, true, json["Settings"].size(), "bool", key))
    return true;

  return false;
}

// Get type uint8_t settings — read from cache, no JSON parse
template <> uint8_t Settings::loadSetting<uint8_t>(const char* key) {
  // uint8_t settings reuse bool cache fields where applicable.
  if (strcmp(key, "ForcePMKID") == 0)
    return (uint8_t)_cache.ForcePMKID;

  if (strcmp(key, "ForceProbe") == 0)
    return (uint8_t)_cache.ForceProbe;

  if (strcmp(key, "SavePCAP") == 0)
    return (uint8_t)_cache.SavePCAP;

  if (strcmp(key, "EnableLED") == 0)
    return (uint8_t)_cache.EnableLED;

  if (strcmp(key, "EPDeauth") == 0)
    return (uint8_t)_cache.EPDeauth;

  if (strcmp(key, "ChanHop") == 0)
    return (uint8_t)_cache.ChanHop;

  DynamicJsonDocument json(JSON_SETTING_SIZE);
  deserializeJson(json, this->json_settings_string);

  for (int i = 0; i < (int)json["Settings"].size(); i++) {
    const char* setting_name = json["Settings"][i]["name"] | "";

    if (strcmp(setting_name, key) == 0)
      return json["Settings"][i]["value"].as<uint8_t>();
  }

  return 0;
}

// ---------------------------------------------------------------------------
// saveSetting — writes SPIFFS, updates json_settings_string, rebuilds cache.
// ---------------------------------------------------------------------------

template <typename T>
T Settings::saveSetting(const char* key, bool value) {}

template <> bool Settings::saveSetting<bool>(const char* key, bool value) {
  DynamicJsonDocument json(JSON_SETTING_SIZE);

  if (deserializeJson(json, this->json_settings_string)) {
    return false;
  }

  String settings_string;

  for (int i = 0; i < (int)json["Settings"].size(); i++) {
    const char* setting_name = json["Settings"][i]["name"] | "";

    if (strcmp(setting_name, key) == 0) {
      json["Settings"][i]["value"] = value;

      if (!this->_commitDocument(json, settings_string))
        return false;

      this->json_settings_string = settings_string;

      // Keep the cache in sync — no re-parse needed, just update the field.
      if (strcmp(key, "ForcePMKID") == 0)
        _cache.ForcePMKID = value;
      else if (strcmp(key, "ForceProbe") == 0)
        _cache.ForceProbe = value;
      else if (strcmp(key, "SavePCAP") == 0)
        _cache.SavePCAP = value;
      else if (strcmp(key, "EnableLED") == 0)
        _cache.EnableLED = value;
      else if (strcmp(key, "EPDeauth") == 0)
        _cache.EPDeauth = value;
      else if (strcmp(key, "ChanHop") == 0)
        _cache.ChanHop = value;

      // Routine updates never print credentials or tokens.

      return true;
    }
  }

  return false;
}

template <typename T>
T Settings::saveSetting(const char* key, String value) {}

template <> bool Settings::saveSetting<bool>(const char* key, String value) {
  DynamicJsonDocument json(JSON_SETTING_SIZE);

  if (deserializeJson(json, this->json_settings_string)) {
    return false;
  }

  String settings_string;

  for (int i = 0; i < (int)json["Settings"].size(); i++) {
    const char* setting_name = json["Settings"][i]["name"] | "";

    if (strcmp(setting_name, key) == 0) {
      json["Settings"][i]["value"] = value;

      if (!this->_commitDocument(json, settings_string))
        return false;

      this->json_settings_string = settings_string;

      // Keep the cache in sync for String fields.
      if (strcmp(key, "ClientSSID") == 0)
        _cache.ClientSSID = value;
      else if (strcmp(key, "ClientPW") == 0)
        _cache.ClientPW = value;
      else if (strcmp(key, "wu") == 0)
        _cache.wu = value;
      else if (strcmp(key, "wt") == 0)
        _cache.wt = value;
      else if (strcmp(key, WDG_KEY_NAME) == 0)
        _cache.wdg_key = value;

      // Routine updates never print credentials or tokens.

      return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// toggleSetting — reads current bool value from cache (fast), then delegates
// to saveSetting which will update the cache again.
// ---------------------------------------------------------------------------
bool Settings::toggleSetting(const char* key) {
  // Use the cached value to decide direction — avoids an extra JSON parse.
  bool current = this->loadSetting<bool>(key);
  const bool desired = !current;
  if (!saveSetting<bool>(key, desired)) {
    Serial.printf("Could not persist setting '%s'\n", key);
    return current;
  }
  return desired;
}

// ---------------------------------------------------------------------------
// Rarely-called utility functions — keep JSON parsing, they are not hot paths.
// ---------------------------------------------------------------------------

String Settings::setting_index_to_name(int i) {
  DynamicJsonDocument json(JSON_SETTING_SIZE);

  deserializeJson(json, this->json_settings_string);

  return json["Settings"][i]["name"];
}

int Settings::getNumberSettings() {
  DynamicJsonDocument json(JSON_SETTING_SIZE);

  deserializeJson(json, this->json_settings_string);

  return json["Settings"].size();
}

const char* Settings::getSettingType(const char* key) {
  static char type_buf[16];  // persistent buffer (adjust size if needed)

  DynamicJsonDocument json(JSON_SETTING_SIZE);
  deserializeJson(json, this->json_settings_string);

  for (int i = 0; i < (int)json["Settings"].size(); i++) {
    const char* name = json["Settings"][i]["name"];
    
    if (name && strcmp(name, key) == 0) {
      const char* type = json["Settings"][i]["type"];
      
      if (type) {
        strncpy(type_buf, type, sizeof(type_buf));
        type_buf[sizeof(type_buf) - 1] = '\0';
        return type_buf;
      }
    }
  }

  return "";
}

void Settings::printJsonSettings(String json_string) {
  DynamicJsonDocument json(JSON_SETTING_SIZE);

  deserializeJson(json, json_string);

  Serial.println("Settings\n----------------------------------------------");
  for (int i = 0; i < (int)json["Settings"].size(); i++) {
    Serial.println("Name: " + json["Settings"][i]["name"].as<String>());
    Serial.println("Type: " + json["Settings"][i]["type"].as<String>());
    const String name = json["Settings"][i]["name"].as<String>();
    const bool secret = name == "ClientPW" || name == "SavedWiFi" ||
                        name == "wt" || name == WDG_KEY_NAME;
    Serial.println("Value: " + (secret ? String("[redacted]") :
                   json["Settings"][i]["value"].as<String>()) + "\n");
  }
}

// ---------------------------------------------------------------------------
// createDefaultSettings — sets json_settings_string then rebuilds the cache.
// ---------------------------------------------------------------------------

bool Settings::createDefaultSettings(fs::FS &fs, bool spec, uint8_t index, const char* typeStr, const char* name) {
  (void)fs;
  String settings_string;

  if (!spec) {
    DynamicJsonDocument jsonBuffer(JSON_SETTING_SIZE);

    jsonBuffer["Settings"][0]["name"] = "ForcePMKID";
    jsonBuffer["Settings"][0]["type"] = "bool";
    jsonBuffer["Settings"][0]["value"] = false;
    jsonBuffer["Settings"][0]["range"]["min"] = false;
    jsonBuffer["Settings"][0]["range"]["max"] = true;

    jsonBuffer["Settings"][1]["name"] = "ForceProbe";
    jsonBuffer["Settings"][1]["type"] = "bool";
    jsonBuffer["Settings"][1]["value"] = false;
    jsonBuffer["Settings"][1]["range"]["min"] = false;
    jsonBuffer["Settings"][1]["range"]["max"] = true;

    jsonBuffer["Settings"][2]["name"] = "SavePCAP";
    jsonBuffer["Settings"][2]["type"] = "bool";
    jsonBuffer["Settings"][2]["value"] = true;
    jsonBuffer["Settings"][2]["range"]["min"] = false;
    jsonBuffer["Settings"][2]["range"]["max"] = true;

    jsonBuffer["Settings"][3]["name"] = "EnableLED";
    jsonBuffer["Settings"][3]["type"] = "bool";
    jsonBuffer["Settings"][3]["value"] = true;
    jsonBuffer["Settings"][3]["range"]["min"] = false;
    jsonBuffer["Settings"][3]["range"]["max"] = true;

    jsonBuffer["Settings"][4]["name"] = "EPDeauth";
    jsonBuffer["Settings"][4]["type"] = "bool";
    jsonBuffer["Settings"][4]["value"] = false;
    jsonBuffer["Settings"][4]["range"]["min"] = false;
    jsonBuffer["Settings"][4]["range"]["max"] = true;

    jsonBuffer["Settings"][5]["name"] = "ChanHop";
    jsonBuffer["Settings"][5]["type"] = "bool";
    jsonBuffer["Settings"][5]["value"] = false;
    jsonBuffer["Settings"][5]["range"]["min"] = false;
    jsonBuffer["Settings"][5]["range"]["max"] = true;

    jsonBuffer["Settings"][6]["name"] = "ClientSSID";
    jsonBuffer["Settings"][6]["type"] = "String";
    jsonBuffer["Settings"][6]["value"] = "";
    jsonBuffer["Settings"][6]["range"]["min"] = "";
    jsonBuffer["Settings"][6]["range"]["max"] = "";

    jsonBuffer["Settings"][7]["name"] = "ClientPW";
    jsonBuffer["Settings"][7]["type"] = "String";
    jsonBuffer["Settings"][7]["value"] = "";
    jsonBuffer["Settings"][7]["range"]["min"] = "";
    jsonBuffer["Settings"][7]["range"]["max"] = "";

    jsonBuffer["Settings"][8]["name"] = "wu";
    jsonBuffer["Settings"][8]["type"] = "String";
    jsonBuffer["Settings"][8]["value"] = "";
    jsonBuffer["Settings"][8]["range"]["min"] = "";
    jsonBuffer["Settings"][8]["range"]["max"] = "";

    jsonBuffer["Settings"][9]["name"] = "wt";
    jsonBuffer["Settings"][9]["type"] = "String";
    jsonBuffer["Settings"][9]["value"] = "";
    jsonBuffer["Settings"][9]["range"]["min"] = "";
    jsonBuffer["Settings"][9]["range"]["max"] = "";

    jsonBuffer["Settings"][10]["name"] = WDG_KEY_NAME;
    jsonBuffer["Settings"][10]["type"] = "String";
    jsonBuffer["Settings"][10]["value"] = "";
    jsonBuffer["Settings"][10]["range"]["min"] = "";
    jsonBuffer["Settings"][10]["range"]["max"] = "";

    saved_wifi::migrate(jsonBuffer);
    geofences::migrate(jsonBuffer);
    if (!_commitDocument(jsonBuffer, settings_string)) return false;
  } else {
    DynamicJsonDocument json(JSON_SETTING_SIZE);

    if (deserializeJson(json, this->json_settings_string)) {
      return false;
    }

    if (strcmp(typeStr, "bool") == 0) {
      json["Settings"][index]["name"] = name;
      json["Settings"][index]["type"] = typeStr;
      json["Settings"][index]["value"] = true;
      json["Settings"][index]["range"]["min"] = false;
      json["Settings"][index]["range"]["max"] = true;

      if (!_commitDocument(json, settings_string)) return false;
    } else if (strcmp(typeStr, "String") == 0) {
      json["Settings"][index]["name"] = name;
      json["Settings"][index]["type"] = typeStr;
      json["Settings"][index]["value"] = "";
      json["Settings"][index]["range"]["min"] = "";
      json["Settings"][index]["range"]["max"] = "";

      if (!_commitDocument(json, settings_string)) return false;
    }
  }


  this->json_settings_string = settings_string;

  // Rebuild cache from the newly written settings.
  this->_buildCache();



  return true;
}

unsigned Settings::getSavedWifiCount() {
  DynamicJsonDocument doc(JSON_SETTING_SIZE);
  if (deserializeJson(doc, json_settings_string)) return 0;
  return min(static_cast<unsigned>(saved_wifi::profiles(doc).size()), saved_wifi::kMaxProfiles);
}
bool Settings::loadSavedWifiCredential(unsigned index, String& ssid, String& password) {
  ssid = ""; password = "";
  DynamicJsonDocument doc(JSON_SETTING_SIZE);
  saved_wifi::Profile profile;
  if (deserializeJson(doc, json_settings_string) || !saved_wifi::read(doc, index, profile)) return false;
  ssid = profile.ssid; password = profile.password;
  return true;
}
saved_wifi::Result Settings::saveWifiCredential(const String& ssid, const String& password, int replace) {
  DynamicJsonDocument doc(JSON_SETTING_SIZE);
  if (deserializeJson(doc, json_settings_string)) return saved_wifi::Error;
  auto result = saved_wifi::save(doc, ssid.c_str(), password.c_str(), replace);
  if (result == saved_wifi::Error || result == saved_wifi::Full) return result;
  String serialized;
  if (!_commitDocument(doc, serialized)) return saved_wifi::Error;
  json_settings_string = serialized;
  _buildCache();
  return result;
}
bool Settings::removeSavedWifiCredential(unsigned index) {
  DynamicJsonDocument doc(JSON_SETTING_SIZE);
  if (deserializeJson(doc, json_settings_string) || !saved_wifi::remove(doc, index)) return false;
  String serialized;
  if (!_commitDocument(doc, serialized)) return false;
  json_settings_string = serialized;
  _buildCache();
  return true;
}
bool Settings::markSavedWifiSuccessful(unsigned index) {
  if (index == 0) return index < getSavedWifiCount();
  String ssid, password;
  return loadSavedWifiCredential(index, ssid, password) &&
         saveWifiCredential(ssid, password) == saved_wifi::Updated;
}

bool Settings::loadGeofence(unsigned index, geofences::Config& fence) {
  DynamicJsonDocument doc(JSON_SETTING_SIZE);
  if (deserializeJson(doc, json_settings_string)) { fence = geofences::Config(); return false; }
  return geofences::load(doc, index, fence);
}
bool Settings::saveGeofence(unsigned index, const geofences::Config& fence) {
  DynamicJsonDocument doc(JSON_SETTING_SIZE);
  if (deserializeJson(doc, json_settings_string) || !geofences::save(doc, index, fence)) return false;
  String serialized;
  if (!_commitDocument(doc, serialized)) return false;
  json_settings_string = serialized;
  return true;
}
bool Settings::clearGeofence(unsigned index) {
  DynamicJsonDocument doc(JSON_SETTING_SIZE);
  if (deserializeJson(doc, json_settings_string) || !geofences::remove(doc, index)) return false;
  String serialized;
  if (!_commitDocument(doc, serialized)) return false;
  json_settings_string = serialized;
  return true;
}

bool Settings::loadGeofences(geofences::Config* fences) {
  if (!fences) return false;
  DynamicJsonDocument doc(JSON_SETTING_SIZE);
  if (deserializeJson(doc, json_settings_string) ||
      !geofences::setting(doc)["value"].is<JsonArray>()) return false;
  for (unsigned i = 0; i < geofences::kMax; ++i) geofences::load(doc, i, fences[i]);
  return true;
}
