# Marauder Eternal Mini V3 1.16.0

This release packages the current Eternal Mini V3 firmware, including the
reliability fixes, six selectively ported upstream enhancements, and the
FindMy diagnostics and scan workflow.

## Reliability

- Bound discovery lists and display queues under memory pressure.
- Improve BLE client and scan cleanup across repeated starts and stops.
- Validate network scan ranges and stream uploads through bounded buffers.
- Retain Eternal's SSID/BSSID grouping, target confirmation, BLE classifiers,
  and GPS recovery behavior.

## Enhancements

- Save up to five WiFi profiles, update duplicate SSIDs, and require an explicit
  replacement when the list is full. Joining saved networks prioritizes visible
  networks by signal strength before trying hidden profiles.
- Configure five Wardrive exclusion geofences with radii from 0.1 to 1 mile.
  Collection pauses inside a zone or without a valid GPS fix when zones are
  enabled; an exit margin prevents boundary flapping.
- Track Remote ID aircraft across WiFi/BLE observations with identity, status,
  and movement history. Export CSV and GPX sessions, separating reception gaps
  into GPX segments while retaining the existing raw-message capture API.
- Decode full iBeacon UUID, major/minor, signed measured power, RSSI, and
  advertisement counts. Follow beacon identity across Bluetooth address changes.
- Add GPS Tracker trip and receiver pages, separate trusted-sample and queued
  GPX counters, and improve handling of slow movement, implausible jumps, and
  fix gaps.
- Add Fox Hunt recency, WiFi band, and sorting filters, plus additional WiFi
  and BLE target categories. Keep Bluetooth Fox Hunt under BLE Discovery,
  preserve grouped AP selection, and expire stale live signal readings.

## FindMy workflow

- Rename the Bluetooth Attacks entry to **FindMy Attacks** and provide separate
  **Test Connection**, **Play Sound**, and persistent **Last Result** actions.
- Report the failure stage, raw Bluetooth error and explanation, connection
  duration, advertised connectability, scan age, and discovered GATT capabilities.
  The connection-only test sends no sound commands and subscribes to no
  notifications.
- Offer **Run FindMy Sniff** when the target list is empty. On exit, refresh the
  target list and return to FindMy Attacks.
- Stop FindMy Sniff automatically after 60 seconds from its menu and serial
  launch paths. Center still stops early; each scan starts a fresh timer.
  Changing modes cancels the timer and pending menu return.
- Provide isolated `selftest findmy` checks for diagnostic behavior, menu
  navigation, timer boundaries, cancellation, restart, and clock rollover.

## Compatibility and validation

- Hardware remains Marauder Mini V3 / ESP32-C5 with 8 MB flash and PSRAM.
  The Arduino ESP32 core remains pinned to `3.3.4`, with partition layout
  `mini-v3-c5-8m-ota-v1`.
- Flasher 1.3.5 uses the release manifest to bundle firmware 1.16.0. The
  application updater writes both OTA slots while preserving settings and
  filesystem data; the merged factory image replaces the complete flash.
- The enhancements were verified sequentially on the Mini, using isolated
  fixtures for unavailable equipment. Live saved-network association, geofence
  travel, and a known Remote ID transmitter were unavailable for those checks.
- The 1.16.0 build passes the native regression checks, 28 desktop flasher
  tests, release image and checksum verification, and the bundled Linux flasher
  self-test. Both application slots were hash-verified after flashing; the Mini
  reports `v1.16.0` and passes FindMy diagnostics and discovery start/stop checks.
- FindMy discovery timing was verified on-device at 60,000 and 60,001 ms,
  including early stop, refreshed menu return, and another BLE scan running
  beyond a cancelled timer's deadline.
- An AirTag advertisement does not guarantee an available BLE connection. The
  Mini does not implement Apple's account-based iPhone Play Sound service.
