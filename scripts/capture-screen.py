#!/usr/bin/env python3
"""Request one RGB888 TFT frame over the Marauder serial CLI and save a PNG."""

from __future__ import annotations

import argparse
import binascii
import pathlib
import struct
import sys
import time
import zlib

import serial


BEGIN = b"SCREENSHOT-BEGIN "
END = b"\nSCREENSHOT-END"


def read_until(port: serial.Serial, marker: bytes, deadline: float) -> bytes:
    received = bytearray()
    while marker not in received:
        if time.monotonic() >= deadline:
            raise TimeoutError(f"timed out waiting for {marker!r}")
        # Read marker bytes one at a time so bytes belonging to the following
        # binary frame are never consumed into this temporary buffer.
        chunk = port.read(1)
        if chunk:
            received.extend(chunk)
    return bytes(received)


def read_exactly(port: serial.Serial, length: int, deadline: float) -> bytes:
    received = bytearray()
    while len(received) < length:
        if time.monotonic() >= deadline:
            raise TimeoutError(
                f"received {len(received)} of {length} screenshot bytes"
            )
        chunk = port.read(min(port.in_waiting or 1, length - len(received)))
        if chunk:
            received.extend(chunk)
    return bytes(received)


def png_chunk(chunk_type: bytes, payload: bytes) -> bytes:
    checksum = binascii.crc32(chunk_type)
    checksum = binascii.crc32(payload, checksum) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + chunk_type + payload + struct.pack(">I", checksum)


def encode_png(width: int, height: int, rgb: bytes) -> bytes:
    expected = width * height * 3
    if len(rgb) != expected:
        raise ValueError(f"expected {expected} RGB bytes, received {len(rgb)}")

    stride = width * 3
    scanlines = b"".join(
        b"\x00" + rgb[offset : offset + stride]
        for offset in range(0, len(rgb), stride)
    )
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(scanlines, level=9))
        + png_chunk(b"IEND", b"")
    )


def capture(device: str, baud: int, timeout: float) -> tuple[int, int, bytes]:
    deadline = time.monotonic() + timeout
    with serial.Serial(device, baudrate=baud, timeout=0.1, exclusive=True) as port:
        port.reset_input_buffer()
        port.write(b"screenshot\n")
        port.flush()

        prefix_data = read_until(port, BEGIN, deadline)
        header_start = prefix_data.rfind(BEGIN) + len(BEGIN)
        header = prefix_data[header_start:] + read_until(port, b"\n", deadline)
        fields = header.strip().split()
        if len(fields) != 4 or fields[0] != b"RGB888":
            raise RuntimeError(f"unexpected screenshot header: {header!r}")

        width, height, length = map(int, fields[1:])
        if length != width * height * 3:
            raise RuntimeError(
                f"device announced {length} bytes for a {width}x{height} RGB888 frame"
            )

        rgb = read_exactly(port, length, deadline)
        trailer = read_until(port, END, deadline)
        if not trailer.endswith(END):
            raise RuntimeError("invalid screenshot trailer")
        return width, height, rgb


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", help="serial device path")
    parser.add_argument("output", type=pathlib.Path, help="output PNG path")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()

    try:
        width, height, rgb = capture(args.device, args.baud, args.timeout)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(encode_png(width, height, rgb))
    except (OSError, RuntimeError, TimeoutError, ValueError, serial.SerialException) as error:
        print(f"capture failed: {error}", file=sys.stderr)
        return 1

    unique_pixels = len({rgb[index : index + 3] for index in range(0, len(rgb), 3)})
    print(f"saved {width}x{height} RGB screenshot to {args.output}")
    print(f"unique pixel colors: {unique_pixels}")
    if unique_pixels <= 2:
        print(
            "warning: nearly uniform capture; the TFT readback path may not be wired",
            file=sys.stderr,
        )
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
