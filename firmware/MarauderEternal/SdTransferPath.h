#pragma once

#include <stddef.h>

constexpr size_t SD_TRANSFER_MAX_PATH_BYTES = 512;

// Decode a hex-encoded UTF-8 SD path without allocating. The output is always
// NUL terminated on success.
bool decodeSdPathHex(const char* encoded, char* output, size_t output_size);

// Only absolute file paths are accepted. Empty components, traversal, control
// characters, and backslashes are rejected before the path reaches the SD API.
bool isSafeSdFilePath(const char* path);

// Serial uploads are deliberately limited to direct Evil Portal templates.
// The menu's file index is non-recursive and recognizes lowercase .html.
bool isEvilPortalHtmlUploadPath(const char* path);

// Validate a complete hexadecimal SHA-256 digest without allocating.
bool isSha256Hex(const char* digest);
