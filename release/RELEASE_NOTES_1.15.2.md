# Marauder Eternal Mini V3 1.15.2

This Mini V3 quality release focuses on safe repeated use, bounded memory, and
clear failure handling across Wi-Fi, Bluetooth, Evil Portal, and saved lists.

## Reliability fixes

- Fixed BLE advertisement bounds errors, a Samsung model-table overrun, stale
  callback busy state, and callback allocations that accumulated across scans.
- Added bounded AP, station, probe-SSID, BLE-device, AirTag, Flipper, generated
  SSID, and discovered-host collections to prevent long scans exhausting RAM.
- Released SAE cryptographic contexts and anti-clogging tokens on stop, checked
  initialization failures, bounded token length, and restricted responses to
  explicitly selected APs.
- Added PSRAM allocation fallback for MAC history and safe behavior if both
  PSRAM and internal allocation fail.
- Drained both capture buffers when a task stops or changes output, preserving
  the final partial PCAP/log block.
- Corrected Find My connection failure handling so service discovery is never
  attempted through a disconnected client.

## Wi-Fi lifecycle and menus

- Raw scans, attacks, and standalone C5 tools now enter and leave through a
  consistent radio teardown path, preventing stale driver state between tasks.
- Scan SSIDs and the shared AP selectors now retain up to 256 BSSIDs; the
  unrelated PineScan and MultiSSID analyzer scratch lists remain capped at 100.
- Rick Roll and Funny SSID beacon modes now use a compact transmit-only Wi-Fi
  profile and recover from stale driver state after Scan SSIDs without clearing
  the retained AP/SSID results.
- Rick Roll and Funny SSID restarts no longer inherit an incompatible channel
  from earlier dual-band activity; both bootstrap on channel 1 and rotate only
  through channels valid for their 2.4 GHz beacon frame format.
- Beacon Spam List and random Beacon Spam now use the compact transmit-only
  Wi-Fi profile, preventing `ESP_ERR_NO_MEM` after scans or other radio tasks
  have retained results in memory.
- Broadcast Deauth and SSID Group Deauth once again open the shared grouped
  Select SSIDs workflow, with individual AP, Select All, and Start Deauth
  controls instead of starting with an empty target selection.
- Connected network scanners preserve their client connection and reject starts
  with a clear message when Wi-Fi has not been joined.
- Connected-network preflight now reads the live driver state, preventing an
  immediate post-connect tool launch from being rejected by a stale UI flag.
- Port Scan All now explains when Ping Scan has not produced any host targets.
- Open networks can be joined and open APs can be started with an empty
  password; saved/open credentials are also accepted by upload workflows.
- Start AP now always performs the requested mode transition, verifies AP
  creation, and reports invalid passwords or driver failures instead of showing
  a false success state.
- Removed password values from serial status messages.
- Corrected the Save/Load AirTags menu destinations.

## Persistence and session safety

- SD output now uses purpose-specific folders for captures, logs, GPS files,
  wardrive data, saved lists, Evil Portal data, configuration imports,
  firmware updates, and scripts. Existing root-level files remain readable.
- Flasher 1.3.1 adds an SD Files browser with selected or full-card downloads
  over USB serial, preserved folder structure, safe path validation, exact byte
  framing, temporary local files, and SHA-256 verification. It can also install
  verified Evil Portal HTML templates directly into `/evil_portal/html`, safely
  replace an existing template, and refresh the device's HTML selection list.
- AP and AirTag lists now use bounded JSON parsing, validated MAC/payload data,
  list replacement instead of duplicate append, corrected packet-count loading,
  and backup-assisted file replacement.
- Settings are migrated in place when fields are added, corrupt settings are
  preserved for diagnosis, and failed toggles no longer report an unsaved value.
- Evil Portal clears partial username/password state at every session boundary
  so values cannot be combined across sessions.

## Deployment

- Firmware identity is `v1.15.2` for Marauder Mini V3 ESP32-C5 hardware.
- The release uses the 8 MB `mini-v3-c5-8m-ota-v1` partition layout with PSRAM.
- Application-only flashing writes both OTA slots; the full-device image remains
  available for blank boards or partition recovery.
