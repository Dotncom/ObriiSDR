import base64
import json
import os
from pathlib import Path
import queue
import random
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from obrii_reticulum.client import SidecarClient


class IqPipe:
    def __init__(self, executable, framing="obrn"):
        self.process = subprocess.Popen([executable, "--framing", framing], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="ascii",
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        self.replies = queue.Queue(maxsize=2)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            while line := self.process.stdout.readline(512):
                self.replies.put(line, timeout=1)
        finally:
            try:
                self.replies.put(None, timeout=1)
            except queue.Full:
                pass

    def __call__(self, frame):
        self.process.stdin.write(frame.hex() + "\n")
        self.process.stdin.flush()
        line = self.replies.get(timeout=15)
        if line is None:
            raise RuntimeError("IQ pipe exited: " + self.process.stderr.read(1000))
        decoded = bytes.fromhex(line)
        if decoded != frame:
            raise AssertionError("IQ pipe changed fragment bytes")
        return decoded

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)
        self.reader.join(timeout=2)
        self.process.stdout.close()
        self.process.stderr.close()


class Mesh:
    def __init__(self, directory, iq=None, framing="obrn", access_by_node=None):
        self.nodes = {}
        self.events = {name: [] for name in "ABC"}
        self.frames = []
        self.iq = iq
        try:
            for name in "ABC":
                access_file = None
                if access_by_node and name in access_by_node:
                    access_file = Path(directory) / (name + "-access.json")
                    access_file.write_text(json.dumps(access_by_node[name]), encoding="utf-8")
                    os.chmod(access_file, 0o600)
                self.nodes[name] = SidecarClient(sys.executable, Path(directory) / name,
                    transport=name == "B", bitrate=1000000, framing=framing, access_file=access_file)
        except Exception:
            self.close()
            raise

    def tick(self):
        for name, node in self.nodes.items():
            tx = node.call("poll_tx")["tx"]
            if tx:
                wire = base64.b64decode(tx["data"])
                self.frames.append(wire)
                if self.iq:
                    wire = self.iq(wire)
                node.call("tx_done", ticket=tx["ticket"], sent=True)
                for receiver in ({"A": "B", "B": "AC", "C": "B"}[name]):
                    self.nodes[receiver].call("rx", data=base64.b64encode(wire).decode())
            self.events[name].extend(node.call("events"))

    def wait(self, condition, timeout=25):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            self.tick()
            if condition():
                return
            time.sleep(0.01)
        raise AssertionError("Mesh condition timed out: " + repr(
            {k: [e["event"] for e in v[-20:]] for k, v in self.events.items()}))

    def close(self):
        for node in self.nodes.values():
            node.close()


