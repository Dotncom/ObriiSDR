#!/usr/bin/env python3
"""Wireshark extcap bridge for Obrii's local, receive-only Wi-Fi packet bus."""

import argparse
import json
from pathlib import Path
import re
import socket
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wifi_packet_bus import SCHEMA, default_descriptor_path


INTERFACE = "obrii_wifi"


def capture(fifo, capture_filter):
    if capture_filter:
        raise ValueError("Capture filters are not supported; use a Wireshark display filter")
    descriptor = json.loads(default_descriptor_path().read_text(encoding="utf-8"))
    if (descriptor.get("schema") != SCHEMA or descriptor.get("host") != "127.0.0.1"
            or descriptor.get("linktype") != 127
            or not isinstance(descriptor.get("port"), int)
            or not 1 <= descriptor["port"] <= 65535
            or not isinstance(descriptor.get("token"), str)
            or not re.fullmatch(r"[0-9a-f]{64}", descriptor["token"])):
        raise ValueError("Invalid Obrii Wi-Fi packet bus descriptor")
    with socket.create_connection(("127.0.0.1", descriptor["port"]), timeout=3) as connection:
        connection.sendall((descriptor["token"] + "\n").encode("ascii"))
        connection.settimeout(None)
        with open(fifo, "wb", buffering=0) as output:
            while True:
                data = connection.recv(65536)
                if not data:
                    return
                output.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--extcap-interfaces", action="store_true")
    parser.add_argument("--extcap-dlts", action="store_true")
    parser.add_argument("--extcap-config", action="store_true")
    parser.add_argument("--extcap-interface")
    parser.add_argument("--extcap-version")
    parser.add_argument("--extcap-capture-filter", default="")
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--fifo")
    parser.add_argument("--debug", action="store_true")
    args, _ = parser.parse_known_args()
    if args.extcap_interfaces:
        print("extcap {version=1.0}{display=Obrii SDR Wi-Fi}")
        print("interface {value=obrii_wifi}{display=Obrii SDR live Wi-Fi frames}")
    elif args.extcap_dlts:
        print("dlt {number=127}{name=IEEE802_11_RADIO}{display=802.11 with radiotap}")
    elif args.extcap_config:
        return 0
    elif args.capture:
        if args.extcap_interface != INTERFACE or not args.fifo:
            parser.error("--capture requires --extcap-interface obrii_wifi and --fifo")
        try:
            capture(args.fifo, args.extcap_capture_filter)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            print(f"Obrii Wi-Fi extcap: {error}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
