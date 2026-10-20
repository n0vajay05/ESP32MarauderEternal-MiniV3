#pragma once

#include <ArduinoJson.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace marauder { namespace ota {
constexpr uint32_t kServerTimeoutMs = 15000;
constexpr uint32_t kSlotBytes = 0x3c0000;
constexpr const char* kRepository = "n0vajay05/ESP32MarauderEternal-MiniV3";
constexpr const char* kApi = "https://api.github.com/repos/n0vajay05/ESP32MarauderEternal-MiniV3/releases/latest";
constexpr const char* kDownload = "https://github.com/n0vajay05/ESP32MarauderEternal-MiniV3/releases/download/";
constexpr const char* kAssetPath = "/github-production-release-asset/1339968948/";

struct Version {
  uint32_t parts[3]{};
  bool parse(const char* text) {
    if (!text) return false;
    if (*text == 'v') ++text;
    for (unsigned i = 0; i < 3; ++i) {
      if (*text < '0' || *text > '9') return false;
      uint32_t value = 0; unsigned digits = 0;
      while (*text >= '0' && *text <= '9') {
        if (++digits > 6) return false;
        value = value * 10 + unsigned(*text++ - '0');
      }
      parts[i] = value;
      if (i < 2 && *text++ != '.') return false;
    }
    return *text == 0;
  }
  int compare(const Version& other) const {
    for (unsigned i = 0; i < 3; ++i)
      if (parts[i] != other.parts[i]) return parts[i] > other.parts[i] ? 1 : -1;
    return 0;
  }
};

