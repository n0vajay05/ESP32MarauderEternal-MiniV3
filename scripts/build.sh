#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SKETCH_DIR="${PROJECT_DIR}/firmware/MarauderEternal"
LIBRARY_DIR="${PROJECT_DIR}/libraries"
BUILD_DIR="${1:-${PROJECT_DIR}/build}"
CACHE_DIR="${BUILD_DIR}/cache"
FQBN="esp32:esp32:esp32c5:FlashSize=8M,PartitionScheme=custom,PSRAM=enabled"

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

"${ARDUINO_CLI}" compile \
  --fqbn "${FQBN}" \
  --libraries "${LIBRARY_DIR}" \
  --build-path "${CACHE_DIR}" \
  --build-property "compiler.cpp.extra_flags=-DMARAUDER_MINI_V3" \
  --build-property "compiler.c.extra_flags=-DMARAUDER_MINI_V3" \
  --build-property "compiler.c.elf.extra_flags=-Wl,-zmuldefs" \
  --output-dir "${BUILD_DIR}" \
  "${SKETCH_DIR}"

echo "Build complete: ${BUILD_DIR}/MarauderEternal.ino.bin"
