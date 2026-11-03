import random
import unittest
from obrii_reticulum.fragments import Fragmenter, Reassembler, HEADER, MAX_PACKET


class Fragments(unittest.TestCase):
    def test_all_lengths_and_mtu(self):
        for mtu in (64, 128, 255):
            f, r = Fragmenter(mtu), Reassembler(mtu)
            for length in range(1, MAX_PACKET + 1):
                payload = bytes(i % 251 for i in range(length))
                frames = f.split(payload)
                self.assertTrue(all(22 < len(p) <= mtu for p in frames))
                results = [r.receive(p) for p in reversed(frames)]
                self.assertEqual([p for p in results if p is not None], [payload])
                self.assertIsNone(r.receive(frames[0]))
            self.assertFalse(r.pending)
            self.assertLessEqual(len(r.recent), r.max_recent)

    def test_wire_vector(self):
        f = Fragmenter()
        f._session = 1
        expected = bytes.fromhex("4f42524e0101000000010000000100000003352441c2616263")
        self.assertEqual(f.split(b"abc"), [expected])
        self.assertEqual(Reassembler().receive(expected), b"abc")

    def test_duplicate_conflict_crc_and_missing(self):
        f, r = Fragmenter(), Reassembler(ttl=2)
        parts = f.split(bytes(564))
        self.assertIsNone(r.receive(parts[0], now=1))
        self.assertIsNone(r.receive(parts[0], now=1))
        self.assertEqual(r.duplicates, 1)
        conflict = parts[0][:-1] + b"\x01"
        self.assertIsNone(r.receive(conflict, now=1))
        for p in parts[1:]:
            self.assertIsNone(r.receive(p, now=1))
        self.assertEqual(r.completed, 0)
        self.assertEqual(r.rejected, 1)
        parts = f.split(bytes(564))
        for p in parts[:-1]:
            r.receive(p, now=2)
        # Partial packet expires; a late tail cannot resurrect its missing data.
        self.assertIsNone(r.receive(parts[-1], now=5))
        self.assertEqual(r.expired, 1)
        r.reset()
        self.assertFalse(r.pending)
        parts = f.split(b"abc")
        self.assertIsNone(r.receive(parts[0][:-1] + b"X", now=5))
        self.assertEqual(r.rejected, 2)

    def test_bounds_and_malformed_input(self):
        f, r = Fragmenter(), Reassembler(max_pending=2, max_recent=2, ttl=2)
        for _ in range(3):
            r.receive(f.split(bytes(564))[0], now=1)
        self.assertEqual(len(r.pending), 2)
        self.assertEqual(r.rejected, 1)
        r.receive(b"bad", now=4)
        self.assertFalse(r.pending)
        self.assertEqual(r.expired, 2)
        rng = random.Random(73)
        for _ in range(5000):
            self.assertIsNone(r.receive(rng.randbytes(rng.randrange(400)), now=4))
        valid = f.split(b"test")[0]
        for offset in (0, 4, 5, 14, 16, 18):
            damaged = bytearray(valid)
            damaged[offset] ^= 0x80
            self.assertIsNone(r.receive(bytes(damaged), now=4))
        self.assertLessEqual(len(r.pending), 2)
        with self.assertRaises(ValueError):
            r.receive(valid, now=3)
        for invalid in (float("nan"), float("inf"), True):
            with self.assertRaises(ValueError):
                r.receive(valid, now=invalid)

    def test_configuration_and_sequence(self):
        for mtu in (True, 63, 256, 255.0):
            with self.assertRaises(ValueError):
                Fragmenter(mtu)
        for ttl in (True, 0, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                Reassembler(ttl=ttl)
        f = Fragmenter()
        for value in (b"", bytes(565), bytearray(b"a")):
            with self.assertRaises(ValueError):
                f.split(value)
        f._counter = 0xFFFFFFFF
        with self.assertRaises(OverflowError):
            f.split(b"a")
