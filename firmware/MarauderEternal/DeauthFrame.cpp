#include "DeauthFrame.h"

#include <string.h>

bool setDeauthFrameAddresses(uint8_t* frame, size_t frame_length,
                             const uint8_t destination[6],
                             const uint8_t source[6],
                             const uint8_t bssid[6]) {
  if (frame == nullptr || frame_length < DEAUTH_FRAME_HEADER_SIZE ||
      destination == nullptr || source == nullptr || bssid == nullptr)
    return false;

  memcpy(frame + 4, destination, 6);
  memcpy(frame + 10, source, 6);
  memcpy(frame + 16, bssid, 6);
  return true;
}

bool isBroadcastAddress(const uint8_t address[6]) {
  static constexpr uint8_t broadcast[6] = {
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  return address != nullptr && memcmp(address, broadcast, sizeof(broadcast)) == 0;
}
