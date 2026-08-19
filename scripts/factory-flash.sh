#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /dev/serial/by-id/YOUR_ESP32_SERIAL_DEVICE" >&2
  exit 2
fi

PORT="$1"
if [[ ! -c "${PORT}" ]]; then
  echo "Serial device is not a character device: ${PORT}" >&2
  exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
RELEASE_DIR="${PROJECT_DIR}/release"
FULL_IMAGE_FILENAME="Marauder_Eternal_1.14.3_MiniV3_ESP32-C5.bin"
FULL_IMAGE="${RELEASE_DIR}/${FULL_IMAGE_FILENAME}"
FULL_IMAGE_SHA256="4933f510e8b7bb6d71ece62285606e30733621bb245f2b32ea478c469a80dec1"

if [[ -n "${ESPTOOL_BIN:-}" ]]; then
  ESPTOOL="${ESPTOOL_BIN}"
elif command -v esptool >/dev/null 2>&1; then
  ESPTOOL="$(command -v esptool)"
elif command -v esptool.py >/dev/null 2>&1; then
  ESPTOOL="$(command -v esptool.py)"
elif [[ -x "${HOME}/.arduino15/packages/esp32/tools/esptool_py/5.1.0/esptool" ]]; then
  ESPTOOL="${HOME}/.arduino15/packages/esp32/tools/esptool_py/5.1.0/esptool"
else
  echo "esptool was not found. Set ESPTOOL_BIN to its absolute path." >&2
  exit 1
fi

if [[ ! -f "${FULL_IMAGE}" ]]; then
  echo "Full-device image was not found: ${FULL_IMAGE}" >&2
  exit 1
fi

printf '%s  %s\n' "${FULL_IMAGE_SHA256}" "${FULL_IMAGE_FILENAME}" | \
  (cd "${RELEASE_DIR}" && sha256sum --check -)
"${ESPTOOL}" --chip esp32c5 --port "${PORT}" chip-id
"${ESPTOOL}" --chip esp32c5 --port "${PORT}" --baud 460800 \
  --before default-reset --after hard-reset \
  write-flash 0x0 "${FULL_IMAGE}"
