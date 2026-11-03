import json
from pathlib import Path
import random
import unittest
from unittest.mock import patch
from obrii_reticulum.rnode import Fragmenter, Reassembler


class RNode(unittest.TestCase):
    def test_compiled_firmware_vectors(self):
        reference = json.loads((Path(__file__).parent / "vectors/rnode_firmware_1_86.json").read_text(encoding="utf-8"))
        self.assertEqual(reference["verified_cases"], 8128)
        for v in reference["vectors"]:
            payload = bytes((i * 17 + 3) % 256 for i in range(v["length"]))
            frames = [bytes.fromhex(p) for p in v["frames"]]
            with patch("obrii_reticulum.rnode.secrets.randbits", return_value=v["sequence"]):
                encoded = Fragmenter().split(payload)
            expected = frames[:-1] if v["length"] == 508 else frames
            self.assertEqual(encoded, expected)
            r = Reassembler()
            decoded = [r.receive(p) for p in frames]
            self.assertEqual([p for p in decoded if p is not None], [payload])
            self.assertFalse(r.pending)
            # Upstream's captured KISS output also verifies serial escaping.
            from RNS.Interfaces.RNodeInterface import KISS
            self.assertEqual(bytes.fromhex(v["kiss"]), b"\xc0\x00" + KISS.escape(payload) + b"\xc0")

    def test_all_lengths_sequences_and_identical_halves(self):
        for length in range(1, 509):
            for sequence in range(16):
                payload = bytes(length) # 508-byte packets have identical halves.
                with patch("obrii_reticulum.rnode.secrets.randbits", return_value=sequence):
                    frames = Fragmenter().split(payload)
                self.assertEqual(len(frames), 1 if length <= 254 else 2)
                self.assertTrue(all(2 <= len(f) <= 255 for f in frames))
                r = Reassembler()
                output = [r.receive(p) for p in frames]
                self.assertEqual([p for p in output if p is not None], [payload])
                self.assertFalse(r.pending)

    def test_loss_sequence_changes_single_interruption_and_ttl(self):
        r = Reassembler(ttl=2)
        first = b"\x11" + bytes(254)
        next_first = b"\x21" + bytes([2]) * 254
        tail = b"\x21tail"
        self.assertIsNone(r.receive(first, now=1))
        self.assertIsNone(r.receive(next_first, now=1))
        self.assertEqual(len(r.pending), 1)
        self.assertEqual(r.receive(tail, now=1), bytes([2]) * 254 + b"tail")
        self.assertIsNone(r.receive(tail, now=1)) # A short orphan is never a first half.
        r.receive(first, now=1)
        self.assertIsNone(r.receive(b"\x11tail", now=4))
        self.assertEqual(r.expired, 1)
        r.receive(first, now=4)
        self.assertEqual(r.receive(b"\xf0single", now=4), b"single")
        self.assertFalse(r.pending)
        r.receive(first, now=4)
        r.reset()
        self.assertFalse(r.pending)
        self.assertIsNone(r.receive(b"\x11", now=4)) # Firmware's empty 508-byte trailer.
        self.assertEqual(r.empty_frames, 1)

    def test_validation_and_bounded_noise(self):
        for mtu in (64, 254, 256, True):
            with self.assertRaises(ValueError):
                Fragmenter(mtu)
        for payload in (b"", bytes(509), bytearray(b"a")):
            with self.assertRaises(ValueError):
                Fragmenter().split(payload)
        r = Reassembler()
        for value in (b"", bytes(256), None, bytearray(b"x")):
            self.assertIsNone(r.receive(value, now=1))
        rng = random.Random(71)
        for _ in range(5000):
            r.receive(rng.randbytes(rng.randrange(400)), now=1)
            self.assertLessEqual(len(r.pending), 1)
        for now in (0, True, float("inf"), float("nan")):
            with self.assertRaises(ValueError):
                r.receive(b"\x00a", now=now)
