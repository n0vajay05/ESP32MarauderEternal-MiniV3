#pragma once
#include "OtaPolicy.h"
#include "FirmwareMetadata.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

namespace marauder { namespace ota {
// HTTPS and the isolated on-device flash fixture share the exact same image
// validation/activation path. Neither writes to the running application slot.
class ImageWriter {
 public:
  ImageWriter();
  ~ImageWriter();
  bool begin(const Image& image);
  bool push(const uint8_t* bytes, size_t size);
  bool finish(bool activate);
  void abort();
 private:
  Image image_;
  const esp_partition_t* target_ = nullptr;
  esp_ota_handle_t handle_ = 0;
  uint32_t received_ = 0;
  bool hashing_ = false;
  MarauderFirmware::MetadataScanner scanner_;
  mbedtls_sha256_context hash_;
};
} }
