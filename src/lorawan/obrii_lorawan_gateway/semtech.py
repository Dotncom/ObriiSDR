"""Semtech UDP v2 framing for a future EU868 gateway host.

The host owns UDP endpoints, token matching, scheduling, duty-cycle reservations
and actual TX. UDP acknowledgments do not authenticate a server or prove RF TX.
"""
import base64
from dataclasses import dataclass
import json
import math
import re
import secrets

PUSH_DATA, PUSH_ACK, PULL_DATA, PULL_RESP, PULL_ACK, TX_ACK = range(6)
MAX_DATAGRAM = 16384
ACK_ERRORS = {"NONE", "TOO_LATE", "TOO_EARLY", "COLLISION_PACKET", "COLLISION_BEACON", "TX_FREQ", "TX_POWER", "GPS_UNLOCKED"}


def integer(n, low, high):
    if type(n) is not int or not low <= n <= high:
        raise ValueError("Integer field outside supported range")
    return n


def finite(n, low, high):
    if type(n) not in (int, float) or not math.isfinite(n) or not low <= n <= high:
        raise ValueError("Numeric field outside supported range")
    return n


def frequency(hz):
    integer(hz, 863000000, 870000000)
    if hz % 100:
        raise ValueError("EU868 frequency must be a multiple of 100 Hz")
    if not any(a <= hz <= b for a, b in ((863000000, 868600000), (868700000, 869200000),
                                        (869400000, 869650000), (869700000, 870000000))):
        raise ValueError("Frequency outside supported EU868 subbands")
    return hz


def rate_text(dr):
    integer(dr, 0, 6)
    return f"SF{7 if dr == 6 else 12-dr}BW{250 if dr == 6 else 125}"


def rate_value(text):
    if not isinstance(text, str) or not re.fullmatch(r"SF(?:7|8|9|10|11|12)BW(?:125|250)", text):
        raise ValueError("Unsupported LoRaWAN data rate")
    return next((i for i in range(7) if rate_text(i) == text), None)


def wire_check(wire, dr, uplink):
    integer(dr, 0, 6)
    maximum = (59 if dr < 3 else 123 if dr == 3 else 250) + 5
    if not isinstance(wire, bytes) or not 12 <= len(wire) <= maximum or wire[0] & 31:
        raise ValueError("Invalid LoRaWAN PHYPayload size/version")
    kind = wire[0] >> 5
    if kind == 0 and uplink and len(wire) == 23:
        return
    if kind == 1 and not uplink and len(wire) in (17, 33):
        return
    if kind == 6 and uplink and ((len(wire) == 19 and wire[1] in (0, 2)) or
                                (len(wire) == 24 and wire[1] == 1)):
        return
    if kind not in ((2, 4) if uplink else (3, 5)):
        raise ValueError("Wrong LoRaWAN frame direction")


@dataclass(frozen=True)
class Datagram:
    kind: int
    token: bytes
    gateway: bytes | None = None
    body: dict | None = None


