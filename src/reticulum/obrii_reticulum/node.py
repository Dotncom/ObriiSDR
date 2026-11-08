"""One official RNS instance per process, behind the Obrii packet interface."""
from collections import OrderedDict, deque
import base64
import importlib.metadata
import os
from pathlib import Path
import re
import threading
import time
import RNS
from .fragments import bounded_int
from .access import validate_access
from .transport import RadioInterface, validate_framing

RNS_VERSION = "1.5.5"
APP = "obrii"
ASPECTS = ("mesh",)
MAX_EVENTS = 256


def encode(data):
    return base64.b64encode(data).decode("ascii")


def decode(value, limit):
    if not isinstance(value, str) or len(value) > 4 * ((limit + 2) // 3):
        raise ValueError("Invalid base64 size")
    try:
        data = base64.b64decode(value, validate=True)
    except (ValueError, TypeError) as exc:
        raise ValueError("Invalid base64") from exc
    if len(data) > limit:
        raise ValueError("Payload exceeds limit")
    return data


def hash_bytes(value):
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-fA-F]{32}", value) is None:
        raise ValueError("Expected 16-byte destination/link hash in hex")
    return bytes.fromhex(value)


class StateLock:
    """OS lock is released on process exit, including forced termination."""
    def __init__(self, directory):
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.file = open(directory / "instance.lock", "a+b")
        self.file.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(self.file.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            # Lock before reading: Windows denies reads across another process's
            # locked byte. Locking an empty file also covers this first byte.
            if not self.file.read(1):
                self.file.write(b"\0")
                self.file.flush()
        except OSError:
            self.file.close()
            raise RuntimeError("Reticulum state directory is already in use") from None

    def close(self):
        self.file.close()


class Node:
    def __init__(self, state_dir, *, transport=False, interfaces=("lora0",), mtu=255, bitrate=1100,
                 framing="obrn", access=None):
        if importlib.metadata.version("rns") != RNS_VERSION:
            raise RuntimeError(f"This adapter requires rns=={RNS_VERSION}")
        if type(transport) is not bool or not 1 <= len(interfaces) <= 8 or len(set(interfaces)) != len(interfaces):
            raise ValueError("Invalid node configuration")
        if any(not isinstance(n, str) or re.fullmatch(r"[A-Za-z0-9_.-]{1,32}", n) is None for n in interfaces):
            raise ValueError("Interface names must be 1..32 ASCII letters, digits, _, . or -")
        bounded_int(mtu, 64, 255, "MTU")
        validate_framing(framing, mtu)
        bounded_int(bitrate, 5, 10000000, "bitrate")
        access = validate_access(access, interfaces, framing)
        self.state_dir = Path(state_dir).resolve()
        self._state_lock = StateLock(self.state_dir)
        self._lock = threading.RLock()
        self._event_lock = threading.Lock()
        self._events = deque(maxlen=MAX_EVENTS)
        self.dropped_events = 0
        self._peers = OrderedDict()
        self._links = {}
        self._receipts = OrderedDict()
        self._requests = {}
        self._closed = False
        self.interfaces = {}
        self._configure(transport)
        # No standard/shared/LAN interfaces are loaded from the user's RNS config.
        self.rns = RNS.Reticulum(str(self.state_dir), loglevel=RNS.LOG_NONE, logdest=RNS.LOG_FILE)
        identity_path = self.state_dir / "identity"
        if identity_path.exists():
            self.identity = RNS.Identity.from_file(str(identity_path))
            if self.identity is None:
                raise RuntimeError("Stored identity is corrupt; it was not replaced")
        else:
            self.identity = RNS.Identity()
            temporary = self.state_dir / "identity.new"
            if not self.identity.to_file(str(temporary)):
                raise RuntimeError("Could not store RNS identity")
            os.chmod(temporary, 0o600)
            os.replace(temporary, identity_path)
        self.destination = RNS.Destination(self.identity, RNS.Destination.IN,
                                           RNS.Destination.SINGLE, APP, *ASPECTS)
        self.destination.set_proof_strategy(RNS.Destination.PROVE_ALL)
        self.destination.set_packet_callback(self._packet_received)
        self.destination.set_link_established_callback(self._accept_link)
        self.destination.set_max_request_size(4224) # Payload plus RNS msgpack envelope.
        self.destination.register_request_handler("/obrii/echo", self._echo, allow=RNS.Destination.ALLOW_ALL)
        self.aspect_filter = APP + "." + ".".join(ASPECTS)
        self.receive_path_responses = True
        RNS.Transport.register_announce_handler(self)
        for name in interfaces:
            interface = RadioInterface(RNS.Transport, name, self.event, mtu=mtu, bitrate=bitrate, framing=framing)
            self.rns._add_interface(interface, configured_bitrate=bitrate, **access.get(name, {}))
            self.interfaces[name] = interface

    def _configure(self, transport):
        path = self.state_dir / "config"
        marker = "# Managed by Obrii Reticulum adapter v1\n"
        def template(enabled):
            return (marker + "[reticulum]\nshare_instance = No\nenable_transport = "
                    + ("Yes" if enabled else "No")
                    + "\ninterface_discovery = No\n[logging]\nloglevel = -1\n[interfaces]\n")
        content = template(transport)
        previous = path.read_text(encoding="utf-8") if path.exists() else None
        if previous is not None and previous not in (template(False), template(True)):
            raise ValueError("State config is not an adapter-managed config; refusing to overwrite it")
        if previous != content:
            temporary = self.state_dir / "config.new"
            with open(temporary, "w", encoding="utf-8", newline="\n") as file:
                file.write(content)
            os.replace(temporary, path)

    def event(self, kind, **values):
        with self._event_lock:
            if self._closed:
                return
            if len(self._events) == MAX_EVENTS:
                self.dropped_events += 1
            self._events.append({"event": kind, **values})

    def events(self, limit=128):
        bounded_int(limit, 1, MAX_EVENTS, "event batch")
        with self._event_lock:
            return [self._events.popleft() for _ in range(min(limit, len(self._events)))]

    def received_announce(self, destination_hash, announced_identity, app_data):
        with self._lock:
            if destination_hash not in self._peers and len(self._peers) >= 256:
                self._peers.popitem(last=False)
            peer = {"destination": destination_hash.hex(), "identity": announced_identity.hash.hex(),
                    "public_key": encode(announced_identity.get_public_key()),
                    "app_data": encode(app_data or b""), "seen_at": time.time(),
                    "hops": RNS.Transport.hops_to(destination_hash)}
            self._peers[destination_hash] = peer
            self._peers.move_to_end(destination_hash)
        self.event("announce", **peer, signature_valid=True)

    def _packet_received(self, data, packet):
        # SINGLE packet encryption does not identify/authenticate its sender.
        self.event("packet_received", data=encode(data), packet=packet.get_hash().hex(),
                   encrypted=True, sender_authenticated=False)

    def _echo(self, path, data, request_id, link_id, remote_identity, requested_at):
        return data if isinstance(data, bytes) and len(data) <= 4096 else None

    def _remote(self, destination, expected_identity=None):
        target = hash_bytes(destination)
        identity = RNS.Identity.recall(target)
        if identity is None or not RNS.Transport.has_path(target):
            raise ValueError("No verified announce/path; request_path or announce first")
        if expected_identity is not None and identity.hash != hash_bytes(expected_identity):
            raise ValueError("Remote identity does not match expected fingerprint")
        outgoing = RNS.Destination(identity, RNS.Destination.OUT, RNS.Destination.SINGLE, APP, *ASPECTS)
        if outgoing.hash != target:
            raise ValueError("Destination is outside the configured Obrii application namespace")
        return outgoing

    def announce(self, data=b""):
        if len(data) > 128:
            raise ValueError("Announce app data exceeds 128 bytes")
        self.destination.announce(app_data=data)

    def _track_receipt(self, receipt, packet_hash):
        if receipt is None or receipt is False:
            raise RuntimeError("RNS did not accept the outgoing packet")
        identity = packet_hash.hex()
        def conclude(status):
            with self._lock:
                if self._receipts.pop(identity, None) is None:
                    return
            self.event(status, packet=identity)
        with self._lock:
            self._receipts[identity] = receipt
            receipt.set_delivery_callback(lambda r: conclude("delivered"))
            receipt.set_timeout_callback(lambda r: conclude("delivery_failed"))
            # A very fast host can deliver a proof before callbacks are attached.
            if receipt.get_status() == RNS.PacketReceipt.DELIVERED:
                conclude("delivered")
            elif receipt.get_status() in (RNS.PacketReceipt.FAILED, RNS.PacketReceipt.CULLED):
                conclude("delivery_failed")
        return identity

    def send(self, destination, data, expected_identity=None):
        if not 1 <= len(data) <= RNS.Packet.ENCRYPTED_MDU:
            raise ValueError(f"Single-packet payload must be 1..{RNS.Packet.ENCRYPTED_MDU} bytes")
        with self._lock:
            if len(self._receipts) >= 128:
                raise ValueError("Pending receipt limit reached")
            packet = RNS.Packet(self._remote(destination, expected_identity), data)
            return self._track_receipt(packet.send(), packet.get_hash())

    def _accept_link(self, link):
        with self._lock:
            if len(self._links) >= 32:
                link.teardown()
                return
            self._links[link.link_id.hex()] = link
        self._configure_link(link)
        self._link_ready(link)

    def _configure_link(self, link):
        link.set_packet_callback(lambda data, packet: self.event(
            "link_data", link=link.link_id.hex(), data=encode(data), encrypted=True,
            peer_identity=link.get_remote_identity().hash.hex() if link.get_remote_identity() else None,
            sender_authenticated=link.get_remote_identity() is not None))
        link.set_remote_identified_callback(lambda l, identity: self.event(
            "peer_identified", link=l.link_id.hex(), identity=identity.hash.hex()))
        link.set_link_closed_callback(self._link_closed)

    def _link_ready(self, link):
        if link.initiator:
            link.identify(self.identity)
        self.event("link_active", link=link.link_id.hex(), initiator=link.initiator, encrypted=True,
                   mdu=link.get_mdu(),
                   peer_identity=link.get_remote_identity().hash.hex() if link.get_remote_identity() else None)

    def _link_closed(self, link):
        with self._lock:
            self._links.pop(link.link_id.hex(), None)
        self.event("link_closed", link=link.link_id.hex())

    def open_link(self, destination, expected_identity=None):
        remote = self._remote(destination, expected_identity)
        with self._lock:
            if len(self._links) >= 32:
                raise ValueError("Link limit reached")
            link = RNS.Link(remote, established_callback=self._link_ready, closed_callback=self._link_closed)
            self._configure_link(link)
            self._links[link.link_id.hex()] = link
        return link.link_id.hex()

    def link(self, value):
        identity = hash_bytes(value).hex()
        with self._lock:
            link = self._links.get(identity)
        if link is None:
            raise ValueError("Unknown link")
        return link

    def send_link(self, identity, data):
        link = self.link(identity)
        if link.status != RNS.Link.ACTIVE or not 1 <= len(data) <= link.get_mdu():
            raise ValueError("Link not active or payload exceeds link MDU")
        with self._lock:
            if len(self._receipts) >= 128:
                raise ValueError("Pending receipt limit reached")
            packet = RNS.Packet(link, data)
            return self._track_receipt(packet.send(), packet.get_hash())

    def request(self, identity, data):
        link = self.link(identity)
        if link.status != RNS.Link.ACTIVE or len(data) > 4096:
            raise ValueError("Link not active or echo payload too large")
        completed = False
        def conclude(receipt, success):
            nonlocal completed
            with self._lock:
                if completed:
                    return
                completed = True
                self._requests.pop(receipt.request_id.hex(), None)
            response = receipt.response if success else None
            if success and isinstance(response, bytes) and len(response) <= 4096:
                self.event("response", request=receipt.request_id.hex(), link=identity,
                           data=encode(response))
            else:
                self.event("request_failed", request=receipt.request_id.hex(), link=identity)
        with self._lock:
            if len(self._requests) >= 16:
                raise ValueError("Request limit reached")
            receipt = link.request("/obrii/echo", data, response_callback=lambda r: conclude(r, True),
                                   failed_callback=lambda r: conclude(r, False),
                                   timeout=120, max_response_size=4224)
            if receipt is None or receipt is False:
                raise RuntimeError("RNS did not accept request")
            if not completed:
                self._requests[receipt.request_id.hex()] = receipt
                if receipt.get_status() == RNS.RequestReceipt.READY:
                    conclude(receipt, True)
                elif receipt.get_status() == RNS.RequestReceipt.FAILED:
                    conclude(receipt, False)
        return receipt.request_id.hex()

    def status(self):
        with self._lock:
            return {"adapter_version": "0.2.0", "protocol_version": 1, "rns_version": RNS_VERSION,
                    "destination": self.destination.hash.hex(), "identity": self.identity.hash.hex(),
                    "public_key": encode(self.identity.get_public_key()),
                    "transport_enabled": RNS.Reticulum.transport_enabled(),
                    "single_mdu": RNS.Packet.ENCRYPTED_MDU, "rns_mtu": RNS.Reticulum.MTU,
                    "interfaces": [i.snapshot() for i in self.interfaces.values()],
                    "peers": list(self._peers.values()), "dropped_events": self.dropped_events,
                    "links": [{"id": k, "status": v.status, "initiator": v.initiator,
                               "mdu": v.get_mdu() if v.status == RNS.Link.ACTIVE else None}
                              for k, v in self._links.items()],
                    "receipts_pending": len(self._receipts), "requests_pending": len(self._requests)}

    def close(self):
        with self._lock:
            if self._closed:
                return
            self._closed = True
            links = list(self._links.values())
        for link in links:
            link.teardown()
        for interface in self.interfaces.values():
            interface.detach()
        RNS.Reticulum.exit_handler()
        self._state_lock.close()
