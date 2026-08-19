#ifndef DRONE_REMOTE_ID_H
#define DRONE_REMOTE_ID_H

#include <stdint.h>

namespace DroneRemoteID {

constexpr uint8_t CAPTURED_MESSAGE_TYPES = 6;
constexpr uint8_t CAPTURED_MESSAGE_SIZE = 25;
constexpr uint8_t CAPTURE_METHOD_WIFI = 1;
constexpr uint8_t CAPTURE_METHOD_BLE = 2;

struct CapturedDrone {
  char address[18];
  char id[21];
  uint8_t messages[CAPTURED_MESSAGE_TYPES][CAPTURED_MESSAGE_SIZE];
  uint32_t lastSeen;
  int8_t rssi;
  uint8_t channel;
  uint8_t methods;
  uint8_t messageMask;
};

// Passively receives and decodes ASTM/OpenDroneID broadcasts carried in BLE
// service data, Wi-Fi Beacon vendor IEs, and Wi-Fi NAN service descriptors.
void run();

// Shows the retained results from the most recent scan and copies the selected
// drone's last observed message of each type into `target`.
bool selectCapturedForSpoof(CapturedDrone& target);

}  // namespace DroneRemoteID

#endif
