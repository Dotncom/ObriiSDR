"""Finite localhost REQ/REP IQ check adapted from the USRP lab transport fixture."""

import hashlib
import json
import math
from pathlib import Path
import struct
import sys


def run(directory, zmq):
    counts = (1920, 1920, 3072, 7)
    hashes = []
    with zmq.Context() as context:
        for direction in (0, 1):
            if (directory / "cancel").exists():
                return "CANCELLED", {}
            with context.socket(zmq.REP) as server, context.socket(zmq.REQ) as client:
                for socket in (server, client):
                    socket.setsockopt(zmq.LINGER, 0)
                    socket.setsockopt(zmq.RCVTIMEO, 1500)
                    socket.setsockopt(zmq.SNDTIMEO, 1500)
                    socket.setsockopt(zmq.SNDHWM, 2)
                    socket.setsockopt(zmq.RCVHWM, 2)
                server.setsockopt(zmq.MAXMSGSIZE, 4096)
                client.setsockopt(zmq.MAXMSGSIZE, max(counts) * 8)
                port = server.bind_to_random_port("tcp://127.0.0.1")
                client.connect(f"tcp://127.0.0.1:{port}")
                first = 0
                digest = hashlib.sha256()
                for count in counts:
                    if (directory / "cancel").exists():
                        return "CANCELLED", {}
                    client.send(b"\xff")
                    if server.recv() != b"\xff":
                        raise RuntimeError("Invalid IQ sample request.")
                    payload = b"".join(struct.pack("<ff", math.sin(i * .02 + direction),
                                                    math.cos(i * .02 + direction))
                                       for i in range(first, first + count))
                    server.send(payload)
                    received = client.recv()
                    if received != payload:
                        raise RuntimeError("CF32 samples changed in the ZeroMQ transport.")
                    digest.update(received)
                    first += count
                hashes.append(digest.hexdigest())
        with context.socket(zmq.REQ) as client:
            client.setsockopt(zmq.LINGER, 0)
            client.setsockopt(zmq.SNDTIMEO, 100)
            try:
                client.send(b"\xff")
            except zmq.Again:
                pass
            else:
                raise RuntimeError("ZeroMQ sent a request without a peer.")
    return "PASS", {"summary": {"checks": 3, "samplesPerDirection": sum(counts),
                                 "sha256ByDirection": hashes, "pyzmq": zmq.__version__,
                                 "libzmq": zmq.zmq_version()}}


def main():
    directory = Path(sys.argv[1])
    try:
        import zmq
    except (ImportError, OSError):
        status = "BLOCKED"
        details = {"error": "PyZMQ is required in the selected Python environment."}
    else:
        try:
            status, details = run(directory, zmq)
        except (OSError, RuntimeError, zmq.ZMQError) as error:
            status = "FAIL"
            details = {"error": str(error)}
    if (directory / "cancel").exists():
        status, details = "CANCELLED", {}
    result = {"schema": 1, "action": "zmq-loopback", "status": status, **details}
    temporary = directory / "result.tmp"
    temporary.write_text(json.dumps(result) + "\n", encoding="utf-8")
    temporary.replace(directory / "result.json")
    return {"PASS": 0, "BLOCKED": 2, "CANCELLED": 130, "FAIL": 1}[status]


if __name__ == "__main__":
    raise SystemExit(main())
