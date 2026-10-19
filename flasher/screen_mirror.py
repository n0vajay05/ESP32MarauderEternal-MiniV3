"""Bounded, acknowledged USB screen mirroring for the Mini V3.

This module has no GUI dependency. Normal serial commands use 115200 baud;
a mirror session negotiates 921600 and returns to 115200 on close or lease expiry.
"""
from __future__ import annotations

import binascii
import json
import os
import secrets
import struct
import time
import threading
from dataclasses import dataclass

import serial

MAGIC = b"\xa5MRR"
HEADER = struct.Struct("<4sBBIIBBH")
WIDTH = HEIGHT = 128
MAX_PACKET = 278
CAPABILITY = "screen-mirror-v1"


class MirrorError(RuntimeError):
    pass


@dataclass(frozen=True)
class MirrorPacket:
    sequence: int
    session: int
    row: int | None
    pixels: bytes


class PacketDecoder:
    """Resynchronize through console output, partial reads and corrupt packets."""
    def __init__(self) -> None:
        self.buffer = bytearray()
        self.corrupt_packets = 0

    def feed(self, data: bytes) -> list[MirrorPacket]:
        self.buffer.extend(data)
        packets = []
        while True:
            at = self.buffer.find(MAGIC)
            if at < 0:
                del self.buffer[:-3]
                break
            if at:
                del self.buffer[:at]
            if len(self.buffer) < HEADER.size:
                break
            _, version, kind, sequence, session, row, encoding, length = HEADER.unpack_from(self.buffer)
            if (version != 1 or kind not in (0, 1) or encoding not in (0, 1)
                    or length > WIDTH * 2 or row >= HEIGHT
                    or (kind == 0 and (length or encoding))):
                del self.buffer[0]
                self.corrupt_packets += 1
                continue
            size = HEADER.size + length + 4
            if len(self.buffer) < size:
                break
            candidate = bytes(self.buffer[:size])
            crc, = struct.unpack_from("<I", candidate, size - 4)
            if binascii.crc32(candidate[:-4]) != crc:
                del self.buffer[0]
                self.corrupt_packets += 1
                continue
            payload = candidate[HEADER.size:-4]
            try:
                if kind == 0:
                    pixels = b""
                elif encoding == 0:
                    if len(payload) != WIDTH * 2:
                        raise ValueError("raw row length")
                    pixels = payload
                else:
                    pixels = bytearray()
                    if len(payload) % 3:
                        raise ValueError("run length")
                    for offset in range(0, len(payload), 3):
                        run = payload[offset]
                        if not run or len(pixels) + run * 2 > WIDTH * 2:
                            raise ValueError("run bounds")
                        pixels.extend(payload[offset + 1:offset + 3] * run)
                    if len(pixels) != WIDTH * 2:
                        raise ValueError("incomplete row")
                    pixels = bytes(pixels)
            except ValueError:
                del self.buffer[0]
                self.corrupt_packets += 1
                continue
            del self.buffer[:size]
            packets.append(MirrorPacket(sequence, session, row if kind else None, pixels))
        # Junk never persists beyond the small partial-packet prefix above.
        assert len(self.buffer) < MAX_PACKET
        return packets


