#include <assert.h>
#include <initializer_list>
#include <stdint.h>
#include <string.h>

#include "BeaconFrame.h"
#include "BeaconTxPacer.h"
#include "PortalTxTracker.h"
#include "SelectedAPScheduler.h"
#include "DeauthFrame.h"
#include "DisplayLine.h"
#include "RsnCapabilities.h"
#include "SdTransferPath.h"
#include "UtcTime.h"
#include "WdgResponse.h"

static void testPortalScheduling() {
  struct AP {
    const char* ssid;
    uint8_t channel;
    bool selected;
  };
  const AP aps[] = {{"MainGuest", 1, true}, {"MainGuest", 36, true},
                    {"Other selected SSID", 6, true}, {"Unselected", 1, false},
                    {"MainGuest", 116, true}};
  const auto get = [&aps](uint16_t i) { return aps[i]; };
  uint16_t cursor = 0;
  // Each selected BSSID gets a turn, even across SSIDs and bands.
  for (int round = 0; round < 3; ++round) {
    for (int expected : {0, 1, 2, 4})
      assert(nextSelectedAP(5, cursor, 0, get) == expected);
  }
  // Clients constrain visits to the portal channel without losing selections.
  for (int round = 0; round < 3; ++round)
    assert(nextSelectedAP(5, cursor, 1, get) == 0);
  assert(nextSelectedAP(5, cursor, 11, get) == -1);
  for (int expected : {1, 2, 4, 0})
    assert(nextSelectedAP(5, cursor, 0, get) == expected);
  cursor = 99;
  assert(nextSelectedAP(5, cursor, 0, get) == 0);
  assert(nextSelectedAP(0, cursor, 0, get) == -1 && cursor == 0);

  PortalTxTracker tx;
  tx.submitted();
  tx.submitted();
  tx.submitted();
  assert(tx.pending() == 3);
  tx.completed(true);
  tx.completed(true);
  assert(tx.pending() == 1); // cannot return home with the last frame queued
  tx.completed(false);
  assert(tx.pending() == 0);
  assert(tx.takeFailures() == 1 && tx.takeFailures() == 0);
  tx.submitted();
  tx.completed(true); // callback before the driver's enqueue call returns
  assert(tx.pending() == 0);
  tx.submitted();
  tx.rejected();
  assert(tx.pending() == 0 && tx.takeFailures() == 0);
  tx.completed(false); // stale callback must not underflow the pending count
  assert(tx.pending() == 0 && tx.takeFailures() == 0);
  tx.submitted();
  tx.reset();
  assert(tx.pending() == 0 && tx.takeFailures() == 0);
}

static void testBeaconTxPacing() {
  BeaconTxPacer pacer;
  pacer.reset(100);
  assert(pacer.ready(100));
  pacer.begin(100);
  // A slow driver must not receive more frames just because 8 ms elapsed.
  assert(!pacer.ready(108));
  assert(!pacer.ready(1099));
  assert(!pacer.stalled(1099));
  assert(pacer.stalled(1100));
  assert(!pacer.ready(1100)); // a timeout reports an error, not another enqueue
  pacer.complete();
  assert(pacer.ready(1100));
  assert(!pacer.stalled(1100));

  pacer.begin(1100);
  pacer.complete(); // callback can arrive before esp_wifi_80211_tx returns
  assert(!pacer.ready(1107));
  assert(pacer.ready(1108));
  pacer.begin(1108);
  pacer.rejected(1108);
  assert(!pacer.ready(1157));
  assert(pacer.ready(1158));

  pacer.begin(1158);
  pacer.reset(1200); // restarting clears outstanding state and deadlines
  assert(pacer.ready(1200));
  assert(!pacer.stalled(1200));
  pacer.reset(UINT32_MAX - 3);
  pacer.begin(UINT32_MAX - 3);
  pacer.complete();
  assert(!pacer.ready(3));
  assert(pacer.ready(4));
  pacer.begin(UINT32_MAX - 10);
  assert(!pacer.stalled(988));
  assert(pacer.stalled(989));
}

