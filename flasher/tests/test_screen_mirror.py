import binascii
import json
import struct
import threading
import unittest

from flasher.screen_mirror import HEADER, MAGIC, MirrorError, MirrorKeyboard, MirrorSerialClient, PacketDecoder


def packet(payload=b"\x80\xf8\x00", encoding=1, sequence=1, session=42, row=17, kind=1):
    raw = HEADER.pack(MAGIC, 1, kind, sequence, session, row, encoding, len(payload)) + payload
    return raw + struct.pack("<I", binascii.crc32(raw))


class DecoderTests(unittest.TestCase):
    def test_partial_reads_console_noise_and_coalesced_packets(self):
        wire = b"#normal console\r\n> " + packet() + packet(b"", 0, 2, kind=0)
        decoder = PacketDecoder()
        result = []
        for byte in wire:
            result += decoder.feed(bytes((byte,)))
        self.assertEqual(len(result), 2)
        self.assertEqual(result[0].pixels, b"\xf8\x00" * 128)
        self.assertIsNone(result[1].row)
        self.assertLess(len(decoder.buffer), 278)

    def test_crc_failure_and_truncated_packet_recover(self):
        corrupt = bytearray(packet()); corrupt[-1] ^= 1
        decoder = PacketDecoder()
        result = decoder.feed(bytes(corrupt) + packet(sequence=2))
        self.assertEqual([p.sequence for p in result], [2])
        self.assertGreater(decoder.corrupt_packets, 0)

    def test_rejects_malformed_runs_raw_rows_and_oversized_headers(self):
        bad = (packet(b"\0\xf8\0"), packet(b"\x81\xf8\0"), packet(b"\x01\xf8\0"),
               packet(b"\x80\xf8"), packet(b"\0" * 10, 0), packet(row=128),
               HEADER.pack(MAGIC, 1, 1, 1, 42, 0, 0, 65535))
        for wire in bad:
            with self.subTest(wire=wire):
                decoder = PacketDecoder()
                self.assertEqual(len(decoder.feed(wire + packet())), 1)
                self.assertGreater(decoder.corrupt_packets, 0)

    def test_native_raw_color_order(self):
        pixels = b"\xf8\x00\x07\xe0\x00\x1f" + b"\0\0" * 125
        result = PacketDecoder().feed(packet(pixels, 0))
        self.assertEqual(result[0].pixels, pixels)

    def test_junk_buffer_and_duplicate_ack_packets_remain_bounded(self):
        decoder = PacketDecoder()
        self.assertEqual(decoder.feed(b"x" * 100000), [])
        self.assertLessEqual(len(decoder.buffer), 3)
        self.assertEqual(len(decoder.feed(packet() + packet())), 2)


class FakeSerial:
    def __init__(self, capability=True, corrupt_first=False, wrong_session=False, keyboard=False):
        self.rx = bytearray(); self.writes = []; self.baudrate = 115200
        self.capability = capability; self.corrupt_first = corrupt_first
        self.wrong_session = wrong_session; self.closed = False
        self.keyboard = keyboard
        self.sequence = 0

    @property
    def in_waiting(self):
        return len(self.rx)

    def write(self, data):
        self.writes.append(data)
        fields = data.decode().strip().split()
        if fields[0] == "protocolinfo":
            result = {"protocol": 1, "tx": fields[-1], "capabilities": ["screen-mirror-v1"] if self.capability else []}
            self.rx += b"@MARAUDER:" + json.dumps(result).encode() + b"\n"
        elif fields[1] in ("start", "stop"):
            result = {"protocol": 1, "session": int(fields[2], 16), "status": "ready" if fields[1] == "start" else "stopped",
                      "width": 128, "height": 128, "baud": 921600, "input": self.keyboard}
            self.rx += b"@MIRROR:" + json.dumps(result).encode() + b"\n"
        elif fields[1] == "poll":
            session = int(fields[2], 16)
            if self.wrong_session:
                self.rx += packet(session=session ^ 1)
            self.sequence += 1
            wire = bytearray(packet(session=session, sequence=self.sequence))
            if self.corrupt_first:
                wire[-1] ^= 1; self.corrupt_first = False
            self.rx += wire
        return len(data)

    def read(self, size):
        data = bytes(self.rx[:size]); del self.rx[:size]; return data

    def read_until(self, marker, size):
        at = self.rx.find(marker)
        return self.read(min(size, at + len(marker) if at >= 0 else len(self.rx)))

    def close(self):
        self.closed = True


