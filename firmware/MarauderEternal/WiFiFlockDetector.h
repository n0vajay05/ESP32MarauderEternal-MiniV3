#ifndef WIFI_FLOCK_DETECTOR_H
#define WIFI_FLOCK_DETECTOR_H

#include <stddef.h>
#include <stdint.h>

#include <esp_wifi_types.h>

namespace WiFiFlockDetector {

enum class DeviceClass : uint8_t {
  FlockAlpr,
  Raven,
  SoundThinking
};

enum class Confidence : uint8_t {
  Low,
  Medium,
  High
};

struct Match {
  uint8_t mac[6]{};
  int8_t rssi = 0;
  uint8_t channel = 0;
  DeviceClass deviceClass = DeviceClass::FlockAlpr;
  Confidence confidence = Confidence::Low;
  const char* method = "";
};

// Targeted, receive-only Wi-Fi classification shared by the standalone Flock
// scanner and wardriving. It recognizes Flock ALPR/Raven infrastructure and
// SoundThinking/ShotSpotter indicators without matching generic camera brands.
bool matchPacket(const wifi_promiscuous_pkt_t* packet,
                 wifi_promiscuous_pkt_type_t type, Match& match);

const char* classLabel(DeviceClass deviceClass);
const char* confidenceLabel(Confidence confidence);

// Shared weighted channel plan used by standalone and wardrive Flock scans.
const uint8_t* scanChannelPlan(size_t& count);
uint16_t channelDwellMs(uint8_t channel);

// Interactive, receive-only detector for the Mini V3 screen and joystick.
void run();

}  // namespace WiFiFlockDetector

#endif