class Integration(unittest.TestCase):
    def test_mesh(self):
        self.exercise_mesh()

    @unittest.skipUnless(os.environ.get("OBRII_RETICULUM_IQ_PIPE"), "Native IQ pipe not configured")
    def test_mesh_through_lora_iq(self):
        iq = IqPipe(os.environ["OBRII_RETICULUM_IQ_PIPE"])
        try:
            self.exercise_mesh(iq)
        finally:
            iq.close()

    def test_rnode_mesh(self):
        self.exercise_mesh(framing="rnode")

    def test_rnode_ifac_mesh(self):
        self.exercise_mesh(framing="rnode", access=True)

    @unittest.skipUnless(os.environ.get("OBRII_RETICULUM_IQ_PIPE"), "Native IQ pipe not configured")
    def test_rnode_mesh_through_lora_iq(self):
        iq = IqPipe(os.environ["OBRII_RETICULUM_IQ_PIPE"], framing="rnode")
        try:
            self.exercise_mesh(iq, framing="rnode")
        finally:
            iq.close()

    @unittest.skipUnless(os.environ.get("OBRII_RETICULUM_IQ_PIPE"), "Native IQ pipe not configured")
    def test_rnode_ifac_through_lora_iq(self):
        iq = IqPipe(os.environ["OBRII_RETICULUM_IQ_PIPE"], framing="rnode")
        try:
            self.exercise_mesh(iq, framing="rnode", access=True)
        finally:
            iq.close()

    def test_wrong_ifac_network_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = {k: {"lora0": {"passphrase": "test-correct" if k != "C" else "test-wrong"}}
                        for k in "ABC"}
            mesh = Mesh(directory, framing="rnode", access_by_node=settings)
            try:
                a, b, c = (mesh.nodes[k] for k in "ABC")
                c.call("announce")
                b.call("announce")
                mesh.wait(lambda: a.call("path", destination=b.initial_status["destination"])["known"])
                for node in (a, b):
                    self.assertFalse(node.call("path", destination=c.initial_status["destination"])["known"])
                self.assertFalse(c.call("path", destination=b.initial_status["destination"])["known"])
                self.assertTrue(mesh.frames)
            finally:
                mesh.close()

    def exercise_mesh(self, iq=None, framing="obrn", access=False):
        with tempfile.TemporaryDirectory() as directory:
            settings = {k: {"lora0": {"network_name": "obrii-test-network", "passphrase": "test-only-secret"}}
                        for k in "ABC"} if access else None
            mesh = Mesh(directory, iq=iq, framing=framing, access_by_node=settings)
            try:
                a, _, c = (mesh.nodes[k] for k in "ABC")
                self.assertEqual(a.initial_status["interfaces"][0]["ifac_enabled"], access)
                self.assertNotIn("test-only-secret", json.dumps(a.initial_status))
                remote = c.initial_status["destination"]
                c.call("announce")
                mesh.wait(lambda: a.call("path", destination=remote)["known"])
                self.assertGreaterEqual(a.call("path", destination=remote)["hops"], 2)
                with self.assertRaisesRegex(ValueError, "fingerprint"):
                    a.call("link_open", destination=remote, expected_identity="00" * 16)
                payload = bytes(i % 251 for i in range(383))
                packet = a.call("send", destination=remote, data=base64.b64encode(payload).decode())["packet"]
                mesh.wait(lambda: any(e["event"] == "packet_received" for e in mesh.events["C"]))
                message = next(e for e in mesh.events["C"] if e["event"] == "packet_received")
                self.assertEqual(base64.b64decode(message["data"]), payload)
                self.assertFalse(message["sender_authenticated"])
                mesh.wait(lambda: any(e["event"] == "delivered" and e["packet"] == packet for e in mesh.events["A"]))
                self.assertFalse(any(e["event"] == "packet_received" for e in mesh.events["B"]))
                link = a.call("link_open", destination=remote, expected_identity=c.initial_status["identity"])["link"]
                mesh.wait(lambda: any(e["event"] == "link_active" and e["link"] == link for e in mesh.events["A"]))
                mesh.wait(lambda: any(e["event"] == "peer_identified" for e in mesh.events["C"]))
                mdu = next(v["mdu"] for v in a.call("status")["links"] if v["id"] == link)
                link_payload = random.Random(14).randbytes(mdu)
                with self.assertRaises(ValueError):
                    a.call("link_send", link=link, data=base64.b64encode(bytes(mdu + 1)).decode())
                a.call("link_send", link=link, data=base64.b64encode(link_payload).decode())
                mesh.wait(lambda: any(e["event"] == "link_data" for e in mesh.events["C"]))
                received = next(e for e in mesh.events["C"] if e["event"] == "link_data")
                self.assertTrue(received["sender_authenticated"])
                self.assertEqual(base64.b64decode(received["data"]), link_payload)
                self.assertEqual(received["peer_identity"], a.initial_status["identity"])
                request_data = random.Random(42).randbytes(4096) # Incompressible, spans multiple RNS Resource parts.
                request = a.call("request_echo", link=link, data=base64.b64encode(request_data).decode())["request"]
                mesh.wait(lambda: any(e["event"] == "response" and e["request"] == request for e in mesh.events["A"]))
                reply = next(e for e in mesh.events["A"] if e["event"] == "response" and e["request"] == request)
                self.assertEqual(base64.b64decode(reply["data"]), request_data)
                self.assertEqual(a.call("status")["requests_pending"], 0)
                self.assertTrue(all(len(f) <= 255 for f in mesh.frames))
                if framing == "obrn":
                    self.assertTrue(any(f[5] >= 3 for f in mesh.frames))
                else:
                    self.assertTrue(any(f[0] & 1 for f in mesh.frames))
                    self.assertEqual(a.call("status")["interfaces"][0]["rns_mtu"], 508)
                mesh.wait(lambda: a.call("status")["receipts_pending"] == 0)
                a.call("online", online=False)
                with self.assertRaisesRegex(ValueError, "did not accept"):
                    a.call("link_send", link=link, data=base64.b64encode(b"offline").decode())
                self.assertEqual(a.call("status")["receipts_pending"], 0)
                a.call("online", online=True)
                marker = base64.b64encode(b"after reconnect").decode()
                resumed = a.call("link_send", link=link, data=marker)["packet"]
                mesh.wait(lambda: any(e["event"] == "link_data" and e["data"] == marker for e in mesh.events["C"]))
                mesh.wait(lambda: any(e["event"] == "delivered" and e["packet"] == resumed for e in mesh.events["A"]))
                a.call("link_close", link=link)
                mesh.wait(lambda: any(e["event"] == "link_closed" for e in mesh.events["C"]))
            finally:
                mesh.close()


if __name__ == "__main__":
    unittest.main()
