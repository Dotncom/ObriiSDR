# CLI Digital Decoder Profiles

Digital profiles are opt-in JSON files for `dmr`, `rtty`, `ft8`, `ft4`, and `m17` module starts.
They are separate from receiver presets. No GUI settings, key files, environment
variables, or default profile locations are read automatically.

## Module Start Syntax

Select a profile explicitly when starting the requested live decoder:

```text
module start dmr [BACKEND_ID] [--profile FILE.json]
module start rtty [--profile FILE.json]
module start ft8 [--profile FILE.json]
module start ft4 [--profile FILE.json]
module start m17 [--profile FILE.json]
```

Keep the existing positional `OPTION` mapped to `request.backend` for old DMR
backend IDs, AX.25 ports, and monitor presets. A profile is **never** inferred
from `backend`, even when the value ends in `.json`. Parse `--profile` only for
the five digital start commands, require exactly one following filename, reject
duplicates, missing values and extra tokens, and convert the filename using
`QFileInfo(filename).absoluteFilePath()`. Do not read its contents in the parser.
DMR may have both a backend ID and a profile; its explicit backend ID overrides
`dmrVoiceBackendId` in the profile. Retain the existing quoting/tokenization for
paths with spaces. The request uses only existing fields:

```json
{"action":"module","operation":"start","name":"dmr","backend":"mbelib","path":"C:/private/dmr.json"}
```

Omit `backend` or `path` when not supplied. An explicitly empty or non-string
`path` is invalid. Do not print profile contents, embed them in command requests,
or pass keys in process arguments. The manager independently validates paths,
files, schema, types, ranges and key material before starting a worker.

## Profile Schema

Every profile requires integer `version: 1` and an exact matching `module`.
Unknown fields, wrong types, fractional integer fields, and unsupported settings
are rejected. The maximum file size is 65536 bytes, path length 4096 characters,
and keyring length 256 entries. Files must be readable, regular, nonempty `.json`
files. Hex keys must contain only hex digits, with no prefix or separators;
leading zeros are significant. Unlike GUI normalization, input is never silently
stripped or truncated.

Fixed ARC4 example, using a public synthetic test key, not an operational secret:

```json
{
  "version": 1,
  "module": "dmr",
  "settings": {
    "dmrPrivacyMode": "arc4",
    "dmrPrivacyKeyId": 7,
    "dmrPrivacyKeyHex": "0123456789"
  }
}
```

`dmrPrivacyMode` is `none` (default), `arc4`, `aes256`, `hytera-bp`, or `auto`.
ARC4 requires 10 hex digits; AES256 requires 64; Hytera Basic requires 10, 32, or
64. `dmrPrivacyKeyId` is an integer 0..255. Fixed selection can instead omit
`dmrPrivacyKeyHex` and resolve the exact `(mode, keyId)` entry in `dmrPrivacyKeys`.
A conflicting inline key and selected keyring entry is rejected. Fixed ID 0 has
the existing shared processor's wildcard PI-KID semantics, not a new CLI cipher.

Automatic algorithm/KID selection example:

```json
{
  "version": 1,
  "module": "dmr",
  "settings": {"dmrPrivacyAutoKeySelection": true},
  "dmrPrivacyKeys": [
    {"mode": "arc4", "keyId": 7, "keyHex": "0123456789"},
    {"mode": "aes256", "keyId": 7, "keyHex": "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F"}
  ]
}
```

`mode: auto` also selects automatic privacy. Auto requires a nonempty keyring
of ARC4/AES256 entries and no fixed key or nonzero fixed ID. Keyring entry fields
are exactly `mode`, `keyId`, `keyHex`. Duplicate `(mode, keyId)` pairs are invalid;
the same KID under different algorithms is valid. Hytera Basic is fixed-only.
Selection follows the shared processor's PI algorithm/KID and per-slot alignment;
missing keys are not guessed. Actual decryption uses `DmrPrivacyProcessor` and
`DmrPrivacyCodec`, including their existing alignment, MI and muting behavior.

Optional DMR root fields: `dmrVoiceBackendId` (empty/default or 1..32 ASCII letters,
digits, underscore or hyphen), `dmrVoiceOutputEnabled` (default true), and
`dmrVoicePayloadEnabled` (default true). Voice output requires payload decoding.
Other supported fields in DMR `settings` are:

| Field | Type / Range |
| --- | --- |
| `dmrPrivacyAutoKeySelection` | boolean, default false |
| `dmrLabEnabled` | boolean, default false |
| `dmrLabColorCode` | integer -1..15, default -1 |
| `dmrLabTimeslot` | integer 0..2, default 0 |
| `dmrLabSourceId`, `dmrLabTargetId` | integer 0..16777215, default 0 |
| `dmrManualTimingEnabled` | boolean, default false |
| `dmrManualTimingOffset` | integer -80..80, default 0 |
| `dmrSlicerRatio` | number 0.45..0.80, default 0.625 |
| `dmrAdaptiveSlicer` | boolean, default true |
| `dmrAmbeLayout` | integer 0..4, shared `DmrAmbeLayout`, default 0 |

GUI `dmrPrivacyVariant`, `dmrPrivacyLayout`, and `dmrPrivacyFrameOffset` are
external GopherTrunk controls, not consumed by the native shared decoder. They
are rejected rather than advertised as working. Baseband/channel sample-rate
controls are likewise not exposed by `processIndependentDmrIq` and are rejected.