static void testSsidBeacons() {
  const uint8_t mac[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  uint8_t frame[MAX_SSID_BEACON_SIZE + 1];
  // Reuse storage in both length directions, including the maximum SSID.
  const char* names[] = {"Dora the Internet Explorer", "Loading...",
                        "01234567890123456789012345678901", "A"};
  for (const char* name : names) {
    const size_t ssid_length = strlen(name);
    for (uint8_t channel : {1, 6, 11}) {
      memset(frame, 0xa5, sizeof(frame));
      const size_t length = buildSsidBeaconFrame(
          frame, sizeof(frame), name, mac, channel, 0x0807060504030201ULL);
      assert(length == 57 + ssid_length);
      assert(frame[length] == 0xa5); // never write past the returned length
      assert(frame[0] == 0x80 && frame[1] == 0);
      for (size_t i = 4; i < 10; ++i)
        assert(frame[i] == 0xff);
      assert((frame[10] & 0x03) == 0x02);
      assert(memcmp(frame + 10, frame + 16, 6) == 0);
      for (size_t i = 0; i < 8; ++i)
        assert(frame[24 + i] == i + 1);
      assert(frame[32] == 100 && frame[33] == 0);
      assert((frame[34] & 0x11) == 0x01); // ESS, open

      // Parse the complete IE chain like a receiver, rejecting trailing junk,
      // truncated elements, duplicate SSID tags and mismatched channel tags.
      const uint8_t expected_ids[] = {0, 1, 3, 5};
      size_t offset = 36;
      size_t tag = 0;
      while (offset < length) {
        assert(offset + 2 <= length);
        const uint8_t id = frame[offset++];
        const uint8_t size = frame[offset++];
        assert(tag < sizeof(expected_ids) && id == expected_ids[tag++]);
        assert(offset + size <= length);
        if (id == 0) {
          assert(size == ssid_length);
          assert(memcmp(frame + offset, name, size) == 0);
        } else if (id == 1) {
          assert(size == 8 && frame[offset] == 0x82);
        } else if (id == 3) {
          assert(size == 1 && frame[offset] == channel);
        } else {
          assert(size == 4 && frame[offset + 1] == 1);
        }
        offset += size;
      }
      assert(offset == length && tag == sizeof(expected_ids));
      uint8_t bssid[6];
      memcpy(bssid, frame + 10, sizeof(bssid));
      assert(buildSsidBeaconFrame(frame, sizeof(frame), "Other", mac, channel, 9));
      assert(memcmp(bssid, frame + 10, sizeof(bssid)) != 0);
      assert(buildSsidBeaconFrame(frame, sizeof(frame), name, mac, channel, 10));
      assert(memcmp(bssid, frame + 10, sizeof(bssid)) == 0);
    }
  }
  assert(buildSsidBeaconFrame(frame, sizeof(frame), "Flock", mac, 1, 0, true));
  assert(memcmp(frame + 10, mac, sizeof(mac)) == 0);
  assert(!buildSsidBeaconFrame(nullptr, sizeof(frame), "A", mac, 1, 0));
  assert(!buildSsidBeaconFrame(frame, sizeof(frame), nullptr, mac, 1, 0));
  assert(!buildSsidBeaconFrame(frame, sizeof(frame), "A", nullptr, 1, 0));
  assert(!buildSsidBeaconFrame(frame, sizeof(frame), "", mac, 1, 0));
  assert(!buildSsidBeaconFrame(frame, sizeof(frame),
                             "012345678901234567890123456789012", mac, 1, 0));
  assert(!buildSsidBeaconFrame(frame, 57, "A", mac, 1, 0));
  for (uint8_t channel : {0, 12, 36, 255})
    assert(!buildSsidBeaconFrame(frame, sizeof(frame), "A", mac, channel, 0));
  const uint8_t multicast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  assert(!buildSsidBeaconFrame(frame, sizeof(frame), "A", multicast, 1, 0));
}

int main() {
  testPortalScheduling();
  testBeaconTxPacing();
  testSsidBeacons();
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
