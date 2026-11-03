"""RNS custom interface with an explicitly polled, host-owned radio queue."""
from collections import deque
import threading
import time
from RNS.Interfaces.Interface import Interface
from .fragments import Fragmenter, Reassembler, MAX_PACKET, bounded_int
from . import rnode


def validate_framing(framing, mtu):
    if framing not in ("obrn", "rnode"):
        raise ValueError("Framing must be obrn or rnode")
    bounded_int(mtu, 64, 255, "MTU")
    if framing == "rnode" and mtu != 255:
        raise ValueError("RNode framing requires radio MTU 255")


class RadioInterface(Interface):
    DEFAULT_IFAC_SIZE = 8
    HW_MTU = MAX_PACKET
    FIXED_MTU = True

    def __init__(self, owner, name, event, *, mtu=255, bitrate=1100, queue_limit=32, framing="obrn"):
        validate_framing(framing, mtu)
        super().__init__()
        if not isinstance(name, str) or not name or len(name) > 32:
            raise ValueError("Invalid radio interface name")
        self.name = name
        self.owner = owner
        self.framing = framing
        self.HW_MTU = rnode.MAX_PACKET if framing == "rnode" else MAX_PACKET
        # Match the official RNodeInterface: no link-MTU promotion above the
        # normal RNS MTU. 508 includes room for the optional interface access code.
        self.FIXED_MTU = framing != "rnode"
        self.bitrate = bounded_int(bitrate, 5, 10000000, "bitrate")
        self.limit = bounded_int(queue_limit, 1, 256, "radio queue size")
        self.IN = self.OUT = True
        self.online = True
        self.shared_medium = True
        self._event = event
        self._fragmenter = rnode.Fragmenter(mtu) if framing == "rnode" else Fragmenter(mtu)
        self._reassembler = rnode.Reassembler(mtu) if framing == "rnode" else Reassembler(mtu)
        self._queue = deque()
        self._active = None
        self._ticket = 0
        self._lock = threading.RLock()
        self.packets_accepted = self.packets_sent = self.fragments_sent = self.failures = 0

    def __str__(self):
        return f"ObriiRadio[{self.name}]"

    def process_outgoing(self, data):
        packet = bytes(data)
        with self._lock:
            if not self.online or self.detached or len(self._queue) >= self.limit:
                self.tx_drops += 1
                self.tx_dropped_bytes += len(packet)
                self._event("tx_rejected", interface=self.name, reason="offline_or_full")
                return
            try:
                frames = self._fragmenter.split(packet)
            except (ValueError, OverflowError):
                self.tx_drops += 1
                self.tx_dropped_bytes += len(packet)
                self._event("tx_rejected", interface=self.name, reason="packet_size")
                return
            self._queue.append(deque(frames))
            self.txb += len(packet)
            self.packets_accepted += 1

    def next_tx(self, busy=False):
        if type(busy) is not bool:
            raise ValueError("busy must be boolean")
        with self._lock:
            if busy or not self.online or self._active or not self._queue:
                return None
            self._ticket += 1
            self._active = (self._ticket, self._queue[0][0], time.monotonic())
            return self._active[:2]

    def complete_tx(self, ticket, sent):
        if type(ticket) is not int or type(sent) is not bool:
            raise ValueError("Invalid TX completion")
        with self._lock:
            if self._active is None or ticket != self._active[0]:
                raise ValueError("Stale or unknown TX ticket")
            self._active = None
            if not sent:
                self._queue.popleft()
                self.failures += 1
                self._event("tx_failed", interface=self.name, ticket=ticket)
                return
            self.fragments_sent += 1
            self._queue[0].popleft()
            if not self._queue[0]:
                self._queue.popleft()
                self.packets_sent += 1

    def receive(self, frame):
        with self._lock:
            if not self.online or self.detached:
                return False
            packet = self._reassembler.receive(frame)
            if packet is not None:
                self.rxb += len(packet)
        if packet is not None:
            self.owner.inbound(packet, self)
            return True
        return False

    def set_online(self, online):
        if type(online) is not bool:
            raise ValueError("online must be boolean")
        with self._lock:
            if self.detached and online:
                raise ValueError("Detached interface cannot reconnect")
            self.online = online
            if not online:
                self._queue.clear()
                self._active = None
                self._reassembler.reset()
        self._event("interface_state", interface=self.name, online=online)

    def discontinuity(self):
        with self._lock:
            self._reassembler.reset()

    def detach(self):
        self.set_online(False)
        self.detached = True

    def snapshot(self):
        with self._lock:
            r = self._reassembler
            return {"name": self.name, "online": self.online, "framing": self.framing, "mtu": self._fragmenter.mtu,
                    "ifac_enabled": getattr(self, "ifac_identity", None) is not None,
                    "ifac_size_bits": self.ifac_size * 8 if getattr(self, "ifac_identity", None) is not None else 0,
                    "rns_mtu": self.HW_MTU, "bitrate": self.bitrate, "queued_packets": len(self._queue),
                    "active_ticket": self._active[0] if self._active else None,
                    "active_seconds": time.monotonic() - self._active[2] if self._active else 0,
                    "packets_accepted": self.packets_accepted, "packets_sent": self.packets_sent,
                    "fragments_sent": self.fragments_sent, "tx_failures": self.failures,
                    "tx_drops": self.tx_drops, "rx_bytes": self.rxb, "rx_packets": r.completed,
                    "rx_rejected": r.rejected, "rx_expired": r.expired,
                    "rx_duplicates": r.duplicates, "rx_empty_frames": getattr(r, "empty_frames", 0),
                    "reassembly_pending": len(r.pending)}
