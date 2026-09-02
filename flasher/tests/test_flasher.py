from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from flasher.marauder_eternal_flasher import (
    APP_NAME,
    APPLICATION_OFFSET,
    APPLICATION_SLOT_OFFSETS,
    ESP32_C5_IMAGE_ID,
    ESP_IMAGE_MAGIC,
    OutputCapture,
    PortEntry,
    SD_FILES_DOWNLOADING_TEXT,
    SD_FILES_LOADING_TEXT,
    SD_FILE_UPLOADING_TEXT,
    SD_WINDOW_HEIGHT,
    SD_WINDOW_WIDTH,
    SdFileEntry,
    SdProtocolError,
    SdSerialClient,
    decode_sd_path,
    encode_sd_path,
    evil_portal_upload_path,
    extract_percent,
    format_file_size,
    friendly_failure,
    inspect_firmware,
    local_path_for_sd,
    parse_sd_protocol_message,
    port_preference,
    validate_firmware,
)


class SdWindowPresentationTests(unittest.TestCase):
    def test_sd_window_uses_requested_larger_default(self) -> None:
        self.assertEqual(SD_WINDOW_WIDTH, 1296)
        self.assertEqual(SD_WINDOW_HEIGHT, 988)

    def test_sd_wait_messages_are_explicit(self) -> None:
        self.assertEqual(SD_FILES_LOADING_TEXT, "SD Card Files Loading....")
        self.assertIn("Downloading", SD_FILES_DOWNLOADING_TEXT)
        self.assertIn("Uploading", SD_FILE_UPLOADING_TEXT)


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


def protocol_line(**values: object) -> bytes:
    message: dict[str, object] = {"protocol": 1, "code": "OK"}
    message.update(values)
    return b"@MARAUDER:" + json.dumps(message, separators=(",", ":")).encode() + b"\n"


class FakeSerialConnection:
    def __init__(self, incoming: bytes) -> None:
        self.incoming = bytearray(incoming)
        self.written = bytearray()
        self.timeout = 0.25

    def write(self, payload: bytes) -> int:
        self.written.extend(payload)
        return len(payload)

    def flush(self) -> None:
        pass

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        maximum = len(self.incoming) if size is None else min(size, len(self.incoming))
        marker = self.incoming.find(expected, 0, maximum)
        count = marker + len(expected) if marker >= 0 else maximum
        result = bytes(self.incoming[:count])
        del self.incoming[:count]
        return result

    def read(self, size: int) -> bytes:
        count = min(size, len(self.incoming))
        result = bytes(self.incoming[:count])
        del self.incoming[:count]
        return result

    def close(self) -> None:
        pass


