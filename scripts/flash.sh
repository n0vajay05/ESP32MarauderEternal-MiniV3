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
MANIFEST="${RELEASE_DIR}/manifest.json"
APP_FILENAME="$(python3 -c 'import json,sys; print(next(image["file"] for image in json.load(open(sys.argv[1]))["images"] if image["offset"] == "0x10000"))' "${MANIFEST}")"
APP_IMAGE="${RELEASE_DIR}/${APP_FILENAME}"

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

(cd "${RELEASE_DIR}" && sha256sum --check SHA256SUMS)
"${ESPTOOL}" --chip esp32c5 --port "${PORT}" chip-id
"${ESPTOOL}" --chip esp32c5 --port "${PORT}" --baud 460800 \
  --before default-reset --after hard-reset \
  write-flash 0x10000 "${APP_IMAGE}" 0x400000 "${APP_IMAGE}"
