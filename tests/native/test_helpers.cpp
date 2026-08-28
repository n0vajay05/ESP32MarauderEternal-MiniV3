#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "BeaconFrame.h"
#include "DisplayLine.h"
#include "WdgResponse.h"

int main() {
  uint8_t beacon[64] = {};
  assert(!setBeaconFrameChannel(nullptr, sizeof(beacon), 4, 6));
  assert(!setBeaconFrameChannel(beacon, 54, 4, 6));
  assert(setBeaconFrameChannel(beacon, sizeof(beacon), 4, 11));
  assert(beacon[54] == 11);

  char line[9] = {};
  fitDisplayLine(line, sizeof(line), "GPS");
  assert(strcmp(line, "GPS     ") == 0);
  fitDisplayLine(line, sizeof(line), "1234567890");
  assert(strcmp(line, "12345678") == 0);
  fitDisplayLine(line, sizeof(line), nullptr);
  assert(strcmp(line, "        ") == 0);
  assert(resolveDisplayTextSize(true, 3) == 1);
  assert(resolveDisplayTextSize(false, 0) == 1);
  assert(resolveDisplayTextSize(false, 2) == 2);

  char reason[64] = {};
  assert(extractWdgErrorReason(
      "HTTP/1.1 409 Conflict\r\n\r\n{\"detail\":\"duplicate file\"}",
      reason, sizeof(reason)));
  assert(strcmp(reason, "Duplicate upload") == 0);
  assert(extractWdgErrorReason("{\"message\":\"bad\\nrequest\"}", reason,
                               sizeof(reason)));
  assert(strcmp(reason, "bad request") == 0);
  assert(extractWdgErrorReason("HTTP/1.1 503 Unavailable\r\n\r\n", reason,
                               sizeof(reason)));
  assert(strcmp(reason, "503 Unavailable") == 0);
  assert(!extractWdgErrorReason(nullptr, reason, sizeof(reason)));
  assert(!extractWdgErrorReason("", reason, sizeof(reason)));
  return 0;
}
