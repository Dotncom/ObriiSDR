"""Local, receive-only PCAP fan-out for decoded Wi-Fi frames."""

from collections import deque
import hmac
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import threading


PCAP_HEADER = struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 127)
SCHEMA = "obrii-wifi-packet-bus-v1"


def default_descriptor_path():
    override = os.environ.get("OBRII_WIFI_BUS_DESCRIPTOR")
    if override:
        return Path(override)
    if os.name == "nt":
        local_app_data = os.environ.get("LOCALAPPDATA")
        if not local_app_data:
            local_app_data = str(Path.home() / "AppData" / "Local")
        return Path(local_app_data) / "ObriiSDR" / "wireless-phy" / "packet-bus.json"
    data_home = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))
    return data_home / "ObriiSDR" / "wireless-phy" / "packet-bus.json"


class PacketBus:
    def __init__(self, descriptor=None, history_bytes=2 * 1024 * 1024,
                 client_bytes=4 * 1024 * 1024, max_clients=4):
        self.descriptor = Path(descriptor) if descriptor else default_descriptor_path()
        self.history_bytes = history_bytes
        self.client_bytes = client_bytes
        self.max_clients = max_clients
        self.token = secrets.token_hex(32)
        self.condition = threading.Condition()
        self.history = deque()
        self.history_size = 0
        self.clients = []
        self.listener = None
        self.accept_thread = None
        self.stopped = threading.Event()
        self.slow_clients = 0

    def start(self):
        self.descriptor.parent.mkdir(parents=True, exist_ok=True)
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.listener.bind(("127.0.0.1", 0))
            self.listener.listen(self.max_clients)
            self.listener.settimeout(0.25)
            info = {"schema": SCHEMA, "host": "127.0.0.1",
                    "port": self.listener.getsockname()[1], "token": self.token,
                    "pid": os.getpid(), "linktype": 127,
                    "timestamp_origin": "decoder-wall-clock-not-rf"}
            temporary = self.descriptor.with_name(self.descriptor.name + "." + self.token[:12] + ".tmp")
            fd = os.open(temporary, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as output:
                    json.dump(info, output)
                os.replace(temporary, self.descriptor)
            finally:
                if temporary.exists():
                    temporary.unlink()
            self.accept_thread = threading.Thread(target=self._accept, name="ObriiWifiPacketBus", daemon=True)
            self.accept_thread.start()
        except Exception:
            self.listener.close()
            self.listener = None
            raise

    def publish(self, record):
        if not self.listener or self.stopped.is_set():
            return
        with self.condition:
            self.history.append(record)
            self.history_size += len(record)
            while self.history_size > self.history_bytes and self.history:
                self.history_size -= len(self.history.popleft())
            for client in self.clients:
                if client["closed"]:
                    continue
                if client["size"] + len(record) > self.client_bytes:
                    client["closed"] = True
                    self.slow_clients += 1
                else:
                    client["queue"].append(record)
                    client["size"] += len(record)
            self.condition.notify_all()

    def _accept(self):
        listener = self.listener
        while not self.stopped.is_set():
            try:
                conn, address = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            if address[0] != "127.0.0.1":
                conn.close()
                continue
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        client = None
        try:
            conn.settimeout(2)
            response = bytearray()
            while b"\n" not in response and len(response) < 128:
                part = conn.recv(128 - len(response))
                if not part:
                    return
                response.extend(part)
            if not hmac.compare_digest(response.split(b"\n", 1)[0], self.token.encode("ascii")):
                return
            client = {"socket": conn, "queue": deque(), "size": 0, "closed": False}
            with self.condition:
                if self.stopped.is_set() or len(self.clients) >= self.max_clients:
                    return
                client["queue"].append(PCAP_HEADER)
                client["size"] += len(PCAP_HEADER)
                for record in self.history:
                    client["queue"].append(record)
                    client["size"] += len(record)
                self.clients.append(client)
            while not self.stopped.is_set():
                with self.condition:
                    self.condition.wait_for(lambda: client["queue"] or client["closed"] or self.stopped.is_set())
                    if client["closed"] or self.stopped.is_set():
                        return
                    record = client["queue"].popleft()
                    client["size"] -= len(record)
                conn.sendall(record)
        except (OSError, TimeoutError):
            pass
        finally:
            with self.condition:
                if client in self.clients:
                    self.clients.remove(client)
            conn.close()

    def close(self):
        self.stopped.set()
        if self.listener:
            self.listener.close()
        with self.condition:
            sockets = [client["socket"] for client in self.clients]
            self.condition.notify_all()
        for conn in sockets:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if self.accept_thread:
            self.accept_thread.join(timeout=1)
        self.listener = None
        try:
            if json.loads(self.descriptor.read_text(encoding="utf-8")).get("token") == self.token:
                self.descriptor.unlink()
        except (OSError, ValueError):
            pass
