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
  "${PROJECT_DIR}/firmware/MarauderEternal/DeauthFrame.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/DisplayLine.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/RsnCapabilities.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/SdTransferPath.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/UtcTime.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/WdgResponse.cpp" \
  -o "${TEST_DIR}/test_helpers"

"${TEST_DIR}/test_helpers"
g++ -std=c++17 -Wall -Wextra -Werror \
  -I"${PROJECT_DIR}/firmware/MarauderEternal" \
  "${PROJECT_DIR}/tests/native/test_reliability.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/MemoryGuard.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/IPv4Range.cpp" \
  -o "${TEST_DIR}/test_reliability"

"${TEST_DIR}/test_reliability"
echo "Native helper tests passed"
g++ -std=c++17 -Wall -Wextra -Werror \
  -I"${PROJECT_DIR}/firmware/MarauderEternal" \
  -I"${PROJECT_DIR}/libraries/ArduinoJson/src" \
  "${PROJECT_DIR}/tests/native/test_saved_wifi.cpp" \
  -o "${TEST_DIR}/test_saved_wifi"
"${TEST_DIR}/test_saved_wifi"
g++ -std=c++17 -Wall -Wextra -Werror \
  -I"${PROJECT_DIR}/firmware/MarauderEternal" \
  -I"${PROJECT_DIR}/libraries/ArduinoJson/src" \
  "${PROJECT_DIR}/tests/native/test_geofences.cpp" \
  -o "${TEST_DIR}/test_geofences"
"${TEST_DIR}/test_geofences"
g++ -std=c++17 -Wall -Wextra -Werror \
  -I"${PROJECT_DIR}/firmware/MarauderEternal" \
  "${PROJECT_DIR}/tests/native/test_drone_tracking.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/DroneTracking.cpp" \
  -o "${TEST_DIR}/test_drone_tracking"
"${TEST_DIR}/test_drone_tracking"
g++ -std=c++17 -Wall -Wextra -Werror \
  -I"${PROJECT_DIR}/firmware/MarauderEternal" \
  "${PROJECT_DIR}/tests/native/test_ibeacon.cpp" \
  -o "${TEST_DIR}/test_ibeacon"
"${TEST_DIR}/test_ibeacon"
g++ -std=c++17 -Wall -Wextra -Werror \
  -I"${PROJECT_DIR}/firmware/MarauderEternal" \
  "${PROJECT_DIR}/tests/native/test_gps_tracker.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/GpsTrackerStats.cpp" \
  "${PROJECT_DIR}/firmware/MarauderEternal/UtcTime.cpp" \
  -o "${TEST_DIR}/test_gps_tracker"
"${TEST_DIR}/test_gps_tracker"
g++ -std=c++17 -Wall -Wextra -Werror \
  -I"${PROJECT_DIR}/firmware/MarauderEternal" \
  "${PROJECT_DIR}/tests/native/test_fox_hunt.cpp" \
  -o "${TEST_DIR}/test_fox_hunt"
"${TEST_DIR}/test_fox_hunt"
g++ -std=c++17 -Wall -Wextra -Werror \
  -I"${PROJECT_DIR}/firmware/MarauderEternal" \
  "${PROJECT_DIR}/tests/native/test_tracker_diagnostics.cpp" \
  -o "${TEST_DIR}/test_tracker_diagnostics"
"${TEST_DIR}/test_tracker_diagnostics"
