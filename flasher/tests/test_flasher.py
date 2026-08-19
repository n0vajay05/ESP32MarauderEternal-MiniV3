from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path

from flasher.marauder_eternal_flasher import (
    APP_NAME,
    APPLICATION_OFFSET,
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
        payload = bytearray(b"\xff" * 0x8010)
        payload[0x2000] = ESP_IMAGE_MAGIC
        payload[0x200C:0x200E] = ESP32_C5_IMAGE_ID.to_bytes(2, "little")
        payload[0x8000:0x8002] = b"\xaa\x50"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "complete.bin"
            path.write_bytes(payload)
            firmware = inspect_firmware(path)
        self.assertEqual(firmware.image_type, "Full-device image")
        self.assertEqual(firmware.offset, 0)

    def test_detects_application_image_at_application_offset(self) -> None:
        payload = bytearray(b"\x00" * 64)
        payload[0] = ESP_IMAGE_MAGIC
        payload[12:14] = ESP32_C5_IMAGE_ID.to_bytes(2, "little")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "custom.app.bin"
            path.write_bytes(payload)
            firmware = inspect_firmware(path)
        self.assertEqual(firmware.image_type, "Application image")
        self.assertEqual(firmware.offset, APPLICATION_OFFSET)

    def test_rejects_image_for_another_chip(self) -> None:
        payload = bytearray(b"\x00" * 64)
        payload[0] = ESP_IMAGE_MAGIC
        payload[12:14] = (9).to_bytes(2, "little")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "other-chip.bin"
            path.write_bytes(payload)
            with self.assertRaisesRegex(ValueError, "not built for an ESP32-C5"):
                inspect_firmware(path)

    def test_rejects_individual_boot_component(self) -> None:
        payload = bytearray(b"\x00" * 64)
        payload[0] = ESP_IMAGE_MAGIC
        payload[12:14] = ESP32_C5_IMAGE_ID.to_bytes(2, "little")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "image.bootloader.bin"
            path.write_bytes(payload)
            with self.assertRaisesRegex(ValueError, "not an individual boot component"):
                inspect_firmware(path)

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
