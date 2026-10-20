# Marauder Eternal Flasher

Flasher 1.7.0 is a one-click desktop flasher for the Marauder Mini V3 with an
ESP32-C5 and 8 MB flash. Standard packages include the verified Marauder
Eternal 1.16.4 full-device image and also allow the user to choose another
`.bin`.

## Download updates

The launcher checks the
[Eternal Mini V3 releases](https://github.com/n0vajay05/ESP32MarauderEternal-MiniV3/releases)
when it opens. **Check Online For Update**, next to **Choose BIN** and **Use
Included**, runs the same check manually. Both checks look for firmware and
flasher releases independently, including flasher-only releases. The check reads
the 100 most recent release entries and ignores drafts and prereleases.
It compares numeric firmware versions
against the newer of the included and selected firmware. A newer release shows
**Newest MarauderEternal version (version number) found, do you want to
download?** Otherwise it shows **Up To Date**. If the server cannot be reached,
it shows **Failed to connect to update server, please try again later.**

Accept the download and choose where to save its BIN. The app downloads the
application image, verifies its size, SHA-256, device identity, version, and
partition layout, then selects it. Click **Connect & Flash** separately when
ready. Application images preserve saved settings when flashed to a compatible
Mini. Failed or cancelled downloads leave the previous selection and any
existing destination file intact.

If the included BIN or manifest is missing, or the selected BIN becomes
unavailable or invalid, the launcher can still open. The button says **Download
Firmware** and fetches the latest stable release even if its version matches
the launcher's release. Flashing is disabled until a valid BIN is selected;
mirroring and SD Files remain available. Choosing a valid BIN or completing a
download restores **Check Online For Update**. Startup checks wait until active
USB operations finish, and network work runs in a background thread.

A newer flasher for the current platform shows **Newest MarauderEternal Flasher
version (version number) found, do you want to download and restart?** Accepting
downloads the matching Linux or Windows x86-64 application, verifies its size,
SHA-256, executable architecture, version, and startup health, then replaces and
restarts the launcher. A separate helper waits for the launcher to exit before
replacement, including Windows EXE locks. The old executable is retained with
a `.previous` suffix; a replacement/launch failure preserves or restores it.
The selected external firmware file is passed to the new launcher. If both
updates are available, the flasher is offered first and firmware is checked
again after restart. Declining the flasher still allows the firmware update.

Portable packages update at their existing location, which must be writable.
When run from Python source, choose where to save the new standalone launcher;
the source checkout is preserved. Unsupported desktop architectures continue
to support firmware downloads without offering an incompatible flasher.

## Publish desktop updates

Builds generate a versioned native executable plus `flasher-linux-x86_64.json`
or `flasher-windows-x86_64.json` and platform checksums. Upload these generated
files as GitHub **release assets** so installed launchers can find them. Actions
build artifacts alone are not downloadable by the public updater.

The desktop build workflow builds both native packages. Publishing a release
runs it and attaches the platform update assets automatically. To populate an
existing draft release before publication, run **Build desktop flashers** with
its `release_tag` input; leaving the input blank creates build artifacts only.
The selected workflow ref supplies the source for a manually dispatched build.
For immutable releases, upload assets while the release is still a draft, then
publish it. No release is created or published by the build workflow.

For a new desktop-only release without a firmware manifest, the Linux job also
attaches the current checked-in firmware bundle. Existing firmware assets are
preserved when a release already has a manifest. This keeps the Mini's WiFi OTA
checker compatible with GitHub's latest release while flasher versions advance
independently.

Update `flasher_version` through `scripts/package_release.py` and the matching
launcher fallback/workflow version when releasing desktop changes. Firmware
and flasher versions remain independent. Keep update protocol 1 and the
`--update-health`, `--apply-flasher-update`, and `--firmware` handoff interfaces
compatible in later self-updating packages.

## Use the application

1. Connect the Mini V3 with a USB data cable.
2. Start `MarauderEternalFlasher` on Linux or
   `MarauderEternalFlasher.exe` on Windows AFTER building it via powershell script, with python installed in your windows environment.
3. Select the detected serial device if more than one is connected.
4. Leave the included firmware selected, or click **Choose BIN** to select a
   different ESP32-C5 full-device or application image.
5. Click **Connect & Flash** once.
6. Leave the device connected until the progress bar reaches 100% and the app
   reports success.

## Present the Mini's screen

Select the Mini's USB serial device and click **Screen Mirror**. A separate
window shows the live screen at an enlarged, sharp pixel scale. While that
window has focus, use **arrow keys** for the Mini's directional buttons and
**C** for Center/select; held keys support the existing button hold behavior.
The physical buttons remain usable. Keyboard input is enabled only during an
active mirror connection and releases on focus loss or disconnect. Click the
screen to give it focus. Use **F11** or **Fullscreen** for presentations,
**Esc** to leave fullscreen, and **Save PNG** to save the native screen image.
The window can be shared or captured with OBS.

Open the viewer before starting the activity you want to demonstrate. Opening
USB serial can reset the Mini with some host drivers; the viewer waits for
startup when needed. It uses the USB cable and does not require WiFi or an IP
address. Click **Disconnect**
or close the mirror window before flashing firmware or opening SD Files. **Reconnect**
resumes after a USB error. The Mini restores normal serial mode automatically
if the viewer stops responding.

Screen Mirror requires firmware advertising `screen-mirror-v1`; older firmware
shows an upgrade message. The included current firmware provides that support.
Keyboard control additionally requires `screen-mirror-input-v1`; firmware with
the original mirror support remains viewable using the Mini's physical buttons.

## Copy files from the SD card

1. Start the device normally and stop any active scan, capture, attack, or Evil
   Portal session.
2. Select its serial device in the flasher and click **SD Files — Browse &
   Download**.
3. Click **Refresh**, select one or more files, then use **Download Selected**.
   Use **Download All** to copy the complete SD directory structure.

To install a captive-portal template, click **Upload Evil Portal HTML** and
choose a non-empty `.html` file smaller than 30,000 bytes. The flasher stores it
under `/evil_portal/html`, verifies its SHA-256 digest before committing it, and
asks before replacing a template with the same name. The device refreshes its
**WiFi > Attacks > Evil Portal > Select EP HTML File** list as soon as the
upload completes.

The SD card can remain installed in the Mini V3. Transfers use the normal USB
serial connection, preserve folders during batch downloads, and verify every
file with SHA-256 before replacing its destination. Interrupted HTML uploads
leave the prior template intact. Files such as Evil Portal credentials may
contain sensitive data and should be handled securely.

The SD Files window keeps one serial connection open for its full lifetime, so
listing, downloading, and uploading do not repeatedly reset the device. While
the window is connected, the Mini V3 shows the current USB SD operation and
locks its normal menus. Close the window to return to normal operation; holding
the device's center button also provides an emergency exit.

The enlarged SD Files window shows a prominent red activity message while its
file list is loading and throughout downloads, uploads, and session shutdown,
so a serial operation is never mistaken for an unresponsive application.
The status, progress, and action rows are reserved independently of the file
list, keeping Refresh, Upload, Download All, and Download Selected visible at
the initial window size and under desktop display scaling.

The app validates the selected image, detects its safe address, requires an
ESP32-C5 response before writing, shows live write progress, and reports a
specific reason for common failures. Recognized full-device images are written
at `0x0`; ESP32-C5 application images are written to both OTA slots at
`0x10000` and `0x400000`. Individual bootloader, partition-table, and boot
metadata files are rejected.

The included full-device image replaces all 8 MB of flash and erases existing
settings and logs. Application-only images preserve the other flash regions
but require an already-compatible partition table.

### Linux serial permissions

If the app reports permission denied, add the current user to the serial-port
group used by the distribution (commonly `dialout`), then log out and back in:

```bash
sudo usermod -aG dialout "$USER"
```

### Manual download mode

Automatic reset normally enters the ESP32-C5 ROM downloader. If connection
fails and the board exposes BOOT/FLASH and RESET/EN controls, hold BOOT while
pressing and releasing RESET, then retry the flash.

## Build standalone applications

PyInstaller must build separately on each target operating system; it does not
cross-compile a Windows executable from Linux.

On Linux:

```bash
./scripts/build-gui-linux.sh
```

On Windows PowerShell:

```powershell
.\scripts\build-gui-windows.ps1
```

Outputs are written under `dist/linux` or `dist/windows`. The GitHub Actions
workflow at `.github/workflows/build-flasher.yml` builds and tests both targets
on native runners.

## Run from source

Create a Python environment, install `flasher/requirements-build.txt`, then
run:

```bash
python -m flasher.marauder_eternal_flasher
```

Run non-GUI checks with:

```bash
python -m unittest discover -v
python -m flasher.marauder_eternal_flasher --self-test
```

See `THIRD_PARTY_NOTICES.md` for desktop flasher dependency licensing.
