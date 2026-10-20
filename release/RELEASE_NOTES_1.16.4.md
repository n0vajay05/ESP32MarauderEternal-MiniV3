# Marauder Eternal Mini V3 1.16.4

Release 1.16.4 bundles firmware 1.16.4 with Desktop Flasher 1.7.0. It adds
desktop firmware downloads, missing-firmware recovery, and native flasher
self-updates. Firmware and flasher version numbers are independent.

## Desktop updates

- Check both firmware and flasher versions during the same startup/manual
  operation. Flasher-only releases are supported. Offer newer native Linux or
  Windows x86-64 launchers before firmware updates.
- After confirmation, verify the flasher package and startup health, wait for
  the running launcher to exit, replace it, and restart with the selected
  external firmware file. Retain the prior executable and restore it if the
  replacement cannot launch. A separate helper handles Windows file locks.
- Generate versioned native binaries, platform metadata, and checksums. The
  GitHub desktop workflow attaches these assets on release publication or when
  manually run with an existing release tag. Firmware is not flashed during a
  flasher update.
- Check the release repository automatically on launch or manually with
  **Check Online For Update**, beside **Choose BIN** and **Use Included**.
  Show the requested newer-version download prompt, **Up To Date**, or
  **Failed to connect to update server, please try again later.**
- When no usable BIN is available, keep the launcher open and label the button
  **Download Firmware**. Fetch the latest stable release regardless of the
  bundled version. Missing or invalid included files do not prevent recovery.
- Download into a temporary file, verify size, SHA-256, embedded version,
  Mini V3 identity, and partition layout, then save and select the application
  image. Downloading never starts flashing. Failed downloads preserve any
  existing destination file and the prior firmware selection.
- Use background networking, defer startup checks during device operations,
  and clean up partial downloads when the launcher closes.

## Firmware carried forward

WiFi OTA updates, saved update-network settings, **Forget Network**, the SD
startup fix, USB screen mirroring, and arrow/C controls remain included. See
[1.16.3](RELEASE_NOTES_1.16.3.md) for the OTA and SD changes and their preceding
on-device verification. This release changes the embedded firmware identity to
`v1.16.4` without changing those device features or the partition layout.

## Distribution

- Refresh source identity, VERSION, documentation, and both GitHub Actions
  firmware-version settings to 1.16.4.
- Rebuild all five firmware images, including the application-only OTA image
  and the 8 MB full-device image; regenerate the release manifest and SHA-256
  checksums. The Linux flasher includes the new full-device image.
- Build versioned native flasher executables, platform update metadata, and
  checksums. The desktop workflow builds Linux and Windows x86-64 packages and
  attaches their update assets to a release when published or when manually
  dispatched with its existing release tag. See
  [publishing desktop updates](../flasher/README.md#publish-desktop-updates).
- Preserve the current firmware bundle on desktop-only releases so the Mini's
  WiFi OTA checker continues to find matching firmware assets.

## Validation

- Verified the rebuilt firmware identity, sizes, partition layout, merged
  contents, and all firmware and Linux package checksums. The firmware and
  manifest embedded in the Linux flasher match the release byte for byte.
- Passed native regression checks, all 91 desktop tests, the Linux bundle's
  self-test, and simulated GUI checks. A Linux package containing neither a BIN
  nor a manifest also passed its self-test and GUI checks.
- Passed isolated GUI checks for startup/manual update prompts, **Up To Date**,
  connection errors, missing/deleted BIN recovery, preserving firmware
  selection, and the flasher-update handoff. No device was opened.
- The desktop update functionality was also verified during development using
  isolated GUI checks and real process replacement/restart with test
  executables, backups, and a selected firmware path containing spaces.
  Rollback, changed-file rejection, platform checks, and Windows file-lock
  retries have automated coverage. Native Windows packaging and execution
  require the Windows CI runner.
- Prepared this release without flashing the Mini. No new on-device validation
  is claimed for version 1.16.4.

Hardware and pinned dependencies remain Marauder Mini V3, ESP32-C5, 8 MB flash
with PSRAM, Arduino ESP32 core 3.3.4, and `mini-v3-c5-8m-ota-v1`. All earlier
Eternal enhancements remain included.
