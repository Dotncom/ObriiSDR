"""Bounded Obrii LoRa transport framing. RNS packets remain opaque.

This envelope is NOT the on-air RNode firmware format. Two Obrii adapters
can exchange it; RNS destinations, crypto and packet format are unchanged.
"""
from collections import OrderedDict
from dataclasses import dataclass, field
import math
import secrets
import struct
import time
import zlib

HEADER = struct.Struct("!4sBBQHHI")
MAGIC = b"OBRN"
VERSION = 1
MAX_PACKET = 564


def bounded_int(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{name} must be an integer in {low}..{high}")
    return value


class Fragmenter:
    def __init__(self, mtu=255):
        self.mtu = bounded_int(mtu, 64, 255, "MTU")
        self._session = secrets.randbits(32)
        self._counter = 0

    def split(self, packet):
        if not isinstance(packet, bytes) or not 1 <= len(packet) <= MAX_PACKET:
            raise ValueError("RNS transport packet must be 1..564 bytes")
        if self._counter == 0xFFFFFFFF:
            raise OverflowError("Fragment sequence exhausted; recreate the interface")
        self._counter += 1
        identity = self._session << 32 | self._counter
        capacity = self.mtu - HEADER.size
        count = (len(packet) + capacity - 1) // capacity
        checksum = zlib.crc32(packet)
        return [HEADER.pack(MAGIC, VERSION, count, identity, i, len(packet), checksum)
                + packet[i * capacity:(i + 1) * capacity] for i in range(count)]


@dataclass
class _Partial:
    count: int
    total: int
    crc: int
    expires: float
    pieces: dict = field(default_factory=dict)


class Reassembler:
    def __init__(self, mtu=255, ttl=120.0, max_pending=64, max_recent=128):
        self.mtu = bounded_int(mtu, 64, 255, "MTU")
        if type(ttl) not in (int, float) or not 1 <= ttl <= 3600:
            raise ValueError("Fragment TTL must be 1..3600 seconds")
        self.ttl = ttl
        self.max_pending = bounded_int(max_pending, 1, 1024, "pending limit")
        self.max_recent = bounded_int(max_recent, 1, 4096, "recent limit")
        self.pending = {}
        self.recent = OrderedDict()
        self.rejected = self.expired = self.completed = self.duplicates = 0
        self._time = 0.0

    def reset(self):
        self.pending.clear()
        self.recent.clear()

    def _expire(self, now):
        if type(now) not in (int, float) or not math.isfinite(now):
            raise ValueError("Fragment clock must be finite")
        if now < self._time:
            raise ValueError("Fragment clock moved backwards")
        self._time = now
        for identity, item in list(self.pending.items()):
            if item.expires <= now:
                del self.pending[identity]
                self.expired += 1
        while self.recent and next(iter(self.recent.values())) <= now:
            self.recent.popitem(last=False)

    def _remember(self, identity, now):
        self.recent.pop(identity, None)
        self.recent[identity] = now + self.ttl
        while len(self.recent) > self.max_recent:
            self.recent.popitem(last=False)

    def receive(self, frame, now=None):
        now = time.monotonic() if now is None else now
        self._expire(now)
        if not isinstance(frame, bytes) or not HEADER.size < len(frame) <= self.mtu:
            self.rejected += 1
            return None
        magic, version, count, identity, index, total, crc = HEADER.unpack_from(frame)
        capacity = self.mtu - HEADER.size
        expected_count = (total + capacity - 1) // capacity
        expected_length = min(capacity, total - index * capacity)
        if (magic != MAGIC or version != VERSION or not 1 <= total <= MAX_PACKET
                or count != expected_count or not 0 <= index < count
                or len(frame) != HEADER.size + expected_length):
            self.rejected += 1
            return None
        if identity in self.recent:
            self.duplicates += 1
            return None
        item = self.pending.get(identity)
        if item is None:
            if len(self.pending) >= self.max_pending:
                self.rejected += 1
                return None
            item = self.pending[identity] = _Partial(count, total, crc, now + self.ttl)
        piece = frame[HEADER.size:]
        if ((item.count, item.total, item.crc) != (count, total, crc)
                or index in item.pieces and item.pieces[index] != piece):
            self.pending.pop(identity, None)
            self._remember(identity, now)
            self.rejected += 1
            return None
        if index in item.pieces:
            self.duplicates += 1
            return None
        item.pieces[index] = piece
        if len(item.pieces) != count:
            return None
        packet = b"".join(item.pieces[i] for i in range(count))
        del self.pending[identity]
        self._remember(identity, now)
        if len(packet) != total or zlib.crc32(packet) != crc:
            self.rejected += 1
            return None
        self.completed += 1
        return packet
