#!/usr/bin/env python3
"""One-click firmware flasher for Marauder Eternal Mini V3 devices."""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import queue
import re
import secrets
import struct
import sys
import tempfile
import threading
import time
import traceback
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Iterable

import tkinter as tk
from tkinter import filedialog, font as tkfont, messagebox, ttk

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # Shown as a useful GUI error instead of failing at import.
    serial = None
    list_ports = None


APP_NAME = "Marauder Eternal Flasher"


def release_manifest_path() -> Path:
    bundle_root = getattr(sys, "_MEIPASS", None)
    if bundle_root:
        return Path(bundle_root) / "firmware" / "manifest.json"
    return Path(__file__).resolve().parents[1] / "release" / "manifest.json"


def load_release_manifest() -> dict[str, object]:
    path = release_manifest_path()
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError) as error:
        raise RuntimeError(f"Release manifest is missing or invalid: {path}") from error


RELEASE_MANIFEST = load_release_manifest()
APP_VERSION = str(RELEASE_MANIFEST["flasher_version"])
FIRMWARE_VERSION = str(RELEASE_MANIFEST["version"])
INCLUDED_FILENAME = str(RELEASE_MANIFEST["full_device_image"]["file"])
INCLUDED_SHA256 = str(RELEASE_MANIFEST["full_device_image"]["sha256"])
FLASH_SIZE = 8 * 1024 * 1024
APPLICATION_OFFSET = 0x10000
APPLICATION_SLOT_OFFSETS = (0x10000, 0x400000)
APPLICATION_LIMIT = 0x3D0000 - APPLICATION_OFFSET
ESP_IMAGE_MAGIC = 0xE9
ESP32_C5_IMAGE_ID = 0x17
PARTITION_MAGIC = b"\xaa\x50"
FIRMWARE_METADATA_MAGIC = b"MRDRFWID"
FIRMWARE_METADATA_END_MAGIC = b"DIWFRDRM"
FIRMWARE_METADATA_SCHEMA = 2
EXPECTED_HARDWARE = "Marauder Eternal Mini V3"
EXPECTED_CHIP = "esp32c5"
EXPECTED_PARTITION_LAYOUT = "mini-v3-c5-8m-ota-v1"
FLASH_BAUD = "460800"
MIN_ACTIVITY_LINES = 7
DEFAULT_WINDOW_WIDTH = 720
DEFAULT_WINDOW_HEIGHT = 720
# Keep the browser comfortably larger than its original 1080 x 760 layout.
SD_WINDOW_WIDTH = 1296
SD_WINDOW_HEIGHT = 988
SD_WINDOW_MIN_WIDTH = 1080
SD_WINDOW_MIN_HEIGHT = 845
SD_FILES_LOADING_TEXT = "SD Card Files Loading...."
SD_FILES_DOWNLOADING_TEXT = "SD Card Files Downloading...."
SD_FILE_UPLOADING_TEXT = "SD Card File Uploading...."
SD_MODE_CLOSING_TEXT = "SD Card Mode Closing...."
SD_SERIAL_BAUD = 115200
SD_SERIAL_READY_TIMEOUT = 20.0
SD_SERIAL_PROMPT = b"\n> "
SD_PROTOCOL_PREFIX = b"@MARAUDER:"
SD_PROTOCOL_VERSION = 1
SD_TRANSFER_MAX_PATH_BYTES = 512
SD_SESSION_CAPABILITY = "sd-session"
SD_REQUIRED_CAPABILITIES = frozenset(
    ("sd-list", "sd-download", SD_SESSION_CAPABILITY)
)
SD_UPLOAD_CAPABILITY = "sd-upload"
SD_MAX_LIST_ENTRIES = 10000
EVIL_PORTAL_HTML_DIR = "/evil_portal/html"
EVIL_PORTAL_MAX_HTML_BYTES = 30000
EVIL_PORTAL_MAX_FILENAME_BYTES = 240

ANSI_ESCAPE_RE = re.compile(r"\x1b(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")
PERCENT_RE = re.compile(r"(?<![\d.])(100(?:\.0+)?|\d{1,2}(?:\.\d+)?)\s*%")


@dataclass(frozen=True)
class PortEntry:
    device: str
    description: str
    hwid: str
    vid: int | None = None
    pid: int | None = None

    @property
    def label(self) -> str:
        description = self.description.strip() if self.description else "Serial device"
        return f"{self.device}  —  {description}"


@dataclass(frozen=True)
class FirmwareImage:
    path: Path
    image_type: str
    offset: int
    size: int
    sha256: str
    included: bool = False
    identity_verified: bool = True

    @property
    def offset_text(self) -> str:
        return f"0x{self.offset:x}"

    @property
    def size_text(self) -> str:
        return f"{self.size / (1024 * 1024):.2f} MB"

    @property
    def flash_offsets(self) -> tuple[int, ...]:
        return APPLICATION_SLOT_OFFSETS if self.offset == APPLICATION_OFFSET else (self.offset,)

    @property
    def detail(self) -> str:
        if self.offset == APPLICATION_OFFSET:
            offsets = " + ".join(f"0x{offset:x}" for offset in APPLICATION_SLOT_OFFSETS)
            return f"{self.image_type}  •  {self.size_text}  •  writes at {offsets}"
        return f"{self.image_type}  •  {self.size_text}  •  writes at {self.offset_text}"


class UnsafeFirmwareError(ValueError):
    """Raised when an image is valid ESP firmware but lacks Eternal identity."""


@dataclass(frozen=True)
class FirmwareIdentity:
    hardware: str
    chip: str
    version: str
    partition_layout: str


@dataclass(frozen=True)
class SdFileEntry:
    path: str
    size: int
    modified: int = 0

    @property
    def name(self) -> str:
        return self.path.rsplit("/", 1)[-1]


class SdProtocolError(RuntimeError):
    """Raised when SD serial transfer negotiation or validation fails."""

    def __init__(self, message: str, code: str = "PROTOCOL_ERROR") -> None:
        super().__init__(message)
        self.code = code


