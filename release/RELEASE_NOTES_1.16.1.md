# Marauder Eternal Mini V3 1.16.1

Firmware 1.16.1 and desktop Flasher 1.4.0 add a live USB screen mirror for
presentations. Connect the Mini, select its serial device in the flasher, and
click **Screen Mirror**. Operate the Mini with its physical buttons while the
computer displays its screen.

## Screen Mirror

- Capture actual display writes, including text, sprites, colors, and rotation.
- Enlarge the display with sharp pixel scaling, enter fullscreen with **F11**,
  leave fullscreen with **Esc**, and save the native image with **Save PNG**.
- Reconnect after a USB error. Flashing and SD file operations are locked while
  the viewer owns the serial port.
- Retain one 32 KiB RGB565 capture buffer, preferably in PSRAM, and use a
  4 KiB background task to serve updates during blocking foreground operations.
- Transfer at most one row per packet (278 bytes including header and CRC),
  compress rows when useful, and retry corrupt packets. The Mini has no frame
  queue; the desktop retains only the latest pending image.
- Negotiate 921600 baud for mirroring and restore normal 115200-baud serial
  operation on disconnect or after a four-second session timeout.
- Detect the `screen-mirror-v1` firmware capability and offer an upgrade message
  for older firmware. Add `selftest mirror` and `mirror status` diagnostics.

The viewer uses the USB cable and needs no WiFi connection or IP address. Open
it before starting the activity you want to present: some USB serial drivers
reset the Mini when opening its port, and the viewer waits for startup.

## Release packages

- Update firmware identity, release filenames, manifests, checksums, desktop
  package identity, documentation, and GitHub Actions artifact versions.
- Flasher 1.4.0 bundles the verified firmware 1.16.1 image. Linux and Windows
  packages build on their respective GitHub Actions runners, with a SHA-256
  checksum included in each desktop artifact.
- Hardware remains Marauder Mini V3 / ESP32-C5 with 8 MB flash and PSRAM,
  Arduino ESP32 core 3.3.4, and partition layout `mini-v3-c5-8m-ota-v1`.
- Application updates write both OTA slots while preserving settings and
  filesystem data. The merged factory image replaces the complete flash.

The saved WiFi profiles, Wardrive geofences, Remote ID tracking and exports,
iBeacon details, GPS dashboards, Fox Hunt filters, and FindMy workflow from
[1.16.0](RELEASE_NOTES_1.16.0.md) remain included.

## Validation

- Pass all native regression checks and 37 desktop tests, including mirror
  capture bounds, rotations, packet framing, CRC handling, and reconnect paths.
- Pass source and bundled Linux GUI checks and the standalone flasher self-test;
  verify that the bundled firmware matches the release image and SHA-256 digest.
- Flash both application slots on the Mini and verify both hashes. The device
  reports `v1.16.1` and passes isolated FindMy diagnostics and scan/menu checks.
- Verify a complete mirror frame against the device screenshot with zero pixel
  differences below the changing status bar. Confirm live updates during BLE
  discovery, a 32 KiB buffer in PSRAM, and normal serial operation after closing
  the mirror session.
- Build and verify the Linux executable locally. Windows packaging is prepared
  in GitHub Actions and must run on its Windows runner.
