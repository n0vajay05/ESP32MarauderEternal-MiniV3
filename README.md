# ESP32 Marauder Eternal 1.16.2

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
- Version: `v1.16.2`
- Desktop flasher: `1.5.0`, including firmware `1.16.2`
- Release notes: [1.16.2](release/RELEASE_NOTES_1.16.2.md)
- Arduino ESP32 core used for the verified build: `3.3.4`
- Board target: `esp32:esp32:esp32c5`
- Options: `FlashSize=8M,PartitionScheme=custom,PSRAM=enabled`

The source contains a compile-time guard that rejects non-ESP32-C5 board
targets. `MARAUDER_MINI_V3` is also fixed by the project configuration.

## 1.16.2 update

Firmware 1.16.2 and Flasher 1.5.0 add **arrow-key** and **C** controls to the USB
Screen Mirror introduced in [1.16.1](release/RELEASE_NOTES_1.16.1.md). Keyboard
controls are available only while mirroring is active and the viewer has focus.
Held keys reproduce the Mini's button behavior; focus loss or disconnect
releases all virtual buttons. Physical controls remain usable.

The viewer retains fullscreen, PNG capture, and reconnection controls. The Mini
captures actual display writes using one 32 KiB buffer and sends bounded row
updates from a background task. See
[USB screen mirror](#usb-screen-mirror) for setup and resource details.

<img width="686" height="415" alt="screenshare" src="https://github.com/user-attachments/assets/be713cda-46be-4555-9b6e-36e1f9443e51" />
<img width="260" height="344" alt="companionapp" src="https://github.com/user-attachments/assets/a24635b1-1d57-447c-aa03-23731d7e5028" />

## Enhancements carried forward from 1.16.0

Eternal selectively incorporates these features from the official
1.18.0 release while retaining Eternal's SSID grouping, target confirmation,
and device classifiers. The packaged release images include the reliability
fixes, enhancements, and FindMy workflow described below. Build the same source
with `scripts/build.sh`.

- **Saved WiFi Profiles** under WiFi General stores up to five networks.
  Join Saved WiFi tries visible networks by signal strength, then hidden
  profiles. A full list requires an explicit replacement; duplicate SSIDs
  update the existing profile.
- **Device > Wardrive Geofences** configures five exclusion zones with
  radii from 0.1 to 1 mile. Collection pauses inside a zone or while GPS is
  unavailable when zones are enabled. An exit margin prevents boundary flapping.
- **Remote ID** retains aircraft identity across WiFi/BLE observations, shows
  tracking status and a movement trail, and writes CSV plus GPX session exports
  to `/logs/remoteid_N.*` on SD. Reception gaps create separate GPX segments.
- **Bluetooth Sniffers > iBeacon Sniff / iBeacon Info** provides full UUID,
  major/minor, signed measured power, RSSI, and advertisement counts. Identity
  follows UUID/major/minor across changing Bluetooth addresses.
- **GPS > GPS Tracker** has trip and receiver pages; use Left/Right to switch
  and Center to stop. Distance rejects implausible jumps, accommodates slow
  walking, and starts a new segment after fix loss. Trusted samples and queued
  GPX points have separate counters.
- **Fox Hunt** in WiFi Sniffers and **Bluetooth > BLE Discovery** offers recent (30 seconds),
  WiFi band, and signal/name/channel sorting options. WiFi keeps APs grouped
  by SSID and adds stations, Pineapple, and MultiSSID targets. Bluetooth adds
  FindMy, Flipper, iBeacon, Meta, and Flock categories using the existing
  classifiers. Filters apply only to Fox Hunt. Live signal expires after
  reception stops; iBeacon hunts follow beacon identity across address changes.

**Bluetooth Attacks > FindMy Attacks** opens a target action menu. When the
list is empty, choose **Run FindMy Sniff** to discover targets. The scan stops
automatically after **60 seconds**, or press Center to stop early; it returns
to FindMy Attacks and refreshes its target list. The same 60-second limit applies
when FindMy Sniff is started from Bluetooth Sniffers or serial commands. If no
targets were found, the scan option remains available. After a fresh scan,
select your target and choose **Test Connection** to inspect
the link and sound service without sending sound commands or subscribing to
notifications. **Play Sound** remains a separate action. The persistent result
screen shows the failure stage, raw Bluetooth error and explanation, connection
time, last advertised connectability and scan age, and discovered services.
Owner proximity is a possible explanation for a connection failure, not a
condition the Mini can confirm. A successful connection test checks advertised
GATT capabilities; it does not guarantee a later sound write will be accepted.
The Mini does not implement Apple's account-based iPhone Play Sound service.

Serial diagnostics: `findmy --list`, `findmy --scan`, `findmy -t <index> --test`,
`findmy --status`, and `findmy --result`. `selftest findmy` uses isolated fixtures.

Run `scripts/test-native.sh` for the portable regression checks. Serial
`selftest wifi`, `selftest geofence`, `selftest remoteid`, `selftest ibeacon`,
`selftest gpstracker`, and `selftest fox` exercise isolated synthetic fixtures
on the Mini with radio modes stopped. Device verification artifacts from this
workspace are under `build/verification/`.

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

## WiFi firmware updates

Open **Device > Update Firmware > Update Over WiFi**. An SD card is not
required. Choose **Scan SSIDs** to select a nearby network, or **Add SSID
Manually** to enter a network name, including a hidden SSID. Use the joystick
to move around the keyboard and Center to select a character. **Mode** cycles
through lowercase, uppercase, numbers, and symbols; **Del** removes a character,
**Back** cancels, and **Enter** at the bottom right finishes the entry.
Passwords are masked. Open networks accept an empty password; networks that
require a captive portal or enterprise login are not supported.

After connecting, the Mini checks the
[Eternal Mini V3 update repository](https://github.com/n0vajay05/ESP32MarauderEternal-MiniV3/releases)
with a **15-second** server timeout. An unreachable or unusable update server
shows **Failed to connect to update server**. A successful connection returns
to the update menu with **Connected: SSID** in its top banner. When disconnected,
the banner reads **Please Connect To Update**. **Network Info** displays
the received IP address, subnet mask, gateway, primary DNS, and secondary DNS.
Select a field to edit its address with the joystick number pad, then choose
**Apply Changes** to reconnect with the manual settings. **Use DHCP** restores
automatic configuration. A secondary DNS of `0.0.0.0` leaves it unused.

Select **Check For Updates** from the update menu. The Mini checks the
latest published stable release, compares numeric version numbers, and offers
**Install** when the release is newer. Keep power connected while installing.
Firmware downloads directly into the inactive flash slot; the Mini checks its
size, SHA-256 checksum, embedded hardware identity, version, and partition layout
before selecting it for boot. It reboots into the update and accepts it after
startup checks pass. A failed or interrupted download keeps the current boot
image; an update that fails startup verification can roll back to the previous
slot. Releases must include the existing `manifest.json` and matching Mini V3
ESP32-C5 `.app.bin` assets. OTA updates the application, preserving settings and
SD contents; partition or bootloader changes require USB flashing.

Update mode stops other radio features and limits application requests to this
repository's GitHub release API and release assets, including GitHub's required
asset redirects. Normal WiFi association, DHCP, and DNS traffic is also needed.
It starts no portal, web server, or general internet client. The last successful
update network and any manual address settings are saved separately from the
five general WiFi profiles and reconnect on the next visit. On first use, an
existing saved WiFi profile can be reused. **Forget Network** disconnects and
clears the saved update network and its manual settings. An explicitly forgotten
network is not automatically imported again from the general WiFi profiles.
Backing out of **Update Firmware**
disconnects WiFi, turns the radio off, and disables automatic reconnection.
Backing out of its WiFi submenu keeps the session available until you leave
Update Firmware. **Update From SD** remains available when an SD card is present.

USB mirroring and its arrow/C controls work throughout network setup. Serial
`update --status` reports update state without credentials; `update --exit`
closes the update session. `selftest ota` runs isolated policy, keyboard, and
HTTP framing checks without connecting to a network. TLS allocations use PSRAM
so the HTTPS handshake leaves internal RAM available for WiFi and the UI;
normal allocation is restored after each request. Connection diagnostics include
the failed stage, native TLS error, and HTTP status in `update --status`.
`selftest ota download` uses the saved update network to download and verify
the published firmware's checksum and embedded identity without writing flash,
then disconnects WiFi.

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
release/Marauder_Eternal_1.16.2_MiniV3_ESP32-C5.bin
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

## USB screen mirror

Connect the Mini with a USB data cable, select it in the desktop flasher, and
click **Screen Mirror**. The separate viewer enlarges the Mini's live display.
While the viewer has keyboard focus, use the **arrow keys** for the directional
buttons and **C** for Center/select. Holding a key reproduces the button's hold
behavior; the physical buttons remain usable. Keyboard controls are available
only during an active mirror connection. Switching focus, opening Save PNG, or
disconnecting releases the virtual buttons. Click the screen to give it focus.
Use **Fullscreen** or **F11** for presentations, **Esc** to leave fullscreen,
and **Save PNG** to capture an image.
OBS and screen-sharing software can capture the viewer window.

The viewer uses USB serial. It needs no WiFi connection, browser, or IP address.
Open the viewer before starting the activity you want to present. Opening the
USB serial port can reset the Mini with some host drivers; the viewer waits
for startup when needed. Closing the viewer
releases the serial port and restores its normal baud rate; a lost connection
also expires automatically. Flashing and SD Files are unavailable while the
viewer owns the port. Older firmware without the mirror capability displays
an upgrade message; install the included current firmware first.
Firmware with mirroring but without `screen-mirror-input-v1` remains viewable
with its physical buttons; the viewer identifies that keyboard support needs
a firmware update.

The Mini retains one 32 KiB RGB565 shadow of the actual panel writes, preferably
in PSRAM, so connecting does not require a redraw of the active screen. A
4 KiB background task handles mirror requests even during a blocking foreground
operation. Each USB packet contains at most one row (278 bytes including its
header and CRC), with compression when useful and acknowledgement/retry for
corrupt packets. There is no frame queue on the Mini. The desktop keeps only
the latest pending image. The USB session negotiates 921600 baud and returns to
115200 on disconnect or timeout. Keyboard state travels in those same
session-bound row requests, with a 600 ms input lease that releases keys if
updates stop.

`selftest mirror` checks physical display capture paths with synthetic graphics
and deliberately blocks the foreground for 6.5 seconds to exercise background
mirroring. Run it with radio modes stopped. `mirror status` reports the capture
buffer location, free heap, and capture/transfer timing counters.

## SD card layout

The Mini V3 initializes the SD card before the display on their shared SPI bus.
If the normal 4 MHz mount fails, startup retries once at 1 MHz. The serial boot
log reports a successful mount or a failure; a failed mount leaves SD features
unavailable until the next restart.

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