class MirrorSerialClient:
    def __init__(self, port: str, connection: object | None = None,
                 cancel: threading.Event | None = None) -> None:
        self.port = port
        self.connection = connection
        self.owns_connection = connection is None
        self.session = secrets.randbits(32) or 1
        self.sequence = 0
        self.decoder = PacketDecoder()
        self.image = bytearray(WIDTH * HEIGHT * 3)
        self.received_rows: set[int] = set()
        self.rows = 0
        self.info: dict[str, object] = {}
        self.started = False
        self.cancel = cancel or threading.Event()

    def _send(self, text: str) -> None:
        payload = (text + "\n").encode("ascii")
        if self.connection.write(payload) != len(payload):
            raise MirrorError("USB mirror command was only partially sent.")

    def _message(self, prefix: bytes, timeout: float, transaction: object,
                 field: str = "session", cancellable: bool = True) -> dict[str, object] | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if cancellable and self.cancel.is_set():
                raise MirrorError("Screen Mirror connection cancelled.")
            line = self.connection.read_until(b"\n", 4096)
            at = line.find(prefix)
            if at < 0:
                continue
            try:
                message = json.loads(line[at + len(prefix):])
            except (ValueError, UnicodeDecodeError):
                continue
            if isinstance(message, dict) and message.get(field) == transaction:
                return message
        return None

    def __enter__(self) -> "MirrorSerialClient":
        try:
            if self.connection is None:
                connection = serial.Serial(port=None, baudrate=115200,
                                           timeout=.1, write_timeout=.5)
                connection.dtr = False
                connection.rts = False
                if os.name == "posix":
                    connection.exclusive = True
                connection.port = self.port
                connection.open()
                self.connection = connection
            transaction = secrets.token_hex(8)
            # Probe immediately: attaching to an active device must not wait
            # for a boot prompt or stop its current operation.
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                self._send(f"protocolinfo --machine {transaction}")
                message = self._message(b"@MARAUDER:", 1, transaction, "tx")
                if message is not None:
                    break
            else:
                raise MirrorError("The Mini did not respond. Check its USB connection.")
            if CAPABILITY not in message.get("capabilities", []):
                raise MirrorError("This firmware does not support Screen Mirror. Flash the included current firmware first.")
            self._send(f"mirror start {self.session:08x}")
            message = self._message(b"@MIRROR:", 3, self.session)
            if message is None:
                raise MirrorError("Screen Mirror did not start. Reconnect and try again.")
            if message.get("status") != "ready":
                reason = message.get("status", "unknown error")
                if reason == "no-memory":
                    raise MirrorError("The Mini has insufficient memory for Screen Mirror.")
                if reason == "storage-busy":
                    raise MirrorError("Close SD Files before starting Screen Mirror.")
                raise MirrorError(f"The Mini could not start Screen Mirror: {reason}")
            if (message.get("protocol"), message.get("width"), message.get("height"), message.get("baud")) != (1, WIDTH, HEIGHT, 921600):
                raise MirrorError("The Mini returned an unsupported mirror format.")
            self.info = message
            self.connection.baudrate = 921600
            self.started = True
            return self
        except Exception:
            self.close()
            raise

    def poll(self) -> bool:
        if not self.started:
            raise MirrorError("Screen Mirror is not connected.")
        for _ in range(3):
            self._send(f"mirror poll {self.session:08x} {self.sequence:08x}")
            deadline = time.monotonic() + .4
            while time.monotonic() < deadline:
                if self.cancel.is_set():
                    raise MirrorError("Screen Mirror connection cancelled.")
                chunk = self.connection.read(min(self.connection.in_waiting or 1, 4096))
                for packet in self.decoder.feed(chunk):
                    if packet.session != self.session:
                        continue
                    if packet.sequence == self.sequence:
                        # A retry can leave a duplicate reply in the RX buffer.
                        continue
                    self.sequence = packet.sequence
                    if packet.row is None:
                        return False
                    start = packet.row * WIDTH * 3
                    rgb = bytearray(WIDTH * 3)
                    for x in range(WIDTH):
                        pixel = (packet.pixels[x * 2] << 8) | packet.pixels[x * 2 + 1]
                        r, g, b = (pixel >> 11) & 31, (pixel >> 5) & 63, pixel & 31
                        rgb[x * 3:x * 3 + 3] = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
                    self.image[start:start + WIDTH * 3] = rgb
                    self.received_rows.add(packet.row)
                    self.rows += 1
                    return True
        raise MirrorError("Screen Mirror lost its USB connection. Reconnect to resume.")

    def close(self) -> None:
        if self.connection is None:
            return
        if self.started:
            try:
                self._send(f"mirror stop {self.session:08x}")
                message = self._message(b"@MIRROR:", 1, self.session, cancellable=False)
                if message is not None and message.get("status") == "stopped":
                    self.connection.baudrate = 115200
            except (OSError, serial.SerialException, MirrorError):
                pass
            self.started = False
        if self.owns_connection:
            self.connection.close()
            self.connection = None

    def __exit__(self, *_args: object) -> None:
        self.close()
