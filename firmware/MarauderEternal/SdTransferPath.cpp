#include "SdTransferPath.h"

#include <string.h>

namespace {
int hexValue(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}
}

bool decodeSdPathHex(const char* encoded, char* output, size_t output_size) {
  if (encoded == nullptr || output == nullptr || output_size < 2)
    return false;

  const size_t encoded_length = strlen(encoded);
  if (encoded_length == 0 || (encoded_length & 1U) != 0)
    return false;

  const size_t decoded_length = encoded_length / 2;
  if (decoded_length >= output_size ||
      decoded_length >= SD_TRANSFER_MAX_PATH_BYTES)
    return false;

  for (size_t index = 0; index < decoded_length; ++index) {
    const int high = hexValue(encoded[index * 2]);
    const int low = hexValue(encoded[index * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    const char decoded = static_cast<char>((high << 4) | low);
    if (decoded == '\0')
      return false;
    output[index] = decoded;
  }
  output[decoded_length] = '\0';
  return true;
}

bool isSafeSdFilePath(const char* path) {
  if (path == nullptr || path[0] != '/' || path[1] == '\0')
    return false;

  const size_t length = strlen(path);
  if (length >= SD_TRANSFER_MAX_PATH_BYTES || path[length - 1] == '/')
    return false;

  const char* component = path + 1;
  for (const char* cursor = component;; ++cursor) {
    const unsigned char value = static_cast<unsigned char>(*cursor);
    if (value == '\\' || (value != '\0' && value < 0x20))
      return false;

    if (value == '/' || value == '\0') {
      const size_t component_length = static_cast<size_t>(cursor - component);
      if (component_length == 0 ||
          (component_length == 1 && component[0] == '.') ||
          (component_length == 2 && component[0] == '.' &&
           component[1] == '.'))
        return false;
      if (value == '\0')
        break;
      component = cursor + 1;
    }
  }
  return true;
}

bool isEvilPortalHtmlUploadPath(const char* path) {
  static constexpr char prefix[] = "/evil_portal/html/";
  static constexpr char suffix[] = ".html";
  static constexpr size_t max_filename_bytes = 240;

  if (!isSafeSdFilePath(path) || strncmp(path, prefix, sizeof(prefix) - 1) != 0)
    return false;

  const char* filename = path + sizeof(prefix) - 1;
  const size_t filename_length = strlen(filename);
  const size_t suffix_length = sizeof(suffix) - 1;
  if (filename_length <= suffix_length ||
      filename_length > max_filename_bytes || strchr(filename, '/') != nullptr)
    return false;

  return strcmp(filename + filename_length - suffix_length, suffix) == 0;
}

bool isSha256Hex(const char* digest) {
  if (digest == nullptr || strlen(digest) != 64)
    return false;
  for (size_t index = 0; index < 64; ++index) {
    const char value = digest[index];
    if (!((value >= '0' && value <= '9') ||
          (value >= 'a' && value <= 'f') ||
          (value >= 'A' && value <= 'F')))
      return false;
  }
  return true;
}
