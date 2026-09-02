#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t DEAUTH_FRAME_HEADER_SIZE = 24;

bool setDeauthFrameAddresses(uint8_t* frame, size_t frame_length,
                             const uint8_t destination[6],
                             const uint8_t source[6],
                             const uint8_t bssid[6]);

bool isBroadcastAddress(const uint8_t address[6]);
