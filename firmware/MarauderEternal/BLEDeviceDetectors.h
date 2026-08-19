#pragma once

#include <Arduino.h>

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

void run(DetectorType detector);

}  // namespace BLEDeviceDetectors
