# ESP32 Marauder Eternal 1.14.3

This workspace is the standalone Marauder Eternal firmware for one hardware
target only:

- Marauder Mini V3
- ESP32-C5 revision 1 or later
- 8 MB flash with PSRAM enabled
- 128 x 128 ST7735 display

It contains the source and pinned local Arduino libraries needed to reproduce
the firmware, plus ready-to-flash release images. It intentionally excludes
other Marauder board profiles, historical binaries, backups, installers,
photos, test projects, and unrelated tooling.

## Release identity

- Product: `ESP32 Marauder Eternal`
- Version: `v1.14.3`
- Authors shown on the splash screen: `JustCallMeKoKo/N0vajay05`
- Arduino ESP32 core used for the verified build: `3.3.4`
- Board target: `esp32:esp32:esp32c5`
- Options: `FlashSize=8M,PartitionScheme=custom,PSRAM=enabled`

The source contains a compile-time guard that rejects non-ESP32-C5 board
targets. `MARAUDER_MINI_V3` is also fixed by the project configuration.

## Flash the existing device

The normal flasher updates only the application partition at `0x10000`, which
preserves the installed bootloader, partition table, NVS settings, and stored
device data:

```bash
./scripts/flash.sh /dev/serial/by-id/YOUR_ESP32_SERIAL_DEVICE
```

The script checks the release hashes and confirms that the connected chip is
an ESP32-C5 before writing.

## Single-file factory flash

For a blank board, or when the bootloader and partition table must also be
restored, use this one firmware payload:

```text
release/Marauder_Eternal_1.14.3_MiniV3_ESP32-C5.factory.bin
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
- safe detection of full-device (`0x0`) and application (`0x10000`) images;
- a single **Connect & Flash** button;
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
