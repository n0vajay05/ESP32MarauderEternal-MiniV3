#pragma once

#include <stddef.h>
#include <stdint.h>

enum PmfStatus : uint8_t {
  PMF_STATUS_UNKNOWN = 0,
  PMF_STATUS_NONE,
  PMF_STATUS_CAPABLE,
  PMF_STATUS_REQUIRED
};

// Parse 802.11 tagged parameters (information elements), beginning with the
// first element ID. PMF flags are bits 6 (required) and 7 (capable) of the
// little-endian RSN Capabilities field.
PmfStatus parsePmfStatus(const uint8_t* information_elements, size_t length);
