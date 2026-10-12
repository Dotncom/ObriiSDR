#!/usr/bin/env python3
"""Check the live table's 802.11 control-frame header handling."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wifi_phy_decoder import live_wifi_row, wifi_frame_type

ra = bytes.fromhex('2eec0263ced1')
ta = bytes.fromhex('789a18f3a987')
for code, name, has_ta in ((0x94, 'Block Ack', True),
                           (0xb4, 'RTS', True),
                           (0xc4, 'CTS', False),
                           (0xd4, 'ACK', False)):
    frame = bytes((code, 0, 0, 0)) + ra + (ta if has_ta else b'')
    row = live_wifi_row(frame, 2462e6, 1, 123.0)
    assert wifi_frame_type(frame) == name
    assert row is not None and len(row) == 17
    assert row[4] == '2e:ec:02:63:ce:d1'
    assert row[3] == ('78:9a:18:f3:a9:87' if has_ta else '')
    assert row[6] == '' and row[14] == '' and row[15] == name
assert live_wifi_row(b'\xc4\x00', 2462e6, 1, 123.0) is None
print('Short and long control frames: live table PASS')
