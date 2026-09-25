#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t MAX_SSID_BEACON_SIZE = 89;

// Build an open 2.4 GHz beacon, without FCS. Returns the exact transmit length,
// or zero for invalid input. By default each SSID gets a stable local BSSID
// derived from the interface MAC; use_interface_mac preserves that MAC.
size_t buildSsidBeaconFrame(uint8_t* frame, size_t capacity, const char* ssid,
                          const uint8_t* interface_mac, uint8_t channel,
                          uint64_t timestamp_us, bool use_interface_mac = false);

bool setBeaconFrameChannel(
  uint8_t* frame,
  size_t frame_size,
  size_t ssid_length,
  uint8_t channel
);
