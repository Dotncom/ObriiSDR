import base64
from pathlib import Path
import sys
import tempfile
import unittest
from obrii_reticulum.client import SidecarClient


class Service(unittest.TestCase):
    def test_identity_persistence_role_change_and_state_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            with SidecarClient(sys.executable, directory) as first:
                identity = first.initial_status["identity"]
                destination = first.initial_status["destination"]
                self.assertEqual(len(base64.b64decode(first.initial_status["public_key"])), 64)
                with self.assertRaisesRegex(RuntimeError, "already in use"):
                    SidecarClient(sys.executable, directory)
            with SidecarClient(sys.executable, directory, transport=True, framing="rnode") as second:
                self.assertEqual(second.initial_status["identity"], identity)
                self.assertEqual(second.initial_status["destination"], destination)
                self.assertTrue(second.initial_status["transport_enabled"])
                self.assertEqual(second.initial_status["interfaces"][0]["framing"], "rnode")
            (Path(directory) / "identity").write_bytes(b"broken identity")
            with self.assertRaisesRegex(RuntimeError, "corrupt"):
                SidecarClient(sys.executable, directory)
            self.assertEqual((Path(directory) / "identity").read_bytes(), b"broken identity")

    def test_foreign_config_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config"
            original = "[interfaces]\n[[LAN]]\ntype=AutoInterface\n"
            path.write_text(original, encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "refusing to overwrite"):
                SidecarClient(sys.executable, directory)
            self.assertEqual(path.read_text(encoding="utf-8"), original)

    def test_rpc_validation_and_limits(self):
        with tempfile.TemporaryDirectory() as directory:
            with SidecarClient(sys.executable, directory) as node:
                for op, args in (
                    ("run_shell", {}), ("status", {"unexpected": 1}),
                    ("rx", {"data": "%%%"}), ("poll_tx", {"busy": 1}),
                    ("tx_done", {"ticket": True, "sent": True}),
                    ("online", {"online": "true"}), ("events", {"limit": 257}),
                    ("path", {"destination": "bad"}),
                    ("send", {"destination": "00" * 16, "data": base64.b64encode(bytes(384)).decode()}),
                    ("rx", {"data": base64.b64encode(bytes(256)).decode()}),
                    ("poll_tx", {"interface": "missing"}),
                ):
                    with self.subTest(op=op, args=args):
                        with self.assertRaises(ValueError):
                            node.call(op, **args)
                for _ in range(260):
                    node.call("online", online=False)
                self.assertGreaterEqual(node.call("status")["dropped_events"], 4)
                self.assertEqual(len(node.call("events", limit=256)), 256)
                node.call("online", online=True)
                node.call("announce")
                self.assertIsNotNone(node.call("poll_tx")["tx"])
                node.call("online", online=False)
                self.assertIsNone(node.call("poll_tx")["tx"])
                node.call("online", online=True)
                self.assertEqual(node.call("status")["interfaces"][0]["queued_packets"], 0)

    def test_oversized_stream_stops_child(self):
        with tempfile.TemporaryDirectory() as directory:
            with SidecarClient(sys.executable, directory) as node:
                node.process.stdin.write("x" * 32769 + "\n")
                node.process.stdin.flush()
                self.assertEqual(node.process.wait(timeout=10), 1)

    def test_invalid_startup_and_client_timeout(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "MTU"):
                SidecarClient(sys.executable, directory, mtu=256)
            self.assertFalse((Path(directory) / "identity").exists())
            for timeout in (0, True, float("nan")):
                with self.assertRaises(ValueError):
                    SidecarClient(sys.executable, directory, timeout=timeout)
