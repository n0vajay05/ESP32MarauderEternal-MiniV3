#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "BeaconFrame.h"
#include "DeauthFrame.h"
#include "DisplayLine.h"
#include "RsnCapabilities.h"
#include "SdTransferPath.h"
#include "UtcTime.h"
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

  uint8_t deauth[26] = {0xc0};
  const uint8_t ap[6] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
  const uint8_t station[6] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25};
  const uint8_t broadcast[6] = {
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  assert(!setDeauthFrameAddresses(nullptr, sizeof(deauth), station, ap, ap));
  assert(!setDeauthFrameAddresses(deauth, 23, station, ap, ap));
  assert(setDeauthFrameAddresses(deauth, sizeof(deauth), station, ap, ap));
  assert(memcmp(deauth + 4, station, 6) == 0);
  assert(memcmp(deauth + 10, ap, 6) == 0);
  assert(memcmp(deauth + 16, ap, 6) == 0);
  assert(isBroadcastAddress(broadcast));
  assert(!isBroadcastAddress(station));

  const uint8_t open_network_ies[] = {0, 3, 'A', 'P', '1'};
  assert(parsePmfStatus(open_network_ies, sizeof(open_network_ies)) ==
         PMF_STATUS_NONE);

  const uint8_t pmf_capable_ies[] = {
      48, 20,
      0x01, 0x00,
      0x00, 0x0f, 0xac, 0x04,
      0x01, 0x00,
      0x00, 0x0f, 0xac, 0x04,
      0x01, 0x00,
      0x00, 0x0f, 0xac, 0x02,
      0x80, 0x00};
  assert(parsePmfStatus(pmf_capable_ies, sizeof(pmf_capable_ies)) ==
         PMF_STATUS_CAPABLE);

  uint8_t pmf_required_ies[sizeof(pmf_capable_ies)] = {};
  memcpy(pmf_required_ies, pmf_capable_ies, sizeof(pmf_capable_ies));
  pmf_required_ies[sizeof(pmf_required_ies) - 2] = 0xc0;
  assert(parsePmfStatus(pmf_required_ies, sizeof(pmf_required_ies)) ==
         PMF_STATUS_REQUIRED);

  const uint8_t malformed_rsn_ies[] = {48, 20, 0x01};
  assert(parsePmfStatus(malformed_rsn_ies, sizeof(malformed_rsn_ies)) ==
         PMF_STATUS_UNKNOWN);
  assert(parsePmfStatus(nullptr, 0) == PMF_STATUS_UNKNOWN);

  char sd_path[SD_TRANSFER_MAX_PATH_BYTES] = {};
  assert(decodeSdPathHex("2f63617074757265732f6561706f6c2e70636170",
                         sd_path, sizeof(sd_path)));
  assert(strcmp(sd_path, "/captures/eapol.pcap") == 0);
  assert(isSafeSdFilePath(sd_path));
  assert(!decodeSdPathHex("abc", sd_path, sizeof(sd_path)));
  assert(!decodeSdPathHex("2f00", sd_path, sizeof(sd_path)));
  assert(!isSafeSdFilePath("/captures/../config/key.txt"));
  assert(!isSafeSdFilePath("/captures//file.pcap"));
  assert(!isSafeSdFilePath("relative/file.pcap"));
  assert(!isSafeSdFilePath("/"));
  assert(isEvilPortalHtmlUploadPath("/evil_portal/html/login.html"));
  assert(!isEvilPortalHtmlUploadPath("/evil_portal/html/login.HTML"));
  assert(!isEvilPortalHtmlUploadPath("/evil_portal/html/nested/login.html"));
  assert(!isEvilPortalHtmlUploadPath("/captures/login.html"));
  assert(isSha256Hex(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  assert(isSha256Hex(
      "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789"));
  assert(!isSha256Hex("0123"));
  assert(!isSha256Hex(
      "g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));

  using marauder::clock::UtcDateTime;
  assert(marauder::clock::isLeapYear(2024));
  assert(!marauder::clock::isLeapYear(2100));
  assert(marauder::clock::daysInMonth(2024, 2) == 29);
  assert(marauder::clock::daysInMonth(2025, 2) == 28);
  assert(marauder::clock::isValid({2026, 9, 1, 12, 34, 56}));
  assert(!marauder::clock::isValid({2025, 2, 29, 0, 0, 0}));

  int64_t unix_seconds = 0;
  assert(marauder::clock::toUnixSeconds({2024, 2, 29, 12, 34, 56},
                                        unix_seconds));
  assert(unix_seconds == 1709210096LL);
  UtcDateTime round_trip{};
  assert(marauder::clock::fromUnixSeconds(unix_seconds, round_trip));
  assert(round_trip.year == 2024 && round_trip.month == 2 &&
         round_trip.day == 29 && round_trip.hour == 12 &&
         round_trip.minute == 34 && round_trip.second == 56);
  return 0;
}
