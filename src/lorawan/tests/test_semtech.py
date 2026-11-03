import base64
import json
import random
import unittest
from obrii_lorawan_gateway.semtech import *


class Semtech(unittest.TestCase):
    def test_headers_and_push(self):
        gateway = bytes.fromhex("0102030405060708")
        wire = bytes.fromhex("00040302010403020105040302050403022d106a990e12")
        # Transport codec only: gateway does not possess the device's MIC keys.
        frame = push_data(gateway, [RxPacket(wire, 0xffffffff, 868100000, 5, -90, 3.5)], b"\x12\x34")
        self.assertEqual(frame[:12], bytes.fromhex("021234000102030405060708"))
        parsed = decode(frame)
        row = parsed.body["rxpk"][0]
        self.assertEqual(base64.b64decode(row["data"]), wire)
        self.assertEqual(row["datr"], "SF7BW125")
        self.assertEqual(pull_data(gateway, b"\x12\x34"), bytes.fromhex("021234020102030405060708"))
        self.assertEqual(encode(Datagram(PULL_ACK, b"\x12\x34")), bytes.fromhex("02123404"))
        self.assertEqual(decode(tx_ack(gateway, b"\x12\x34", "TOO_LATE")).body["txpk_ack"]["error"], "TOO_LATE")

    def down(self, **changes):
        p = {"tmst": 32, "freq": 869.525, "datr": "SF12BW125", "size": 17,
             "data": base64.b64encode(bytes.fromhex("20493eeb51fba2116f810edb3742975142")).decode()}
        p.update(changes)
        return decode(encode(Datagram(PULL_RESP, b"\x01\x02", body={"txpk": p})))

    def test_scheduled_downlink_and_wrap(self):
        d = parse_downlink(self.down())
        self.assertEqual(d.frequency_hz, 869525000)
        self.assertEqual(d.data_rate, 0)
        self.assertTrue(d.invert_iq)
        self.assertFalse(d.crc_enabled)
        self.assertEqual(d.timestamp, 32)
        self.assertEqual(timestamp_delta(32, 0xfffffff0), 48)
        self.assertEqual(timestamp_delta(0xfffffff0, 32), -48)
        self.assertIsNone(parse_downlink(self.down(imme=True)).timestamp)
        unpadded = base64.b64encode(d.wire).decode().rstrip("=")
        self.assertEqual(parse_downlink(self.down(data=unpadded)).wire, d.wire)

    def test_bad_parameters(self):
        for fields in ({"size": 18}, {"data": "!"}, {"tmst": True}, {"tmst": -1},
                       {"freq": 868.65}, {"freq": 869.5250001}, {"datr": "SF12BW250"},
                       {"datr": "SF6BW125"}, {"modu": "FSK"}, {"codr": "4/8"},
                       {"ipol": 1}, {"tmms": 123}, {"powe": 100}, {"prea": 0}):
            with self.subTest(fields=fields):
                with self.assertRaises(ValueError):
                    parse_downlink(self.down(**fields))
        for value in (b"", bytes(17000), b"\x01\x00\x00\x04", b"\x02\x00\x00\x02"):
            with self.assertRaises(ValueError):
                decode(value)
        for suffix in (b'{"x":NaN}', b'{"x":1,"x":2}', b'[]', b'{'):
            with self.assertRaises(ValueError):
                decode(b"\x02\x00\x00\x03" + suffix)

    def test_bounded_noise(self):
        rng = random.Random(94)
        for _ in range(5000):
            try:
                decode(rng.randbytes(rng.randrange(600)))
            except ValueError:
                pass
