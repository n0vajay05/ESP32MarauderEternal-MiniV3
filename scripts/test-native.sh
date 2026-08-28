#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
TEST_DIR="$(mktemp -d /tmp/marauder-eternal-native-tests.XXXXXX)"
trap 'rm -rf -- "${TEST_DIR}"' EXIT

g++ -std=c++17 -Wall -Wextra -Werror \
  -I"${PROJECT_DIR}/firmware/MarauderEternal" \
  "${PROJECT_DIR}/tests/native/test_helpers.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/BeaconFrame.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/DisplayLine.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/WdgResponse.cpp" \
  -o "${TEST_DIR}/test_helpers"

"${TEST_DIR}/test_helpers"
echo "Native helper tests passed"
