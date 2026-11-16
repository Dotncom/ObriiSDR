# CLI Function Coverage

`ObriiCli capabilities` discovers the current build without opening hardware.

| Area | Console surface | Boundary |
| --- | --- | --- |
| RX/audio | Native Fobos, HackRF, RTL, Soapy, UHD, rtl_tcp, recorded and virtual IQ, analog demodulation, filters, WAV/PCM | The CLI selects simplified audio channelization. GUI SAM/DSB reception and some backend-specific GPIO, clock and Opera Cake controls have no CLI equivalent. |
| Network | GUI-compatible server/client, three processing modes, optional browser VFOs, standalone compute service | Trusted-network service; browser VFOs do not own physical tuning |
| Virtual RF | Local/TCP source and sink, standalone packet or sample-mixing hub, per-link frequency and clock mismatch in `ObriiIqHub` | The clock model is local to the virtual mixer. Distributed clock sync and the interactive Mesh Designer are separate. |
| DSP graph | Supported local/external DDC, PFB, FFT/audio branches, source IDs and auxiliary recordings | Not arbitrary MIMO scheduling; descriptive wires stay descriptive |
| Spectrum | Asynchronous 512..50M FFT, Hz/bin, detectors, VBW, hold/average/percentiles, ten markers, power/OBW/ACPR/tone measurements | One in-flight FFT window, explicit temporal skips; advanced scientific postprocessing still shares the control thread |
| Calibration | Explicit correction tables, live noise reference and Y-factor, batch calculations and exports | Relative references are not absolute dBm calibration; float frequency-grid limits remain |
| Recording | Startup captures plus runtime start/stop of CF32, CI8/SigMF, channel IQ, spectrum frames and audio; triggers/branch recording | Runtime control on RX host, five slots and bounded queue; spectrum files support up to 2097152 exact bins |
| Diagnostics | Passive doctor, explicit temporary write and loopback port probes, structured JSON | Does not open hardware or prove backend ABI/RF correctness |
| Digital RX | CW/BPSK31, optional FT8/FT4/RTTY/DMR/M17, known-key DMR profiles, AX.25 KISS RX, SSTV/APT/WEFAX | FT4 uses USB PCM. M17 independently FM-demodulates channel IQ with optional Codec2 voice. The FT8 QSO manager is a GUI tool. |
| TX | Independent file/virtual/HackRF/UHD worker, CW/FT8/FT4/RTTY/AX.25/DMR/media/signal lab, live analog microphone/PTT, manual CW, KISS TX to virtual IQ or UHD | M17 voice TX accepts WAV in the CLI. Live Codec2 M17 microphone/PTT is available in the GUI. |
| GNSS | Recorded and explicit live full-source IQ to PocketSDR, JSON NMEA, bounded queues, counted EOF and cancellation | Navigation accuracy, backend throughput and actual RF reception need hardware tests |
| Research | On-demand IQ/spectrum, density, pulses, constellation/eye, phase/autocorrelation and comb metrics, paged JSON/CSV | Bounded snapshots, not continuous arbitrary research graphs or coherent two-channel RF |
| Multi-SDR | Timed dual UHD RX, per-channel settings, spectrum mosaics and auxiliary recording | Live coherent alignment, combining and replay of aligned Multi-SDR IQ are GUI tools. |
| Visuals | Explicit companion spectrum/waterfall/scope/constellation/eye/frequency windows | Not every GUI research view or editor has a companion |
| Experiments | Versioned receiver presets, graph/VFO/science restoration and offline results | JSON does not embed IQ or all calibration evidence; optional active work stays disarmed |

Specialized FPV/DMR hunters, live Wi-Fi PHY management, terrain/HF map planning,
some specialized visual views and hardware-specific operations still
belong to GUI tools. Analyzer and FPGA Lab remain separate applications, not
hidden GUI dependencies of the headless receiver. Do not describe CLI as a
fully completed one-to-one replacement until those remaining contracts exist
and have been tested on the target hardware.

## Shared Implementation Boundary

GUI and CLI already compile the same AudioProcessor, audio-filter chain,
IqChannelizer, several native backend adapters, digital decoders, TX generators,
GNSS stream, calibration math, spectrum-science math and DSP/network engines.
A correction inside those shared modules reaches both applications after both
are rebuilt. Matching settings and inputs remain necessary: the CLI audio wrapper
currently selects simplified channelization explicitly.

Fobos native-session operations now use `FobosReceiverCore` in primary GUI RX
settings/readout, Multi-SDR configuration and CLI. GUI and CLI CPU FFT paths use
`FftTransformCore`; their PCM WAV, IQ encoding and spectrum-container writers
use `RecordingFormatCore`. These are statically linked, without a new IQ queue
or per-sample virtual calls. See `shared_runtime_core.md` for boundaries and tests.

This is not one common application runtime. Device ownership, worker lifecycle,
GPU/HF policy, UI persistence, research orchestration and command mappings remain
separate adapters. GUI-only settings do not automatically become CLI commands.
Matching settings/input coverage are necessary for numerical comparisons.

Live `module start ft4` and `module start m17` reuse the GUI's production
decoders through the optional `ObriiCliModule` worker. They do not load during
ordinary RX. FT4 emits decoded text; M17 emits packet/status text and eligible
clear voice through the existing decoded-module PCM path. M17's
`module m17 encode|decode` symbol-file utility remains separate and unchanged;
its module-list metadata name is `m17-symbols`. See
[FT4 and M17 Reception](ft4_m17_receive.md) and
[Digital Profiles](cli_digital_profiles.md) for input paths and limits.

## Verification

Synthetic Windows tests cover FFT/DFT and accumulation, calibration math,
known-key DMR profiles, localhost compute failure/isolation, browser VFO
sessions, recorded/live GNSS lifetime and exact byte/EOF accounting, research
metrics/paging, PCM/PTT restart/watchdog, and virtual TX/RX reconnect/buffering. The
asynchronous FFT suite checks numerical parity and nonblocking bounded submission;
file RX produces byte-identical WAV with and without 1M FFT. Runtime-recording
tests cover all five types, final headers/counts, overflow and independent stop;
doctor tests cover passive checks and explicitly authorized probes. The
`tools/cli_parity_selftest.ps1` integration test exercises actual CLI commands
without hardware; `tools/cli_experiment_selftest.ps1` checks restoration.
Source-archive receiver, live-TX and GNSS builds were checked with MSVC, including
file/virtual TX integration from that clean build. Raspberry Pi 5 arm64 builds,
all three shared-core suites and file RX/audio isolation with 1M FFT also passed.
Physical microphone behavior, real PocketSDR navigation, Raspberry live RF load
and RF TX interoperability still need target-system tests. The embedded GUI
Console now dispatches primary controls, metrics and retained RF trace pages
through the same typed command core as CLI hosts. It is not a wrapper around
every CLI module. `commands` advertises the actual frontend surface; the optional
Agent API defaults to read-only and never grants file or RF TX access. See
`command_dispatcher.md` and `command_console.md`.

The 9.0.3 release checks exercised the CLI FT4/M17 workers with generated IQ or
PCM on Windows and Raspberry Pi ARM64. The fixture and manager/lifecycle checks
are described in `ft4_m17_receive.md`.
