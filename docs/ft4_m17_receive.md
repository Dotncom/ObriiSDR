# FT4 and M17 Reception

These receive modes are available in the desktop **Digital Audio** panel. Enable
digital decoding and start the ordinary receiver or IQ playback. Results appear
in the shared decoded-text pane. The CLI also exposes the same production
decoders through the optional `ObriiCliModule` worker, using explicit
`module start ft4` or `module start m17`. Ordinary CLI RX starts neither mode.
The M17 symbol-file utility and FT4 transmitter remain separate and unchanged.
This does not add live reception to Android or the independent remote VFO protocol.

## FT4

Select **FT4** and tune to the band's FT4 USB dial frequency. The default passband
is 3000 Hz; the receiver searches audio carriers from 100 to 3000 Hz. It uses
the pinned MIT ft8_lib candidate search, soft-decision LDPC and CRC validation.
The analysis runs in the digital decoder worker, using bounded overlapping
7.5-second windows, and does not depend on UI frame rate or an exact UTC slot
boundary. First results need a complete window. Repeated candidates within one
transmission are suppressed; the same CQ in a later 7.5-second slot remains
visible. This is live text reception, not an FT4 automatic QSO/TX station.
The existing FT8 station remains specific to FT8. Signal quality and calibrated
SNR/DT reporting are not implemented for this first FT4 receiver.

