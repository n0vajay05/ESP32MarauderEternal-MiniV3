from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path

from flasher.marauder_eternal_flasher import (
    APP_NAME,
    APPLICATION_OFFSET,
    APPLICATION_SLOT_OFFSETS,
    ESP32_C5_IMAGE_ID,
    ESP_IMAGE_MAGIC,
    OutputCapture,
    PortEntry,
    extract_percent,
    friendly_failure,
    inspect_firmware,
    port_preference,
    validate_firmware,
)


def eternal_metadata() -> bytes:
    def field(value: str, size: int) -> bytes:
        encoded = value.encode("ascii")
        return encoded + b"\x00" * (size - len(encoded))

    return b"".join(
        (
            b"MRDRFWID",
            bytes((2, 0, 0, 0)),
            field("Marauder Eternal Mini V3", 48),
            field("esp32c5", 12),
            field("v1.15.1", 16),
            field("mini-v3-c5-8m-ota-v1", 32),
            b"DIWFRDRM",
        )
    )


def partition_entry(subtype: int, offset: int, size: int) -> bytes:
    return (
        b"\xaa\x50"
        + bytes((0, subtype))
        + offset.to_bytes(4, "little")
        + size.to_bytes(4, "little")
        + b"app\x00".ljust(16, b"\x00")
        + b"\x00" * 4
    )


class FirmwareValidationTests(unittest.TestCase):
    def test_valid_firmware(self) -> None:
        payload = b"firmware-image"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "firmware.bin"
            path.write_bytes(payload)
            validate_firmware(path, len(payload), hashlib.sha256(payload).hexdigest())

    def test_rejects_wrong_size(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "firmware.bin"
            path.write_bytes(b"short")
            with self.assertRaisesRegex(ValueError, "size is invalid"):
                validate_firmware(path, 100, hashlib.sha256(b"short").hexdigest())

    def test_rejects_wrong_hash(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "firmware.bin"
            path.write_bytes(b"payload")
            with self.assertRaisesRegex(ValueError, "integrity check failed"):
                validate_firmware(path, 7, "0" * 64)

    def test_detects_full_device_image_at_zero(self) -> None:
        payload = bytearray(b"\xff" * (8 * 1024 * 1024))
        payload[0x2000] = ESP_IMAGE_MAGIC
        payload[0x200C:0x200E] = ESP32_C5_IMAGE_ID.to_bytes(2, "little")
        payload[0x8000:0x8020] = partition_entry(0x10, 0x10000, 0x3C0000)
        payload[0x8020:0x8040] = partition_entry(0x11, 0x400000, 0x3C0000)
        payload[0x9000 : 0x9000 + len(eternal_metadata())] = eternal_metadata()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "complete.bin"
            path.write_bytes(payload)
            firmware = inspect_firmware(path)
        self.assertEqual(firmware.image_type, "Full-device image")
        self.assertEqual(firmware.offset, 0)

    def test_rejects_truncated_full_device_image(self) -> None:
        payload = bytearray(b"\xff" * 0x9200)
        payload[0x2000] = ESP_IMAGE_MAGIC
        payload[0x200C:0x200E] = ESP32_C5_IMAGE_ID.to_bytes(2, "little")
        payload[0x8000:0x8020] = partition_entry(0x10, 0x10000, 0x3C0000)
        payload[0x8020:0x8040] = partition_entry(0x11, 0x400000, 0x3C0000)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "truncated.bin"
            path.write_bytes(payload)
            with self.assertRaisesRegex(ValueError, "exactly 8 MB"):
                inspect_firmware(path, allow_unsafe=True)

    def test_detects_application_image_at_application_offset(self) -> None:
        payload = bytearray(b"\x00" * 256)
        payload[0] = ESP_IMAGE_MAGIC
        payload[12:14] = ESP32_C5_IMAGE_ID.to_bytes(2, "little")
        payload[64 : 64 + len(eternal_metadata())] = eternal_metadata()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "custom.app.bin"
            path.write_bytes(payload)
            firmware = inspect_firmware(path)
        self.assertEqual(firmware.image_type, "Application image")
        self.assertEqual(firmware.offset, APPLICATION_OFFSET)
        self.assertEqual(firmware.flash_offsets, APPLICATION_SLOT_OFFSETS)

    def test_rejects_unidentified_application_without_override(self) -> None:
        payload = bytearray(b"\x00" * 64)
        payload[0] = ESP_IMAGE_MAGIC
        payload[12:14] = ESP32_C5_IMAGE_ID.to_bytes(2, "little")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "unknown.app.bin"
            path.write_bytes(payload)
            with self.assertRaisesRegex(ValueError, "not identified"):
                inspect_firmware(path)
            firmware = inspect_firmware(path, allow_unsafe=True)
        self.assertFalse(firmware.identity_verified)

    def test_rejects_image_for_another_chip(self) -> None:
        payload = bytearray(b"\x00" * 64)
        payload[0] = ESP_IMAGE_MAGIC
        payload[12:14] = (9).to_bytes(2, "little")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "other-chip.bin"
            path.write_bytes(payload)
            with self.assertRaisesRegex(ValueError, "not built for an ESP32-C5"):
                inspect_firmware(path, allow_unsafe=True)

    def test_rejects_individual_boot_component(self) -> None:
        payload = bytearray(b"\x00" * 64)
        payload[0] = ESP_IMAGE_MAGIC
        payload[12:14] = ESP32_C5_IMAGE_ID.to_bytes(2, "little")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "image.bootloader.bin"
            path.write_bytes(payload)
            with self.assertRaisesRegex(ValueError, "not an individual boot component"):
                inspect_firmware(path, allow_unsafe=True)

    def test_app_name_is_plain_flasher(self) -> None:
        self.assertEqual(APP_NAME, "Marauder Eternal Flasher")


class OutputTests(unittest.TestCase):
    def test_extracts_live_progress(self) -> None:
        self.assertEqual(extract_percent("Writing... 47.5%"), 47.5)
        self.assertEqual(extract_percent("\x1b[32m100.0%\x1b[0m"), 100.0)
        self.assertIsNone(extract_percent("Connecting..."))

    def test_capture_splits_carriage_return_updates(self) -> None:
        lines: list[str] = []
        capture = OutputCapture(lines.append)
        capture.write("Writing 10%\rWriting 20%\nDone")
        capture.flush()
        self.assertEqual(lines, ["Writing 10%", "Writing 20%", "Done"])


class FailureReasonTests(unittest.TestCase):
    def test_permission_reason(self) -> None:
        self.assertIn("access was denied", friendly_failure("Permission denied: /dev/ttyUSB0"))

    def test_bootloader_reason(self) -> None:
        self.assertIn("download mode", friendly_failure("Failed to connect: No serial data received"))

    def test_wrong_chip_reason(self) -> None:
        self.assertIn("not an ESP32-C5", friendly_failure("Unexpected chip: ESP32-S3; ESP32C5 expected"))

    def test_verification_reason(self) -> None:
        self.assertIn("verification failed", friendly_failure("Hash of data does not match"))


class PortSelectionTests(unittest.TestCase):
    def test_cp210x_is_preferred(self) -> None:
        generic = PortEntry("/dev/ttyS0", "Serial", "", None, None)
        cp210x = PortEntry("/dev/ttyUSB0", "CP2102N", "USB", 0x10C4, 0xEA60)
        self.assertLess(port_preference(cp210x), port_preference(generic))


if __name__ == "__main__":
    unittest.main()
