#include "RsnCapabilities.h"

namespace {

bool readLe16(const uint8_t* data, size_t length, size_t offset,
              uint16_t& value) {
  if (data == nullptr || offset + 2 > length)
    return false;
  value = static_cast<uint16_t>(data[offset]) |
          (static_cast<uint16_t>(data[offset + 1]) << 8);
  return true;
}

}  // namespace

PmfStatus parsePmfStatus(const uint8_t* information_elements, size_t length) {
  if (information_elements == nullptr)
    return PMF_STATUS_UNKNOWN;

  size_t position = 0;
  while (position + 2 <= length) {
    const uint8_t element_id = information_elements[position];
    const size_t element_length = information_elements[position + 1];
    position += 2;
    if (element_length > length - position)
      return PMF_STATUS_UNKNOWN;

    if (element_id != 48) {
      position += element_length;
      continue;
    }

    const uint8_t* rsn = information_elements + position;
    size_t offset = 0;
    // Version (2), group cipher (4), then the pairwise suite count.
    if (element_length < 8)
      return PMF_STATUS_UNKNOWN;
    offset = 6;

    uint16_t pairwise_count = 0;
    if (!readLe16(rsn, element_length, offset, pairwise_count))
      return PMF_STATUS_UNKNOWN;
    offset += 2;
    const size_t pairwise_bytes = static_cast<size_t>(pairwise_count) * 4;
    if (pairwise_bytes > element_length - offset)
      return PMF_STATUS_UNKNOWN;
    offset += pairwise_bytes;

    uint16_t akm_count = 0;
    if (!readLe16(rsn, element_length, offset, akm_count))
      return PMF_STATUS_UNKNOWN;
    offset += 2;
    const size_t akm_bytes = static_cast<size_t>(akm_count) * 4;
    if (akm_bytes > element_length - offset)
      return PMF_STATUS_UNKNOWN;
    offset += akm_bytes;

    // RSN Capabilities is optional. Its absence means neither PMF flag was
    // advertised, not that the scan failed.
    if (offset == element_length)
      return PMF_STATUS_NONE;

    uint16_t capabilities = 0;
    if (!readLe16(rsn, element_length, offset, capabilities))
      return PMF_STATUS_UNKNOWN;
    if ((capabilities & (1U << 6)) != 0)
      return PMF_STATUS_REQUIRED;
    if ((capabilities & (1U << 7)) != 0)
      return PMF_STATUS_CAPABLE;
    return PMF_STATUS_NONE;
  }

  return position == length ? PMF_STATUS_NONE : PMF_STATUS_UNKNOWN;
}
