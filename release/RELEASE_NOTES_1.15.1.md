# Marauder Eternal Mini V3 1.15.1

This release stabilizes the ESP32-C5 Mini V3 firmware and its deployment
package after the 1.14.4 feature work.

## Highlights

- Hardened Wi-Fi management-frame parsing against truncated or malformed
  packets and corrected AP/station parent selection handling.
- Reworked PCAP buffering for checked allocation, atomic record writes,
  bounded loss behavior, and recoverable SD write failures.
- Fixed Evil Portal HTML ownership, repeated-start cleanup, input length
  handling, and asynchronous lifetime problems.
- Corrected GPS queue handling, non-blocking baud recovery, stale-fix behavior,
  and NMEA formatting buffers.
- Fixed NimBLE 2 scan startup/restart calls and Find My sound-result reporting.
- Added strict Mini V3 firmware identity and partition-layout metadata for SD
  updates and desktop-flasher validation.
- Hardened CLI option and index validation and made bulk selections
  deterministic.
- Application-only flashing now updates both OTA slots so rollback cannot
  restore an older firmware copy.
- Pinned release builds to ESP32 Arduino core 3.3.4, restored the targeted Wi-Fi
  raw-frame linker wrapper, and added application-slot size enforcement.

## Existing 1.14.4 feature set retained

- Strongest-first SSID/AP selection, grouped AP targeting, Fox Hunt hierarchy,
  and SSID Finder.
- Mini V3 GPS lock indicator and scrollable date/time field.
- Device brightness control and Mini V3 game stability/speed improvements.

## Deployment

- Use the application image for an existing compatible Mini V3. The supplied
  scripts and desktop flasher write it to `0x10000` and `0x400000` while
  preserving NVS and stored data.
- Use the 8 MB full-device image at `0x0` for blank devices or partition-table
  recovery. It replaces all flash contents, including saved settings and logs.

Only Marauder Mini V3 hardware using the ESP32-C5, 8 MB flash, PSRAM, and the
`mini-v3-c5-8m-ota-v1` partition layout is supported.
