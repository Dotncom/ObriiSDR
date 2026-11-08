#!/usr/bin/env python3
"""No-radio test for the local Wi-Fi packet bus and Wireshark extcap bridge."""

import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wifi_packet_bus import PacketBus, PCAP_HEADER


EXTCAP = str(Path(__file__).with_name("wifi_extcap.py"))


def wait_until(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("Timed out waiting for extcap data")


def record(payload, seconds):
    return struct.pack("<IIII", seconds, 123456, len(payload), len(payload)) + payload


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tshark", help="Optional bundled TShark for a complete extcap capture")
    parser.add_argument("--decoder", action="store_true", help="Also check GNU Radio PacketSink at the PCAP cap")
    args = parser.parse_args()
    interfaces = subprocess.check_output([sys.executable, "-I", "-B", EXTCAP, "--extcap-interfaces"], text=True)
    dlts = subprocess.check_output([sys.executable, "-I", "-B", EXTCAP, "--extcap-interface", "obrii_wifi", "--extcap-dlts"], text=True)
    assert "interface {value=obrii_wifi}" in interfaces
    assert "dlt {number=127}" in dlts
    with tempfile.TemporaryDirectory(prefix="obrii-wifi-bus-") as directory:
        descriptor = Path(directory) / "bus.json"
        target = Path(directory) / "capture.pcap"
        env = os.environ.copy()
        env["OBRII_WIFI_BUS_DESCRIPTOR"] = str(descriptor)
        bus = PacketBus(descriptor=descriptor)
        bus.start()
        try:
            info = json.loads(descriptor.read_text(encoding="utf-8"))
            with socket.create_connection(("127.0.0.1", info["port"])) as invalid:
                invalid.sendall(b"invalid\n")
                assert not invalid.recv(1)
            older = record(b"\x00" * 12 + b"OLD", 1)
            newer = record(b"\x00" * 12 + b"NEW", 2)
            bus.publish(older)
            process = subprocess.Popen([sys.executable, "-I", "-B", EXTCAP,
                                        "--extcap-interface", "obrii_wifi", "--capture",
                                        "--fifo", str(target)], env=env, stderr=subprocess.PIPE)
            try:
                wait_until(lambda: target.exists() and target.stat().st_size >= len(PCAP_HEADER) + len(older))
                bus.publish(newer)
                expected = PCAP_HEADER + older + newer
                wait_until(lambda: target.stat().st_size >= len(expected))
                assert target.read_bytes() == expected
            finally:
                bus.close()
                assert process.wait(timeout=5) == 0, process.stderr.read().decode(errors="replace")
            assert not descriptor.exists()
        finally:
            bus.close()
        if args.tshark:
            bus = PacketBus(descriptor=descriptor)
            bus.start()
            try:
                capture = Path(directory) / "tshark.pcapng"
                process = subprocess.Popen([args.tshark, "-i", "obrii_wifi", "-c", "2", "-w", str(capture)],
                                           env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    wait_until(lambda: process.poll() is not None or capture.exists(), timeout=10)
                    bus.publish(record(b"\x00" * 12 + b"ONE", 3))
                    bus.publish(record(b"\x00" * 12 + b"TWO", 4))
                    stdout, stderr = process.communicate(timeout=15)
                    assert process.returncode == 0, stderr.decode(errors="replace")
                    lines = subprocess.check_output([args.tshark, "-r", str(capture),
                                                     "-T", "fields", "-e", "frame.number"],
                                                    text=True).splitlines()
                    assert lines == ["1", "2"], (lines, stdout, stderr)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=5)
            finally:
                bus.close()
        if args.decoder:
            os.environ["OBRII_WIFI_BUS_DESCRIPTOR"] = str(descriptor)
            import pmt
            from wifi_phy_decoder import PacketSink
            capped = Path(directory) / "capped.pcap"
            sink = PacketSink(str(capped), 2.412e9, live=True)
            try:
                sink.pcap_packets = 500000
                frame = bytes(24)
                sink.receive(pmt.cons(pmt.make_dict(), pmt.init_u8vector(len(frame), list(frame))))
                assert sink.limited and sink.count == 1 and sink.pcap_packets == 500000
                assert sink.bus and sink.bus.history_size > 0
            finally:
                sink.close()
            assert capped.stat().st_size == len(PCAP_HEADER)
            assert not descriptor.exists()
    print("Wi-Fi packet bus/extcap self-test passed")


if __name__ == "__main__":
    main()
