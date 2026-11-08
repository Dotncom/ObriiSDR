"""JSON-lines sidecar. stdin/stdout only, no network listener or RF device."""
import argparse
import json
import sys
from .node import Node, decode, encode, hash_bytes
from .access import load_access

MAX_LINE = 32768


def command(node, request):
    if not isinstance(request, dict):
        raise ValueError("Command must be an object")
    op = request.get("op")
    allowed = {
        "status": (), "events": ("limit",), "announce": ("data",),
        "request_path": ("destination",), "path": ("destination",),
        "send": ("destination", "data", "expected_identity"),
        "link_open": ("destination", "expected_identity"), "link_close": ("link",),
        "link_send": ("link", "data"), "request_echo": ("link", "data"),
        "poll_tx": ("interface", "busy"), "tx_done": ("interface", "ticket", "sent"),
        "rx": ("interface", "data"), "online": ("interface", "online"),
        "discontinuity": ("interface",), "shutdown": (),
    }
    if not isinstance(op, str) or op not in allowed:
        raise ValueError("Unknown operation")
    if set(request) - {"id", "op", *allowed[op]}:
        raise ValueError("Unknown command argument")
    if op == "status":
        return node.status()
    if op == "events":
        return node.events(request.get("limit", 128))
    if op == "announce":
        node.announce(decode(request.get("data", ""), 128))
        return {"queued": True}
    if op in ("request_path", "path"):
        import RNS
        identity = hash_bytes(request["destination"])
        if op == "request_path":
            RNS.Transport.request_path(identity)
        return {"known": RNS.Transport.has_path(identity), "hops": RNS.Transport.hops_to(identity)}
    if op == "send":
        return {"packet": node.send(request["destination"], decode(request["data"], 4096), request.get("expected_identity"))}
    if op == "link_open":
        return {"link": node.open_link(request["destination"], request.get("expected_identity"))}
    if op == "link_close":
        node.link(request["link"]).teardown()
        return {"closed": True}
    if op == "link_send":
        return {"packet": node.send_link(request["link"], decode(request["data"], 4096))}
    if op == "request_echo":
        return {"request": node.request(request["link"], decode(request["data"], 4096))}
    if op == "shutdown":
        return {"stopping": True}
    name = request.get("interface", "lora0")
    if not isinstance(name, str) or name not in node.interfaces:
        raise ValueError("Unknown interface")
    interface = node.interfaces[name]
    if op == "poll_tx":
        tx = interface.next_tx(request.get("busy", False))
        return {"tx": {"ticket": tx[0], "data": encode(tx[1])} if tx else None}
    if op == "tx_done":
        interface.complete_tx(request["ticket"], request["sent"])
        return {"completed": True}
    if op == "rx":
        return {"packet_complete": interface.receive(decode(request["data"], 255))}
    if op == "online":
        interface.set_online(request["online"])
        return {"online": interface.online}
    if op == "discontinuity":
        interface.discontinuity()
        return {"reset": True}
    raise ValueError("Unsupported operation")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", required=True)
    parser.add_argument("--transport", action="store_true")
    parser.add_argument("--interface", action="append", default=None)
    parser.add_argument("--mtu", type=int, default=255)
    parser.add_argument("--bitrate", type=int, default=1100)
    parser.add_argument("--framing", choices=("obrn", "rnode"), default="obrn")
    parser.add_argument("--access-file", help="Local JSON file with optional per-interface IFAC credentials")
    args = parser.parse_args()
    node = None
    try:
        node = Node(args.state_dir, transport=args.transport, interfaces=tuple(args.interface or ["lora0"]),
                    mtu=args.mtu, bitrate=args.bitrate, framing=args.framing, access=load_access(args.access_file))
        while True:
            line = sys.stdin.buffer.readline(MAX_LINE + 1)
            if not line:
                break
            if len(line) > MAX_LINE or not line.endswith(b"\n"):
                # Stop on oversized/unterminated input; never interpret its tail as a command.
                raise ValueError("JSON line exceeds 32768 bytes or is unterminated")
            request = None
            identity = None
            try:
                request = json.loads(line, parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Non-finite JSON")))
                identity = request.get("id") if isinstance(request, dict) else None
                if identity is not None and (not isinstance(identity, str) or len(identity) > 64):
                    raise ValueError("Command ID must be a short string")
                result = command(node, request)
                reply = {"id": identity, "ok": True, "result": result}
            except (ValueError, KeyError, TypeError, RuntimeError, OverflowError) as exc:
                reply = {"id": identity if isinstance(identity, str) and len(identity) <= 64 else None,
                         "ok": False, "error": str(exc)[:300]}
            print(json.dumps(reply, ensure_ascii=True, allow_nan=False, separators=(",", ":")), flush=True)
            if reply["ok"] and request["op"] == "shutdown":
                break
    except Exception as exc:
        print(f"Obrii Reticulum startup/stream failure: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    finally:
        if node is not None:
            node.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