inline uint32_t remaining(uint32_t start, uint32_t now, uint32_t budget) {
  const uint32_t elapsed = now - start;
  return elapsed < budget ? budget - elapsed : 0;
}
inline bool safeName(const std::string& name, size_t maximum = 96) {
  if (name.empty() || name.size() > maximum || name == "." || name.find("..") != std::string::npos) return false;
  for (char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
  return true;
}
struct Url { std::string host, path; };
inline bool splitUrl(const std::string& url, Url& result) {
  if (url.size() > 2048 || url.compare(0, 8, "https://")) return false;
  for (unsigned char c : url) if (c <= 32 || c == 127 || c == '\\' || c == '#') return false;
  const size_t slash = url.find('/', 8);
  if (slash == std::string::npos) return false;
  result.host = url.substr(8, slash - 8); result.path = url.substr(slash);
  if (result.host.empty() || result.host.find_first_of("@:[]%") != std::string::npos) return false;
  return true;
}
inline std::string assetUrl(const std::string& tag, const std::string& name) {
  return safeName(tag) && safeName(name) ? std::string(kDownload) + tag + "/" + name : "";
}
inline bool allowedUrl(const std::string& url, bool assetRedirect = false) {
  if (url == kApi) return true;
  Url parsed; if (!splitUrl(url, parsed)) return false;
  if (parsed.host == "github.com" && url.compare(0, strlen(kDownload), kDownload) == 0) {
    const std::string rest = url.substr(strlen(kDownload));
    const size_t slash = rest.find('/');
    return slash != std::string::npos && safeName(rest.substr(0, slash)) && safeName(rest.substr(slash + 1));
  }
  if (!assetRedirect || (parsed.host != "release-assets.githubusercontent.com" && parsed.host != "objects.githubusercontent.com")) return false;
  if (parsed.path.compare(0, strlen(kAssetPath), kAssetPath)) return false;
  const size_t query = parsed.path.find('?');
  const std::string object = parsed.path.substr(strlen(kAssetPath), query == std::string::npos ? query : query - strlen(kAssetPath));
  return safeName(object, 96); // Exact repository ID, not another repository's CDN objects.
}
inline bool ipv4(const char* text, uint32_t& result) {
  if (!text) return false;
  result = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (*text < '0' || *text > '9') return false;
    unsigned value = 0, digits = 0;
    while (*text >= '0' && *text <= '9') {
      value = value * 10 + unsigned(*text++ - '0');
      if (++digits > 3 || value > 255) return false;
    }
    result = (result << 8) | value;
    if (i < 3 && *text++ != '.') return false;
  }
  return *text == 0;
}
inline bool unicast(uint32_t ip) { return ip && (ip >> 24) < 224 && (ip >> 24) != 127 && (ip >> 24) != 0; }
struct Network {
  bool manual = false;
  uint32_t ip = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
  bool valid() const {
    if (!manual) return true;
    const uint32_t inverse = ~mask;
    return mask && inverse >= 3 && !(inverse & (inverse + 1)) &&
        unicast(ip) && unicast(gateway) && unicast(dns1) && (!dns2 || unicast(dns2)) &&
        (ip & mask) == (gateway & mask) && ip != gateway &&
        (ip & inverse) && (ip & inverse) != inverse &&
        (gateway & inverse) && (gateway & inverse) != inverse;
  }
};
inline bool validPassword(const std::string& password) {
  if (password.empty() || (password.size() >= 8 && password.size() <= 63)) return true;
  if (password.size() != 64) return false;
  for (char c : password) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  return true;
}
struct Release {
  std::string tag, manifestUrl, appUrl, appFile, assetDigest;
  uint32_t assetBytes = 0;
};
inline bool readRelease(JsonVariantConst doc, Release& release) {
  if (doc["draft"] != false || doc["prerelease"] != false) return false;
  release = {};
  release.tag = doc["tag_name"] | "";
  if (!safeName(release.tag)) return false;
  unsigned count = 0, manifests = 0, apps = 0;
  for (JsonObjectConst asset : doc["assets"].as<JsonArrayConst>()) {
    if (++count > 32) return false;
    const std::string name = asset["name"] | "";
    const std::string url = asset["browser_download_url"] | "";
    if (name == "manifest.json") {
      if (url != assetUrl(release.tag, name) || ++manifests != 1) return false;
      release.manifestUrl = url;
    }
    if (name.size() > 8 && name.compare(name.size() - 8, 8, ".app.bin") == 0) {
      if (url != assetUrl(release.tag, name) || ++apps != 1 || !asset["size"].is<uint32_t>()) return false;
      release.appUrl = url; release.appFile = name;
      release.assetBytes = asset["size"].as<uint32_t>();
      release.assetDigest = asset["digest"] | "";
    }
  }
  return manifests == 1 && apps == 1 && release.assetBytes >= 1024 && release.assetBytes <= kSlotBytes;
}
struct Image { std::string version, file, url, sha256; uint32_t bytes = 0; };
inline bool readManifest(JsonVariantConst doc, const Release& release, Image& image) {
  image = {};
  if (strcmp(doc["product"] | "", "ESP32 Marauder Eternal") ||
      strcmp(doc["hardware"] | "", "Marauder Mini V3") || strcmp(doc["chip"] | "", "ESP32-C5") ||
      strcmp(doc["flash_size"] | "", "8MB") || doc["psram"] != true ||
      strcmp(doc["partition_layout"] | "", "mini-v3-c5-8m-ota-v1")) return false;
  image.version = doc["version"] | ""; Version version;
  if (!version.parse(image.version.c_str()) || image.version[0] == 'v') return false;
  const std::string expected = "Marauder_Eternal_" + image.version + "_MiniV3_ESP32-C5.app.bin";
  if (release.appFile != expected) return false;
  unsigned matches = 0;
  for (JsonObjectConst record : doc["images"].as<JsonArrayConst>()) {
    if (expected != (record["file"] | "")) continue;
    if (++matches != 1 || !record["bytes"].is<uint32_t>()) return false;
    image.bytes = record["bytes"].as<uint32_t>(); image.sha256 = record["sha256"] | "";
    if (image.bytes != release.assetBytes || image.bytes < 1024 || image.bytes > kSlotBytes || image.sha256.size() != 64) return false;
    for (char c : image.sha256) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  if (matches != 1 || (!release.assetDigest.empty() && release.assetDigest != "sha256:" + image.sha256)) return false;
  image.file = expected; image.url = release.appUrl;
  return true;
}
} }
