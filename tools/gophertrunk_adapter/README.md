# Obrii SDR virtual DMR adapter

These Apache-2.0 source files are compiled **inside** a compatible upstream
[GopherTrunk](https://github.com/MattCheramie/GopherTrunk) checkout because they
use its internal DMR framing and pure-Go AMBE+2 vocoder packages. They are not
linked into Obrii SDR. The bundled upstream LICENSE applies to this adapter.

The reviewed upstream module uses Go 1.25 and toolchain 1.25.11. A newer checkout
may change internal APIs; the build scripts run regression tests before building.
The scripts copy this adapter into `cmd/fobos-dmr-virtual`; preserve custom changes
there before running them. They do not clone sources or install Go automatically.

Windows:

```powershell
./tools/build_gopher_adapter.ps1 -SourceRoot C:/path/to/GopherTrunk -GoExecutable C:/path/to/go.exe
```

Linux/Raspberry:

```sh
git clone https://github.com/MattCheramie/GopherTrunk.git /path/to/GopherTrunk
bash tools/build_gopher_adapter.sh /path/to/GopherTrunk
```

The Linux default output is `build/linux-release/gophertrunk/fobos-dmr-virtual`,
next to the normal Linux application. Windows outputs to
`downloads/gophertrunk/bin` for the existing deploy script.

Obrii sends one selected channel as ordered 132-dibit bursts. Managed/manual
bridge operation must use `-interleaved=false`; the adapter assembles A-F and
rejects missing/reordered bursts or RF cadence gaps rather than stitching them.
Privacy keys stay local. Only authorized known-key ARC4 is experimental here;
AES-256 voice decryption is explicitly **not implemented**. Use DSD-neo for AES.
Hytera Basic beta uses Obrii's native mbelib/OpenDMR path or DSD-neo;
this adapter reports it as unsupported.
Verified A-F late-entry MI is committed after voice decoding, for the next
superframe. Without PI, the first encrypted superframe stays muted.
Verbose privacy JSON distinguishes `voiceMi` (used for the completed voice)
from `nextMi` (committed after it). Neither is proof of an intelligible decode.

Tests cover the DMR Golay wire convention, three-bit correction, PI CRC rejection,
32-step MI LFSR, RC4 byte consumption across skipped frames and A-F continuity.
They also compare actual vocoder input with known plaintext to catch an MI
applied one superframe too early, both with PI and on late entry.
Optional `OBRII_GOPHER_FIXTURE=/private/bridge.jsonl` enables a local recording
comparison without requiring or logging the recording's actual key. Captures,
keys and decoded voice must not be committed or included in release packages.
