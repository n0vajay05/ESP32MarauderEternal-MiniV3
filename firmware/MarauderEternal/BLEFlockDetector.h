#pragma once

#include <Arduino.h>

// Passive, target-specific Flock/Raven/ShotSpotter signatures. The reviewed
// research sources are listed in WiFiFlockDetector.cpp; no generic camera or
// tracker classes are included here.
namespace BLEFlockDetector {

struct Match {
  bool detected = false;
  bool highConfidence = true;
  bool isRaven = false;
  bool isSoundThinking = false;
  bool ravenHasNewGps = false;
  bool ravenHasOldHealth = false;
  bool ravenHasOldLocation = false;
  bool ravenHasPower = false;
  bool ravenHasNetwork = false;
  bool ravenHasUpload = false;
  bool ravenHasError = false;
  uint16_t matchedRavenUuid = 0;
  const char* method = "";
  String serial;
};

Match classify(const uint8_t mac[6], const String& name,
               const uint8_t* payload, size_t payloadLength);
void considerServiceUuid(Match& match, const char* uuid);
const char* ravenFirmware(const Match& match);
const char* classLabel(const Match& match);

}  // namespace BLEFlockDetector
