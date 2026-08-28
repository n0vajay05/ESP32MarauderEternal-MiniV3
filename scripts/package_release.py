#!/usr/bin/env python3
"""Create or verify the Mini V3 release bundle from an Arduino CLI build."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
from datetime import date
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
RELEASE_DIR = PROJECT_ROOT / "release"
CONFIG_PATH = PROJECT_ROOT / "firmware" / "MarauderEternal" / "configs.h"
CORE_VERSION = "3.3.4"
HARDWARE = "Marauder Mini V3"
METADATA_HARDWARE = "Marauder Eternal Mini V3"
CHIP = "ESP32-C5"
METADATA_CHIP = "esp32c5"
PARTITION_LAYOUT = "mini-v3-c5-8m-ota-v1"
FQBN = "esp32:esp32:esp32c5:FlashSize=8M,PartitionScheme=custom,PSRAM=enabled"
FLASH_SIZE = 8 * 1024 * 1024
APP_SLOT_SIZE = 0x3C0000
APP_OFFSETS = (0x10000, 0x400000)
METADATA_MAGIC = b"MRDRFWID"
METADATA_END_MAGIC = b"DIWFRDRM"
METADATA_SIZE = 128


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def firmware_version() -> str:
    source = CONFIG_PATH.read_text(encoding="utf-8")
    match = re.search(r'^\s*#define\s+MARAUDER_VERSION\s+"v([^\"]+)"', source, re.MULTILINE)
    if not match:
        raise RuntimeError(f"MARAUDER_VERSION was not found in {CONFIG_PATH}")
    return match.group(1)


def git_value(*arguments: str) -> str:
    result = subprocess.run(
        ["git", *arguments], cwd=PROJECT_ROOT, check=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    return result.stdout.strip()


def identity_from(payload: bytes) -> tuple[str, str, str, str]:
    marker = payload.find(METADATA_MAGIC)
    while marker >= 0:
        candidate = payload[marker : marker + METADATA_SIZE]
        if len(candidate) == METADATA_SIZE and candidate[-8:] == METADATA_END_MAGIC:
            if candidate[8] != 2:
                raise RuntimeError(f"Unsupported firmware metadata schema {candidate[8]}")

            def field(start: int, end: int) -> str:
                value = candidate[start:end]
                terminator = value.find(b"\x00")
                if terminator <= 0:
                    raise RuntimeError("Firmware identity has an invalid field")
                return value[:terminator].decode("ascii")

            return field(12, 60), field(60, 72), field(72, 88), field(88, 120)
        marker = payload.find(METADATA_MAGIC, marker + 1)
    raise RuntimeError("Marauder Eternal identity metadata is missing")


def validate_identity(path: Path, expected_version: str) -> None:
    identity = identity_from(path.read_bytes())
    expected = (METADATA_HARDWARE, METADATA_CHIP, f"v{expected_version}", PARTITION_LAYOUT)
    if identity != expected:
        raise RuntimeError(f"Unexpected firmware identity in {path.name}: {identity!r}")


def partition_entries(payload: bytes) -> set[tuple[int, int, int, int]]:
    entries: set[tuple[int, int, int, int]] = set()
    offset = 0
    while offset + 32 <= len(payload) and payload[offset : offset + 2] == b"\xaa\x50":
        entries.add((payload[offset + 2], payload[offset + 3],
                     int.from_bytes(payload[offset + 4 : offset + 8], "little"),
                     int.from_bytes(payload[offset + 8 : offset + 12], "little")))
        offset += 32
    return entries


def verify_release() -> None:
    manifest_path = RELEASE_DIR / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    version = firmware_version()
    if manifest.get("version") != version:
        raise RuntimeError("Manifest and firmware versions do not match")
    if manifest.get("partition_layout") != PARTITION_LAYOUT:
        raise RuntimeError("Manifest partition layout is missing or incorrect")
    if manifest.get("application_flash_offsets") != ["0x10000", "0x400000"]:
        raise RuntimeError("Manifest does not require both OTA application slots")

    records = [manifest["full_device_image"], *manifest["images"]]
    for record in records:
        path = RELEASE_DIR / record["file"]
        if not path.is_file():
            raise RuntimeError(f"Release file is missing: {path.name}")
        if path.stat().st_size != record["bytes"]:
            raise RuntimeError(f"Release size does not match for {path.name}")
        if sha256(path) != record["sha256"]:
            raise RuntimeError(f"Release hash does not match for {path.name}")

    full_path = RELEASE_DIR / manifest["full_device_image"]["file"]
    if full_path.stat().st_size != FLASH_SIZE:
        raise RuntimeError("Full-device image is not exactly 8 MB")
    image_by_offset = {int(record["offset"], 16): record for record in manifest["images"]}
    full = full_path.read_bytes()
    for offset, record in image_by_offset.items():
        component = (RELEASE_DIR / record["file"]).read_bytes()
        if full[offset : offset + len(component)] != component:
            raise RuntimeError(f"Merged image differs from {record['file']} at 0x{offset:x}")

    required_partitions = {
        (0, 0x10, 0x10000, APP_SLOT_SIZE),
        (0, 0x11, 0x400000, APP_SLOT_SIZE),
    }
    partitions = (RELEASE_DIR / image_by_offset[0x8000]["file"]).read_bytes()
    if not required_partitions.issubset(partition_entries(partitions)):
        raise RuntimeError("Release partition table does not contain both required OTA slots")

    app_path = RELEASE_DIR / image_by_offset[0x10000]["file"]
    if app_path.stat().st_size > APP_SLOT_SIZE:
        raise RuntimeError("Application image exceeds its OTA slot")
    validate_identity(app_path, version)
    validate_identity(full_path, version)

    checksum_lines = (RELEASE_DIR / "SHA256SUMS").read_text(encoding="ascii").splitlines()
    expected_lines = sorted(f"{record['sha256']}  {record['file']}" for record in records)
    if checksum_lines != expected_lines:
        raise RuntimeError("SHA256SUMS does not exactly match the release manifest")

    print(f"Release {version} verified: {len(records)} images, both OTA slots, valid identity")


def package_release(build_dir: Path) -> None:
    build_dir = build_dir.resolve()
    version = firmware_version()
    base_name = f"Marauder_Eternal_{version}_MiniV3_ESP32-C5"
    sources = {
        "full": build_dir / "MarauderEternal.ino.merged.bin",
        "bootloader": build_dir / "MarauderEternal.ino.bootloader.bin",
        "partitions": build_dir / "MarauderEternal.ino.partitions.bin",
        "app": build_dir / "MarauderEternal.ino.bin",
    }
    boot_app0 = (
        Path.home() / ".arduino15" / "packages" / "esp32" / "hardware" /
        "esp32" / CORE_VERSION / "tools" / "partitions" / "boot_app0.bin"
    )
    sources["boot_app0"] = boot_app0
    for label, source in sources.items():
        if not source.is_file():
            raise RuntimeError(f"{label} build image is missing: {source}")

    destinations = {
        "full": RELEASE_DIR / f"{base_name}.bin",
        "bootloader": RELEASE_DIR / f"{base_name}.bootloader.bin",
        "partitions": RELEASE_DIR / f"{base_name}.partitions.bin",
        "boot_app0": RELEASE_DIR / f"{base_name}.boot_app0.bin",
        "app": RELEASE_DIR / f"{base_name}.app.bin",
    }
    RELEASE_DIR.mkdir(parents=True, exist_ok=True)
    for label, destination in destinations.items():
        shutil.copyfile(sources[label], destination)

    if destinations["full"].stat().st_size != FLASH_SIZE:
        raise RuntimeError("Arduino merged image is not exactly 8 MB")
    if destinations["app"].stat().st_size > APP_SLOT_SIZE:
        raise RuntimeError("Application image exceeds its OTA slot")
    validate_identity(destinations["app"], version)
    validate_identity(destinations["full"], version)

    components = (
        ("bootloader", "0x2000"),
        ("partitions", "0x8000"),
        ("boot_app0", "0xe000"),
        ("app", "0x10000"),
    )
    images = [
        {
            "file": destinations[label].name,
            "offset": offset,
            "bytes": destinations[label].stat().st_size,
            "sha256": sha256(destinations[label]),
        }
        for label, offset in components
    ]
    source_dirty = bool(git_value("status", "--porcelain", "--untracked-files=all"))
    full = destinations["full"]
    manifest = {
        "product": "ESP32 Marauder Eternal",
        "version": version,
        "flasher_version": "1.2.0",
        "hardware": HARDWARE,
        "chip": CHIP,
        "flash_size": "8MB",
        "psram": True,
        "partition_layout": PARTITION_LAYOUT,
        "application_flash_offsets": ["0x10000", "0x400000"],
        "fqbn": FQBN,
        "esp32_arduino_core": CORE_VERSION,
        "source_base_commit": git_value("rev-parse", "HEAD"),
        "source_worktree_modified": source_dirty,
        "build_date": date.today().isoformat(),
        "full_device_image": {
            "file": full.name,
            "offset": "0x0",
            "bytes": full.stat().st_size,
            "sha256": sha256(full),
            "contains": [
                "bootloader@0x2000",
                "partitions@0x8000",
                "boot_app0@0xe000",
                "application@0x10000",
            ],
        },
        "images": images,
    }
    (RELEASE_DIR / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    records = [manifest["full_device_image"], *images]
    checksum_lines = sorted(f"{record['sha256']}  {record['file']}" for record in records)
    (RELEASE_DIR / "SHA256SUMS").write_text(
        "\n".join(checksum_lines) + "\n", encoding="ascii"
    )

    expected_names = {path.name for path in destinations.values()}
    for old_image in RELEASE_DIR.glob("Marauder_Eternal_*_MiniV3_ESP32-C5*.bin"):
        if old_image.name not in expected_names:
            old_image.unlink()

    verify_release()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", nargs="?", type=Path, default=PROJECT_ROOT / "build")
    parser.add_argument("--verify", action="store_true", help="verify the existing release only")
    arguments = parser.parse_args()
    if arguments.verify:
        verify_release()
    else:
        package_release(arguments.build_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