def resource_path() -> Path:
    """Return the included image path in source and bundled builds."""
    bundle_root = getattr(sys, "_MEIPASS", None)
    if bundle_root:
        return Path(bundle_root) / "firmware" / INCLUDED_FILENAME
    return Path(__file__).resolve().parents[1] / "release" / INCLUDED_FILENAME


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as firmware:
        for chunk in iter(lambda: firmware.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_firmware(
    path: Path,
    expected_size: int = FLASH_SIZE,
    expected_sha256: str = INCLUDED_SHA256,
) -> None:
    if not path.is_file():
        raise FileNotFoundError(f"Embedded firmware is missing: {path}")
    actual_size = path.stat().st_size
    if actual_size != expected_size:
        raise ValueError(
            f"Firmware size is invalid: expected {expected_size} bytes, found {actual_size}."
        )
    actual_hash = sha256_file(path)
    if actual_hash.lower() != expected_sha256.lower():
        raise ValueError(
            "Firmware integrity check failed. Re-download or rebuild the flasher application."
        )


def _image_chip_id(header: bytes) -> int | None:
    if len(header) < 14 or header[0] != ESP_IMAGE_MAGIC:
        return None
    return int.from_bytes(header[12:14], "little")


def _decode_metadata_field(value: bytes, name: str) -> str:
    terminator = value.find(b"\x00")
    if terminator <= 0:
        raise UnsafeFirmwareError(f"Firmware {name} metadata is invalid.")
    try:
        return value[:terminator].decode("ascii")
    except UnicodeDecodeError as error:
        raise UnsafeFirmwareError(f"Firmware {name} metadata is invalid.") from error


def read_firmware_identity(path: Path) -> FirmwareIdentity:
    metadata_size = 128
    overlap = b""
    with path.open("rb") as firmware:
        while chunk := firmware.read(64 * 1024):
            data = overlap + chunk
            search_from = 0
            while True:
                marker = data.find(FIRMWARE_METADATA_MAGIC, search_from)
                if marker < 0:
                    break
                if marker + metadata_size <= len(data):
                    candidate = data[marker : marker + metadata_size]
                    if candidate[-8:] == FIRMWARE_METADATA_END_MAGIC:
                        schema = candidate[8]
                        if schema != FIRMWARE_METADATA_SCHEMA:
                            raise UnsafeFirmwareError(
                                f"Firmware metadata schema {schema} is not supported."
                            )
                        return FirmwareIdentity(
                            _decode_metadata_field(candidate[12:60], "hardware"),
                            _decode_metadata_field(candidate[60:72], "chip"),
                            _decode_metadata_field(candidate[72:88], "version"),
                            _decode_metadata_field(candidate[88:120], "partition layout"),
                        )
                search_from = marker + 1
            overlap = data[-(metadata_size - 1) :]
    raise UnsafeFirmwareError(
        "The selected image is not identified as Marauder Eternal firmware."
    )


def validate_firmware_identity(path: Path) -> FirmwareIdentity:
    identity = read_firmware_identity(path)
    if identity.hardware != EXPECTED_HARDWARE or identity.chip != EXPECTED_CHIP:
        raise UnsafeFirmwareError(
            "The selected image targets a different Eternal device or chip."
        )
    if identity.partition_layout != EXPECTED_PARTITION_LAYOUT:
        raise UnsafeFirmwareError(
            "The selected image uses an incompatible Mini V3 partition layout."
        )
    return identity


def validate_partition_table(prefix: bytes) -> None:
    required = {
        (0x00, 0x10, 0x10000, 0x3C0000),
        (0x00, 0x11, 0x400000, 0x3C0000),
    }
    found: set[tuple[int, int, int, int]] = set()
    offset = 0x8000
    while offset + 32 <= len(prefix) and prefix[offset : offset + 2] == PARTITION_MAGIC:
        _, partition_type, subtype, address, size = struct.unpack_from(
            "<HBBII", prefix, offset
        )
        found.add((partition_type, subtype, address, size))
        offset += 32
    missing = required - found
    if missing:
        raise ValueError(
            "The full-device image does not contain the required Mini V3 OTA partitions."
        )


def inspect_firmware(
    path: Path, included: bool = False, allow_unsafe: bool = False
) -> FirmwareImage:
    """Validate a C5 firmware image and safely determine its flash offset."""
    path = path.expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(f"Firmware file was not found: {path}")
    if path.suffix.lower() != ".bin":
        raise ValueError("Select a firmware file with a .bin extension.")

    size = path.stat().st_size
    if size < 32:
        raise ValueError("The selected firmware file is empty or too small to be valid.")
    if size > FLASH_SIZE:
        raise ValueError("The selected firmware is larger than the device's 8 MB flash.")

    digest = sha256_file(path)
    if included:
        validate_firmware(path)

    with path.open("rb") as firmware:
        prefix = firmware.read(min(size, 0x9000))

    is_full_image = (
        len(prefix) >= 0x8002
        and prefix[0x2000] == ESP_IMAGE_MAGIC
        and prefix[0x8000:0x8002] == PARTITION_MAGIC
    )
    if is_full_image:
        if size != FLASH_SIZE:
            raise ValueError(
                "A Mini V3 full-device image must be exactly 8 MB."
            )
        chip_id = _image_chip_id(prefix[0x2000:0x2020])
        if chip_id != ESP32_C5_IMAGE_ID:
            raise ValueError("The selected full-device image is not built for an ESP32-C5.")
        validate_partition_table(prefix)
        try:
            validate_firmware_identity(path)
            identity_verified = True
        except UnsafeFirmwareError:
            if not allow_unsafe:
                raise
            identity_verified = False
        return FirmwareImage(
            path, "Full-device image", 0x0, size, digest, included,
            identity_verified
        )

    chip_id = _image_chip_id(prefix[:32])
    if chip_id is None:
        raise ValueError(
            "The selected file is not a recognized ESP32-C5 full-device or application image."
        )
    if chip_id != ESP32_C5_IMAGE_ID:
        raise ValueError("The selected application image is not built for an ESP32-C5.")
    component_name = path.name.lower()
    if any(name in component_name for name in ("bootloader", "partition", "boot_app0")):
        raise ValueError(
            "Select a complete full-device image or an application image, not an individual boot component."
        )
    if size > APPLICATION_LIMIT:
        raise ValueError(
            f"The application image is too large for the {APPLICATION_LIMIT}-byte application partition."
        )
    try:
        validate_firmware_identity(path)
        identity_verified = True
    except UnsafeFirmwareError:
        if not allow_unsafe:
            raise
        identity_verified = False
    return FirmwareImage(
        path, "Application image", APPLICATION_OFFSET, size, digest, included,
        identity_verified
    )


def strip_ansi(value: str) -> str:
    return ANSI_ESCAPE_RE.sub("", value).replace("\x08", "")


def extract_percent(value: str) -> float | None:
    matches = PERCENT_RE.findall(strip_ansi(value))
    if not matches:
        return None
    return min(100.0, max(0.0, float(matches[-1])))


def friendly_failure(details: str) -> str:
    text = strip_ansi(details).lower()
    if "permission denied" in text or "access is denied" in text:
        return (
            "The serial port could not be opened because access was denied. Close serial "
            "monitors and check your serial-port permissions."
        )
    if any(
        phrase in text
        for phrase in (
            "could not open port",
            "no such file or directory",
            "the system cannot find the file",
            "device disconnected",
        )
    ):
        return "The selected serial device is unavailable or was disconnected. Reconnect it and refresh the port list."
    if any(
        phrase in text
        for phrase in (
            "failed to connect",
            "no serial data received",
            "wrong boot mode",
            "timed out waiting for packet header",
            "invalid head of packet",
        )
    ):
        return (
            "The ESP32-C5 did not enter download mode. Reconnect it and try again; if the "
            "board exposes BOOT and RESET, hold BOOT while pressing RESET, then retry."
        )
    if any(
        phrase in text
        for phrase in (
            "wrong chip",
            "unexpected chip",
            "not an esp32-c5",
            "not esp32-c5",
            "not esp32c5",
            "esp32c5 expected",
        )
    ):
        return "The connected device is not an ESP32-C5 Marauder Mini V3. Nothing was written."
    if any(
        phrase in text
        for phrase in ("hash of data does not match", "verify failed", "checksum failed")
    ):
        return (
            "Flash verification failed. Try a shorter USB data cable and a direct USB port, "
            "then flash again."
        )
    if "no module named" in text and "esptool" in text:
        return "The flasher package is incomplete because its esptool component is missing."
    if "firmware integrity check failed" in text or "firmware size is invalid" in text:
        return "The selected firmware image is damaged or incomplete. Choose a valid image and retry."
    if "not built for an esp32-c5" in text or "not a recognized esp32-c5" in text:
        return "The selected file is not compatible with the ESP32-C5. Choose a valid C5 firmware image."
    if "serial" in text and ("timeout" in text or "read failed" in text):
        return "Communication with the device was interrupted. Check USB power and the data cable, then retry."
    return "Flashing did not complete. Review the technical details below for the reported error."


def port_preference(entry: PortEntry) -> tuple[int, str]:
    searchable = f"{entry.description} {entry.hwid}".lower()
    score = 0
    if entry.vid == 0x10C4 or "cp210" in searchable or "silicon labs" in searchable:
        score += 100
    if "esp32" in searchable or "espressif" in searchable:
        score += 80
    if entry.vid is not None:
        score += 10
    return (-score, entry.device.lower())


def discover_ports() -> list[PortEntry]:
    if list_ports is None:
        return []
    entries = [
        PortEntry(
            device=port.device,
            description=port.description or "Serial device",
            hwid=port.hwid or "",
            vid=port.vid,
            pid=port.pid,
        )
        for port in list_ports.comports()
    ]
    return sorted(entries, key=port_preference)


class OutputCapture:
    """File-like stream that emits complete CR/LF-delimited esptool updates."""

    def __init__(self, callback: Callable[[str], None]) -> None:
        self.callback = callback
        self.buffer = ""
        self.encoding = "utf-8"

    def write(self, value: str) -> int:
        if not value:
            return 0
        self.buffer += str(value)
        while True:
            cr = self.buffer.find("\r")
            lf = self.buffer.find("\n")
            positions = [position for position in (cr, lf) if position >= 0]
            if not positions:
                break
            split_at = min(positions)
            line = self.buffer[:split_at]
            self.buffer = self.buffer[split_at + 1 :]
            if line.strip():
                self.callback(line)
        return len(value)

    def flush(self) -> None:
        if self.buffer.strip():
            self.callback(self.buffer)
        self.buffer = ""

    def isatty(self) -> bool:
        # esptool emits live percentage updates when output behaves like a terminal.
        return True


def invoke_esptool(arguments: list[str], output: Callable[[str], None]) -> None:
    try:
        import esptool
    except ImportError as error:
        raise RuntimeError("No module named esptool; install the flasher dependencies.") from error

    capture = OutputCapture(output)
    try:
        with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
            esptool.main(arguments)
    except SystemExit as error:
        code = error.code if isinstance(error.code, int) else 1
        if code != 0:
            raise RuntimeError(f"esptool exited with status {code}.") from error
    finally:
        capture.flush()


def validate_sd_path(path: str) -> str:
    if not path.startswith("/") or path == "/" or path.endswith("/"):
        raise SdProtocolError("The device returned an invalid SD file path.", "INVALID_PATH")
    if "\\" in path or any(ord(character) < 0x20 for character in path):
        raise SdProtocolError("The device returned an unsafe SD file path.", "INVALID_PATH")
    components = path[1:].split("/")
    if any(component in ("", ".", "..") for component in components):
        raise SdProtocolError("The device returned an unsafe SD file path.", "INVALID_PATH")
    try:
        encoded = path.encode("utf-8")
    except UnicodeEncodeError as error:
        raise SdProtocolError("The SD file path is not valid UTF-8.", "INVALID_PATH") from error
    if len(encoded) >= SD_TRANSFER_MAX_PATH_BYTES:
        raise SdProtocolError("The SD file path is too long to transfer safely.", "INVALID_PATH")
    return path


def encode_sd_path(path: str) -> str:
    return validate_sd_path(path).encode("utf-8").hex()


def decode_sd_path(encoded: object) -> str:
    if not isinstance(encoded, str) or not encoded or len(encoded) % 2:
        raise SdProtocolError("The device returned an invalid encoded SD path.", "INVALID_PATH")
    try:
        raw = bytes.fromhex(encoded)
        path = raw.decode("utf-8")
    except (ValueError, UnicodeDecodeError) as error:
        raise SdProtocolError("The device returned an invalid encoded SD path.", "INVALID_PATH") from error
    return validate_sd_path(path)


def local_path_for_sd(destination: Path, sd_path: str) -> Path:
    path = validate_sd_path(sd_path)
    root = destination.expanduser().resolve()
    candidate = root.joinpath(*path[1:].split("/")).resolve()
    try:
        common = Path(os.path.commonpath((str(root), str(candidate))))
    except ValueError as error:
        raise SdProtocolError("The local destination is not safe.", "INVALID_DESTINATION") from error
    if common != root:
        raise SdProtocolError("The local destination escapes the chosen folder.", "INVALID_DESTINATION")
    return candidate


def evil_portal_upload_path(source: Path) -> str:
    name = source.name
    if source.suffix.casefold() != ".html":
        raise SdProtocolError(
            "Select an HTML file for the Evil Portal template.",
            "INVALID_UPLOAD_PATH",
        )
    stem = name[: -len(source.suffix)]
    if not stem:
        raise SdProtocolError(
            "The Evil Portal HTML file needs a filename before .html.",
            "INVALID_UPLOAD_PATH",
        )
    filename = stem + ".html"
    if len(filename.encode("utf-8")) > EVIL_PORTAL_MAX_FILENAME_BYTES:
        raise SdProtocolError(
            "The Evil Portal HTML filename is too long.",
            "INVALID_UPLOAD_PATH",
        )
    return validate_sd_path(f"{EVIL_PORTAL_HTML_DIR}/{filename}")


def format_file_size(size: int) -> str:
    value = float(max(0, size))
    units = ("B", "KB", "MB", "GB")
    unit = units[0]
    for unit in units:
        if value < 1024.0 or unit == units[-1]:
            break
        value /= 1024.0
    return f"{int(value)} {unit}" if unit == "B" else f"{value:.1f} {unit}"


def format_modified(timestamp: int) -> str:
    if timestamp <= 0:
        return "—"
    try:
        return datetime.fromtimestamp(timestamp).strftime("%Y-%m-%d %H:%M")
    except (OSError, OverflowError, ValueError):
        return "—"


def parse_sd_protocol_message(line: bytes | str) -> dict[str, object] | None:
    raw = line.encode("utf-8", "replace") if isinstance(line, str) else line
    marker = raw.find(SD_PROTOCOL_PREFIX)
    if marker < 0:
        return None
    payload = raw[marker + len(SD_PROTOCOL_PREFIX) :].strip()
    try:
        message = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SdProtocolError("The device returned a malformed protocol message.") from error
    if not isinstance(message, dict) or message.get("protocol") != SD_PROTOCOL_VERSION:
        raise SdProtocolError("The device uses an unsupported serial protocol version.")
    return message


def _message_integer(message: dict[str, object], name: str) -> int:
    value = message.get(name)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise SdProtocolError(f"The device returned an invalid {name} value.")
    return value


def friendly_sd_failure(error: BaseException) -> str:
    code = getattr(error, "code", "")
    if code == "DEVICE_BUSY":
        return "Stop the active scan, capture, attack, or portal task on the device, then refresh SD Files."
    if code in ("SD_NOT_READY", "SD_NOT_SUPPORTED"):
        return "The SD card is not mounted. Reinsert it, wait for the main menu, and try again."
    if code == "PATH_NOT_FOUND":
        return "The selected SD file no longer exists. Refresh the file list and try again."
    if code in ("INVALID_PATH", "NOT_A_FILE"):
        return "The selected SD path is not a downloadable file. Refresh the file list."
    if code == "INVALID_UPLOAD_PATH":
        return "Only named .html files can be uploaded directly to /evil_portal/html."
    if code == "INVALID_SIZE":
        return "The Evil Portal template must be non-empty and smaller than 30,000 bytes."
    if code == "FILE_EXISTS":
        return "That Evil Portal template already exists. Confirm replacement and try again."
    if code == "CAPABILITY_MISSING":
        return "This device firmware does not support SD Files yet. Flash the included current firmware first."
    if code == "UPLOAD_CAPABILITY_MISSING":
        return "This device firmware does not support SD uploads yet. Flash the included current firmware first."
    if code == "HASH_MISMATCH":
        return "The SD transfer failed SHA-256 verification and was not committed. Try again."
    if code == "TRANSFER_TIMEOUT":
        return "The SD transfer timed out. Check the USB cable and make sure the device remains powered."
    if code in (
        "DIRECTORY_FAILED", "TEMP_CLEANUP_FAILED", "FILE_OPEN_FAILED",
        "FILE_WRITE_FAILED", "BACKUP_FAILED", "COMMIT_FAILED",
    ):
        return "The device could not safely write the Evil Portal template. Check SD free space and card health."
    text = str(error).lower()
    if "permission" in text or "access is denied" in text:
        return "The serial port could not be opened. Close serial monitors and check port permissions."
    if "could not open" in text or "no such file" in text or "disconnected" in text:
        return "The selected device disconnected. Reconnect it, refresh the port list, and try again."
    return str(error) or "The SD file operation did not complete."


class SdSerialClient:
    """Machine-protocol client for listing and downloading Mini V3 SD files."""

    def __init__(self, port: str, connection: object | None = None) -> None:
        self.port = port
        self.connection = connection
        self._owns_connection = connection is None

    def __enter__(self) -> "SdSerialClient":
        if self.connection is not None:
            return self
        if serial is None:
            raise RuntimeError("pyserial is missing from the flasher package.")

        connection = serial.Serial(port=None, baudrate=SD_SERIAL_BAUD,
                                   timeout=0.25, write_timeout=5)
        connection.dtr = False
        connection.rts = False
        if os.name == "posix":
            connection.exclusive = True
        connection.port = self.port
        connection.open()
        # Opening the Mini V3's CP2102N port can pulse the ESP32-C5 reset
        # lines.  Do not send a machine command until the firmware has
        # completed setup and printed its command prompt.  In particular,
        # do not mistake the "heap ... -> ..." boot message for that prompt.
        original_timeout = connection.timeout
        try:
            connection.timeout = SD_SERIAL_READY_TIMEOUT
            connection.read_until(SD_SERIAL_PROMPT, 65536)
        finally:
            connection.timeout = original_timeout
        connection.reset_input_buffer()
        self.connection = connection
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> None:
        self.close()

    def close(self) -> None:
        if self.connection is not None and self._owns_connection:
            self.connection.close()  # type: ignore[attr-defined]
            self.connection = None

    def _send(self, command: str) -> None:
        if self.connection is None:
            raise SdProtocolError("The serial connection is not open.")
        payload = (command + "\n").encode("ascii")
        self.connection.write(payload)  # type: ignore[attr-defined]
        self.connection.flush()  # type: ignore[attr-defined]

    def _read_message(self, transaction: str, command: str,
                      timeout: float = 8.0) -> dict[str, object]:
        if self.connection is None:
            raise SdProtocolError("The serial connection is not open.")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.connection.read_until(b"\n", 4096)  # type: ignore[attr-defined]
            if not line:
                continue
            message = parse_sd_protocol_message(line)
            if message is None:
                continue
            if message.get("tx") != transaction or message.get("command") != command:
                continue
            if message.get("status") == "error":
                code = str(message.get("code", "DEVICE_ERROR"))
                raise SdProtocolError(f"Device reported {code}.", code)
            return message
        raise SdProtocolError("Timed out waiting for the device response.", "TRANSFER_TIMEOUT")

    def protocol_info(
        self,
        required_capabilities: frozenset[str] = SD_REQUIRED_CAPABILITIES,
    ) -> dict[str, object]:
        transaction = secrets.token_hex(8)
        self._send(f"protocolinfo --machine {transaction}")
        message = self._read_message(transaction, "protocolinfo")
        if message.get("status") != "success":
            raise SdProtocolError("The device did not complete protocol negotiation.")
        capabilities = message.get("capabilities")
        if not isinstance(capabilities, list) or not all(
                isinstance(capability, str) for capability in capabilities):
            raise SdProtocolError("The device returned invalid capability information.")
        missing = required_capabilities - set(capabilities)
        if missing:
            code = (
                "UPLOAD_CAPABILITY_MISSING"
                if missing == {SD_UPLOAD_CAPABILITY}
                else "CAPABILITY_MISSING"
            )
            raise SdProtocolError(
                "The connected firmware does not provide SD file transfer.",
                code,
            )
        return message

    def set_session(self, active: bool) -> None:
        transaction = secrets.token_hex(8)
        state = "begin" if active else "end"
        self._send(
            f"sdsession --machine {transaction} --state {state}"
        )
        message = self._read_message(transaction, "sdsession", timeout=5.0)
        if message.get("status") != "success":
            raise SdProtocolError("The device did not change SD transfer mode.")

    def list_files(self) -> list[SdFileEntry]:
        transaction = secrets.token_hex(8)
        self._send(f"sdlist --machine {transaction}")
        entries: list[SdFileEntry] = []
        seen: set[str] = set()
        started = False
        while True:
            message = self._read_message(transaction, "sdlist")
            status = message.get("status")
            if status == "started":
                if started:
                    raise SdProtocolError("The device restarted the SD file listing unexpectedly.")
                started = True
            elif status == "file":
                if not started:
                    raise SdProtocolError("The SD listing started without a header.")
                path = decode_sd_path(message.get("pathHex"))
                if path in seen:
                    raise SdProtocolError("The device returned a duplicate SD file path.")
                seen.add(path)
                entries.append(SdFileEntry(
                    path=path,
                    size=_message_integer(message, "bytes"),
                    modified=_message_integer(message, "modified"),
                ))
                if len(entries) > SD_MAX_LIST_ENTRIES:
                    raise SdProtocolError("The SD card contains too many files to display safely.")
            elif status == "success":
                if not started:
                    raise SdProtocolError("The SD listing completed without starting.")
                if _message_integer(message, "files") != len(entries):
                    raise SdProtocolError("The SD listing file count did not match.")
                expected_bytes = sum(entry.size for entry in entries)
                if _message_integer(message, "bytes") != expected_bytes:
                    raise SdProtocolError("The SD listing byte count did not match.")
                return sorted(entries, key=lambda entry: entry.path.casefold())
            else:
                raise SdProtocolError("The device returned an unknown SD listing state.")

    def download_file(
        self,
        entry: SdFileEntry,
        destination: Path,
        progress: Callable[[int, int], None] | None = None,
    ) -> Path:
        transaction = secrets.token_hex(8)
        encoded_path = encode_sd_path(entry.path)
        self._send(
            f"sdget --machine {transaction} --path-hex {encoded_path}"
        )
        header = self._read_message(transaction, "sdget")
        if header.get("status") != "started":
            raise SdProtocolError("The SD download did not return a start header.")
        if decode_sd_path(header.get("pathHex")) != entry.path:
            raise SdProtocolError("The device returned a different SD file path.")
        expected_size = _message_integer(header, "bytes")

        destination = destination.expanduser()
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary_fd, temporary_name = tempfile.mkstemp(
            prefix=f".{destination.name}.", suffix=".part",
            dir=str(destination.parent),
        )
        os.close(temporary_fd)
        temporary = Path(temporary_name)
        digest = hashlib.sha256()
        transferred = 0
        try:
            if self.connection is None:
                raise SdProtocolError("The serial connection is not open.")
            idle_deadline = time.monotonic() + 8.0
            with temporary.open("wb") as output:
                while transferred < expected_size:
                    requested = min(16 * 1024, expected_size - transferred)
                    block = self.connection.read(requested)  # type: ignore[attr-defined]
                    if not block:
                        if time.monotonic() >= idle_deadline:
                            raise SdProtocolError(
                                "Timed out while receiving SD file data.",
                                "TRANSFER_TIMEOUT",
                            )
                        continue
                    idle_deadline = time.monotonic() + 8.0
                    output.write(block)
                    digest.update(block)
                    transferred += len(block)
                    if progress is not None:
                        progress(transferred, expected_size)

            trailer = self._read_message(transaction, "sdget", timeout=12.0)
            if trailer.get("status") != "success":
                raise SdProtocolError("The SD download did not complete successfully.")
            if _message_integer(trailer, "bytes") != expected_size:
                raise SdProtocolError("The SD download byte count did not match.")
            expected_digest = trailer.get("sha256")
            if not isinstance(expected_digest, str) or not re.fullmatch(
                    r"[0-9a-f]{64}", expected_digest):
                raise SdProtocolError("The device returned an invalid file digest.")
            if digest.hexdigest() != expected_digest:
                raise SdProtocolError(
                    "The SD download digest did not match.", "HASH_MISMATCH"
                )
            os.replace(temporary, destination)
            if entry.modified > 0:
                with contextlib.suppress(OSError, OverflowError, ValueError):
                    os.utime(destination, (entry.modified, entry.modified))
            return destination
        except Exception:
            with contextlib.suppress(OSError):
                temporary.unlink()
            raise

    def upload_evil_portal_html(
        self,
        source: Path,
        overwrite: bool = False,
        progress: Callable[[int, int], None] | None = None,
    ) -> SdFileEntry:
        source = source.expanduser()
        if not source.is_file():
            raise SdProtocolError("The selected HTML file does not exist.")
        destination = evil_portal_upload_path(source)
        size = source.stat().st_size
        if size <= 0 or size >= EVIL_PORTAL_MAX_HTML_BYTES:
            raise SdProtocolError(
                "The Evil Portal HTML file has an unsupported size.",
                "INVALID_SIZE",
            )

        digest = hashlib.sha256()
        with source.open("rb") as input_file:
            for block in iter(lambda: input_file.read(16 * 1024), b""):
                digest.update(block)
        digest_hex = digest.hexdigest()

        transaction = secrets.token_hex(8)
        command = (
            f"sdput --machine {transaction} --path-hex {encode_sd_path(destination)} "
            f"--bytes {size} --sha256 {digest_hex}"
        )
        if overwrite:
            command += " --overwrite"
        self._send(command)

        header = self._read_message(transaction, "sdput", timeout=12.0)
        if header.get("status") != "ready":
            raise SdProtocolError("The SD upload did not return a ready header.")
        if decode_sd_path(header.get("pathHex")) != destination:
            raise SdProtocolError("The device returned a different SD upload path.")
        if _message_integer(header, "bytes") != size:
            raise SdProtocolError("The device returned a different SD upload size.")
        if self.connection is None:
            raise SdProtocolError("The serial connection is not open.")

        transferred = 0
        with source.open("rb") as input_file:
            while transferred < size:
                block = input_file.read(min(4096, size - transferred))
                if not block:
                    raise SdProtocolError(
                        "The local HTML file changed during upload.",
                        "HASH_MISMATCH",
                    )
                written = self.connection.write(block)  # type: ignore[attr-defined]
                if written != len(block):
                    raise SdProtocolError(
                        "The serial connection accepted only part of the upload.",
                        "TRANSFER_TIMEOUT",
                    )
                transferred += written
                if progress is not None:
                    progress(transferred, size)
        self.connection.flush()  # type: ignore[attr-defined]

        trailer = self._read_message(transaction, "sdput", timeout=15.0)
        if trailer.get("status") != "success":
            raise SdProtocolError("The SD upload did not complete successfully.")
        if _message_integer(trailer, "bytes") != size:
            raise SdProtocolError("The SD upload byte count did not match.")
        if decode_sd_path(trailer.get("pathHex")) != destination:
            raise SdProtocolError("The device committed a different SD upload path.")
        returned_digest = trailer.get("sha256")
        if returned_digest != digest_hex:
            raise SdProtocolError(
                "The uploaded file digest did not match.", "HASH_MISMATCH"
            )
        return SdFileEntry(destination, size, 0)


class FlasherApp:
    BACKGROUND = "#0b0f14"
    PANEL = "#141b22"
    PANEL_ALT = "#1b242d"
    TEXT = "#f4f7fa"
    MUTED = "#94a3b8"
    ACCENT = "#7565e8"
    ACCENT_ACTIVE = "#897bf2"
    SUCCESS = "#35c78b"
    ERROR = "#ef6461"
    BRIGHT_ERROR = "#ff2b2b"

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.ports: list[PortEntry] = []
        self.port_by_label: dict[str, PortEntry] = {}
        self.flashing = False
        self.sd_busy = False
        self.sd_window: tk.Toplevel | None = None
        self.sd_client: SdSerialClient | None = None
        self.quit_after_sd_close = False
        self.sd_entries: list[SdFileEntry] = []
        self.sd_entry_by_item: dict[str, SdFileEntry] = {}
        self.raw_output: list[str] = []
        self.highest_progress = 0.0
        self.firmware = inspect_firmware(resource_path(), included=True)

        self.port_var = tk.StringVar()
        self.firmware_path_var = tk.StringVar(value=f"Included Marauder Eternal {FIRMWARE_VERSION}")
        self.firmware_detail_var = tk.StringVar(value=self.firmware.detail)
        self.firmware_warning_var = tk.StringVar()
        self.status_var = tk.StringVar(value="Connect a Marauder Mini V3 to begin")
        self.percent_var = tk.StringVar(value="0%")
        self.progress_var = tk.DoubleVar(value=0.0)
        self.sd_status_var = tk.StringVar(value="Connect to read the SD card")
        self.sd_activity_var = tk.StringVar(value="")
        self.sd_progress_var = tk.DoubleVar(value=0.0)
        self.sd_progress_text_var = tk.StringVar(value="")
        self._update_firmware_warning()

        self._configure_window()
        self._build_styles()
        self._build_ui()
        self._ensure_activity_lines()
        self.refresh_ports()
        self.root.after(100, self._drain_events)
        self.root.after(1800, self._automatic_refresh)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _configure_window(self) -> None:
        self.root.title(APP_NAME)
        self.root.geometry(f"{DEFAULT_WINDOW_WIDTH}x{DEFAULT_WINDOW_HEIGHT}")
        self.root.minsize(680, 680)
        self.root.configure(background=self.BACKGROUND)

    def _build_styles(self) -> None:
        style = ttk.Style(self.root)
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("App.TFrame", background=self.BACKGROUND)
        style.configure("Panel.TFrame", background=self.PANEL)
        style.configure(
            "Port.TCombobox",
            fieldbackground=self.PANEL_ALT,
            background=self.PANEL_ALT,
            foreground=self.TEXT,
            arrowcolor=self.TEXT,
            bordercolor="#334155",
            lightcolor="#334155",
            darkcolor="#334155",
            padding=8,
        )
        style.map(
            "Port.TCombobox",
            fieldbackground=[("readonly", self.PANEL_ALT)],
            foreground=[("readonly", self.TEXT)],
            selectbackground=[("readonly", self.PANEL_ALT)],
            selectforeground=[("readonly", self.TEXT)],
        )
        style.configure(
            "Flash.Horizontal.TProgressbar",
            troughcolor="#26313d",
            background=self.ACCENT,
            bordercolor="#26313d",
            lightcolor=self.ACCENT,
            darkcolor=self.ACCENT,
            thickness=18,
        )
        style.configure(
            "Success.Horizontal.TProgressbar",
            troughcolor="#26313d",
            background=self.SUCCESS,
            bordercolor="#26313d",
            lightcolor=self.SUCCESS,
            darkcolor=self.SUCCESS,
            thickness=18,
        )
        style.configure(
            "Error.Horizontal.TProgressbar",
            troughcolor="#26313d",
            background=self.ERROR,
            bordercolor="#26313d",
            lightcolor=self.ERROR,
            darkcolor=self.ERROR,
            thickness=18,
        )
        self.sd_tree_font = tkfont.nametofont("TkDefaultFont").copy()
        self.sd_tree_font.configure(size=11)
        self.sd_tree_heading_font = tkfont.nametofont("TkDefaultFont").copy()
        self.sd_tree_heading_font.configure(size=10, weight="bold")
        style.configure(
            "Sd.Treeview",
            background=self.PANEL,
            fieldbackground=self.PANEL,
            foreground=self.TEXT,
            font=self.sd_tree_font,
            rowheight=max(36, self.sd_tree_font.metrics("linespace") + 14),
            bordercolor="#334155",
        )
        style.map(
            "Sd.Treeview",
            background=[("selected", self.ACCENT)],
            foreground=[("selected", "white")],
        )
        style.configure(
            "Sd.Treeview.Heading",
            background=self.PANEL_ALT,
            foreground=self.TEXT,
            font=self.sd_tree_heading_font,
            padding=(8, 8),
            relief="flat",
        )
        style.map("Sd.Treeview.Heading", background=[("active", "#2a3744")])

    def _label(
        self,
        parent: tk.Misc,
        text: str | None = None,
        size: int = 11,
        color: str | None = None,
        weight: str = "normal",
        textvariable: tk.StringVar | None = None,
        **kwargs: object,
    ) -> tk.Label:
        try:
            parent_style = str(parent.cget("style"))
        except tk.TclError:
            parent_style = ""
        default_background = self.PANEL if "Panel" in parent_style else self.BACKGROUND
        return tk.Label(
            parent,
            text=text,
            textvariable=textvariable,
            background=kwargs.pop("background", default_background),
            foreground=color or self.TEXT,
            font=("TkDefaultFont", size, weight),
            **kwargs,
        )

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, style="App.TFrame", padding=(28, 24))
        outer.pack(fill="both", expand=True)

        self._label(outer, "ESP32 MARAUDER ETERNAL", 11, self.ACCENT, "bold").pack(anchor="w")
        self._label(outer, "Flasher", 25, self.TEXT, "bold").pack(anchor="w", pady=(2, 2))
        self._label(
            outer,
            f"Mini V3 • ESP32-C5 • Firmware {FIRMWARE_VERSION}",
            10,
            self.MUTED,
        ).pack(anchor="w", pady=(0, 18))

        panel = ttk.Frame(outer, style="Panel.TFrame", padding=18)
        panel.pack(fill="x")
        self._label(panel, "Device", 10, self.MUTED, "bold").pack(anchor="w")

        port_row = ttk.Frame(panel, style="Panel.TFrame")
        port_row.pack(fill="x", pady=(7, 14))
        self.port_combo = ttk.Combobox(
            port_row,
            textvariable=self.port_var,
            state="readonly",
            style="Port.TCombobox",
            font=("TkDefaultFont", 10),
        )
        self.port_combo.pack(side="left", fill="x", expand=True)
        self.refresh_button = tk.Button(
            port_row,
            text="Refresh",
            command=self.refresh_ports,
            background=self.PANEL_ALT,
            foreground=self.TEXT,
            activebackground="#2a3744",
            activeforeground=self.TEXT,
            relief="flat",
            padx=14,
            pady=8,
            cursor="hand2",
        )
        self.refresh_button.pack(side="left", padx=(9, 0))

        self._label(panel, "Firmware", 10, self.MUTED, "bold").pack(anchor="w")
        firmware_row = ttk.Frame(panel, style="Panel.TFrame")
        firmware_row.pack(fill="x", pady=(7, 5))
        self.firmware_entry = tk.Entry(
            firmware_row,
            textvariable=self.firmware_path_var,
            state="readonly",
            readonlybackground=self.PANEL_ALT,
            foreground=self.TEXT,
            relief="flat",
            font=("TkDefaultFont", 10),
        )
        self.firmware_entry.pack(side="left", fill="x", expand=True, ipady=8)
        self.browse_button = tk.Button(
            firmware_row,
            text="Choose BIN",
            command=self.choose_firmware,
            background=self.PANEL_ALT,
            foreground=self.TEXT,
            activebackground="#2a3744",
            activeforeground=self.TEXT,
            relief="flat",
            padx=12,
            pady=8,
            cursor="hand2",
        )
        self.browse_button.pack(side="left", padx=(9, 0))
        self.included_button = tk.Button(
            firmware_row,
            text="Use Included",
            command=self.use_included_firmware,
            background=self.PANEL_ALT,
            foreground=self.TEXT,
            activebackground="#2a3744",
            activeforeground=self.TEXT,
            relief="flat",
            padx=12,
            pady=8,
            cursor="hand2",
        )
        self.included_button.pack(side="left", padx=(7, 0))
        self._label(
            panel,
            textvariable=self.firmware_detail_var,
            size=9,
            color=self.MUTED,
        ).pack(anchor="w", pady=(0, 13))

        self.flash_button = tk.Button(
            panel,
            text="Connect & Flash",
            command=self.start_flash,
            background=self.ACCENT,
            foreground="white",
            activebackground=self.ACCENT_ACTIVE,
            activeforeground="white",
            disabledforeground="#7c8491",
            relief="flat",
            font=("TkDefaultFont", 12, "bold"),
            padx=18,
            pady=12,
            cursor="hand2",
        )
        self.flash_button.pack(fill="x")
        self.sd_files_button = tk.Button(
            panel,
            text="SD Files — Browse & Download",
            command=self.open_sd_files,
            background=self.PANEL_ALT,
            foreground=self.TEXT,
            activebackground="#2a3744",
            activeforeground=self.TEXT,
            disabledforeground="#7c8491",
            relief="flat",
            font=("TkDefaultFont", 10, "bold"),
            padx=16,
            pady=10,
            cursor="hand2",
        )
        self.sd_files_button.pack(fill="x", pady=(8, 0))
        self._label(
            panel,
            textvariable=self.firmware_warning_var,
            size=9,
            color=self.MUTED,
        ).pack(anchor="w", pady=(9, 0))

        progress_header = ttk.Frame(outer, style="App.TFrame")
        progress_header.pack(fill="x", pady=(22, 7))
        self.status_label = self._label(
            progress_header,
            textvariable=self.status_var,
            size=10,
            color=self.MUTED,
            weight="bold",
        )
        self.status_label.pack(side="left", anchor="w")
        self._label(
            progress_header,
            textvariable=self.percent_var,
            size=10,
            color=self.TEXT,
            weight="bold",
        ).pack(side="right")

        self.progress = ttk.Progressbar(
            outer,
            variable=self.progress_var,
            maximum=100,
            style="Flash.Horizontal.TProgressbar",
        )
        self.progress.pack(fill="x")

        self._label(outer, "Activity", 10, self.MUTED, "bold").pack(anchor="w", pady=(20, 7))
        log_frame = tk.Frame(outer, background=self.PANEL, highlightthickness=1, highlightbackground="#283541")
        log_frame.pack(fill="both", expand=True)
        self.log = tk.Text(
            log_frame,
            width=1,
            height=10,
            wrap="word",
            state="disabled",
            background=self.PANEL,
            foreground="#c8d2dc",
            insertbackground=self.TEXT,
            selectbackground=self.ACCENT,
            relief="flat",
            padx=12,
            pady=10,
            font=("TkFixedFont", 9),
        )
        scrollbar = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=scrollbar.set)
        self.log.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        self._label(
            outer,
            f"Flasher {APP_VERSION}  •  Firmware flashing and verified SD downloads",
            8,
            "#657384",
        ).pack(anchor="e", pady=(10, 0))

    def _ensure_activity_lines(self) -> None:
        """Size the initial window so at least seven activity rows are visible."""
        self.root.update_idletasks()
        line_height = tkfont.Font(font=self.log.cget("font")).metrics("linespace")
        log_padding = int(self.log.cget("pady")) * 2
        required_log_height = line_height * MIN_ACTIVITY_LINES + log_padding + 4
        non_log_height = self.root.winfo_reqheight() - self.log.winfo_reqheight()
        desired_height = max(DEFAULT_WINDOW_HEIGHT, non_log_height + required_log_height)
        desired_height = min(desired_height, self.root.winfo_screenheight() - 80)
        desired_width = max(DEFAULT_WINDOW_WIDTH, self.root.winfo_reqwidth())
        desired_width = min(desired_width, self.root.winfo_screenwidth() - 80)
        self.root.geometry(f"{desired_width}x{desired_height}")

    def visible_activity_lines(self) -> int:
        line_height = tkfont.Font(font=self.log.cget("font")).metrics("linespace")
        log_padding = int(self.log.cget("pady")) * 2
        return max(0, (self.log.winfo_height() - log_padding) // line_height)

    def _update_firmware_warning(self) -> None:
        if not self.firmware.identity_verified:
            message = (
                "WARNING: Device identity was not verified. Flash only if you independently "
                "confirmed this image and its partition layout."
            )
        elif self.firmware.offset == 0:
            message = "Full-device images replace boot data, partitions, the application, and saved settings."
        else:
            message = "Application-only flashing writes both OTA slots and preserves saved settings."
        self.firmware_warning_var.set(message)

    def _select_firmware(self, firmware: FirmwareImage, display_path: str) -> None:
        self.firmware = firmware
        self.firmware_path_var.set(display_path)
        self.firmware_detail_var.set(firmware.detail)
        self._update_firmware_warning()
        self.raw_output.clear()
        if self.ports:
            self.status_var.set("Ready to connect and flash")

    def choose_firmware(self) -> None:
        if self.flashing or self.sd_busy:
            return
        selected = filedialog.askopenfilename(
            parent=self.root,
            title="Choose firmware BIN",
            initialdir=str(self.firmware.path.parent),
            filetypes=(("Firmware BIN files", "*.bin"), ("All files", "*.*")),
        )
        if not selected:
            return
        try:
            firmware = inspect_firmware(Path(selected))
        except UnsafeFirmwareError as error:
            proceed = messagebox.askyesno(
                APP_NAME,
                f"{error}\n\nThis image cannot be verified as Eternal Mini V3 firmware. "
                "Continue only if you independently trust it and know its partition layout.",
                icon="warning",
            )
            if not proceed:
                return
            try:
                firmware = inspect_firmware(Path(selected), allow_unsafe=True)
            except Exception as unsafe_error:
                messagebox.showerror(APP_NAME, str(unsafe_error))
                return
        except Exception as error:
            messagebox.showerror(APP_NAME, str(error))
            return
        self._select_firmware(firmware, str(firmware.path))

    def use_included_firmware(self) -> None:
        if self.flashing or self.sd_busy:
            return
        try:
            firmware = inspect_firmware(resource_path(), included=True)
        except Exception as error:
            messagebox.showerror(APP_NAME, str(error))
            return
        self._select_firmware(firmware, f"Included Marauder Eternal {FIRMWARE_VERSION}")

    def refresh_ports(self) -> None:
        if (self.flashing or self.sd_busy or
                (self.sd_window is not None and self.sd_window.winfo_exists())):
            return
        selected_device = self.selected_port()
        self.ports = discover_ports()
        self.port_by_label = {entry.label: entry for entry in self.ports}
        values = list(self.port_by_label)
        self.port_combo.configure(values=values)

        selected = next((entry for entry in self.ports if entry.device == selected_device), None)
        if selected:
            self.port_var.set(selected.label)
        elif self.ports:
            self.port_var.set(self.ports[0].label)
        else:
            self.port_var.set("")

        self.flash_button.configure(state="normal" if self.ports else "disabled")
        self.sd_files_button.configure(state="normal" if self.ports else "disabled")
        if not self.ports:
            self.status_var.set("No serial device detected — connect the device and refresh")
        elif not self.raw_output:
            self.status_var.set("Ready to connect and flash")

    def selected_port(self) -> str | None:
        entry = self.port_by_label.get(self.port_var.get())
        return entry.device if entry else None

    def open_sd_files(self, refresh: bool = True) -> None:
        if self.flashing or self.sd_busy:
            return
        if not self.selected_port():
            messagebox.showerror(APP_NAME, "Select a serial device before browsing SD files.")
            return
        if self.sd_window is not None and self.sd_window.winfo_exists():
            self.sd_window.lift()
            self.sd_window.focus_force()
            return

        window = tk.Toplevel(self.root)
        self.sd_window = window
        window.title(f"{APP_NAME} — SD Files")
        screen_width = window.winfo_screenwidth()
        screen_height = window.winfo_screenheight()
        width = min(SD_WINDOW_WIDTH, max(760, screen_width - 40))
        height = min(SD_WINDOW_HEIGHT, max(560, screen_height - 60))
        window.geometry(f"{width}x{height}")
        window.minsize(
            min(SD_WINDOW_MIN_WIDTH, width),
            min(SD_WINDOW_MIN_HEIGHT, height),
        )
        window.configure(background=self.BACKGROUND)
        window.transient(self.root)
        window.protocol("WM_DELETE_WINDOW", self._close_sd_files)

        outer = ttk.Frame(window, style="App.TFrame", padding=(22, 20))
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(4, weight=1, minsize=120)
        self._label(outer, "SD FILES", 11, self.ACCENT, "bold").grid(
            row=0, column=0, sticky="w"
        )
        self._label(
            outer,
            "Copy SD files or install Evil Portal HTML templates over USB serial.",
            14,
            self.TEXT,
            "bold",
        ).grid(row=1, column=0, sticky="w", pady=(3, 2))
        self.sd_description_label = self._label(
            outer,
            "Opening this window puts the device in locked USB SD mode. Downloaded files are "
            "SHA-256 verified; some files may contain sensitive credentials.",
            9,
            self.MUTED,
            wraplength=max(600, width - 60),
            justify="left",
        )
        self.sd_description_label.grid(
            row=2, column=0, sticky="ew", pady=(0, 14)
        )
        window.bind(
            "<Configure>",
            lambda event: self.sd_description_label.configure(
                wraplength=max(500, event.width - 60)
            ) if event.widget is window else None,
        )

        self.sd_activity_label = self._label(
            outer,
            textvariable=self.sd_activity_var,
            size=13,
            color=self.BRIGHT_ERROR,
            weight="bold",
            anchor="w",
        )
        self.sd_activity_label.grid(
            row=3, column=0, sticky="ew", pady=(0, 12)
        )

        tree_frame = tk.Frame(
            outer, background=self.PANEL, highlightthickness=1,
            highlightbackground="#283541",
        )
        tree_frame.grid(row=4, column=0, sticky="nsew")
        tree_frame.rowconfigure(0, weight=1)
        tree_frame.columnconfigure(0, weight=1)
        self.sd_tree = ttk.Treeview(
            tree_frame,
            columns=("size", "modified"),
            show="tree headings",
            selectmode="extended",
            style="Sd.Treeview",
        )
        self.sd_tree.heading("#0", text="SD path", anchor="w")
        self.sd_tree.heading("size", text="Size", anchor="e")
        self.sd_tree.heading("modified", text="Modified", anchor="w")
        self.sd_tree.column("#0", width=690, minwidth=360, stretch=True)
        self.sd_tree.column("size", width=110, minwidth=90, stretch=False, anchor="e")
        self.sd_tree.column("modified", width=170, minwidth=155, stretch=False)
        tree_scroll_y = ttk.Scrollbar(
            tree_frame, orient="vertical", command=self.sd_tree.yview
        )
        tree_scroll_x = ttk.Scrollbar(
            tree_frame, orient="horizontal", command=self.sd_tree.xview
        )
        self.sd_tree.configure(
            yscrollcommand=tree_scroll_y.set,
            xscrollcommand=tree_scroll_x.set,
        )
        self.sd_tree.grid(row=0, column=0, sticky="nsew")
        tree_scroll_y.grid(row=0, column=1, sticky="ns")
        tree_scroll_x.grid(row=1, column=0, sticky="ew")
        self.sd_tree.bind("<Double-1>", lambda _event: self.download_selected_sd_files())

        progress_row = ttk.Frame(outer, style="App.TFrame")
        progress_row.grid(row=5, column=0, sticky="ew", pady=(13, 5))
        self._label(
            progress_row, textvariable=self.sd_status_var,
            size=9, color=self.MUTED, weight="bold",
        ).pack(side="left")
        self._label(
            progress_row, textvariable=self.sd_progress_text_var,
            size=9, color=self.TEXT, weight="bold",
        ).pack(side="right")
        self.sd_progress = ttk.Progressbar(
            outer,
            variable=self.sd_progress_var,
            maximum=100,
            style="Flash.Horizontal.TProgressbar",
        )
        self.sd_progress.grid(row=6, column=0, sticky="ew", pady=(0, 12))

        button_row = ttk.Frame(outer, style="App.TFrame")
        button_row.grid(row=7, column=0, sticky="ew")
        self.sd_refresh_button = tk.Button(
            button_row, text="Refresh", command=self.refresh_sd_files,
            background=self.PANEL_ALT, foreground=self.TEXT,
            activebackground="#2a3744", activeforeground=self.TEXT,
            relief="flat", padx=14, pady=9, cursor="hand2",
        )
        self.sd_refresh_button.pack(side="left")
        self.sd_upload_button = tk.Button(
            button_row, text="Upload Evil Portal HTML",
            command=self.upload_evil_portal_html,
            background=self.PANEL_ALT, foreground=self.TEXT,
            activebackground="#2a3744", activeforeground=self.TEXT,
            disabledforeground="#7c8491", relief="flat",
            padx=14, pady=9, cursor="hand2",
        )
        self.sd_upload_button.pack(side="left", padx=(8, 0))
        self.sd_download_selected_button = tk.Button(
            button_row, text="Download Selected",
            command=self.download_selected_sd_files,
            background=self.ACCENT, foreground="white",
            activebackground=self.ACCENT_ACTIVE, activeforeground="white",
            disabledforeground="#7c8491", relief="flat",
            padx=16, pady=9, cursor="hand2",
        )
        self.sd_download_selected_button.pack(side="right")
        self.sd_download_all_button = tk.Button(
            button_row, text="Download All", command=self.download_all_sd_files,
            background=self.PANEL_ALT, foreground=self.TEXT,
            activebackground="#2a3744", activeforeground=self.TEXT,
            disabledforeground="#7c8491", relief="flat",
            padx=14, pady=9, cursor="hand2",
        )
        self.sd_download_all_button.pack(side="right", padx=(0, 8))
        self._set_sd_controls(False)
        if refresh:
            self.refresh_sd_files()

    def _close_sd_files(self) -> None:
        if self.sd_busy:
            messagebox.showwarning(
                APP_NAME,
                "An SD operation is still in progress. Wait for it to finish before closing this window.",
                parent=self.sd_window,
            )
            return
        if self.sd_client is not None:
            self.sd_activity_var.set(SD_MODE_CLOSING_TEXT)
            self.sd_status_var.set("Closing USB SD mode and returning the device to normal…")
            self._set_sd_controls(True)
            threading.Thread(target=self._sd_close_worker, daemon=True).start()
            return
        self._destroy_sd_window()

    def _destroy_sd_window(self) -> None:
        if self.sd_window is not None and self.sd_window.winfo_exists():
            self.sd_window.destroy()
        self.sd_window = None
        self.sd_entries = []
        self.sd_entry_by_item.clear()
        self.sd_activity_var.set("")
        self._set_sd_controls(False)

    def _sd_close_worker(self) -> None:
        client = self.sd_client
        self.sd_client = None
        try:
            if client is not None:
                client.set_session(False)
        except Exception:
            # The port may disappear while closing. Closing it still releases
            # the desktop side; a device reset always exits transfer mode.
            pass
        finally:
            if client is not None:
                client.close()
        self.events.put(("sd_closed", None))

    def _sd_client_for(self, port: str, upload: bool = False) -> SdSerialClient:
        if self.sd_client is not None:
            if self.sd_client.port != port:
                raise SdProtocolError(
                    "The selected serial device changed while SD Files was open."
                )
            if upload:
                self.sd_client.protocol_info(
                    SD_REQUIRED_CAPABILITIES |
                    frozenset((SD_UPLOAD_CAPABILITY,))
                )
            self.sd_client.set_session(True)
            return self.sd_client

        required = SD_REQUIRED_CAPABILITIES
        if upload:
            required |= frozenset((SD_UPLOAD_CAPABILITY,))
        client = SdSerialClient(port)
        try:
            client.__enter__()
            client.protocol_info(required)
            client.set_session(True)
        except Exception:
            client.close()
            raise
        self.sd_client = client
        return client

    def _set_sd_controls(self, active: bool) -> None:
        self.sd_busy = active
        session_open = self.sd_window is not None and self.sd_window.winfo_exists()
        main_locked = active or session_open
        main_state = "disabled" if main_locked else "normal"
        self.port_combo.configure(state="disabled" if main_locked else "readonly")
        self.firmware_entry.configure(state="disabled" if main_locked else "readonly")
        self.refresh_button.configure(state=main_state)
        self.browse_button.configure(state=main_state)
        self.included_button.configure(state=main_state)
        self.flash_button.configure(
            state=main_state if self.ports else "disabled"
        )
        self.sd_files_button.configure(
            state=main_state if self.ports else "disabled"
        )
        if self.sd_window is not None and self.sd_window.winfo_exists():
            sd_state = "disabled" if active else "normal"
            self.sd_refresh_button.configure(state=sd_state)
            self.sd_upload_button.configure(state=sd_state)
            have_files = bool(self.sd_entries) and not active
            download_state = "normal" if have_files else "disabled"
            self.sd_download_selected_button.configure(state=download_state)
            self.sd_download_all_button.configure(state=download_state)

    def refresh_sd_files(self) -> None:
        if self.flashing or self.sd_busy:
            return
        port = self.selected_port()
        if not port:
            messagebox.showerror(APP_NAME, "Select a serial device first.", parent=self.sd_window)
            return
        self.sd_activity_var.set(SD_FILES_LOADING_TEXT)
        self.sd_status_var.set("Connecting and reading the SD file list…")
        self.sd_progress_text_var.set("")
        self.sd_progress_var.set(0)
        self._set_sd_controls(True)
        threading.Thread(
            target=self._sd_list_worker, args=(port,), daemon=True
        ).start()

    def _sd_list_worker(self, port: str) -> None:
        try:
            client = self._sd_client_for(port)
            entries = client.list_files()
            self.events.put(("sd_list_done", entries))
        except Exception as error:
            self.events.put(("sd_error", friendly_sd_failure(error)))

    def _show_sd_entries(self, entries: list[SdFileEntry]) -> None:
        if self.sd_window is None or not self.sd_window.winfo_exists():
            return
        self.sd_tree.delete(*self.sd_tree.get_children())
        self.sd_entry_by_item.clear()
        self.sd_entries = entries
        for entry in entries:
            item = self.sd_tree.insert(
                "", "end", text=entry.path,
                values=(format_file_size(entry.size), format_modified(entry.modified)),
            )
            self.sd_entry_by_item[item] = entry
        total = sum(entry.size for entry in entries)
        self.sd_status_var.set(
            f"{len(entries)} file{'s' if len(entries) != 1 else ''} — {format_file_size(total)} total"
        )
        self.sd_activity_var.set("")
        self.sd_progress_var.set(0)
        self.sd_progress_text_var.set("")
        self._set_sd_controls(False)

    def _selected_sd_entries(self) -> list[SdFileEntry]:
        if self.sd_window is None or not self.sd_window.winfo_exists():
            return []
        return [
            self.sd_entry_by_item[item]
            for item in self.sd_tree.selection()
            if item in self.sd_entry_by_item
        ]

    def download_selected_sd_files(self) -> None:
        entries = self._selected_sd_entries()
        if not entries:
            messagebox.showinfo(
                APP_NAME, "Select one or more files to download.", parent=self.sd_window
            )
            return
        self._choose_sd_destinations(entries, force_directory=False)

    def download_all_sd_files(self) -> None:
        if not self.sd_entries:
            messagebox.showinfo(APP_NAME, "The SD card has no files to download.", parent=self.sd_window)
            return
        self._choose_sd_destinations(self.sd_entries, force_directory=True)

    def upload_evil_portal_html(self) -> None:
        if self.flashing or self.sd_busy:
            return
        selected = filedialog.askopenfilename(
            parent=self.sd_window,
            title="Choose an Evil Portal HTML template",
            filetypes=(("HTML files", "*.html *.HTML"), ("All files", "*.*")),
        )
        if not selected:
            return

        source = Path(selected)
        try:
            destination = evil_portal_upload_path(source)
            size = source.stat().st_size
            if size <= 0 or size >= EVIL_PORTAL_MAX_HTML_BYTES:
                raise SdProtocolError(
                    "The Evil Portal template must be non-empty and smaller than 30,000 bytes.",
                    "INVALID_SIZE",
                )
        except (OSError, SdProtocolError) as error:
            messagebox.showerror(
                APP_NAME, friendly_sd_failure(error), parent=self.sd_window
            )
            return

        existing = next(
            (entry for entry in self.sd_entries
             if entry.path.casefold() == destination.casefold()),
            None,
        )
        overwrite = existing is not None
        if overwrite and not messagebox.askyesno(
            APP_NAME,
            f"{destination} already exists on the SD card. Replace it only after "
            "the new file passes SHA-256 verification?",
            icon="warning",
            parent=self.sd_window,
        ):
            return

        port = self.selected_port()
        if not port:
            messagebox.showerror(
                APP_NAME, "The selected device is no longer available.",
                parent=self.sd_window,
            )
            return
        self.sd_progress_var.set(0)
        self.sd_progress_text_var.set("0%")
        self.sd_activity_var.set(SD_FILE_UPLOADING_TEXT)
        self.sd_status_var.set(f"Preparing {destination}…")
        self._set_sd_controls(True)
        threading.Thread(
            target=self._sd_upload_worker,
            args=(port, source, overwrite),
            daemon=True,
        ).start()

    def _sd_upload_worker(
        self, port: str, source: Path, overwrite: bool
    ) -> None:
        try:
            client = self._sd_client_for(port, upload=True)

            def report(current: int, total: int) -> None:
                percent = min(100.0, current * 100.0 / max(total, 1))
                self.events.put((
                    "sd_progress",
                    (percent, f"Uploading {source.name}"),
                ))

            entry = client.upload_evil_portal_html(
                source, overwrite=overwrite, progress=report
            )
            entries = client.list_files()
            self.events.put(("sd_upload_done", (entry, entries)))
        except Exception as error:
            self.events.put(("sd_error", friendly_sd_failure(error)))

    def _choose_sd_destinations(
        self, entries: list[SdFileEntry], force_directory: bool
    ) -> None:
        if self.flashing or self.sd_busy:
            return
        default_directory = Path.home() / "Downloads"
        if not default_directory.is_dir():
            default_directory = Path.home()

        targets: list[tuple[SdFileEntry, Path]] = []
        if len(entries) == 1 and not force_directory:
            selected = filedialog.asksaveasfilename(
                parent=self.sd_window,
                title="Save SD file",
                initialdir=str(default_directory),
                initialfile=entries[0].name,
                confirmoverwrite=True,
            )
            if not selected:
                return
            targets.append((entries[0], Path(selected)))
        else:
            selected = filedialog.askdirectory(
                parent=self.sd_window,
                title="Choose a folder for the SD files",
                initialdir=str(default_directory),
                mustexist=True,
            )
            if not selected:
                return
            destination = Path(selected)
            try:
                targets = [
                    (entry, local_path_for_sd(destination, entry.path))
                    for entry in entries
                ]
            except SdProtocolError as error:
                messagebox.showerror(APP_NAME, friendly_sd_failure(error), parent=self.sd_window)
                return
            existing = sum(target.exists() for _entry, target in targets)
            if existing and not messagebox.askyesno(
                APP_NAME,
                f"{existing} local file{'s' if existing != 1 else ''} already exist. "
                "Replace them after each download is verified?",
                icon="warning",
                parent=self.sd_window,
            ):
                return

        port = self.selected_port()
        if not port:
            messagebox.showerror(APP_NAME, "The selected device is no longer available.", parent=self.sd_window)
            return
        self.sd_progress_var.set(0)
        self.sd_progress_text_var.set("0%")
        self.sd_activity_var.set(SD_FILES_DOWNLOADING_TEXT)
        self.sd_status_var.set("Starting verified SD download…")
        self._set_sd_controls(True)
        threading.Thread(
            target=self._sd_download_worker,
            args=(port, targets),
            daemon=True,
        ).start()

    def _sd_download_worker(
        self, port: str, targets: list[tuple[SdFileEntry, Path]]
    ) -> None:
        total_bytes = sum(entry.size for entry, _destination in targets)
        completed_bytes = 0
        try:
            client = self._sd_client_for(port)
            for index, (entry, destination) in enumerate(targets, start=1):
                base_bytes = completed_bytes

                def report(current: int, actual_total: int,
                           entry_path: str = entry.path,
                           item_index: int = index) -> None:
                    denominator = max(total_bytes - entry.size + actual_total, 1)
                    current_total = base_bytes + current
                    percent = min(100.0, current_total * 100.0 / denominator)
                    self.events.put((
                        "sd_progress",
                        (percent, f"{item_index}/{len(targets)}  {entry_path}"),
                    ))

                client.download_file(entry, destination, report)
                completed_bytes += entry.size
            self.events.put(("sd_download_done", (len(targets), targets[-1][1])))
        except Exception as error:
            self.events.put(("sd_error", friendly_sd_failure(error)))

    def _automatic_refresh(self) -> None:
        if (not self.flashing and not self.sd_busy and
                (self.sd_window is None or not self.sd_window.winfo_exists())):
            self.refresh_ports()
        self.root.after(1800, self._automatic_refresh)

    def _append_log(self, message: str) -> None:
        clean = strip_ansi(message).strip()
        if not clean:
            return
        self.log.configure(state="normal")
        self.log.insert("end", clean + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _set_progress(self, value: float) -> None:
        bounded = min(100.0, max(0.0, value))
        bounded = max(self.highest_progress, bounded)
        self.highest_progress = bounded
        self.progress_var.set(bounded)
        self.percent_var.set(f"{int(round(bounded))}%")

    def _set_flashing_controls(self, active: bool) -> None:
        self.flashing = active
        state = "disabled" if active else "normal"
        self.port_combo.configure(state="disabled" if active else "readonly")
        self.firmware_entry.configure(state="disabled" if active else "readonly")
        self.refresh_button.configure(state=state)
        self.browse_button.configure(state=state)
        self.included_button.configure(state=state)
        self.flash_button.configure(state=state if self.ports else "disabled")
        self.sd_files_button.configure(state=state if self.ports else "disabled")

    def start_flash(self) -> None:
        if self.flashing or self.sd_busy:
            return
        port = self.selected_port()
        if not port:
            messagebox.showerror(APP_NAME, "Select a serial device before flashing.")
            return
        try:
            firmware = inspect_firmware(
                self.firmware.path,
                included=self.firmware.included,
                allow_unsafe=not self.firmware.identity_verified,
            )
        except Exception as error:
            messagebox.showerror(APP_NAME, str(error))
            return
        self.firmware = firmware
        self.firmware_detail_var.set(firmware.detail)
        self._update_firmware_warning()

        self.raw_output.clear()
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.progress.configure(style="Flash.Horizontal.TProgressbar")
        self.highest_progress = 0.0
        self._set_progress(0)
        self.status_label.configure(foreground=self.MUTED)
        self.status_var.set("Checking selected firmware…")
        self.flash_button.configure(text="Flashing…")
        self._set_flashing_controls(True)

        thread = threading.Thread(target=self._flash_worker, args=(port, firmware), daemon=True)
        thread.start()

    def _flash_worker(self, port: str, firmware: FirmwareImage) -> None:
        write_phase = 0
        last_raw_percent = -1.0
        write_passes = len(firmware.flash_offsets)

        def output(line: str) -> None:
            nonlocal write_phase, last_raw_percent
            clean = strip_ansi(line).strip()
            if not clean:
                return
            self.raw_output.append(clean)
            self.events.put(("log", clean))
            raw_percent = extract_percent(clean)
            if raw_percent is not None:
                if (last_raw_percent >= 90.0 and raw_percent <= 10.0 and
                        write_phase < write_passes - 1):
                    write_phase += 1
                last_raw_percent = raw_percent
                combined_percent = (
                    (write_phase + raw_percent / 100.0) / write_passes
                ) * 100.0
                # Reserve 0–6% for validation/connection and 99–100% for verification/reset.
                self.events.put(("progress", 6.0 + combined_percent * 0.93))

        try:
            if sha256_file(firmware.path) != firmware.sha256:
                raise ValueError("The selected firmware file changed after it was checked. Choose it again.")
            self.events.put(("progress", 2.0))
            self.events.put(("status", f"Connecting to {port}…"))
            self.events.put(("log", f"Selected firmware: {firmware.path}"))
            self.events.put(("log", f"Image type: {firmware.image_type}"))
            self.events.put(
                ("log", "Flash address(es): " + ", ".join(
                    f"0x{offset:x}" for offset in firmware.flash_offsets
                ))
            )
            self.events.put(("log", f"Firmware SHA-256: {firmware.sha256}"))
            self.events.put(("log", f"Opening serial port: {port}"))

            invoke_esptool(
                ["--chip", "esp32c5", "--port", port, "chip-id"],
                output,
            )
            self.events.put(("progress", 6.0))
            self.events.put(("status", "ESP32-C5 connected — writing selected image…"))

            flash_arguments = [
                    "--chip",
                    "esp32c5",
                    "--port",
                    port,
                    "--baud",
                    FLASH_BAUD,
                    "--before",
                    "default-reset",
                    "--after",
                    "hard-reset",
                    "write-flash",
            ]
            for offset in firmware.flash_offsets:
                flash_arguments.extend((f"0x{offset:x}", str(firmware.path)))
            invoke_esptool(flash_arguments, output)
            self.events.put(("done", (True, "Firmware flashed and verified successfully.")))
        except Exception as error:  # Error details are surfaced in the activity panel.
            details = "\n".join(self.raw_output + [f"{type(error).__name__}: {error}"])
            self.events.put(("log", f"ERROR: {type(error).__name__}: {error}"))
            self.events.put(("done", (False, friendly_failure(details))))

    def _drain_events(self) -> None:
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "log":
                    self._append_log(str(payload))
                elif kind == "progress":
                    self._set_progress(float(payload))
                elif kind == "status":
                    self.status_var.set(str(payload))
                elif kind == "done":
                    success, message = payload  # type: ignore[misc]
                    self._finish(bool(success), str(message))
                elif kind == "sd_list_done":
                    self._show_sd_entries(list(payload))  # type: ignore[arg-type]
                elif kind == "sd_progress":
                    percent, label = payload  # type: ignore[misc]
                    self.sd_progress_var.set(float(percent))
                    self.sd_progress_text_var.set(f"{int(round(float(percent)))}%")
                    self.sd_status_var.set(str(label))
                elif kind == "sd_download_done":
                    count, final_path = payload  # type: ignore[misc]
                    self.sd_progress_var.set(100)
                    self.sd_progress_text_var.set("100%")
                    self.sd_status_var.set(
                        f"Downloaded and verified {int(count)} file{'s' if int(count) != 1 else ''}"
                    )
                    self.sd_activity_var.set("")
                    self._set_sd_controls(False)
                    messagebox.showinfo(
                        APP_NAME,
                        f"Downloaded and SHA-256 verified {int(count)} SD file"
                        f"{'s' if int(count) != 1 else ''}.\n\nLast file: {final_path}",
                        parent=self.sd_window,
                    )
                elif kind == "sd_upload_done":
                    entry, entries = payload  # type: ignore[misc]
                    self._show_sd_entries(list(entries))
                    self.sd_progress_var.set(100)
                    self.sd_progress_text_var.set("100%")
                    self.sd_status_var.set(
                        f"Uploaded and verified {entry.path}"
                    )
                    for item, listed_entry in self.sd_entry_by_item.items():
                        if listed_entry.path == entry.path:
                            self.sd_tree.selection_set(item)
                            self.sd_tree.see(item)
                            break
                    messagebox.showinfo(
                        APP_NAME,
                        f"Uploaded and SHA-256 verified:\n\n{entry.path}\n\n"
                        "It is now available in Select EP HTML File on the device.",
                        parent=self.sd_window,
                    )
                elif kind == "sd_error":
                    self.sd_progress_var.set(0)
                    self.sd_progress_text_var.set("")
                    self.sd_activity_var.set("SD Card Operation Failed.")
                    self.sd_status_var.set("SD operation failed")
                    self._set_sd_controls(False)
                    messagebox.showerror(APP_NAME, str(payload), parent=self.sd_window)
                elif kind == "sd_closed":
                    self._set_sd_controls(False)
                    self._destroy_sd_window()
                    if self.quit_after_sd_close:
                        self.root.destroy()
                        return
        except queue.Empty:
            pass
        self.root.after(100, self._drain_events)

    def _finish(self, success: bool, message: str) -> None:
        self._set_flashing_controls(False)
        self.flash_button.configure(text="Flash Another Device" if success else "Try Again")
        if success:
            self.progress.configure(style="Success.Horizontal.TProgressbar")
            self._set_progress(100)
            self.status_label.configure(foreground=self.SUCCESS)
            self.status_var.set("SUCCESS — device is ready")
            messagebox.showinfo(APP_NAME, message)
        else:
            self.progress.configure(style="Error.Horizontal.TProgressbar")
            self.status_label.configure(foreground=self.ERROR)
            self.status_var.set("FAILED — no successful firmware verification")
            messagebox.showerror(APP_NAME, message)

    def _on_close(self) -> None:
        if self.flashing or self.sd_busy:
            messagebox.showwarning(
                APP_NAME,
                "A device operation is still in progress. Wait for it to finish before closing the app.",
            )
            return
        if self.sd_window is not None and self.sd_window.winfo_exists():
            if self.sd_client is not None:
                self.quit_after_sd_close = True
                self._close_sd_files()
                return
            self._destroy_sd_window()
        self.root.destroy()


def self_test() -> int:
    try:
        firmware = inspect_firmware(resource_path(), included=True)
        import esptool
        import serial
        esptool_output: list[str] = []
        invoke_esptool(["version"], esptool_output.append)
        if not any("5.1.0" in line for line in esptool_output):
            raise RuntimeError("The embedded esptool command did not run correctly.")
        discovered_count = len(discover_ports())
    except Exception as error:
        print(f"SELF-TEST FAILED: {error}", file=sys.stderr)
        return 1
    print(f"{APP_NAME} {APP_VERSION}")
    print(f"Firmware: {firmware.path}")
    print(f"Firmware type: {firmware.image_type} at {firmware.offset_text}")
    print(f"Firmware SHA-256: {firmware.sha256}")
    print(f"esptool: {getattr(esptool, '__version__', 'available')}")
    print(f"pyserial: {getattr(serial, 'VERSION', 'available')}")
    print(f"Serial enumeration: available ({discovered_count} port(s) found)")
    print("SELF-TEST PASSED")
    return 0


def ui_smoke_test() -> int:
    root = tk.Tk()
    app = FlasherApp(root)
    root.update()
    visible_lines = app.visible_activity_lines()
    if visible_lines < MIN_ACTIVITY_LINES:
        root.destroy()
        raise RuntimeError(
            f"Activity log shows only {visible_lines} lines; at least {MIN_ACTIVITY_LINES} are required."
        )
    if not app.sd_files_button.winfo_exists():
        root.destroy()
        raise RuntimeError("The SD Files control was not created.")
    smoke_port = PortEntry("SMOKE", "Test serial device", "")
    app.ports = [smoke_port]
    app.port_by_label = {smoke_port.label: smoke_port}
    app.port_var.set(smoke_port.label)
    app.open_sd_files(refresh=False)
    root.update()
    if app.sd_window is None or not app.sd_tree.winfo_exists():
        root.destroy()
        raise RuntimeError("The SD Files browser did not render.")
    if not app.sd_upload_button.winfo_exists():
        root.destroy()
        raise RuntimeError("The Evil Portal HTML upload control did not render.")
    if not app.sd_activity_label.winfo_exists():
        root.destroy()
        raise RuntimeError("The SD operation activity message did not render.")
    window_bottom = app.sd_window.winfo_rooty() + app.sd_window.winfo_height()
    for control in (
        app.sd_refresh_button,
        app.sd_upload_button,
        app.sd_download_all_button,
        app.sd_download_selected_button,
    ):
        if control.winfo_rooty() + control.winfo_height() > window_bottom:
            root.destroy()
            raise RuntimeError(f"The SD control {control.cget('text')} is clipped.")
    app._close_sd_files()
    root.destroy()
    print(f"UI SMOKE TEST PASSED ({visible_lines} activity lines visible)")
    return 0


def main(arguments: Iterable[str] | None = None) -> int:
    args = list(arguments if arguments is not None else sys.argv[1:])
    if args == ["--self-test"]:
        return self_test()
    if args == ["--ui-smoke-test"]:
        return ui_smoke_test()
    if args:
        print(f"Unknown arguments: {' '.join(args)}", file=sys.stderr)
        return 2
    root = tk.Tk()
    FlasherApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        if os.environ.get("DISPLAY") or sys.platform.startswith("win"):
            try:
                messagebox.showerror(APP_NAME, "The flasher could not start. See the console for details.")
            except Exception:
                pass
        raise
