#ifndef WIFI_CAMERA_DETECTOR_H
#define WIFI_CAMERA_DETECTOR_H

#include <stdint.h>

namespace WiFiCameraDetector {

constexpr uint8_t MAX_DEAUTH_TARGETS = 32;

struct DeauthLink {
  uint8_t camera[6];
  uint8_t bssid[6];
  uint8_t destination[6];
  uint8_t channel;
  uint32_t txSuccess;
  uint32_t txFailed;
};

struct DeauthTarget {
  DeauthLink links[MAX_DEAUTH_TARGETS];
  uint8_t count;
  char vendor[12];
  bool sameBrand;
  bool includesApScope;
  bool apOnly;
};

// Passively identifies Wi-Fi camera candidates from camera-specific OUIs,
// recognizable camera-brand SSIDs, and clients of fingerprinted hidden Eufy
// HomeBase networks. It does not transmit or claim that a match proves the
// device's role.
void run();

// Passively discovers camera candidates and asks the operator to select either
// one candidate or all link-ready candidates of the same detected brand, then
// explicitly confirm the scope. No frame is transmitted by this function.
bool selectDeauthTarget(DeauthTarget& target);

}  // namespace WiFiCameraDetector

#endif
