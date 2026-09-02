# Marauder Eternal Mini V3 1.15.5

This Mini V3 quality release focuses on reliable repeated use, bounded memory,
clear targeting, and a complete USB workflow for firmware and SD-card files.

## Reliability and lifecycle fixes

- Unified Wi-Fi teardown and startup between scans, beacon tools, deauth modes,
  and Evil Portal so stale driver state is not carried into the next task.
- Added checked startup for Wi-Fi mode, driver, promiscuous filter, and callback
  stages. A failed radio start now returns to idle instead of presenting a
  nonfunctional active screen.
- Reworked Broadcast, SSID Group, Station, and Evil Portal deauth scheduling to
  use bounded per-loop transmissions and explicit target cursors.
- Added live deauth target, channel, RSSI, BSSID, PMF, attempt, accepted, and
  failure status. Driver acceptance is correctly presented as transmission
  status rather than proof that a client disconnected.
- Fixed retained radio state and memory pressure when switching repeatedly
  among deauth, Evil Portal, scans, and beacon modes.
- Reused AP and dynamic menu list containers to reduce heap fragmentation, and
  removed temporary full-size serial-capture and per-advertisement BLE heap
  allocations.
- Bounded AP, station, probe-SSID, BLE, AirTag, Flipper, host, PineScan, and
  MultiSSID collections; AP/SSID selectors retain up to 256 BSSIDs.
- Fixed a settings-migration rollback path that could remove the original
  settings file if its backup rename failed.
- Added safe null initialization and cleanup for AP station lists.
- Corrected SD-card size reporting so reading the display value no longer
  destroys the stored capacity value.
- Fixed BLE advertisement bounds/callback state, SAE context cleanup, Find My
  connection failure handling, saved-list validation, and final capture-buffer
  draining.

## Wi-Fi targeting and menus

- **Scan SSIDs**, **Select SSIDs**, **SSID Finder**, **Select Stations**, and
  **Select Probe SSIDs** are grouped under WiFi Sniffers with strongest signals
  first.
- SSID groups expose their individual AP radios, RSSI, BSSID, and channel, with
  distinct navigation and selected colors plus individual or Select All
  targeting.
- Station selection is grouped beneath the matching SSID and is also available
  directly from Station Deauth.
- Broadcast Deauth and SSID Group Deauth open the shared SSID/AP selector before
  starting, preventing ambiguous empty-target failures.
- Fox Hunt uses the same hierarchy, permits exactly one AP, and reports that
  constraint when another target is attempted.
- SSID Finder follows the strongest AP in a selected SSID group with hysteresis,
  filtered RSSI, lock/resume, and found-progress controls.
- ForcePMKID and ForceProbe snapshot and respect only explicitly selected APs;
  without a selection they remain receive-only and say why.
- Rick Roll, Funny SSID, list beacon, and random beacon modes use a compact
  transmit profile and deterministic compatible channel startup.
- Connected-network scanners preserve their client connection and provide
  actionable preflight messages when Wi-Fi has not been joined.

## Evil Portal and SD files

- Evil Portal offers Manual SSIDs and signal-based Auto SSIDs, uses the shared
  SSID/AP selection model, and removes the nonfunctional AP Config entry.
- EPDeauth shows live radio status, portal client count, and credentials captured
  only during the current session in one scrollable view.
- SD output is organized into purpose-specific folders for captures, logs, GPS,
  wardrive data, saved lists, Evil Portal content, configuration, firmware, and
  scripts while retaining compatibility with older root-level files.
- Flasher 1.3.5 includes the verified 1.15.5 image, browses and downloads SD
  files over USB serial with preserved folders and SHA-256 verification, and
  installs validated Evil Portal HTML templates into `/evil_portal/html` with
  safe replacement and immediate menu refresh.
- Enlarged the SD Files browser, increased DPI-aware row spacing, and added a
  horizontal path scrollbar so filenames and controls remain fully visible.
- Increased the SD Files default window to 1296 x 988 and added prominent red
  loading, download, upload, and shutdown messages for every blocking SD task.
- Reserved the lower progress and action rows separately from the expandable
  file list so every SD control remains visible without manually resizing.
- SD Files now keeps one serial connection for the whole window. The device
  enters a locked USB SD screen with list/download/upload status and returns to
  its regular menu when the session closes.

## Interface and hardware

- Added persistent screen brightness control and Mini V3-native game controls,
  pacing, and cleanup fixes.
- GPS status stays hidden until a valid fix, then appears solid green. GPS Data
  exposes receiver/NMEA diagnostics and scrolls the complete highlighted date
  and time field.
- GPS date/time now synchronizes the system UTC clock used by SD timestamps.
  Device > Set Date/Time supplies a manual UTC editor, and the last valid clock
  value prevents a 1979 FAT timestamp while the next GPS fix is pending.
- The release is restricted to Marauder Mini V3 ESP32-C5 hardware and uses the
  8 MB `mini-v3-c5-8m-ota-v1` partition layout with PSRAM enabled.
- Application-only flashing writes both OTA application slots; the 8 MB merged
  factory image remains available for blank devices or partition recovery.
