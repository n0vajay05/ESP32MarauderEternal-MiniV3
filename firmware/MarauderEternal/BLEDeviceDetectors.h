#pragma once

#include <Arduino.h>
#include "IBeacon.h"
class TFT_eSprite;

namespace BLEDeviceDetectors {

enum class DetectorType : uint8_t {
  Meshtastic,
  MeshCore,
  SmartTag,
  Tile,
  Axon,
  IBeacon,
  NyanBox,
};

void run(DetectorType detector, uint32_t durationMs = 0);
bool observeIBeaconAdvertisement(const uint8_t* data, size_t length, const char* address, int8_t rssi, uint32_t now);
unsigned iBeaconCount();
bool snapshotIBeacon(unsigned index, marauder::IBeaconObservation& out);
void printIBeacons();
void runIBeaconChecks();
bool renderIBeaconPreview(TFT_eSprite& frame);

}  // namespace BLEDeviceDetectors
