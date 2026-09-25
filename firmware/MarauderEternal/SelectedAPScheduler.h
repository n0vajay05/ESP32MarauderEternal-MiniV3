#pragma once

#include <stdint.h>

// A zero required_channel allows every selected radio, regardless of SSID or
// band. A nonzero channel temporarily defers the others without deselecting them.
template <typename GetAccessPoint>
int nextSelectedAP(uint16_t count, uint16_t& cursor, uint8_t required_channel,
                   GetAccessPoint get_access_point) {
  if (count == 0) {
    cursor = 0;
    return -1;
  }
  if (cursor >= count)
    cursor = 0;
  for (uint16_t attempt = 0; attempt < count; ++attempt) {
    const uint16_t index = cursor;
    cursor = (cursor + 1) % count;
    const auto ap = get_access_point(index);
    if (ap.selected && (required_channel == 0 || ap.channel == required_channel))
      return index;
  }
  return -1;
}