Reference: [WSJT FT4/FT8 protocol paper](https://wsjt.sourceforge.io/FT4_FT8_QEX.pdf).
Vendored decoder revision and portability notes: `third_party/ft8_lib/VENDORED.md`.

## M17

Select **M17**, center the actual RF channel, and use the default 12.5 kHz
bandwidth. The production FM discriminator feeds a 4800-symbol/s 4FSK receiver
with a matched root-raised-cosine filter, sync-based timing/polarity recovery,
Viterbi FEC, CRC-validated LSF and packet reassembly. Text results show source,
destination, CAN and packet content/hex. The stream decoder can reconstruct LSF
from LICH fragments for late entry. It uses its own libm17 decoder context,
independent of the Open Radio symbol-file laboratory.

Unencrypted, unsigned legacy/v2 Codec2 **3200 voice** and **1600 voice+data**
streams produce 48 kHz mono speaker audio; discriminator noise is not played.
Codec2 is loaded dynamically. Without it, metadata and packet reception still
work, and the decoder status explicitly reports that voice is unavailable.
Encrypted, signed and newer TYPE layouts are metadata-only; no decryption,
signature verification, v3 voice-mode claim or 1600 auxiliary-data UI is included.
Reference: [M17 protocol specifications](https://m17project.org/).

Both modes reset on mode/rate changes; retuning clears reception state. Existing
digital controls do not change the new receivers' protocol parameters.
Live RF sensitivity, oscillator-error tolerance, long-duration acquisition and
real-radio interoperability still need hardware/capture qualification.

## CLI Live Reception

Run `ObriiCli module list` to inspect installed workers. FT4 and live M17 are
listed as `ft4` and `m17`; `m17-symbols` identifies the separate file utility,
whose commands remain `module m17 encode|decode`. `ObriiCliModule` must be
beside the receiver executable. It is launched only on request and shares the
GUI's `DigitalDecoder`, `Ft4AudioDecoder`, and `M17AudioDecoder` implementation.

For FT4, start a local receiver or file replay in USB with a 3000 Hz passband,
then issue these commands from another local terminal:

```bash
ObriiCli module start ft4
ObriiCli module status ft4
ObriiCli module stop ft4
```

The worker consumes 48 kHz mono PCM16LE. Initial output needs a complete
7.5-second window. Decoded messages are JSON `text` events with `module: "ft4"`;
there is no automatic FT4 QSO/TX controller or calibrated SNR/DT report.

For M17, center the actual channel and select a 12.5 kHz channel bandwidth:

```bash
ObriiCli module start m17
ObriiCli module status m17
ObriiCli module stop m17
```

M17 consumes channel IQ, not the receiver's conventional PCM output. The worker
uses the shared independent IQ/FM demodulator before feeding its M17 decoder.
Main analog audio filters do not provide or replace that discriminator path.
Text/status events carry `module: "m17"`. Clear supported voice is emitted as
`voicePcm` and routed through the existing decoded-module callback:
`--module-voice-stdout` writes raw 48 kHz mono PCM16LE and sends JSON to stderr.
`--audio-play` can play decoded voice locally when playback support is present.
It is mutually exclusive with `--audio-stdout`; ordinary `--audio-file` keeps
its existing receiver-audio WAV meaning, not a new decoded-M17 voice meaning.
Missing Codec2 is reported in status while packet reception remains usable.

FT4/M17 may run simultaneously without changing receiver tuning. That tests
independent PCM/channel-IQ demand, not the presence of both real signals in the
same channel. A network client needs suitable USB PCM for FT4 and channel/full
IQ for M17; server audio alone cannot provide the latter's independent IQ path.
Read `module status` for running state, input frames, drops and decoder status.

Explicit optional profiles are minimal and mode-specific:

```json
{"version":1,"module":"ft4"}
```

```json
{"version":1,"module":"m17"}
```

Use `module start ft4 --profile FILE.json` or the matching M17 command. DMR
settings/keys, backend overrides and RTTY settings are rejected for these
profiles. No GUI profile, key file or saved decoder selection is read implicitly.
See [CLI Digital Profiles](cli_digital_profiles.md).

## Codec2 runtime and redistribution

On Windows, run `tools/install_codec2_runtime.ps1` from the workspace. Extraction
requires Python with `zstandard==0.25.0`; a local installation is sufficient:

```powershell
python -m pip install --target build/decode-tools zstandard==0.25.0
./tools/install_codec2_runtime.ps1
```

The script fetches SHA256-pinned official MSYS2 UCRT64 packages:

| Component | Package | License |
| --- | --- | --- |
| Codec2 | codec2 1.2.0-2 | LGPL-2.1-or-later |
| GCC runtime | libgcc 16.2.0-6 | GPL-3.0-or-later WITH GCC-exception-3.1 |
| Threads runtime | libwinpthread 14.0.0.r569.g34eeafb7d-1 | MIT/BSD |

It installs `libcodec2.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`, their
notices and the exact Codec2 source package into `build/runtime`. Full Windows
packaging includes these when installed and rejects incomplete dependencies or
missing notices/source. The Lite package does not include this optional runtime;
its M17 packet reception works and voice reports the missing codec.

Source packages, including build recipes/patches:

- https://mirror.msys2.org/mingw/sources/mingw-w64-codec2-1.2.0-2.src.tar.zst
- https://mirror.msys2.org/mingw/sources/mingw-w64-gcc-16.2.0-6.src.tar.zst
- https://mirror.msys2.org/mingw/sources/mingw-w64-winpthreads-14.0.0.r569.g34eeafb7d-1.src.tar.zst

Codec2 source SHA256: `f7555940c2126fd2de5fb41f71908a4e047b0a376e8119030f1d8a5d786f9a97`.
The LGPL shared library remains replaceable. Retain its source package and all
notices with distributed voice-enabled binaries; see `THIRD_PARTY_LICENSES.txt`.
On Debian/Raspberry Pi OS, install the distribution's `libcodec2-dev` package
(provides the discoverable `libcodec2.so` name); no Windows DLL is used there.
The GUI receiver code builds without a Codec2 development dependency.

## Checks

Build/run `ObriiExtraReceiveModesSelfTest` (or its CTest entry). It covers FT4
PCM and RF-IQ decoding, repeat suppression/consecutive slots/silence; M17
noisy/inverted RRC packets, 96 kHz input, corrupted-packet rejection, RF-IQ
demodulation, Codec2 3200/1600 voice, LICH late entry and encrypted-stream mute.
Voice tests require the optional Codec2 runtime on the executable's library path.
These are deterministic synthetic tests, not a claim of over-the-air validation.

The same self-test can exercise the actual optional CLI worker, reusing its
existing synthetic waveforms instead of maintaining a second fixture generator:

```powershell
$env:PATH = (Resolve-Path ./build/runtime).Path + ';' + $env:PATH
./build/steamdeck-msvc-release/Release/ObriiExtraReceiveModesSelfTest.exe --cli-worker ./build/runtime/ObriiCliModule.exe
```

The test executable is a build artifact, not a distributed runtime binary.
The equivalent registered CTest is `ObriiCliExtraReceiveWorkerSelfTest`; use
the build tree's CTest environment so its runtime DLL dependencies are visible.
The worker checks framed FT4 PCM and M17 channel IQ, reset/restart/configuration,
invalid frame/rate handling, and supported clear voice versus encrypted-stream
mute. `tools/cli_extra_receive_selftest.ps1` separately exercises production
manager start/status/stop/restart with file-only silence/tone input; it does not
prove decoding a real FT4 or M17 transmission. Its inputs contain no protocol
fixtures, require no SDR, and open no GUI. Existing CLI command, runtime-recording,
parity and FFT-isolation PowerShell suites remain separate regressions.

Windows file-only prerelease checks against the rebuilt production CLI:

| Suite | Result |
| --- | --- |
| `cli_extra_receive_selftest.ps1` | PASS, exit 0: idle state, unique live modules, profiles, simultaneous inputs, independent restart and quit with workers active |
| `command_cli_integration_selftest.ps1` | PASS, exit 0 |
| `cli_runtime_integration_selftest.ps1` | PASS, exit 0 |
| `cli_parity_selftest.ps1` | PASS, exit 0 |
| `cli_fft_isolation_selftest.ps1` | PASS, exit 0: identical WAV with/without 1M FFT |

These checks did not open an SDR or the main GUI. This CLI FT4/M17 change has
not been tested on Raspberry Pi; no new Pi/SSH or RF qualification is claimed.