RTTY's existing `DigitalDecoder` exposes no configurable baud/tone/polarity
setter and `RadioSettings` has no RTTY fields. The following explicit settings
are accepted only at the shared decoder's actual fixed values:

```json
{"version":1,"module":"rtty","rtty":{"baud":45.45,"markHz":2125,"spaceHz":2295,"polarity":"auto"}}
```

The `rtty` object and its fields are optional. Other values are rejected, not
silently ignored. Supporting other baud rates, tones or forced polarity requires
a separately owned change to `DigitalDecoder` and `RadioSettings` first; the CLI
does not implement a second decoder or transform the audio to emulate one.
FT8, FT4 and M17 currently expose no additional profile-level decoder knobs.
Each accepts only the minimal matching profile:

```json
{"version":1,"module":"ft8"}
```

```json
{"version":1,"module":"ft4"}
```

```json
{"version":1,"module":"m17"}
```

For FT4/M17, DMR settings/keys, voice-backend overrides and RTTY configuration
are rejected rather than silently applied. M17's optional Codec2 runtime is
discovered by the shared decoder, not selected by a DMR voice-backend ID.
These profiles apply to `module start m17`, not to the unchanged
`module m17 encode|decode` symbol-file utility. See
[FT4 and M17 Reception](ft4_m17_receive.md).

## Lifecycle And Privacy

The manager sends a framed `Configure` payload before any input samples:
`{"listeningFrequencyHz":435000000,"digitalProfile":{...}}`. The worker validates
it again and applies `setDmrVoiceBackendId`, `setDmrVoiceOutputEnabled`,
`setDmrVoicePayloadEnabled`, `setDmrPrivacyKeys`, and `configure(RadioSettings)`.
Frequency-only Configure frames preserve the active profile. Reset clears stream
state and reapplies the selected settings; retunes and queue-overflow recovery
preserve/replay the selection. Profiles are never parsed or loaded per RX frame.

Stop/start, including restart after worker exit, reuses the last successful
selection **in this manager's memory**, without rereading the file. A different
explicit file replaces the selection. To return to defaults, explicitly load a
minimal profile with just `version` and `module`. No keys are persisted by this
feature; a fresh parent process requires explicit selection again. This is not a
guarantee of locked memory, secure erasure, or protection against process dumps.

Status exposes only `profileSelected`, never paths, settings or keys. Profile
validation errors contain no input values or JSON excerpts. Worker Qt diagnostic
messages are reduced to static severity text because external backend diagnostics
cannot be trusted to redact secrets. Protect profile files with local file
permissions. Decoded traffic/voice remains normal output, not profile diagnostics.
No module is started, key file discovered, or digital processing requested during
ordinary RX unless a module has explicitly been started.

## Parent Source Dependencies And Tests

In **both** the root and standalone `src/cli` build definitions, the parent must:

1. Add `src/utils/dmrprivacyutils.cpp` to `ObriiCli` and add `src/utils` to its
   include directories. `climodulemanager.h` now also needs the existing
   `src/core` include directory for `radiosettings.h`.
2. Add `src/cli/climodulemanager.cpp` and `.h` to `ObriiCliModule`. The shared
   validation implementation is there; the worker must not carry a divergent
   parser. Its existing `src/cli`, `src/core`, `src/utils` includes and
   `dmrprivacyutils.cpp` dependency already satisfy the validation dependencies.
   Crucially, remove the worker's direct `dmrprivacycodec.cpp`/`aes.c` compilation
   and link the existing root `ObriiDmrCrypto` target, which has
   `AES256=1 CBC=0 CTR=0 ECB=1`. For the standalone build, define the same crypto
   library or apply those exact flags consistently to both translation units.
   The unconfigured tiny_aes default is AES128 and fails the AES256 vectors.
   Do not link both direct objects and the library, or mix AES contexts compiled
   with different flags. No cipher is introduced; other DSP, FFTW and platform
   threading/audio dependencies remain unchanged.
3. Add a Qt Core executable `ObriiCliDigitalProfileSelfTest` from
   `tools/cli_digital_profile_selftest.cpp`, `src/cli/climodulemanager.cpp`,
   `src/utils/dmrprivacyutils.cpp`, `src/digital/dmrprivacyprocessor.cpp`,
   `src/digital/dmrprivacycodec.cpp`, and `third_party/tiny_aes/aes.c`, with include
   directories `src/cli`, `src/core`, `src/utils`, `src/digital`,
   `third_party/tiny_aes`. Prefer removing the codec/tiny_aes direct sources and
   linking `ObriiDmrCrypto`; otherwise compile them with the exact AES256 flags
   above, not the library defaults.
   Depend on `ObriiCliModule`, colocate both executables, and invoke the test with
   the absolute path of `tools/fixtures/dmr_motorola_vectors.json`.

The Windows standalone runner avoids editing either build definition:

```powershell
./tools/cli_digital_profile_selftest.ps1
```

It compiles current sources into `build/cli-digital-profile-selftest`, uses the
installed MSVC/Qt5 toolchain, and runs bounded synthetic invalid/valid profile,
known-key ARC4/AES256 decryption, actual worker Configure, RTTY Baudot, redaction,
disabled RX demand, and reset/restart tests. It does not require RF hardware,
operational keys, or a voice backend. It verifies the existing privacy processor
against public synthetic AMBE vectors, not intelligible vocoder audio. Parser
flag behavior needs a parent CLI integration test after the parser is updated.
