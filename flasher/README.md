# Marauder Eternal Flasher

This is a one-click desktop flasher for the Marauder Mini V3 with an ESP32-C5
and 8 MB flash. Packaged applications include the verified Marauder Eternal
1.14.3 full-device image and also allow the user to choose another `.bin`.

## Use the application

1. Connect the Mini V3 with a USB data cable.
2. Start `MarauderEternalFlasher` on Linux or
   `MarauderEternalFlasher.exe` on Windows.
3. Select the detected serial device if more than one is connected.
4. Leave the included firmware selected, or click **Choose BIN** to select a
   different ESP32-C5 full-device or application image.
5. Click **Connect & Flash** once.
6. Leave the device connected until the progress bar reaches 100% and the app
   reports success.

The app validates the selected image, detects its safe address, requires an
ESP32-C5 response before writing, shows live write progress, and reports a
specific reason for common failures. Recognized full-device images are written
at `0x0`; ESP32-C5 application images are written at `0x10000`. Individual
bootloader, partition-table, and boot metadata files are rejected.

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
