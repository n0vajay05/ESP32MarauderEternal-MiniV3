#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
VENV_DIR="${PROJECT_DIR}/.build/gui-venv-linux"
WORK_DIR="${PROJECT_DIR}/.build/pyinstaller-linux"
DIST_DIR="${PROJECT_DIR}/dist/linux"

python3 -m venv "${VENV_DIR}"
"${VENV_DIR}/bin/python" -m pip install --upgrade pip
"${VENV_DIR}/bin/python" -m pip install -r "${PROJECT_DIR}/flasher/requirements-build.txt"

cd "${PROJECT_DIR}"
"${VENV_DIR}/bin/python" -m unittest discover -v
"${VENV_DIR}/bin/pyinstaller" \
  --clean \
  --noconfirm \
  --distpath "${DIST_DIR}" \
  --workpath "${WORK_DIR}" \
  "${PROJECT_DIR}/flasher/MarauderEternalFlasher.spec"

"${DIST_DIR}/MarauderEternalFlasher" --self-test
sha256sum "${DIST_DIR}/MarauderEternalFlasher" > "${DIST_DIR}/SHA256SUMS"
echo "Linux application: ${DIST_DIR}/MarauderEternalFlasher"
