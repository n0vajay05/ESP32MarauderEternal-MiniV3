#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SKETCH_DIR="${PROJECT_DIR}/firmware/MarauderEternal"
LIBRARY_DIR="${PROJECT_DIR}/libraries"
BUILD_DIR="${1:-${PROJECT_DIR}/build}"
CACHE_DIR="${BUILD_DIR}/cache"
FQBN="esp32:esp32:esp32c5:FlashSize=8M,PartitionScheme=custom,PSRAM=enabled"
EXPECTED_CORE_VERSION="3.3.4"
APPLICATION_PARTITION_SIZE=$((0x3c0000))

if [[ -n "${ARDUINO_CLI_BIN:-}" ]]; then
  ARDUINO_CLI="${ARDUINO_CLI_BIN}"
elif command -v arduino-cli >/dev/null 2>&1; then
  ARDUINO_CLI="$(command -v arduino-cli)"
elif [[ -x /tmp/arduino-cli-bin/arduino-cli ]]; then
  ARDUINO_CLI=/tmp/arduino-cli-bin/arduino-cli
else
  echo "arduino-cli was not found. Set ARDUINO_CLI_BIN to its absolute path." >&2
  exit 1
fi

mkdir -p "${BUILD_DIR}" "${CACHE_DIR}"

INSTALLED_CORE_VERSION="$("${ARDUINO_CLI}" core list | awk '$1 == "esp32:esp32" {print $2}')"
if [[ "${INSTALLED_CORE_VERSION}" != "${EXPECTED_CORE_VERSION}" ]]; then
  echo "ESP32 Arduino core ${EXPECTED_CORE_VERSION} is required; found ${INSTALLED_CORE_VERSION:-none}." >&2
  exit 1
fi

"${ARDUINO_CLI}" compile \
  --warnings all \
  --fqbn "${FQBN}" \
  --libraries "${LIBRARY_DIR}" \
  --build-path "${CACHE_DIR}" \
  --build-property "compiler.cpp.extra_flags=-DMARAUDER_MINI_V3" \
  --build-property "compiler.c.extra_flags=-DMARAUDER_MINI_V3" \
  --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=ieee80211_raw_frame_sanity_check" \
  --output-dir "${BUILD_DIR}" \
  "${SKETCH_DIR}"

APP_IMAGE="${BUILD_DIR}/MarauderEternal.ino.bin"
APP_SIZE="$(stat -c '%s' "${APP_IMAGE}")"
if (( APP_SIZE > APPLICATION_PARTITION_SIZE )); then
  echo "Application is ${APP_SIZE} bytes and exceeds the ${APPLICATION_PARTITION_SIZE}-byte OTA slot." >&2
  exit 1
fi

echo "Build complete: ${APP_IMAGE} (${APP_SIZE}/${APPLICATION_PARTITION_SIZE} bytes)"
