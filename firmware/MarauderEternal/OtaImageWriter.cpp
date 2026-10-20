#include "OtaImageWriter.h"

namespace marauder { namespace ota {
ImageWriter::ImageWriter() { mbedtls_sha256_init(&hash_); }
ImageWriter::~ImageWriter() { abort(); mbedtls_sha256_free(&hash_); }
void ImageWriter::abort() { if (handle_) esp_ota_abort(handle_); handle_ = 0; }
bool ImageWriter::begin(const Image& image) {
  if (handle_ || image.bytes < 1024 || image.bytes > kSlotBytes || image.sha256.size() != 64) return false;
  Version version; if (!version.parse(image.version.c_str())) return false;
  const esp_partition_t* running = esp_ota_get_running_partition();
  target_ = esp_ota_get_next_update_partition(nullptr);
  if (!target_ || target_ == running || target_->size != kSlotBytes || image.bytes > target_->size) return false;
  image_ = image;
  if (esp_ota_begin(target_, image.bytes, &handle_) != ESP_OK) return false;
  hashing_ = mbedtls_sha256_starts(&hash_, 0) == 0;
  if (!hashing_) abort();
  return hashing_;
}
bool ImageWriter::push(const uint8_t* bytes, size_t size) {
  if (!handle_ || !hashing_ || !size || size > image_.bytes - received_) return false;
  if (!received_ && (size < 24 || bytes[0] != 0xe9 || bytes[12] != 0x17 || bytes[13] != 0)) return false;
  for (size_t i = 0; i < size; ++i) scanner_.push(bytes[i]);
  if (mbedtls_sha256_update(&hash_, bytes, size) != 0 || esp_ota_write(handle_, bytes, size) != ESP_OK) return false;
  received_ += size; return true;
}
bool ImageWriter::finish(bool activate) {
  uint8_t digest[32]{}; char hex[65]{};
  if (!handle_ || !hashing_ || received_ != image_.bytes || mbedtls_sha256_finish(&hash_, digest) != 0) { abort(); return false; }
  hashing_ = false;
  for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  if (image_.sha256 != hex || !scanner_.found() ||
      !MarauderFirmware::metadataMatches(scanner_.metadata(), MarauderFirmware::currentMetadata()) ||
      "v" + image_.version != scanner_.metadata().version) { abort(); return false; }
  const esp_ota_handle_t handle = handle_; handle_ = 0; // esp_ota_end always releases its handle.
  if (esp_ota_end(handle) != ESP_OK) return false;
  return !activate || esp_ota_set_boot_partition(target_) == ESP_OK;
}
} }
