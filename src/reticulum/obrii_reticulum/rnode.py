"""Independent implementation of RNode's normal-mode LoRa radio envelope.

Wire contract checked against upstream RNode Firmware 1.86. This is not KISS
serial framing. The tiny radio header carries no sender, length or part index;
fragments must remain in arrival order and cannot safely be deduplicated here.
"""
import math
import secrets
import time
from .fragments import bounded_int

MAX_PACKET = 508
PAYLOAD_PER_FRAME = 254
SPLIT = 0x01


class Fragmenter:
    def __init__(self, mtu=255):
        self.mtu = bounded_int(mtu, 255, 255, "RNode radio MTU")

    def split(self, packet):
        if not isinstance(packet, bytes) or not 1 <= len(packet) <= MAX_PACKET:
            raise ValueError("RNode transport packet must be 1..508 bytes")
        header = secrets.randbits(4) << 4
        if len(packet) > PAYLOAD_PER_FRAME:
            header |= SPLIT
        # Exactly two populated frames at 508 bytes. Upstream TX also emits an
        # empty trailing split header at this boundary; RX does not require it.
        return [bytes([header]) + packet[i:i + PAYLOAD_PER_FRAME]
                for i in range(0, len(packet), PAYLOAD_PER_FRAME)]


class Reassembler:
    def __init__(self, mtu=255, ttl=120.0):
        self.mtu = bounded_int(mtu, 255, 255, "RNode radio MTU")
        if type(ttl) not in (int, float) or not 1 <= ttl <= 3600:
            raise ValueError("Fragment TTL must be 1..3600 seconds")
        self.ttl = ttl
        self.pending = {} # One first half per radio, as in RNode firmware.
        self.rejected = self.expired = self.completed = self.duplicates = 0
        self.empty_frames = 0
        self._time = 0.0

    def reset(self):
        self.pending.clear()

    def receive(self, frame, now=None):
        now = time.monotonic() if now is None else now
        if type(now) not in (int, float) or not math.isfinite(now) or now < self._time:
            raise ValueError("Fragment clock must be finite and monotonic")
        self._time = now
        if self.pending and next(iter(self.pending.values()))[1] <= now:
            self.pending.clear()
            self.expired += 1
        if not isinstance(frame, bytes) or not 1 <= len(frame) <= self.mtu:
            self.rejected += 1
            return None
        if len(frame) == 1:
            # Official firmware can emit this after a 508-byte datagram. It
            # contributes no payload and must not seed the next reassembly.
            if frame[0] & SPLIT:
                self.empty_frames += 1
            else:
                self.rejected += 1
            return None
        header, payload = frame[0], frame[1:]
        sequence = header >> 4
        if not header & SPLIT:
            self.pending.clear()
            self.completed += 1
            return payload
        if sequence in self.pending:
            first, _ = self.pending.pop(sequence)
            self.completed += 1
            return first + payload
        if self.pending:
            self.pending.clear()
            self.rejected += 1
        # All valid first halves are full 255-byte radio frames. A short
        # orphan tail cannot become a first half, unlike upstream's permissive RX.
        if len(frame) != self.mtu:
            self.rejected += 1
            return None
        self.pending[sequence] = (payload, now + self.ttl)
        return None