class SdProtocolTests(unittest.TestCase):
    def test_path_encoding_and_local_mapping(self) -> None:
        path = "/captures/scan 01.pcap"
        self.assertEqual(decode_sd_path(encode_sd_path(path)), path)
        with tempfile.TemporaryDirectory() as directory:
            destination = local_path_for_sd(Path(directory), path)
            self.assertEqual(destination, Path(directory).resolve() / "captures" / "scan 01.pcap")
        with self.assertRaises(SdProtocolError):
            encode_sd_path("/captures/../config/key.txt")

    def test_protocol_parser_ignores_console_and_parses_machine_json(self) -> None:
        self.assertIsNone(parse_sd_protocol_message(b"> ordinary console output\n"))
        message = parse_sd_protocol_message(protocol_line(
            tx="abc", command="sdlist", status="started"
        ))
        self.assertIsNotNone(message)
        self.assertEqual(message["tx"], "abc")  # type: ignore[index]

    def test_lists_files_and_validates_summary(self) -> None:
        tx = "listtx"
        first = SdFileEntry("/captures/eapol.pcap", 12, 100)
        second = SdFileEntry("/logs/scan.log", 8, 200)
        incoming = b"".join((
            protocol_line(tx=tx, command="sdlist", status="started", files=0, bytes=0),
            protocol_line(tx=tx, command="sdlist", status="file",
                          pathHex=encode_sd_path(first.path), bytes=first.size,
                          modified=first.modified),
            protocol_line(tx=tx, command="sdlist", status="file",
                          pathHex=encode_sd_path(second.path), bytes=second.size,
                          modified=second.modified),
            protocol_line(tx=tx, command="sdlist", status="success", files=2, bytes=20),
        ))
        connection = FakeSerialConnection(incoming)
        with patch("flasher.marauder_eternal_flasher.secrets.token_hex", return_value=tx):
            files = SdSerialClient("test", connection).list_files()
        self.assertEqual(files, [first, second])
        self.assertIn(b"sdlist --machine listtx\n", connection.written)

    def test_enters_and_exits_locked_sd_session(self) -> None:
        connection = FakeSerialConnection(b"".join((
            protocol_line(tx="begin", command="sdsession", status="success"),
            protocol_line(tx="end", command="sdsession", status="success"),
        )))
        client = SdSerialClient("test", connection)
        with patch(
            "flasher.marauder_eternal_flasher.secrets.token_hex",
            side_effect=("begin", "end"),
        ):
            client.set_session(True)
            client.set_session(False)
        self.assertEqual(
            bytes(connection.written),
            b"sdsession --machine begin --state begin\n"
            b"sdsession --machine end --state end\n",
        )

    def test_downloads_binary_payload_and_verifies_digest(self) -> None:
        tx = "gettx"
        entry = SdFileEntry("/captures/raw.pcap", 7, 0)
        payload = b"\x00\n@\xffabc"
        digest = hashlib.sha256(payload).hexdigest()
        incoming = b"".join((
            protocol_line(tx=tx, command="sdget", status="started",
                          pathHex=encode_sd_path(entry.path), bytes=len(payload)),
            payload,
            b"\n",
            protocol_line(tx=tx, command="sdget", status="success",
                          bytes=len(payload), sha256=digest),
        ))
        connection = FakeSerialConnection(incoming)
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "raw.pcap"
            with patch("flasher.marauder_eternal_flasher.secrets.token_hex", return_value=tx):
                SdSerialClient("test", connection).download_file(entry, destination)
            self.assertEqual(destination.read_bytes(), payload)
        self.assertIn(encode_sd_path(entry.path).encode(), connection.written)

    def test_uploads_evil_portal_html_and_verifies_digest(self) -> None:
        tx = "puttx"
        payload = b"<html><body>Portal</body></html>\n"
        digest = hashlib.sha256(payload).hexdigest()
        destination = "/evil_portal/html/Portal.html"
        incoming = b"".join((
            protocol_line(tx=tx, command="sdput", status="ready",
                          pathHex=encode_sd_path(destination), bytes=len(payload)),
            protocol_line(tx=tx, command="sdput", status="success",
                          pathHex=encode_sd_path(destination), bytes=len(payload),
                          sha256=digest),
        ))
        connection = FakeSerialConnection(incoming)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Portal.HTML"
            source.write_bytes(payload)
            with patch("flasher.marauder_eternal_flasher.secrets.token_hex",
                       return_value=tx):
                entry = SdSerialClient("test", connection).upload_evil_portal_html(
                    source, overwrite=True
                )
        self.assertEqual(entry, SdFileEntry(destination, len(payload), 0))
        command, transferred = bytes(connection.written).split(b"\n", 1)
        self.assertIn(b"sdput --machine puttx", command)
        self.assertIn(b"--overwrite", command)
        self.assertEqual(transferred, payload)

    def test_evil_portal_upload_path_rejects_other_files(self) -> None:
        self.assertEqual(
            evil_portal_upload_path(Path("Login.HTML")),
            "/evil_portal/html/Login.html",
        )
        with self.assertRaises(SdProtocolError):
            evil_portal_upload_path(Path("notes.txt"))

    def test_digest_failure_does_not_replace_existing_file(self) -> None:
        tx = "badget"
        entry = SdFileEntry("/logs/result.log", 4, 0)
        payload = b"data"
        incoming = b"".join((
            protocol_line(tx=tx, command="sdget", status="started",
                          pathHex=encode_sd_path(entry.path), bytes=len(payload)),
            payload,
            b"\n",
            protocol_line(tx=tx, command="sdget", status="success",
                          bytes=len(payload), sha256="0" * 64),
        ))
        connection = FakeSerialConnection(incoming)
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "result.log"
            destination.write_bytes(b"original")
            with patch("flasher.marauder_eternal_flasher.secrets.token_hex", return_value=tx):
                with self.assertRaisesRegex(SdProtocolError, "digest did not match"):
                    SdSerialClient("test", connection).download_file(entry, destination)
            self.assertEqual(destination.read_bytes(), b"original")
            self.assertEqual(list(Path(directory).glob("*.part")), [])

    def test_formats_sizes(self) -> None:
        self.assertEqual(format_file_size(42), "42 B")
        self.assertEqual(format_file_size(2048), "2.0 KB")


if __name__ == "__main__":
    unittest.main()
