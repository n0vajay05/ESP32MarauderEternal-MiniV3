# ESP32 Marauder Eternal 1.15.5

<img width="314" height="314" alt="esp32_marauder_eternal_source" src="https://github.com/user-attachments/assets/79332db4-7a71-423f-a99d-937090bb196f" />

This workspace is the standalone Marauder Eternal firmware for one hardware
target only:

- Marauder Mini V3
- ESP32-C5 revision 1 or later
- 8 MB flash with PSRAM enabled
- 128 x 128 ST7735 display

It contains the source and pinned local Arduino libraries needed to reproduce
the firmware, plus ready-to-flash release images. It intentionally excludes
other Marauder board profiles, historical binaries, backups, installers,
photos, test projects, and unrelated tooling. This was built from the original 
Marauder code base by JustCallMeKoKo and modified for the Marauder Mini V3 only.

## Release identity

- Product: `ESP32 Marauder Eternal`
- Version: `v1.15.5`
- Arduino ESP32 core used for the verified build: `3.3.4`
- Board target: `esp32:esp32:esp32c5`
- Options: `FlashSize=8M,PartitionScheme=custom,PSRAM=enabled`

The source contains a compile-time guard that rejects non-ESP32-C5 board
targets. `MARAUDER_MINI_V3` is also fixed by the project configuration.

## GPS status and recovery

The GPS indicator at the upper left stays hidden while searching and appears
solid green after a valid position fix. The **GPS > GPS Data** screen reports
whether checksum-valid NMEA traffic is active, the detected UART baud, and the
age of the latest sentence. On the Mini V3 GPS Data screen, press **Down** or
**Right** to highlight and scroll the complete D/T value. Press **Up** or
**Left** to clear the highlight; **Center** returns to the GPS menu.

The receiver starts passively without vendor-specific persistent configuration
or reset commands. Firmware detects 4800, 9600, 19200, 38400, 57600, and
115200 baud, keeps scanning if the receiver starts late, and clears stale fixes.
**NMEA Active** without a fix points to antenna/RF acquisition or sky view;
**NMEA Searching** points to receiver power, UART wiring, or NMEA output.

## Brightness control

Open **Device > Brightness** to adjust the Mini V3 backlight. Use **Up** or
**Right** to make the display brighter, **Down** or **Left** to dim it, and
**Center** to save and return. The selected level is retained across restarts.

## SSID groups

Use **WiFi > WiFi Sniffers > Scan SSIDs** to discover AP radios and
group BSSIDs that advertise the same exact SSID. Open **WiFi > WiFi Sniffers >
Select SSIDs**, choose an SSID, then toggle individual APs by BSSID. Each AP row
starts with its last-scanned RSSI in dBm, followed by BSSID and channel, and the
AP rows are sorted from strongest to weakest signal. SSID rows also show the
strongest AP RSSI at the left and use the same strongest-to-weakest order.
**Select All** at the bottom selects or clears the entire SSID group. Selected
rows remain green while the navigation cursor keeps its accent highlight. The
**SSID Beacon Clone** and **SSID Group Deauth** entries under WiFi Attacks
operate on the resulting AP selections. The ESP32-C5 services different
channels sequentially rather than simultaneously.

**WiFi > Attacks > Funny SSID Beacon** and **Rick Roll Beacon** advertise all
12 funny names or all eight lyric lines on 2.4 GHz channel 1. Each name retains
its own BSSID while the mode runs, so nearby Wi-Fi scanners can discover the
complete set. The screen shows **TX OK/s** (frames accepted by the Wi-Fi driver)
and **Fail/s**, with the driver error if transmission fails. The transmitter
waits for each frame's completion before submitting another, keeping at most
one beacon outstanding in the radio's small transmit-buffer pool. Reception can be
checked with a second device's Wi-Fi scan; these beacon-only names do not offer
an internet connection. Press **Center** to stop.

**ForcePMKID** and **ForceProbe** are selected-target settings. Scan and select
one or more AP radios under **WiFi > WiFi Sniffers > Select SSIDs** before
enabling them. ForcePMKID adds bounded deauthentication attempts while the
EAPOL sniffer runs; ForceProbe does the same while the probe-request sniffer
runs. Both modes snapshot the selected BSSIDs and channels at startup and never
transmit to newly observed or unselected APs. If no AP is selected, the scan is
receive-only and reports **No APs Selected**.

