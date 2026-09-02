#pragma once

#include <Arduino.h>

namespace marauder {
namespace storage {

// Stable SD-card layout. Paths are deliberately centralized so future file
// transfer and browsing tools do not have to rediscover where each producer
// stores its output.
constexpr const char* CAPTURES_DIR = "/captures";
constexpr const char* LOGS_DIR = "/logs";
constexpr const char* GPS_DIR = "/gps";
constexpr const char* WARDRIVE_DIR = "/wardrive";
constexpr const char* LISTS_DIR = "/lists";
constexpr const char* EVIL_PORTAL_DIR = "/evil_portal";
constexpr const char* EVIL_PORTAL_HTML_DIR = "/evil_portal/html";
constexpr const char* CONFIG_DIR = "/config";
constexpr const char* FIRMWARE_DIR = "/firmware";
constexpr const char* SCRIPTS_DIR = "/SCRIPTS";

constexpr const char* SAVED_AIRTAGS = "/lists/Airtags_0.log";
constexpr const char* SAVED_APS = "/lists/APs_0.log";
constexpr const char* SAVED_SSIDS = "/lists/SSIDs_0.log";
constexpr const char* EVIL_PORTAL_CREDENTIALS =
    "/evil_portal/credentials.log";
constexpr const char* EVIL_PORTAL_AP_CONFIG =
    "/evil_portal/ap.config.txt";
constexpr const char* DEFAULT_UPDATE = "/firmware/update.bin";

inline String withLeadingSlash(const String& path) {
  if (path.length() == 0)
    return "/";
  return path.startsWith("/") ? path : "/" + path;
}

inline String relativePath(const String& path) {
  String relative = path;
  while (relative.startsWith("/"))
    relative.remove(0, 1);
  return relative;
}

inline String baseName(const String& path) {
  const int slash = path.lastIndexOf('/');
  return slash >= 0 ? path.substring(slash + 1) : path;
}

inline bool isWardriveUploadCandidate(const String& path) {
  const String name = baseName(path);
  return (name.startsWith("wardrive_") || name.startsWith("wigle-")) &&
         !name.endsWith(".wigle") && !name.endsWith(".wdg") &&
         !name.endsWith(".gpx");
}

}  // namespace storage
}  // namespace marauder
