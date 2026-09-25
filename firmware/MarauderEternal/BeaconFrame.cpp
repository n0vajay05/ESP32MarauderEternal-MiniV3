#include "BeaconFrame.h"

#include <string.h>

size_t buildSsidBeaconFrame(uint8_t* frame, size_t capacity, const char* ssid,
                          const uint8_t* interface_mac, uint8_t channel,
                          uint64_t timestamp_us, bool use_interface_mac) {
  if (!frame || !ssid || !interface_mac || channel < 1 || channel > 11 ||
      (interface_mac[0] & 0x01))
    return 0;

  size_t ssid_length = 0;
  while (ssid_length < 33 && ssid[ssid_length] != '\0')
    ++ssid_length;
  if (ssid_length == 0 || ssid_length > 32)
    return 0;

  const uint8_t tags[] = {
      0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c,
      0x03, 0x01, channel,               // DS parameter: actual radio channel
      0x05, 0x04, 0x00, 0x01, 0x00, 0x00 // TIM: no buffered traffic
  };
  const size_t frame_length = 38 + ssid_length + sizeof(tags);
  if (capacity < frame_length)
    return 0;

  // Start fresh: a shorter SSID must never retain tags from the previous one.
  memset(frame, 0, frame_length);
  frame[0] = 0x80;
  memset(frame + 4, 0xff, 6);
  uint8_t bssid[6];
  memcpy(bssid, interface_mac, sizeof(bssid));
  if (!use_interface_mac) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < ssid_length; ++i)
      hash = (hash ^ static_cast<uint8_t>(ssid[i])) * 16777619u;
    for (size_t i = 0; i < 4; ++i)
      bssid[2 + i] ^= static_cast<uint8_t>(hash >> (8 * i));
    bssid[0] = (bssid[0] & 0xfe) | 0x02; // locally administered, unicast
  }
  memcpy(frame + 10, bssid, sizeof(bssid));
  memcpy(frame + 16, bssid, sizeof(bssid));
  // Sequence control is supplied by esp_wifi_80211_tx(..., true).
  for (size_t i = 0; i < 8; ++i)
    frame[24 + i] = static_cast<uint8_t>(timestamp_us >> (8 * i));
  frame[32] = 0x64; // 100 TU beacon interval
  frame[34] = 0x21; // ESS + short preamble; no privacy bit without an RSN IE
  frame[37] = static_cast<uint8_t>(ssid_length);
  memcpy(frame + 38, ssid, ssid_length);
  memcpy(frame + 38 + ssid_length, tags, sizeof(tags));
  return frame_length;
}

bool setBeaconFrameChannel(
  uint8_t* frame,
  size_t frame_size,
  size_t ssid_length,
  uint8_t channel
) {
  const size_t channel_index = 50 + ssid_length;
  if ((frame == nullptr) || (channel_index >= frame_size))
    return false;

  frame[channel_index] = channel;
  return true;
}
