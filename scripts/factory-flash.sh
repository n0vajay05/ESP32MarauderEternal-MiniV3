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
FACTORY_FILENAME="Marauder_Eternal_1.14.3_MiniV3_ESP32-C5.factory.bin"
FACTORY_IMAGE="${RELEASE_DIR}/${FACTORY_FILENAME}"
FACTORY_SHA256="8904dccf000fcf4f09fd3b28e14bdbcbd84d19e1b93d3a096d405d794c4e2524"

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

if [[ ! -f "${FACTORY_IMAGE}" ]]; then
  echo "Factory image was not found: ${FACTORY_IMAGE}" >&2
  exit 1
fi

printf '%s  %s\n' "${FACTORY_SHA256}" "${FACTORY_FILENAME}" | \
  (cd "${RELEASE_DIR}" && sha256sum --check -)
"${ESPTOOL}" --chip esp32c5 --port "${PORT}" chip-id
"${ESPTOOL}" --chip esp32c5 --port "${PORT}" --baud 460800 \
  --before default-reset --after hard-reset \
  write-flash 0x0 "${FACTORY_IMAGE}"