Broadcast and Station Deauth now show the active AP, channel, RSSI, BSSID, and
the scanned Protected Management Frames (PMF) status. Live **Try**, **OK**, and
**Fail** counters distinguish attempted frames from frames accepted by the
ESP32 Wi-Fi driver. **OK** is not proof that a client received the frame or
disconnected; raw management-frame injection does not provide a client ACK.

When **EPDeauth** is enabled, the running Evil Portal screen uses the same live
target, PMF, **Try**, **OK**, and **Fail** diagnostics. Choosing the portal's
SSID and channel preserves all explicitly selected AP targets. Deauthentication
visits every selected BSSID in turn, including separate 2.4 GHz and 5 GHz radios
with the same SSID. Each visit waits for queued frames to complete before
returning to the portal channel. While clients are connected to the portal,
other channels are deferred and resume automatically after the last client
disconnects. The screen shows selected AP counts by band and the number
temporarily deferred. The single radio serves one channel at a time.

The Evil Portal screen also shows the number
of clients currently associated with the portal and lists captured credentials
below the radio status, newest first. Use **Up** and **Down** to scroll through
the combined status and capture view; **Center** still stops the portal and
returns to the menu.

**WiFi > WiFi Sniffers > Select Stations** uses the same SSID grouping and
strongest-signal ordering. Each SSID row shows selected/total station counts
and the number of AP radios in the group. Opening an SSID lists every unique
captured client associated with any of its APs, together with the applicable
channel. Select individual clients or use **Select All** for the entire SSID;
the corresponding AP targets are selected automatically for Station Deauth.

**WiFi > WiFi Sniffers > Fox Hunt** uses the same SSID and AP hierarchy. Select
one AP to reveal **Start Fox Hunt** at the bottom of its AP list. Fox Hunt does
not offer **Select All** and rejects a second AP until the first is deselected.

**WiFi > WiFi Sniffers > SSID Finder** uses that SSID hierarchy for passive
proximity finding. After choosing an SSID, it scans only the channels used by
that group, filters RSSI changes, and automatically follows the strongest AP.
A stronger AP must remain at least 6 dB ahead for two complete channel cycles
before Finder switches targets, which avoids rapid bouncing between similar
signals. The bullseye shows proximity rather than direction: move the device
and watch the ring, dBm value, and signal trend. Press **Center** to lock or
resume automatic target selection, **Right** to mark the current AP found (or
reset after all are found), and **Left** to exit. The display tracks progress as
**Found x/y** without changing the AP selections used by WiFi Attacks.

## Screenshots
<img width="128" height="128" alt="image" src="https://github.com/user-attachments/assets/f176da9a-eafb-42ac-afda-aac6455f8962" />
<img width="128" height="128" alt="image" src="https://github.com/user-attachments/assets/5ef02b66-e307-4d84-ac74-c161e486a6cf" /> 
<img width="128" height="128" alt="image" src="https://github.com/user-attachments/assets/2d50998b-a543-4501-abea-a0063ae3aaaa" />
<img width="128" height="128" alt="image" src="https://github.com/user-attachments/assets/70f1b90e-86e3-4518-b08f-e84426144d9a" />

## Flash the existing device

The normal flasher writes the same application image to both OTA application
slots at `0x10000` and `0x400000`. This prevents an old copy in the inactive
slot from returning after an OTA rollback while preserving the installed
bootloader, partition table, NVS settings, and stored device data:

```bash
./scripts/flash.sh /dev/serial/by-id/YOUR_ESP32_SERIAL_DEVICE
```

The script checks the release hashes and confirms that the connected chip is
an ESP32-C5 before writing.

## Single-file factory flash

For a blank board, or when the bootloader and partition table must also be
restored, use this one firmware payload:

```text
release/Marauder_Eternal_1.15.5_MiniV3_ESP32-C5.bin
```

It is an 8 MB merged image containing the ESP32-C5 bootloader, partition
table, OTA boot metadata, and Marauder Eternal application at their required
offsets. Flash this file at address `0x0`. No source files, Arduino libraries,
or separate component `.bin` files are required on the flashing computer.

With `esptool` installed, the included convenience script uses that single
image:

```bash
./scripts/factory-flash.sh /dev/serial/by-id/YOUR_ESP32_SERIAL_DEVICE
```

Or distribute only the factory `.bin` and select it at offset `0x0` in a
compatible ESP32-C5 flashing application. The flashing application itself is
still required; the `.bin` is the only firmware file it needs.

