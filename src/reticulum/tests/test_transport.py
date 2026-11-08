from types import SimpleNamespace
import unittest
from unittest.mock import patch
from obrii_reticulum.transport import RadioInterface


def base(interface):
    # Queue tests do not initialise the RNS process-global singleton.
    interface.rxb = interface.txb = interface.tx_drops = interface.tx_dropped_bytes = 0
    interface.detached = False


class Transport(unittest.TestCase):
    def make(self, **kwargs):
        events, inbound = [], []
        owner = SimpleNamespace(inbound=lambda data, interface: inbound.append(data))
        with patch("RNS.Interfaces.Interface.Interface.__init__", base):
            radio = RadioInterface(owner, "test", lambda event, **kw: events.append((event, kw)), **kwargs)
        return radio, events, inbound

    def test_atomic_queue_completion_and_backpressure(self):
        radio, events, _ = self.make(queue_limit=1)
        radio.process_outgoing(bytes(564))
        radio.process_outgoing(b"overflow")
        self.assertEqual(radio.tx_drops, 1)
        self.assertIsNone(radio.next_tx(busy=True))
        first = radio.next_tx()
        self.assertIsNotNone(first)
        self.assertIsNone(radio.next_tx())
        with self.assertRaises(ValueError):
            radio.complete_tx(first[0] + 1, True)
        radio.complete_tx(first[0], True)
        self.assertEqual(radio.snapshot()["queued_packets"], 1)
        second = radio.next_tx()
        radio.complete_tx(second[0], False)
        self.assertIsNone(radio.next_tx())
        self.assertEqual(radio.failures, 1)
        self.assertEqual(radio.fragments_sent, 1)
        self.assertEqual([e[0] for e in events], ["tx_rejected", "tx_failed"])

    def test_receive_offline_and_reset(self):
        radio, _, inbound = self.make()
        payload = bytes(range(251)) * 2
        radio.process_outgoing(payload)
        parts = []
        while tx := radio.next_tx():
            parts.append(tx[1])
            radio.complete_tx(tx[0], True)
        self.assertEqual(radio.packets_sent, 1)
        self.assertFalse(radio.receive(parts[0]))
        radio.discontinuity()
        for p in parts[1:]:
            self.assertFalse(radio.receive(p))
        radio.set_online(False)
        self.assertFalse(radio.receive(parts[0]))
        radio.process_outgoing(b"offline")
        radio.set_online(True)
        for p in reversed(parts):
            radio.receive(p)
        self.assertEqual(inbound, [payload])
        self.assertEqual(radio.rxb, len(payload))
        radio.process_outgoing(b"pending")
        ticket, _ = radio.next_tx()
        radio.set_online(False)
        radio.set_online(True)
        with self.assertRaises(ValueError):
            radio.complete_tx(ticket, True)
        self.assertIsNone(radio.next_tx())
        radio.detach()
        with self.assertRaises(ValueError):
            radio.set_online(True)

    def test_inbound_can_queue_response(self):
        radio, _, _ = self.make()
        radio.owner.inbound = lambda data, interface: interface.process_outgoing(b"reply")
        radio.process_outgoing(b"request")
        ticket, frame = radio.next_tx()
        radio.complete_tx(ticket, True)
        self.assertTrue(radio.receive(frame))
        self.assertIsNotNone(radio.next_tx())

    def test_rnode_mtu_and_consecutive_halves(self):
        radio, _, inbound = self.make(framing="rnode")
        self.assertEqual(radio.HW_MTU, 508)
        self.assertFalse(radio.FIXED_MTU)
        radio.process_outgoing(bytes(508))
        radio.process_outgoing(b"next")
        radio.process_outgoing(bytes(509))
        halves = []
        for _ in range(2):
            ticket, frame = radio.next_tx()
            halves.append(frame)
            radio.complete_tx(ticket, True)
        self.assertEqual(len(halves[0]), 255)
        self.assertEqual(len(halves[1]), 255)
        self.assertEqual(halves[0][0], halves[1][0])
        for frame in halves:
            radio.receive(frame)
        self.assertEqual(inbound, [bytes(508)])
        ticket, following = radio.next_tx()
        self.assertFalse(following[0] & 1)
        self.assertEqual(following[1:], b"next")
        radio.complete_tx(ticket, True)
        self.assertIsNone(radio.next_tx())
        self.assertEqual(radio.tx_drops, 1)
        for options in ({"framing": "rnode", "mtu": 254}, {"framing": "unknown"}):
            with self.assertRaises(ValueError):
                self.make(**options)