class ClientTests(unittest.TestCase):
    def test_controls_only_on_negotiated_active_session_and_release_by_default(self):
        wire = FakeSerial(keyboard=True)
        client = MirrorSerialClient("fixture", wire)
        with self.assertRaisesRegex(MirrorError, "not connected"):
            client.poll(16)
        with client:
            self.assertTrue(client.input_supported)
            client.poll(17)
            client.poll()
            polls = [w.decode().split() for w in wire.writes if w.startswith(b"mirror poll")]
            self.assertEqual([p[-1] for p in polls], ["00000011", "00000000"])
            self.assertTrue(all(int(p[2], 16) == client.session for p in polls))
            for invalid in (-1, 32, "up"):
                with self.assertRaises(ValueError):
                    client.poll(invalid)
        self.assertFalse(client.input_supported)
        with self.assertRaises(MirrorError):
            client.poll(16)

    def test_older_mirror_firmware_stays_view_only(self):
        wire = FakeSerial()
        with MirrorSerialClient("fixture", wire) as client:
            self.assertFalse(client.input_supported)
            client.poll(16)
        polls = [w.decode().split() for w in wire.writes if w.startswith(b"mirror poll")]
        self.assertTrue(all(len(p) == 4 for p in polls))

    def test_focus_release_is_refreshed_during_corrupt_packet_retry(self):
        wire = FakeSerial(keyboard=True, corrupt_first=True)
        states = iter((16, 0))
        with MirrorSerialClient("fixture", wire) as client:
            client.poll(lambda: next(states))
        polls = [w.decode().split() for w in wire.writes if w.startswith(b"mirror poll")]
        self.assertEqual([p[-1] for p in polls], ["00000010", "00000000"])

    def test_negotiates_and_restores_baud_without_stopping_device(self):
        wire = FakeSerial(wrong_session=True)
        with MirrorSerialClient("fixture", wire) as client:
            self.assertEqual(wire.baudrate, 921600)
            self.assertTrue(client.poll())
            self.assertEqual(client.image[17 * 384:17 * 384 + 3], b"\xff\0\0")
            self.assertEqual(client.received_rows, {17})
        self.assertEqual(wire.baudrate, 115200)
        self.assertFalse(wire.closed)  # An injected connection remains owned by its caller.
        self.assertFalse(any(b"stopscan" in request for request in wire.writes))

    def test_corrupt_packet_is_retried_with_previous_ack(self):
        wire = FakeSerial(corrupt_first=True)
        with MirrorSerialClient("fixture", wire) as client:
            self.assertTrue(client.poll())
            polls = [w for w in wire.writes if w.startswith(b"mirror poll")]
            self.assertEqual(len(polls), 2)
            self.assertTrue(all(p.endswith(b"00000000\n") for p in polls))
            self.assertEqual(client.decoder.corrupt_packets, 1)

    def test_older_firmware_has_clear_upgrade_message(self):
        with self.assertRaisesRegex(MirrorError, "Flash the included current firmware"):
            MirrorSerialClient("fixture", FakeSerial(capability=False)).__enter__()

    def test_connect_can_be_cancelled(self):
        cancel = threading.Event(); cancel.set()
        with self.assertRaisesRegex(MirrorError, "cancelled"):
            MirrorSerialClient("fixture", FakeSerial(), cancel).__enter__()


class KeyboardTests(unittest.TestCase):
    def test_short_taps_and_double_taps_preserve_press_release_edges(self):
        keys = MirrorKeyboard()
        for _ in range(2):
            keys.key("C", True); keys.key("c", False)
        self.assertEqual(keys.sample(0), 16)
        self.assertEqual(keys.sample(.01), 16)
        self.assertEqual(keys.sample(.07), 0)
        self.assertEqual(keys.sample(.14), 16)
        self.assertEqual(keys.sample(.21), 0)

    def test_held_keys_ignore_autorepeat_and_support_combinations(self):
        keys = MirrorKeyboard()
        keys.key("Left", True)
        self.assertEqual(keys.sample(0), 4)
        for _ in range(100):
            keys.key("Left", True)
        self.assertEqual(keys.sample(2), 4)
        keys.key("c", True)
        self.assertEqual(keys.sample(2.1), 20)
        keys.key("Left", False)
        self.assertEqual(keys.sample(2.2), 16)
        keys.key("c", False)
        self.assertEqual(keys.sample(2.3), 0)

    def test_focus_loss_cancels_pending_taps_and_held_keys_immediately(self):
        keys = MirrorKeyboard()
        keys.key("Up", True)
        self.assertEqual(keys.sample(0), 1)
        keys.key("c", True); keys.key("c", False)
        keys.clear()
        self.assertEqual(keys.sample(.001), 0)
        self.assertEqual(keys.sample(100), 0)
        keys.key("Up", True)
        self.assertEqual(keys.sample(101), 1)

    def test_extreme_input_is_bounded_and_finishes_released(self):
        keys = MirrorKeyboard()
        keys.key("unrelated", True)
        for _ in range(10000):
            keys.key("Down", True); keys.key("Down", False)
        self.assertLessEqual(len(keys._pending), keys.MAX_PENDING)
        for tick in range(keys.MAX_PENDING + 1):
            value = keys.sample(tick)
        self.assertEqual(value, 0)


if __name__ == "__main__":
    unittest.main()
