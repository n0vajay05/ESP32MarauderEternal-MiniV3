#include "FirmwareMetadata.h"
#include "configs.h"

namespace MarauderFirmware {
namespace {

constexpr uint8_t METADATA_MAGIC[METADATA_MAGIC_SIZE] = {
  'M', 'R', 'D', 'R', 'F', 'W', 'I', 'D'
};

constexpr uint8_t METADATA_END_MAGIC[METADATA_MAGIC_SIZE] = {
  'D', 'I', 'W', 'F', 'R', 'D', 'R', 'M'
};

#if defined(CONFIG_IDF_TARGET_ESP32C5)
  #define MARAUDER_CHIP_ID "esp32c5"
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  #define MARAUDER_CHIP_ID "esp32c6"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  #define MARAUDER_CHIP_ID "esp32s3"
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  #define MARAUDER_CHIP_ID "esp32s2"
#elif defined(CONFIG_IDF_TARGET_ESP32)
  #define MARAUDER_CHIP_ID "esp32"
#else
  #define MARAUDER_CHIP_ID "native"
#endif

// This identity deliberately distinguishes Eternal Mini V3 firmware from
// upstream Marauder Mini V3 images that use the same ESP32-C5 hardware.
const Metadata CURRENT_METADATA = {
  {'M', 'R', 'D', 'R', 'F', 'W', 'I', 'D'},
  METADATA_SCHEMA_VERSION,
  {0, 0, 0},
  "Marauder Eternal Mini V3",
  MARAUDER_CHIP_ID,
  MARAUDER_VERSION,
  MARAUDER_PARTITION_LAYOUT,
  {'D', 'I', 'W', 'F', 'R', 'D', 'R', 'M'}
};

}  // namespace

MetadataScanner::MetadataScanner()
  : candidate{}, magic_index(0), capture_index(0), capturing(false), valid(false) {}

bool MetadataScanner::push(uint8_t byte) {
  if (valid)
    return true;

  if (!capturing) {
    if (byte == METADATA_MAGIC[magic_index]) {
      candidate.magic[magic_index++] = byte;
      if (magic_index == METADATA_MAGIC_SIZE) {
        capturing = true;
        capture_index = METADATA_MAGIC_SIZE;
      }
    }
    else {
      magic_index = byte == METADATA_MAGIC[0] ? 1 : 0;
      if (magic_index == 1)
        candidate.magic[0] = byte;
    }
    return false;
  }

  reinterpret_cast<uint8_t*>(&candidate)[capture_index++] = byte;
  if (capture_index < sizeof(Metadata))
    return false;

  valid = validMetadata(candidate);
  if (!valid) {
    // Recover a nested or overlapping magic prefix iteratively. The previous
    // recursive replay could recurse repeatedly on a crafted image.
    uint8_t replay[sizeof(Metadata)];
    memcpy(replay, &candidate, sizeof(replay));
    candidate = {};
    magic_index = 0;
    capture_index = 0;
    capturing = false;

    size_t nested_magic = sizeof(Metadata);
    for (size_t start = 1;
         start + METADATA_MAGIC_SIZE <= sizeof(Metadata); start++) {
      if (memcmp(replay + start, METADATA_MAGIC, METADATA_MAGIC_SIZE) == 0) {
        nested_magic = start;
        break;
      }
    }

    if (nested_magic < sizeof(Metadata)) {
      const size_t remaining = sizeof(Metadata) - nested_magic;
      memcpy(&candidate, replay + nested_magic, remaining);
      magic_index = METADATA_MAGIC_SIZE;
      capture_index = remaining;
      capturing = true;
    }
    else {
      size_t suffix = METADATA_MAGIC_SIZE - 1;
      while (suffix > 0 &&
             memcmp(replay + sizeof(Metadata) - suffix,
                    METADATA_MAGIC, suffix) != 0)
        suffix--;
      if (suffix > 0) {
        memcpy(candidate.magic, METADATA_MAGIC, suffix);
        magic_index = suffix;
      }
    }
  }
  return valid;
}

bool MetadataScanner::found() const {
  return valid;
}

const Metadata& MetadataScanner::metadata() const {
  return candidate;
}

bool validMetadata(const Metadata& metadata) {
  return memcmp(metadata.magic, METADATA_MAGIC, METADATA_MAGIC_SIZE) == 0 &&
         metadata.schema_version == METADATA_SCHEMA_VERSION &&
         metadata.hardware[0] != '\0' &&
         metadata.chip[0] != '\0' &&
         metadata.version[0] != '\0' &&
         metadata.partition_layout[0] != '\0' &&
         memchr(metadata.hardware, '\0', sizeof(metadata.hardware)) != nullptr &&
         memchr(metadata.chip, '\0', sizeof(metadata.chip)) != nullptr &&
         memchr(metadata.version, '\0', sizeof(metadata.version)) != nullptr &&
         memchr(metadata.partition_layout, '\0',
                sizeof(metadata.partition_layout)) != nullptr &&
         memcmp(metadata.end_magic, METADATA_END_MAGIC,
                METADATA_MAGIC_SIZE) == 0;
}

bool metadataMatches(const Metadata& candidate, const Metadata& current) {
  return validMetadata(candidate) && validMetadata(current) &&
         strncmp(candidate.hardware, current.hardware,
                 sizeof(current.hardware)) == 0 &&
         strncmp(candidate.chip, current.chip, sizeof(current.chip)) == 0 &&
         strncmp(candidate.partition_layout, current.partition_layout,
                 sizeof(current.partition_layout)) == 0;
}

const Metadata& currentMetadata() {
  return CURRENT_METADATA;
}

}  // namespace MarauderFirmware