def encode(packet):
    integer(packet.kind, 0, 5)
    if not isinstance(packet.token, bytes) or len(packet.token) != 2:
        raise ValueError("UDP token must be two bytes")
    has_gateway = packet.kind in (PUSH_DATA, PULL_DATA, TX_ACK)
    if has_gateway != (packet.gateway is not None):
        raise ValueError("Incorrect gateway ID presence")
    if has_gateway and (not isinstance(packet.gateway, bytes) or len(packet.gateway) != 8):
        raise ValueError("Gateway ID must be eight bytes")
    wire = bytes([2]) + packet.token + bytes([packet.kind]) + (packet.gateway or b"")
    if packet.kind in (PUSH_ACK, PULL_DATA, PULL_ACK):
        if packet.body is not None:
            raise ValueError("Unexpected body on control packet")
    elif packet.body is None:
        if packet.kind != TX_ACK:
            raise ValueError("JSON body required")
    elif not isinstance(packet.body, dict):
        raise ValueError("UDP body must be an object")
    else:
        wire += json.dumps(packet.body, allow_nan=False, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    if len(wire) > MAX_DATAGRAM:
        raise ValueError("UDP datagram exceeds limit")
    return wire


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON field")
        result[key] = value
    return result


def decode(wire):
    if not isinstance(wire, bytes) or not 4 <= len(wire) <= MAX_DATAGRAM or wire[0] != 2:
        raise ValueError("Invalid Semtech UDP version/size")
    kind = integer(wire[3], 0, 5)
    offset = 12 if kind in (PUSH_DATA, PULL_DATA, TX_ACK) else 4
    if len(wire) < offset:
        raise ValueError("Truncated gateway ID")
    body = None
    if len(wire) > offset:
        try:
            body = json.loads(wire[offset:].decode("utf-8"), object_pairs_hook=_unique,
                parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Non-finite JSON")))
        except (ValueError, RecursionError) as exc:
            raise ValueError("Invalid Semtech UDP JSON") from exc
    packet = Datagram(kind, wire[1:3], wire[4:12] if offset == 12 else None, body)
    try:
        encode(packet) # Enforce body presence/types on both directions.
    except RecursionError as exc:
        raise ValueError("Semtech UDP JSON nesting exceeds limit") from exc
    return packet


@dataclass(frozen=True)
class RxPacket:
    wire: bytes
    timestamp: int # Last RX sample / RX-finished event, uint32 microseconds.
    frequency_hz: int
    data_rate: int
    rssi_dbm: int = -120
    snr_db: float = 0.0
    channel: int = 0
    rf_chain: int = 0


def push_data(gateway, packets, token=None):
    if not isinstance(packets, (list, tuple)) or not 1 <= len(packets) <= 32:
        raise ValueError("PUSH_DATA supports 1..32 RX packets")
    rows = []
    for p in packets:
        wire_check(p.wire, p.data_rate, True)
        rows.append({"tmst": integer(p.timestamp, 0, 0xffffffff), "freq": frequency(p.frequency_hz)/1e6,
                     "chan": integer(p.channel, 0, 255), "rfch": integer(p.rf_chain, 0, 255),
                     "stat": 1, "modu": "LORA", "datr": rate_text(p.data_rate), "codr": "4/5",
                     "rssi": integer(p.rssi_dbm, -200, 50), "lsnr": finite(p.snr_db, -100, 100),
                     "size": len(p.wire), "data": base64.b64encode(p.wire).decode("ascii")})
    return encode(Datagram(PUSH_DATA, secrets.token_bytes(2) if token is None else token, gateway, {"rxpk": rows}))


def pull_data(gateway, token=None):
    return encode(Datagram(PULL_DATA, secrets.token_bytes(2) if token is None else token, gateway))


def tx_ack(gateway, token, error="NONE"):
    if not isinstance(error, str) or error not in ACK_ERRORS:
        raise ValueError("Unsupported TX_ACK status")
    return encode(Datagram(TX_ACK, token, gateway, {"txpk_ack": {"error": error}}))


@dataclass(frozen=True)
class Downlink:
    wire: bytes
    frequency_hz: int
    data_rate: int
    timestamp: int | None
    immediate: bool
    power_dbm: int
    rf_chain: int
    invert_iq: bool
    crc_enabled: bool
    preamble_symbols: int


def parse_downlink(packet):
    if packet.kind != PULL_RESP or not isinstance(packet.body, dict) or not isinstance(packet.body.get("txpk"), dict):
        raise ValueError("Expected PULL_RESP with txpk")
    p = packet.body["txpk"]
    if p.get("modu", "LORA") != "LORA" or p.get("codr", "4/5") != "4/5":
        raise ValueError("Unsupported LoRaWAN modulation/coding rate")
    dr = rate_value(p.get("datr"))
    if dr is None:
        raise ValueError("Unsupported EU868 data rate")
    if "tmms" in p:
        raise ValueError("GPS-scheduled TX requires a separate clock adapter")
    immediate = p.get("imme", False)
    invert = p.get("ipol", True)
    no_crc = p.get("ncrc", True)
    if any(type(v) is not bool for v in (immediate, invert, no_crc)):
        raise ValueError("TX flags must be booleans")
    stamp = None if immediate else integer(p.get("tmst"), 0, 0xffffffff)
    mhz = finite(p.get("freq"), 863, 870)
    hz = round(mhz * 1e6)
    if abs(mhz*1e6-hz) > 0.01:
        raise ValueError("Frequency has sub-Hz precision")
    frequency(hz)
    data = p.get("data")
    if not isinstance(data, str) or len(data) > 340:
        raise ValueError("Invalid base64 TX payload")
    try:
        wire = base64.b64decode(data + "=" * (-len(data) % 4), validate=True)
    except ValueError as exc:
        raise ValueError("Invalid base64 TX payload") from exc
    wire_check(wire, dr, False)
    if integer(p.get("size"), 0, 255) != len(wire):
        raise ValueError("TX payload size mismatch")
    return Downlink(wire, hz, dr, stamp, immediate, integer(p.get("powe", 14), -20, 27),
                    integer(p.get("rfch", 0), 0, 255), invert, not no_crc,
                    integer(p.get("prea", 8), 6, 1024))


def timestamp_delta(target, now):
    integer(target, 0, 0xffffffff)
    integer(now, 0, 0xffffffff)
    return ((target-now+0x80000000) & 0xffffffff)-0x80000000