The factory image writes the entire 8 MB flash. Using it on an existing device
replaces saved settings, logs, credentials, and other flash contents. Use the
application-only updater above when those contents should be preserved.

## One-click desktop flasher

The `flasher/` directory contains a basic graphical flasher for Linux and
Windows. Packaged builds include the verified 8 MB full-device image and provide:

- automatic serial-port discovery with a manual selector;
- selection of an included image or another compatible `.bin` file;
- safe detection of full-device (`0x0`) and application images; application
  images are written to both OTA slots (`0x10000` and `0x400000`);
- a single **Connect & Flash** button;
- an **SD Files** browser that copies selected files or the complete SD folder
  structure over one persistent USB serial session without removing the card;
- an enlarged SD Files layout with prominent red loading, download, upload, and
  shutdown messages while the user is waiting for serial operations;
- verified Evil Portal HTML uploads directly into `/evil_portal/html`, with
  replacement confirmation and immediate availability in the device menu;
- exact-size transfer framing and SHA-256 verification before a downloaded file
  replaces its local destination or an uploaded template is committed;
- ESP32-C5 identity checking before any write;
- live percentage and progress-bar updates;
- final success or failure status with a useful reason for common failures.

Build the native applications on Linux and Windows with:

```bash
./scripts/build-gui-linux.sh
```

```powershell
.\scripts\build-gui-windows.ps1
```

See `flasher/README.md` for end-user and build instructions. PyInstaller builds
on the target operating system, so the Windows executable is produced on
Windows or by the included GitHub Actions workflow.

## Rebuild

Install Arduino CLI and the Espressif ESP32 core version `3.3.4`, then run:

```bash
./scripts/build.sh
```

The build uses only `firmware/MarauderEternal`, the custom partition table in
that sketch, and the pinned libraries under `libraries/`. Output is written to
`build/`.

To regenerate the versioned release images, manifest, and checksums from a
successful build, run:

```bash
python3 scripts/package_release.py build
python3 scripts/package_release.py --verify
```

## SD card layout

The firmware creates a predictable directory layout whenever an SD card is
mounted. New output is stored by purpose instead of accumulating in the card
root:

```text
/captures/          Wi-Fi packet captures (`.pcap`)
/logs/              Network-scan and Bluetooth logs
/gps/               GPS tracker and POI files (`.gpx`)
/wardrive/          Wardrive logs, POIs, and upload sidecars
/lists/             Saved SSID, AP/station, and AirTag lists
/evil_portal/html/  Evil Portal HTML templates
/evil_portal/       Evil Portal configuration and credential log
/config/            WiGLE and WDG credential import files
/firmware/          SD firmware-update images
/spiffs/            SPIFFS backup/restore data used during migration
/SCRIPTS/           Device scripts
```

Place an SD update at `/firmware/update.bin`. `/update.bin` in the root remains
supported for older cards. Existing root-level captures, saved lists, portal
templates, portal configuration, and API-import files also remain readable;
new files are written to the structured locations. The on-device SD file menu
shows relative paths such as `captures/eapol_0.pcap`, so files with identical
names in different categories remain distinguishable.

The desktop flasher's **SD Files** button reads this directory tree over USB
serial. Stop any active scan, capture, attack, or Evil Portal session first,
then refresh the list and download selected files or the entire card layout.
Batch downloads recreate the SD directory structure on the computer. Each file
is written to a temporary local file and SHA-256 verified before it replaces
the selected destination. While this window is open, the device displays the
active USB SD operation and locks normal controls; closing the window ends the
session and restores the regular menu.

New SD files use the device system clock. A valid GPS date/time automatically
sets that clock in UTC. **Device > Set Date/Time** provides a manual UTC fallback
using the directional buttons and center button. The last valid value is kept
across restarts to prevent the FAT filesystem from falling back to 1979 while a
new GPS fix is pending.

## Sensitive data and authorized use

The Evil Portal feature stores submitted form values as plaintext in the
device's capture output. Treat those files as sensitive, erase them when they
are no longer needed, and use capture and active Wi-Fi features only on systems
you own or are explicitly authorized to test.

## Layout

```text
firmware/MarauderEternal/  Mini V3 ESP32-C5 sketch and assets
libraries/                 Pinned build dependencies only
release/                   Verified flash images and hashes
scripts/                   Build and safe flashing commands
flasher/                   Linux/Windows one-click flasher source
licenses/                  Required third-party notices
```

See `LICENSE` and `licenses/THIRD_PARTY_NOTICES.md` for licensing information.
