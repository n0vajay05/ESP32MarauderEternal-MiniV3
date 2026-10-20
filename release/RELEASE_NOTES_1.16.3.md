# Marauder Eternal Mini V3 1.16.3

Firmware 1.16.3 adds WiFi firmware updates from the Eternal Mini V3 GitHub
release repository and corrects SD startup on the shared display/SD SPI bus.
The desktop Flasher remains version 1.5.0 and bundles firmware 1.16.3.

## WiFi firmware updates

- Open **Device > Update Firmware > Update Over WiFi**. Scan for a network or
  enter an SSID manually, then enter its password with the joystick keyboard.
  Lowercase, uppercase, numbers, symbols, and masked passwords are supported.
- Show **Connected: (SSID)** above the update actions when connected, or
  **Please Connect To Update** otherwise. Open **Network Info** explicitly to
  view or edit IP address, subnet mask, gateway, and both DNS addresses.
- Save the last successful update network and manual address settings.
  **Forget Network** disconnects and clears that profile. A forgotten profile
  is not silently imported again from the general WiFi profiles.
- Check the fixed repository with a 15-second server timeout. Application
  requests are limited to its GitHub release API and release assets, including
  the required GitHub asset redirects. DHCP and DNS remain available for
  network setup. An SD card is not required.
- Offer newer numeric firmware versions, download the application image into
  the inactive OTA slot, and verify its SHA-256 checksum and embedded device,
  version, and partition identity before selecting it for boot. Accept the new
  image after startup checks pass; failed downloads keep the current image.
- Verify HTTPS certificates and use PSRAM for the update task's TLS
  allocations. Restore normal allocation after the request. Serial status
  identifies connection stages and native errors without exposing credentials.
- Disconnect WiFi and disable automatic reconnection when leaving
  **Update Firmware**. USB screen mirroring and arrow/C controls work during
  setup. **Update From SD** remains available with a mounted card.

## SD startup

- Deselect both devices before starting the shared SPI bus and initialize the
  SD card before any TFT traffic.
- Retry a failed 4 MHz mount once at 1 MHz, then report a successful mount or
  failure in the serial boot log.

## Release packages and validation

- Update firmware identity, versioned images, manifests, checksums, release
  documentation, and both GitHub Actions firmware-version settings to 1.16.3.
- Build and verify the release images and Linux flasher bundle, run native
  regression checks and desktop tests, and check the bundled Linux GUI.
  Windows packaging remains available through its native GitHub Actions runner.
- The OTA and SD changes were verified on the Mini in the preceding 1.16.2
  development build: saved-network GitHub connectivity, update menus, isolated
  OTA checks, screen mirroring and controls, and five hardware/software resets
  with complete SD listings. All 204 file names, sizes, and timestamps matched
  the baseline. The user also confirmed a green SD indicator after a full
  power-off/on.
- This 1.16.3 version update is prepared without flashing the device. The
  connected Mini retains its previously installed firmware.

Hardware, pinned dependencies, and partition layout remain unchanged: Marauder
Mini V3, ESP32-C5, 8 MB flash with PSRAM, Arduino ESP32 core 3.3.4, and
`mini-v3-c5-8m-ota-v1`. Earlier enhancements, mirroring, and keyboard controls
remain included.
