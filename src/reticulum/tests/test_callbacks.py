from types import SimpleNamespace
import threading
import unittest
import RNS
from obrii_reticulum.node import Node


class Callbacks(unittest.TestCase):
    def make(self):
        node = Node.__new__(Node) # Callback bookkeeping only; no RNS global instance.
        node._lock = threading.RLock()
        node._receipts, node._requests = {}, {}
        events = []
        node.event = lambda kind, **values: events.append((kind, values))
        return node, events

    def test_fast_proof_and_transmit_rejection(self):
        node, events = self.make()
        receipt = SimpleNamespace(get_status=lambda: RNS.PacketReceipt.DELIVERED)
        receipt.set_delivery_callback = lambda callback: setattr(receipt, "delivered", callback)
        receipt.set_timeout_callback = lambda callback: setattr(receipt, "failed", callback)
        node._track_receipt(receipt, bytes(32))
        receipt.delivered(receipt)
        receipt.failed(receipt)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0][0], "delivered")
        self.assertFalse(node._receipts)
        for rejected in (False, None):
            with self.assertRaises(RuntimeError):
                node._track_receipt(rejected, bytes(32))

    def test_response_before_request_returns(self):
        node, events = self.make()
        receipt = SimpleNamespace(request_id=bytes(16), response=b"ok", get_status=lambda: RNS.RequestReceipt.READY)
        def request(path, data, **kwargs):
            kwargs["response_callback"](receipt)
            return receipt
        node.link = lambda identity: SimpleNamespace(status=RNS.Link.ACTIVE, request=request)
        node.request("00" * 16, b"ok")
        self.assertFalse(node._requests)
        self.assertEqual(events[0][0], "response")
