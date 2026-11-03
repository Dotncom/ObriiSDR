"""Optional local IFAC credentials; never sent over RPC or put in command arguments."""
import json
from pathlib import Path
from .fragments import bounded_int


def load_access(path):
    if path is None:
        return None
    with Path(path).open("rb") as file:
        contents = file.read(65537)
    if len(contents) > 65536:
        raise ValueError("Interface access file exceeds 65536 bytes")
    return json.loads(contents)


def validate_access(access, interfaces, framing):
    if access is None:
        return {}
    if not isinstance(access, dict) or set(access) - set(interfaces):
        raise ValueError("Access config must map configured interface names to IFAC settings")
    result = {}
    for name, settings in access.items():
        if not isinstance(settings, dict) or set(settings) - {"network_name", "passphrase", "ifac_size_bits"}:
            raise ValueError("Invalid interface access settings")
        for key in ("network_name", "passphrase"):
            value = settings.get(key)
            if value is not None and (not isinstance(value, str) or not value or value == "None"
                                      or len(value.encode("utf-8")) > 1024):
                raise ValueError("IFAC name/passphrase must be nonempty strings of at most 1024 UTF-8 bytes")
        if settings.get("network_name") is None and settings.get("passphrase") is None:
            raise ValueError("IFAC settings need a network name or passphrase")
        bits = bounded_int(settings.get("ifac_size_bits", 64), 8, 64 if framing == "rnode" else 512, "IFAC size in bits")
        if bits % 8:
            raise ValueError("IFAC size must be a whole number of bytes")
        result[name] = {"ifac_size": bits // 8, "ifac_netname": settings.get("network_name"),
                        "ifac_netkey": settings.get("passphrase")}
    return result
