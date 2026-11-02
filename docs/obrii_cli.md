# Obrii CLI (headless receiver and network client)

GUI and CLI share native Fobos operations, CPU FFT primitives and recording
formats. `ObriiCli capabilities` reports their `sharedCores` revisions. See
[Shared GUI and CLI Core](shared_runtime_core.md) for development boundaries,
nonvisual regression tests and structured diagnostics suitable for scripts and
AI-assisted analysis. This does not enable any cloud connection or automatic RF.

`ObriiCli` is a separate console program. It does not start Obrii SDR or open a GUI.
It supports Fobos Standard/Agile, native RTL-SDR, SoapySDR, UHD, rtl_tcp,
virtual IPC/TCP and CF32LE file sources, with CF32LE IQ capture, local control,
AM/NFM/WFM/USB/LSB/CW audio, a TCP virtual-IQ feed,
and the standard Obrii GUI network protocol. Server-side audio, channel IQ
and full IQ are available to CLI or GUI clients. The CLI client can play,
pipe or record PCM and can demodulate channel/full IQ locally.
The headless executable prints requested measurements as JSON; it never opens
a window by itself. A separate `ObriiCliView` companion opens a visual window
only for an explicit `view open` command.
Optional digital, image and spectrum workers launch only on request. The CLI is
not yet a replacement for the GUI's complete TX, GNSS, advanced scanners or
full research suite. It executes supported local and external DSP-board branches;
interactive GUI composition remains in the separate visual applications.

`ObriiCli capabilities` reports the command surface and which optional worker
executables are actually installed. See `docs/cli_parity.md` for the explicit
GUI/CLI boundary, rather than assuming every GUI control has a console twin.

## Additional Headless Services

All services below are opt-in. Ordinary RX does not launch their workers.

```bash
# Wait for a virtual TX or IQ hub; start/stop and reconnect independently.
ObriiCli run --backend virtual --virtual-rx-transport tcp --virtual-rx-port 17661 --freq 1M --rate 48k --spectrum --fft 1024
# Local IPC instead: --virtual-rx-transport local --virtual-name obrii-virtual-iq

# Browser listeners get independent VFOs, not physical receiver ownership.
ObriiCli run --backend rtl --freq 145.5M --rate 2.048M --web-bind 127.0.0.1 --web-port 8080
ObriiCli control web status
ObriiCli control web start 127.0.0.1 8080
ObriiCli control web stop

# Hardware-free compute service; endpoint JSON holds address, port, token.
ObriiCli compute endpoint.json
ObriiCli control graph request remote-route.json

# Explicit digital profile; keys are not printed in status/diagnostics.
ObriiCli module start dmr --profile private-dmr.json
ObriiCli module start dmr dsd-neo --profile private-dmr.json

# Calibration: select actual current receiver/FFT settings, then submit request.
ObriiCli control calibration select
ObriiCli control calibration request cold-reference.json
ObriiCli control calibration status
ObriiCli control calibration cancel
# Offline object or batch of up to 64 calibration request objects:
ObriiCli calibration measurements.json

# Recorded GNSS IQ, using an explicitly installed PocketSDR engine:
ObriiCli module gnss --file gps.cf32 --sample-rate 8000000 --center 1575420000 --systems GPS --backend installed
```

Virtual RX requires exact integer-Hz center/rate matching the publisher and
does not resample or remotely tune that publisher. The sample queue is bounded
by `--virtual-rx-queue-mib` (1..64, default 16). `control status`/`metrics` report
connection, frame, drop and rejection counters. A continuous raw-IQ recording
is stopped with an error after a source discontinuity, rather than silently
joining missing samples. Without recording the RX continues and resets DSP.

FFT accepts 512..50000000 points, including non-powers-of-two, independently
of `--block-samples`. `--hz-per-bin 1` chooses `ceil(rate / 1 Hz)` instead of
`--fft`; the two flags are mutually exclusive. Long FFTs require correspondingly
long observation time and memory (up to roughly 1.4 GB at 50 million bins before
additional scientific traces). FFT planning/execution, peak/noise analysis and
network display reduction run on one low-priority worker, not the IQ/audio read
loop. One complete contiguous IQ window is transferred by ownership, without a
second queued window. Input arriving while FFT is busy or before its update
interval is skipped **only for the spectrum**, never implicitly queued forever.
`control metrics` reports `fftWorker` with compute time, completed/stale windows,
`skippedBusySamples` and `skippedIntervalSamples` (decimal strings). These are
not dropped receiver/IQ-recording samples. A sparse spectrum is not continuous
event coverage. Retunes/reset discard old results; no partial window is padded.
Additional scientific trace accumulation/exports, startup disk writes and blocked
stdout consumers can still delay control; low priority is not a real-time
guarantee. Use a moderate FFT or external branches on weak hosts. Bins remain exact;
only the network display is peak-reduced. `control trace live OFFSET COUNT`
pages up to 65536 bins without resampling; replies report total/returned bins
and truncation. Local CSV export and measurements use the full trace.
Experiment JSON snapshots remain limited to 65536 bins; larger experiments
need spectrum-frame/IQ recording or CSV, not a silently truncated JSON bundle.

Detailed schemas and limits: `docs/cli_digital_profiles.md`,
`docs/cli_calibration.md`, `docs/cli_remote_dsp.md`, `docs/cli_web_service.md`
and `docs/cli_gnss.md`. Web/control ports have no Internet-grade access control;
keep loopback defaults or use a trusted LAN/firewall/VPN. Compute endpoint tokens
belong in private local files, not graph presets or command-line arguments.

## Typed commands and optional local agent API

On a running receiver or client host, `ObriiCli control commands` returns the
shared dispatcher catalog, JSON argument schemas and session `instanceId`.
`control commands tune` selects one schema; `control audit` reads bounded
command outcomes without arguments, secrets or raw diagnostic logs. Existing
`control ...` commands continue to work through a trusted-local compatibility
adapter; they are not all exposed to agents. Frontends may support different
handlers, so use their actual catalog.

`ObriiCli control request request.json` sends a typed JSON envelope from a file
of at most 64 KiB, for example:

```json
{"command":"listen","args":{"frequencyHz":101700000},"id":"test-1"}
```

Add `targetInstanceId` from the catalog to reject a request reaching the wrong
session. `request.json` contains command data, not an executable script. The
original CLI control port remains trusted-local and unauthenticated.

The separate Agent API is disabled until explicitly started:

```sh
ObriiCli control agent start 0
ObriiCli control agent status
ObriiCli control agent control on
ObriiCli control agent control off
ObriiCli control agent stop
```

Start returns its actual loopback port and a newly generated session token;
status does not return the token. Do not save that start reply in shared logs.
The JSON-line API is read-only by default, allows receiver controls only after
the explicit local grant, and permanently denies file/TX access. No model is
launched. A GUI Console can connect to it using the Remote selector, port and
token. Binding is `127.0.0.1` only; use a trusted SSH tunnel or the existing SDR
network client for a remote Raspberry. It is not native MCP. Detailed wire
format/limits: `docs/command_control_api.md`; command contract:
`docs/command_dispatcher.md`; GUI console: `docs/command_console.md`.

## Presets and reproducible experiments

Save the current **CLI receiver** configuration from another terminal. Paths
in `control` commands are resolved on the host running `ObriiCli` (normally
localhost). Existing JSON files are never overwritten.

```bash
ObriiCli control preset save session.json
ObriiCli preset list ./presets
ObriiCli preset summary session.json
ObriiCli run --preset session.json
```

The versioned preset restores the selected receiver/backend, rate, center and
listening frequency, gains, audio mode/bandwidth, text decoder, primary audio
filters, VFO settings, RX2-RX8 definitions, loaded DSP graph, scanner/trigger
configuration, detector/VBW/average, trace enablement and ten markers. The
launcher accepts ordinary `run` options after `--preset` to override saved
options, for example `--control-port 17676`. It still validates receiver
capabilities. Auxiliary receivers and graph branches remain **stopped**;
scanner and trigger remain **disarmed**. Start the desired work explicitly
with `control multisdr start ID`, `control graph start`, `control scan start`
or `control trigger arm`. Optional decode workers are also not auto-started.

Save an experiment snapshot to include the currently available spectrum,
exact trace bins, IQ-quality/trigger/scanner status and module counters:

```bash
ObriiCli control experiment save experiment.json
ObriiCli experiment summary experiment.json
ObriiCli experiment trace experiment.json live
ObriiCli experiment measure experiment.json live 100M 100.1M
ObriiCli experiment zero experiment.json 0 100
ObriiCli experiment results experiment.json
ObriiCli run --preset experiment.json
```

`trace` and `results` can be large. `measure` computes peak, noise, SNR,
channel power, occupied bandwidth, widths and selected-band ACPR directly
from stored bins, without any GUI or receiver. `inspect` prints the complete
JSON. `zero` reads saved zero-span time points in pages of up to 1000. The
JSON holds at most 10000 zero-span points and reports `truncated` if the
capture was longer; use live `control zero export FILE.csv` for the complete
series. Live `control trace`, `measure`, `tone`, `science`, `zero`, `quality`,
`graph trace` and `multisdr spectrum|mosaic` remain available without opening
visual windows; their work is opt-in.

The JSON does **not** contain IQ samples, calibration evidence or an automatic
recording. For a truly replayable experiment, first make a CF32LE recording
and run it through `--backend file`, then save the experiment. Restoring checks
the file's path, size and modification time before playback; these are a
change detector, not a cryptographic checksum. A live-hardware preset can
reopen the same device but cannot reproduce past RF conditions. Raw recording
output paths and RF TX commands are intentionally not restored.

## Multi-SDR (local IQ sources)

RX1 remains the primary `run` receiver and its existing audio/network path.
RX2 through RX8 are optional independent local receivers. Configure each
explicitly, then start it; merely adding a slot does not open the hardware.
The native auxiliary backends are Fobos Standard/Agile, HackRF, RTL-SDR and
SoapySDR. UHD is available as a primary CLI receiver, but is not yet an
auxiliary source. `devices` reports the detected devices and sample rates.

```bash
ObriiCli control multisdr devices
ObriiCli control multisdr add 2 rtl-sdr 1 145.5M 2.048M 4096
ObriiCli control multisdr start 2
ObriiCli control multisdr status
ObriiCli control multisdr spectrum 2
ObriiCli control multisdr mosaic
ObriiCli control multisdr tune 2 146M
ObriiCli control multisdr stop 2
ObriiCli control multisdr remove 2
```

`spectrum ID` returns that receiver's latest FFT in JSON. `mosaic` returns a
display-only stitched spectrum, its contributing receiver IDs, uncovered bins
and overlap count. It does not stitch IQ coherently or calibrate gain between
receivers. Frames older than two seconds are excluded. The primary FFT is
computed for this mosaic only while an auxiliary receiver is running. RX2..
RX8 stream into the IQ source registry for opt-in DSP graph branches using
`receiverId: "multi-rx-2"` through `"multi-rx-8"` and `channelId: "rx0"`.
Stopping RX1 leaves an active RX2 graph branch running; `graph stop` stops
the graph separately.
Each auxiliary device and branch costs CPU and USB bandwidth; start only the
receivers needed for a session.

## Independent VFOs and audio filters

The primary audio channel is ID 0 (`control listen`, `demod`, `bandwidth`).
Up to four additional VFOs can be enabled explicitly on a local IQ receiver
or a CLI network client in **full-IQ** mode. Each additional VFO runs its own
DDC and demodulator only while a local PCM client is connected. The command
reply gives a loopback TCP port; its stream is raw signed PCM16LE mono at
48 kHz, suitable for `nc`/`aplay` or an application on the same machine.
These ports are deliberately local-only and do not create additional GUI
network channels. Each active VFO costs DSP time on the receiver/client host.

```bash
ObriiCli control vfo add 1 100.1M nfm 12.5k
ObriiCli control vfo list
ObriiCli control vfo tune 1 100.2M
ObriiCli control vfo demod 1 am
ObriiCli control vfo bandwidth 1 10k
ObriiCli control vfo filters 1 filters.json
ObriiCli control vfo remove 1
```

The primary audio chain uses the same filter JSON format as the GUI. It is
empty by default; stages are built and processed only after loading or adding
one. Indices are zero-based. `save` refuses to overwrite an existing file.
`--audio-filters filters.json` loads a chain at startup in `run` or `client`.
On a network client, filters apply to received audio; if the server already
filtered it, the chains are cumulative.

```bash
ObriiCli control filters add low_pass
ObriiCli control filters set 0 frequencyHz 2400
ObriiCli control filters enable 0 off
ObriiCli control filters move 0 down
ObriiCli control filters list
ObriiCli control filters save filters.json
ObriiCli control filters load filters.json
ObriiCli control filters clear
```

Filter types and fields match the GUI audio-filter editor. Extra VFOs can
load a whole chain with `control vfo filters ID FILE`; editing individual
stages of an extra VFO remains file-based. These manual `vfo` commands are
separate from the opt-in DSP graph mode below.

## DSP graph (local receiver)

`control graph load FILE.json` accepts the same `blocks` and `connections`
JSON shape as the GUI DSP board. `start` runs enabled local `main/rx0` DDC
branches with their configured decimation, FIR, optional PFB bank, branch FFT,
demodulator and audio filters. The CLI publishes IQ to the graph only while
it is started; ordinary receiver operation retains its original hot path.
`status` returns branch health, processing time, gaps and any PCM port.
`trace FFT_ID` returns the latest branch FFT bins on demand. Each enabled
branch audio output has its own loopback-only raw PCM16LE mono 48 kHz port.
`blocks` lists the loaded blocks and connections. `set` changes one supported
numeric or boolean setting transactionally; invalid spans and values are
rejected without replacing the running graph. Frequency fields accept `k`,
`M` and `G` suffixes. Changing an enabled state requires stopping the graph.
`record start` writes one DDC branch as CF32LE IQ with JSON metadata into an
existing directory; `record stop` finalizes it. Recording errors appear in
`graph status`. `graph save FILE.json` writes the edited graph to a new file;
the source file is never overwritten.

```bash
ObriiCli control graph load docs/cli_dsp_graph_example.json
ObriiCli control graph blocks
ObriiCli control graph start
ObriiCli control graph set ddc1 frequencyHz 100.01M
ObriiCli control graph set fft1 fftLength 4096
ObriiCli control graph status
ObriiCli control graph trace fft1
ObriiCli control graph record start ddc1 ./captures
ObriiCli control graph record status
ObriiCli control graph record stop ddc1
ObriiCli control graph save tuned_graph.json
ObriiCli control graph stop
```

The graph can be armed before receiver `start`. FFT/PCM output begins when IQ
arrives. For an auxiliary source, configure RX2 first and load
`docs/cli_multisdr_graph_example.json`; its DDC/FFT/audio branch then receives
RX2 IQ even if the primary RX is stopped. Explicitly configured external
processing and auxiliary IQ recording use the shared engine;
`vfo` commands and GUI network clients remain separate.
GUI workspace views and purely descriptive wires do not become processing
operations in the CLI. DSP graph audio is sent to loopback PCM, not mixed into
the primary `--audio-stdout` stream. This mode is explicit and may use
considerably more CPU and memory on Raspberry than ordinary listening.

## Frequency scanner

The opt-in scanner retunes a **local live RF receiver** through the center
frequencies in `docs/cli_scan_example.json`. It requires at least 250 ms dwell
per center, two qualifying FFT frames, an absolute dBFS threshold and a
minimum level above the median noise floor. On a hit it holds that center,
tunes primary audio to the detected peak when possible, and emits one
`scanHit` JSON line with UTC, center, signal frequency and level. `resume`
continues to the next center; a nonzero `holdMs` resumes automatically.

```bash
ObriiCli control scan load docs/cli_scan_example.json
ObriiCli control scan start
ObriiCli control scan status
ObriiCli control scan resume
ObriiCli control scan stop
```

Scanning is disabled during IQ/spectrum/audio-file recording, file replay
and while an extra VFO has a PCM listener. A failed hardware retune stops
scanning and emits `scanError`. This is an energy/peak scanner, not a protocol
classifier or an exact timestamped trigger. The FFT runs only while scanning
or another requested spectrum feature is active.

## Threshold IQ trigger (beta)

The optional local-RF trigger in `docs/cli_iq_trigger_example.json` watches
the highest FFT bin inside a selected band. `edge` is `rising` or `falling`;
`hysteresisDb` prevents a noisy threshold from toggling on every frame.
It keeps at most 64 MiB of IQ for the requested pre/post windows, then writes
one new CF32LE file and JSON sidecar off the RX thread. `autoRearm` may arm
again after the writer finishes; the default is single-shot. A trigger uses
FFT and IQ copies **only while armed**. It cannot be armed during scanning or
another IQ/spectrum recording. A manual retune or receiver stop disarms it.

```bash
ObriiCli control trigger load docs/cli_iq_trigger_example.json
ObriiCli control trigger arm
ObriiCli control trigger status
ObriiCli control trigger disarm
```

The receiver console emits `triggerFired` and `triggerSaved` (or
`triggerError`). `outputPrefix` is relative to the preset file's directory;
captured files are numbered and never overwritten. `approximateTriggerUtc`
uses the host clock at FFT evaluation, **not** an SDR hardware timestamp.
The capture boundary is block/FFT-granular, so it is not yet suitable for
sample-accurate timing claims. Spectrum-mask, baseline-difference and
zero-span triggers remain GUI-only.

## On-demand IQ quality

`control quality capture [BLOCKS]` measures the next 1..100 IQ blocks and
emits one `iqQuality` JSON event. `status` returns the last result. It reports
I/Q DC, RMS imbalance, correlation, samples near full-scale and observed
wall-clock sample throughput. Nothing is accumulated when not requested.
For a network client this requires `control mode full`; server-side audio
and channel IQ cannot reveal the full receiver input quality.

```bash
ObriiCli control quality capture 20
ObriiCli control quality status
ObriiCli control quality cancel
```

These are software diagnostics of normalized IQ, not calibrated ADC-bit
measurements or reliable USB-discontinuity counters. A short capture's wall
rate includes host scheduling and should not be read as hardware clock accuracy.

## Zero-span level history

`control zero` records a host-clock time series of the mean **linear bin
power** and the highest bin level in a chosen frequency band. It uses the
existing receiver FFT only while running, normally at about 50 ms intervals;
the actual interval can be longer under load. A bounded point limit stops
capture automatically. Use `read` for paged JSON or `export` for a new CSV
file. Retuning or stopping the receiver ends capture. It currently runs on the
receiver-host CLI, not a remote CLI client.

```bash
ObriiCli control zero start 99.99M 100.01M 10000
ObriiCli control zero status
ObriiCli control zero read 0 100
ObriiCli control zero stop
ObriiCli control zero export zero_span.csv
```

This is FFT-observation time, not a sample-accurate zero-span trace or a
calibrated RF power reading. CSV UTC times use the host clock. Narrow bands
must contain at least one current FFT bin; choose a larger FFT for finer
frequency resolution.

## Export a headless server from Obrii SDR

Configure Fobos RF reception in the GUI, then open **Network Settings** and
choose **Export CLI launch script**. Pick Linux/Raspberry (`.sh`) or Windows
(`.ps1`). The script copies the selected Fobos API/device, RF center,
listening frequency, sample rate, LNA/VGA, audio mode/bandwidth, FFT size,
audio enabled state, bind address and network port. It refuses unsupported
inputs, demodulators and invalid settings instead of silently exporting a
different configuration. FFT length is preserved up to 50 million bins.
Filters, optional DMR modules,
scanners, calibration, browser access and other GUI-only features are **not**
part of the export.

Put the script next to `ObriiCli` or at the root of the shared Obrii SDR source
tree. Close Obrii SDR on the receiver host first so the CLI can open the
Fobos device. On Raspberry, run `bash ./obrii-cli-server.sh`; on Windows, run
`powershell -ExecutionPolicy Bypass -File .\obrii-cli-server.ps1`. For remote GUI access, use a
trusted LAN/VPN bind address such as `0.0.0.0`, then connect the GUI client
to that host and the script's network port (21090 by default). The protocol
has no authentication; do not expose the port to the public Internet.
The script starts RX immediately and remains quiet after its startup line;
add `--progress` to its command if you want one JSON status line per second.
The same quiet default applies to local `run`; add `--spectrum` for periodic
peak/noise summaries, or request `control metrics` from another terminal.
Control the same stream from a second
local terminal with `ObriiCli control metrics`, `control tune 101.7M`,
`control demod wfm`, `control bandwidth 200k`, `control audio off`,
`control stop`, `control start`, and `control quit`. `stop` keeps the server
available for a later restart; `quit` exits the process.
Add `--spectrum` when starting the server to enable on-demand `control trace`:
it prints the full ordered dBFS bin array with its start frequency and bin
width as JSON. `control spectrum` returns only the three strongest peaks and
noise floor. The full array is serialized only when requested.
`control measure` evaluates the latest trace using the same
`SpectrumScienceAnalyzer` as the GUI; `control measure 99.9M 100.1M` uses that
range as the A/B marker pair. The JSON reply includes peak/centroid frequency,
noise-floor and SNR estimates, channel power, 90/95/99% occupied bandwidth,
3/6/20 dB peak widths, and adjacent-channel ratios for a selected range.
These are uncalibrated dBFS-derived estimates, not absolute dBm measurements.
This on-demand call does not enable continuous trace accumulation. The CLI
also has the limited trigger, zero-span and IQ-quality tools documented above;
GUI research functions such as persistence/density, GNSS and propagation are
not yet exposed through CLI commands or their own visual companions.

For an explicitly enabled running trace, use `control traces start`, then
`control trace max`, `control trace min` or `control trace avg`. `control traces
status`, `reset` and `stop` report, clear or disable accumulation. The default
receiver path does not update these traces. `control measure max START_HZ END_HZ`
measures the max-hold trace; `min` and `avg` work the same way. Optional
`control science detector sample|positive|negative|rms|average|median|quasi [FRAMES]`
selects the temporal detector (1..256 frames). `control science vbw HZ` sets
linear-power temporal smoothing (0 disables it); `control science average FRAMES`
sets finite-frame averaging (0 retains exponential averaging); and
`control science percentiles on` enables P50/P90/P99 traces. Configure these
before `traces start` or while it is running. Each additional history increases
CPU and memory use, especially with a large FFT. `trace detected|p50|p90|p99`
returns the corresponding bins; `measure` and `tone` accept the same sources.
`traces stop` disables the work and clears its history.

Ten numbered markers are available through `control science marker set N FREQ`,
`clear N`, `peak N`, `pair N N` and `list` (N=1..10). The pair reports signed
frequency and level deltas. Marker positions are held in physical Hz and the
reported levels are sampled from the latest live trace; they do not retune the
receiver. `science status` reports detector, VBW, averaging, percentile and
marker settings. `control science export SOURCE FILE.csv` saves the currently
available full-resolution trace on the receiver host, with absolute frequency
and dBFS columns. The destination must be a new `.csv` file. For example:

```bash
ObriiCli control science detector rms 16
ObriiCli control science vbw 10
ObriiCli control science percentiles on
ObriiCli control traces start
ObriiCli control trace p90
ObriiCli control science export p90 measurement.csv
ObriiCli control science marker set 1 99.9M
ObriiCli control science marker peak 2
ObriiCli control science marker pair 1 2
```

`control tone`
runs the GUI's single-tone spectrum estimator on the latest frame, optionally
with `control tone [live|max|min|avg|detected|p50|p90|p99] FUNDAMENTAL_HZ HALF_WIDTH_HZ LAST_HARMONIC`
(2..20). It returns SNR, SINAD, THD, THD+N and the number of harmonics actually
inside the span. An incomplete harmonic span is explicitly flagged; THD/THD+N
are null when no harmonics are captured. These are spectrum-derived estimates,
not an audio-analyzer calibration. A strong isolated tone is required for a
meaningful result; a noise-only spectrum may still yield a numeric estimate.

## Digital audio text

CW and BPSK31 use their existing GUI PCM decoders in both `run` and `client`
mode. They are off by default; text is emitted as `digitalText` JSON only
when decoded. On local `run`, CW selects CW demodulation and BPSK31 selects
USB if `--mode` was not explicitly supplied. For a network client, tune the
server's demodulator to the matching signal first.

```bash
ObriiCli run --backend rtl --device 1 --freq 144.100M --rate 2.048M \
  --decode cw --decode-tone 700 --cw-wpm 18 --cw-selectivity 6 --cw-alphabet both
ObriiCli client --host 192.168.89.248 --network-port 21090 \
  --decode psk31 --decode-tone 1000 --psk-squelch 50 --psk-afc on
ObriiCli control digital status
ObriiCli control digital reset
ObriiCli control digital tone 850
ObriiCli control digital cw-squelch 10
ObriiCli control digital psk-squelch 70
ObriiCli control digital afc off
```

CW alphabet can be `latin`, `ukrainian` or `both`. Digital status is reported
only when asked, so decoder diagnostics do not spam the console. The CW tone
gate defaults to 8 dB above neighboring audio frequencies;
`--cw-squelch-db 0` disables that additional gate, while a higher value
suppresses more noise at the cost of weaker signals.
While a decoder is running, `control digital` can also change `wpm` (5..60),
`selectivity` (1..10), `alphabet` (`latin`, `ukrainian`, `both`), `cw-squelch`
(0..30 dB), `psk-squelch` (1..100), `afc` (`on`/`off`) and `tone` (100..10000 Hz).
These commands reset decoder state but do not restart RF reception. The mode
itself is selected at startup with `--decode`. FT8, FT4, RTTY, DMR and M17 use separate
on-demand workers below; other GUI digital modes remain GUI-only.

## On-demand modules

Build or install the `ObriiCli*Module` executables beside `ObriiCli`. None of
the optional decoders starts in ordinary receiver mode; `module start` launches
only the requested process. Run the receiver (or `client`) in one terminal and control
its *local* port from another. The examples use the default port 17675:

```bash
ObriiCli run --backend rtl --freq 145.500M --rate 2.048M --mode usb --serve
ObriiCli module list
ObriiCli module start ft8
ObriiCli module status ft8
ObriiCli module stop ft8
```

FT8 consumes demodulated 48 kHz PCM. Tune the listener to the signal and
select USB for ordinary FT8 radio reception; decoded messages arrive as
single-line `ft8` JSON events on the running receiver's console. The existing
GUI RTTY/Baudot decoder is also available as an opt-in PCM module:
`ObriiCli module start rtty`. It expects 45.45 baud, 170 Hz shift, with
2125/2295 Hz audio tones after USB demodulation; decoded characters arrive
as `text` JSON. False characters are possible on noise, and over-the-air
decoding has not been validated here. DMR consumes
channelized IQ, not the conventional analog audio output. Use the default NFM
channel path and tune the listener to the DMR signal:

```bash
ObriiCli run --backend rtl --freq 438.500M --rate 2.048M --mode nfm --serve \
  --module-voice-stdout 2>dmr-events.jsonl | aplay -q -f S16_LE -r 48000 -c 1
ObriiCli module start dmr
ObriiCli module status dmr
ObriiCli module stop dmr
```

`--module-voice-stdout` outputs decoded DMR voice as raw 48 kHz mono PCM16LE;
JSON events go to stderr. It is mutually exclusive with `--audio-stdout` so
analog and decoded voice cannot mix. `--audio-play` can play decoded voice
locally where Qt Multimedia is available. The same module commands work with
`ObriiCli client` when the remote server sends audio (FT8) or channel/full IQ
(DMR); use `control mode channel` or `control mode full` for the latter.
`module status` reports input frames, drops, voice frames and recent status.
DMR can optionally select an installed voice backend with
`module start dmr BACKEND_ID`. An explicit `--profile FILE.json` supplies
fixed/automatic known-key selection and decoder settings without reading private
GUI settings. See `docs/cli_digital_profiles.md`. Real over-the-air FT8/DMR decoding
still requires testing; do not treat the module as a validated replacement
for the GUI decoder. Missing optional worker executables are reported by
`module list` and do not affect ordinary RX.

FT4 and M17 live reception are also opt-in modes of `ObriiCliModule`, using
the same production decoders as the GUI's Digital Audio panel:

```bash
# First terminal: tune to the desired FT4 USB dial frequency.
ObriiCli run --backend rtl --freq 14.080M --rate 2.048M --mode usb --bandwidth 3000 --serve
# Second terminal, on the same receiver host:
ObriiCli module start ft4
ObriiCli module status ft4
ObriiCli module stop ft4
```

FT4 consumes 48 kHz mono PCM after USB demodulation and needs a complete
7.5-second receive window before initial results. It emits `text` JSON with
`module: "ft4"`; it is not an FT4 automatic QSO station or the FT8 station UI.

```bash
# First terminal: center the actual M17 channel, not an audio carrier.
ObriiCli run --backend rtl --freq 433.500M --rate 2.048M --mode nfm --bandwidth 12500 --serve \
  --module-voice-stdout 2>m17-events.jsonl | aplay -q -f S16_LE -r 48000 -c 1
# Second terminal:
ObriiCli module start m17
ObriiCli module status m17
ObriiCli module stop m17
```

M17 consumes channel IQ and performs independent FM demodulation in its worker;
it does not decode the main analog audio/filter output. Text/status JSON carries
`module: "m17"`. Eligible clear Codec2 voice uses the existing `voicePcm`
callback and `--module-voice-stdout` path: raw 48 kHz mono PCM16LE, with JSON on
stderr. `--audio-play` can play that voice locally when playback is available.
Codec2 is optional; missing Codec2 disables voice, not packet reception.
Encrypted/signed or unsupported voice layouts are not decoded as clear audio.
`--audio-file` retains its normal receiver-audio WAV behavior; it is not a new
M17 decoded-voice recording switch. The two modules can run together, but each
still needs its correct input signal; starting both does not retune the receiver
or make USB FT4 audio equivalent to M17 channel IQ.

Optional profiles are just `{"version":1,"module":"ft4"}` or
`{"version":1,"module":"m17"}`; use `--profile FILE.json` with the corresponding
start command. DMR keys/settings and RTTY controls are rejected for these modes.
The same input requirements apply on a network client: FT4 needs usable USB
PCM, while M17 needs channel/full IQ rather than server audio alone. These paths
have synthetic regression coverage, not live RF/interoperability qualification.
Setup, limitations and checks: [FT4 and M17 Reception](ft4_m17_receive.md).

For live 1200-baud AX.25/APRS, tune an FM signal and launch the RX-only
KISS-TNC. The optional argument is its **local** TCP port (default 8001).
Decoded frames are also emitted as JSON on the receiver's console. KISS
clients can read frames but cannot transmit through this module yet.

```bash
ObriiCli module start ax25 8001
ObriiCli module status ax25
ObriiCli module stop ax25
```

SSTV, APT and WEFAX use separate audio decoders. A snapshot writes the most
recent complete or partial image on the **receiver host**; the path must end
in `.png`, and an existing file is never overwritten. `module snapshot`
queues the operation; the `imageSnapshot` or `imageError` JSON event confirms
its result. The demodulator must supply usable 48 kHz audio to the module.

```bash
ObriiCli module start sstv
ObriiCli module snapshot sstv received.png
ObriiCli module stop sstv
ObriiCli module start apt
ObriiCli module start wefax
```

The optional `monitor` worker observes spectrum peak levels in a chosen RF
band. Copy and edit `docs/cli_spectrum_monitor.json`, then start it with an
absolute or relative preset path. A rising threshold crossing prints one
`spectrumThreshold` JSON event with UTC, frequency and level; hysteresis and
holdoff prevent repeated events. The local receiver computes its diagnostic
FFT only while this monitor, a spectrum view, traces, recording or a network
subscriber needs it. A remote client monitors the **display** spectrum sent
by the server, so this is not a calibrated raw-IQ trigger and does not capture
pre/post-trigger IQ.

```bash
ObriiCli module start monitor docs/cli_spectrum_monitor.json
ObriiCli module status monitor
ObriiCli module stop monitor
```

M17 packet-symbol files can be generated and decoded without starting a
receiver. The output format is the same `.m17sym` signed-symbol stream used by
the GUI Open Radio Lab. This utility verifies packet framing and CRC; it does
not start the live `module start m17` receiver or an RF transmitter. `module
list` names the utility `m17-symbols`; its commands below are unchanged. The encoder refuses to replace
an existing file.

```bash
ObriiCli module m17 encode N0CALL ALL "TEST PACKET" packet.m17sym 0
ObriiCli module m17 decode packet.m17sym
```

TX is a separate `ObriiCliTxModule` process. It reuses the application's
waveform generator, runs on demand and does not require a running `ObriiCli`
control server:

```bash
ObriiCli module tx cw "TEST" --tx-sink file --freq 100M --output cw.cf32
ObriiCli module tx rtty "TEST" --tx-sink file --freq 100M --output rtty.cf32
ObriiCli module tx dmr --pcm-file voice.pcm --dmr-source 1234 --dmr-target 9 \
  --tx-sink file --freq 438.5M --output dmr.cf32
ObriiCli module tx ft8 "CQ K1ABC FN42" --tx-sink virtual --freq 100M
ObriiCli module tx ft4 "CQ K1ABC FN42" --tx-sink file --freq 100M --output ft4.cf32
ObriiCli module tx cw "TEST" --tx-sink virtual --virtual-host 192.168.1.5 \
  --virtual-port 17661 --freq 100M
ObriiCli module tx lab --lab-preset docs/cli_signal_lab_ofdm.json \
  --tx-sink file --freq 100M --output ofdm.cf32
ObriiCli module tx ax25 '!4903.50N/07201.75W-Test' --ax25-source N0CALL-1 \
  --ax25-destination APRS --ax25-path WIDE1-1,WIDE2-1 \
  --tx-sink file --freq 144.8M --output aprs.cf32
ObriiCli module tx audio --wav-file voice.wav --voice-mode nfm \
  --deviation 2.5k --signal-bandwidth 12.5k \
  --tx-sink file --freq 145.5M --output voice.cf32
ObriiCli module tx sstv --image-file photo.png --sstv-mode robot36 \
  --voice-mode nfm --tx-sink file --freq 145.5M --output photo.cf32
# Intentional RF output only, on a frequency you are authorized to use:
ObriiCli module tx cw "TEST" --tx-sink hackrf --rf-confirm \
  --freq 144.5M --tx-device 0 --device-rate 2M --tx-vga-db 0
# UHD: generated IQ and device rates must match in this first version.
ObriiCli module tx cw "TEST" --tx-sink uhd --rf-confirm \
  --freq 144.5M --tx-device 0 --tx-channel 0 --rate 2M --device-rate 2M \
  --tx-gain-db 0
```

The `file` sink creates CF32LE IQ and a JSON sidecar; it refuses to overwrite
existing files. The `virtual` sink sends paced IQ to a local IPC virtual RX
by default or to a TCP virtual RX with `--virtual-host`. FT8 virtual TX waits
for the next 15-second UTC slot unless `--no-ft8-align` is set.
FT4 uses 105 symbols in a 7.5-second UTC slot for virtual TX; `--no-slot-align`
disables slot waiting for FT4 or FT8. This TX path generates IQ; the opt-in
`module start ft4` receiver above is separate and does not enable transmission.
Optional `--rate`, `--tx-tone`, `--tx-level` and `--cw-wpm` set generation parameters.
`audio` reads RIFF/WAVE PCM 8/16/24-bit or float32, converts stereo to mono,
normalizes and resamples to `--rate` (48 ksps by default). `--voice-mode`
selects AM/NFM/WFM/DSB/USB/LSB. `--deviation` sets FM peak deviation;
`--signal-bandwidth` applies the same optional two-sided IQ limiter and audio
low-pass logic as the GUI. FM bandwidth must leave room for the chosen
deviation. This one-shot path accepts at most 60 seconds of WAV and 32 MiB of
input. It does not capture a live microphone or offer interactive PTT yet.
`sstv` shares the GUI's Robot 36/72 and Martin M1/M2 encoder. The image is
auto-oriented, fitted without cropping into a 320-pixel-wide frame and padded
with black borders. `--rate` must be 12..200 ksps (media generation is capped
at 20 million IQ samples); this path accepts at most
180 seconds of generated SSTV. HackRF and UHD RF sinks keep their stricter
60-second per-command limit, so long Martin modes currently need file/virtual
output. UHD additionally needs an explicitly supported `--rate` and matching
`--device-rate`, because it does not resample in CLI. These media commands do not transmit RF unless an RF sink and
`--rf-confirm` are both explicit.
DMR input is raw mono PCM16LE at `--pcm-rate` (48 kHz by default), for 1..60
seconds. The DMR IQ output defaults to 192 ksample/s; `--rate` can override it.
`--dmr-cc`, `--dmr-slot` and `--dmr-private` select call metadata.
DMR voice encoding requires a compatible installed AMBE+2 encoder backend;
without it the command fails explicitly. This CLI path currently produces
unencrypted Tier II direct voice only and has not been over-the-air validated.
The optional `hackrf` sink uses the GUI's native HackRF TX backend. It cannot
start without `--rf-confirm`; the RF amplifier stays off, TX VGA defaults to
0 dB, and each command is limited to 60 seconds. `--device-rate` (2..20 MS/s)
sets the hardware IQ rate, whereas `--rate` (up to 2 MS/s for HackRF) is the
generated baseband rate resampled by the backend. `--tx-rf-bandwidth` is the
optional RF filter setting. Check antenna/load, legal frequency and gain
before intentional RF use. This path has compiled and its safety gate was
tested, but it has not yet had an over-the-air CLI test. Slot-aligned FT8/FT4
through this path still needs timing validation. The optional `uhd` sink uses
the installed UHD C API and is likewise gated by `--rf-confirm`. Set
`--tx-device`, `--tx-channel`, `--tx-gain-db`, optional `--tx-antenna`,
`--tx-fpga-image` and `--tx-rf-bandwidth` as needed. `--rate` and
`--device-rate` must be identical:
there is no implicit UHD resampler yet. UHD checks the actual rate and tuned
frequency before streaming, sends bounded chunks, and ends the burst even
after interruption. Each command is limited to 60 seconds and 32 million IQ
samples. This sink has compiled and its fail-closed options have been tested,
but it needs an actual USRP for RF/timing validation. Do not connect RF output
without an antenna or rated load; respect local transmission rules. TX
commands produce JSON events, not a GUI.
`lab` reuses the GUI signal-lab generator for carrier, noise, PSK/QAM, FSK,
chirp, sweep and OFDM IQ. Its JSON preset sets the waveform parameters;
`--rate` and `--tx-level` may override sample rate and amplitude. The generator
limits output to 20 million samples per invocation. The example preset is
`docs/cli_signal_lab_ofdm.json`; use an appropriate sink as above.
`ax25` creates one AX.25 UI frame, AFSK1200-modulated on an NFM IQ carrier.
It requires a source callsign and accepts an APRS information field plus an
optional digipeater path. File/virtual outputs are suitable for offline tests;
on-air use requires a valid callsign and authorization. This is not yet a
transmitting KISS-TNC.

Alternatively, start the server without a generated script:

```bash
./build/linux-release/ObriiCli run --api standard --freq 101.7M --rate 8M \
  --mode wfm --serve --bind 0.0.0.0 --network-port 21090
```

The console client can inspect and control that server without a GUI:

```bash
./build/linux-release/ObriiCli client --host 192.168.89.248 --network-port 21090 --spectrum
./build/linux-release/ObriiCli --control-port 17675 control metrics
```

The first command stays running; the second uses its local loopback control
port. `client --audio-play` plays on the default Windows audio device, or on
Linux when the CLI was built with Qt Multimedia. `client --audio-stdout`
sends 48 kHz mono PCM16LE to stdout while JSON status goes to stderr; pipe
it to `aplay -q -f S16_LE -r 48000 -c 1` on Linux. `client --audio-file
./remote.wav` records the same audio; these options can be combined. WAV
files are new-only and limited to 4 GiB. Use `control mode server`, `control
mode channel` or `control mode full` against the client's local control port
to change network processing mode. Channel/full IQ are demodulated locally;
in full mode, `client --spectrum` calculates a 4096-bin diagnostic FFT.
`control metrics` reports IQ frames/bytes and local IQ audio errors. Keep
full IQ on a sufficiently fast LAN; channel IQ is preferable on slower links.
Add `client --progress` to print one JSON line per second with IQ/PCM byte
rates and connection state. Without it, the client stays quiet apart from
state changes and requested spectrum reports.
There is currently one global server processing mode, so changing it affects
all connected clients. The protocol has no authentication.

## Visual windows on demand

The default combined build places `ObriiCliView` beside `ObriiCli`. For a
CLI-targeted build that also includes the view companion, use
`BUILD_TARGET=ObriiCli BUILD_CLI_VIEW=1 bash ./tools/build_linux.sh` from the
same source tree. The companion requires Qt Widgets and a desktop session to
open windows; `ObriiCli` itself remains headless.
Each window has its own ID, server address, frequency range and dBFS range:

```bash
./build/linux-release/ObriiCli view open both --view-id overview --host 127.0.0.1 --network-port 21090
./build/linux-release/ObriiCli view open spectrum --view-id narrow --host 192.168.89.248 --network-port 21090 --min-hz 99.8M --max-hz 100.2M --min-db -120 --max-db -20
./build/linux-release/ObriiCli view set --view-id narrow --min-hz 99.9M --max-hz 100.1M --paused
./build/linux-release/ObriiCli view status --view-id narrow
./build/linux-release/ObriiCli view snapshot build/narrow.png --view-id narrow
./build/linux-release/ObriiCli view close --view-id narrow
```

Kinds are `spectrum`, `waterfall`, `both`, `level` (peak-level history) and
`scale` (frequency ruler). `--resume` resumes a paused window. `view set`
can also change `--host` and `--network-port` to assign a different network
stream. Each window receives its own copy of the selected server stream;
per-receiver or per-VFO stream selection inside a single server is not yet
implemented. The current visual companion draws server spectrum frames, or
its own small diagnostic FFT from full-IQ network mode. It is not the GUI's
full scientific spectrum renderer. On a Raspberry without a desktop, leave
the view companion unused and connect from a Windows/Linux desktop instead.

## Recording from a console

Recording is opt-in. The following can run together in one `run` command:

```bash
ObriiCli run --backend rtl --device 1 --freq 100M --rate 2.048M --seconds 10 \
  --output capture.cf32 --audio-file audio.wav \
  --channel-iq channel --full-iq8 full --spectrum-frames spectrum.fbspec \
  --fft 4096 --spectrum-ms 100
```

`--output` writes full-band float32 complex IQ with a JSON sidecar.
`--channel-iq` writes decimated, frequency-shifted stereo PCM16 IQ WAV with
metadata; its actual file is `channel_channel_iq.wav` for the example above.
`--full-iq8` writes full-band signed CI8 (`full_full_iq_s8.iq8`) plus JSON and
SigMF metadata. CI8 is lossy and not a substitute for CF32LE in calibrated
measurements. `--spectrum-frames` writes the GUI's replayable Obrii spectrum
frame format. `--audio-file` writes demodulated 48 kHz mono WAV. The selected
audio mode, listening frequency and bandwidth determine the audio and channel
IQ output. Paths must be distinct and must not already exist. Center/rate
retuning is rejected during IQ or spectrum recording to keep metadata valid.
With `--serve`, `control stop` finalizes the recording and exits the process;
a GUI client cannot pause RX mid-file without a recorded discontinuity marker.
The spectrum interval defaults to 50 ms and is only processed when requested;
channel IQ and CI8 quantization likewise impose no work when disabled. The
current CLI recording surface is continuous capture, not the GUI's
threshold-triggered/pre-trigger event recorder; HDF5 experiment export also
remains GUI-only.

## Runtime Recording

```sh
ObriiCli control record start cf32 ./capture.cf32
ObriiCli control record start audio ./voice.wav
ObriiCli control record status
ObriiCli control record stop audio
ObriiCli control record stop
```

These commands run on the **local RX-host CLI control server**, not a GUI server
or network-client proxy. They never stop RX by themselves. Types are `cf32`,
`ci8`, `channel`, `audio` and `spectrum`; one of each can run together. Paths are
exact new data filenames, unlike startup base-name options. Sidecars are appended
to that path. Start/stop are asynchronous: command acceptance is not proof of a
successful disk open. Wait for status `recording`, then `completed` or `failed`.
Metadata/counts and `recordingCompleted`/`recordingFailed` events remain available.
The default 32-MiB/64-block shared queue fails visibly on overflow; it cannot
silently splice missing exact IQ. Reading/demodulation continues on recording
errors. Source/channel settings stay locked until pending data/metadata finalize.
WAV files have RIFF size limits, and spectrum files permit at most 2097152 exact
bins; larger source FFTs require lower FFT for spectrum capture or full IQ instead.
Worker disk operations do not block RX but a stuck filesystem can delay final
process shutdown. Details: [cli_runtime_recording.md](cli_runtime_recording.md).

## Doctor

```sh
ObriiCli doctor
ObriiCli doctor --doctor-dir ./captures --doctor-write-test
ObriiCli doctor --doctor-port 17675 --doctor-port 21090
ObriiCli doctor checks.json
```

Default diagnosis is passive: platform/Qt, fixed adjacent workers/dependencies,
no SDR/microphone opening, no subprocesses, no socket or file writes. Optional
write testing creates/verifies/removes one unique temporary file in an explicitly
chosen existing directory. Explicit ports use IPv4 loopback bind then immediate
close, never listen/connect; an available port is not reserved for subsequent use.
JSON schema version 1 reports checks, severities and summary; exit 0 means no
failures (warnings are allowed), exit 1 means a reported failure. File presence
does not prove driver loadability. No user profiles or keys are dumped, and paths
are redacted. See [cli_doctor.md](cli_doctor.md) for the request-file contract.

## Raspberry Pi terminal

Use one full, current `ObriiSDR-raspberry-source` tree for the GUI, CLI, shared
cores and on-demand workers. A separate CLI checkout or archive is not required.
From that tree, the default build produces both applications:

```bash
cd ~/ObriiSDR-raspberry-source
JOBS=2 bash ./tools/build_linux.sh
```

The root CMake project uses one build cache and output directory,
`build/linux-release`, containing `ObriiSDR`, `ObriiCli` and the workers.
The build uses the same Fobos libraries for both applications, checking
`third_party/fobos-linux` before system installations such as `/usr/local`.
Set `FOBOS_STANDARD_ROOT` and `FOBOS_AGILE_ROOT` only if your libraries are
elsewhere; a second project or library copy is not needed.

If only the CLI and its worker targets need rebuilding, select them in that
same project and output directory:

```bash
BUILD_TARGET=ObriiCli JOBS=2 bash ./tools/build_linux.sh
```

This still configures the root `CMakeLists.txt`, not a separate CLI project.
Changing to the shared build directory does not delete old source/build
folders or saved settings. Existing GUI and CLI settings remain untouched.
The root install rules put the GUI, CLI, view companion and CLI workers
together in the same `bin` directory. After the default combined build,
an optional user-local install is:

```bash
cmake --install build/linux-release --prefix "$HOME/.local"
```

The reduced `build/ObriiCli-raspberry-source.zip`, created on Windows with
`tools/package_cli_source.ps1`, is only an optional developer diagnostic/test
archive. It is not required for Raspberry use or migration. Only when that
reduced archive has no root `CMakeLists.txt` does `BUILD_TARGET=ObriiCli` fall
back to `src/cli`; the full source tree always uses the shared root project.

Run either application from the shared output directory. For CLI reception:

```bash
cd ~/ObriiSDR-raspberry-source
./build/linux-release/ObriiCli devices
./build/linux-release/ObriiCli rates --device 0
./build/linux-release/ObriiCli run --device 0 --api auto --freq 100M --rate 8M --lna 0 --vga 3
```

These commands work in a local terminal inside Raspberry's VNC desktop.
They also work through SSH. `--api auto` selects Agile when its library finds
the receiver, otherwise Standard. Use `--api agile` or `--api standard` to
choose explicitly. In a second terminal:

```bash
cd ~/ObriiSDR-raspberry-source
./build/linux-release/ObriiCli control status
./build/linux-release/ObriiCli control tune 101.7M
./build/linux-release/ObriiCli control gain 0 4
./build/linux-release/ObriiCli control metrics
./build/linux-release/ObriiCli control stop
```

The control server listens **only on 127.0.0.1** (port 17675 by default).
Use a local terminal or SSH for control; it does not expose an unauthenticated
control port on the LAN. Without `--serve`, `run` prints one JSON status line
per second and stops on Ctrl+C, `control stop`, or `--seconds`; a server stays
quiet after startup unless `--progress` is set. Fobos cannot be opened by
another process at the same time.
An RF retune drops about 100 ms of transition samples and marks a discontinuity
for the virtual receiver. Retuning the hardware center is blocked during a
full-IQ recording so its single-frequency metadata remains truthful.

For a quick RF check without the GUI, add `--spectrum`. Once per second the
progress JSON includes a small Hann-windowed FFT snapshot with the strongest
three local peaks, their frequency/level in dBFS, the median noise floor,
FFT-bin width, and the average IQ read rate. This is a diagnostic snapshot,
not an averaged or calibrated scientific trace. It is fully disabled unless
requested and currently supports RF input, not direct-sampling HF:

```bash
./build/linux-release/ObriiCli run --freq 100M --rate 8M --spectrum --fft 4096
./build/linux-release/ObriiCli control spectrum
```

The second command runs in another terminal and returns the latest snapshot.
Use `--fft` from 512 to 50000000; larger transforms accumulate across input
blocks. `--hz-per-bin` is the mutually exclusive resolution-based alternative.

## Listen or record audio

On Raspberry with `aplay` installed, raw PCM can be heard immediately:

```bash
./build/linux-release/ObriiCli run --freq 101.7M --rate 8M \
  --mode wfm --listen 101.7M --audio-stdout 2>obrii-cli-status.log |
  aplay -q -f S16_LE -r 48000 -c 1
```

`--audio-stdout` writes **only** signed 16-bit little-endian mono PCM at
48 kHz to stdout; JSON status goes to stderr. The same pipe works over SSH,
but local VNC terminal use is simplest. For a WAV recording instead:

```bash
./build/linux-release/ObriiCli run --freq 101.7M --rate 8M --seconds 10 \
  --mode wfm --audio-file ./radio.wav
```

Audio processing is off unless `--audio-file`, `--audio-stdout` or
`--audio-play` is specified. On Linux, direct playback requires a build with
Qt Multimedia; otherwise use the `aplay` pipe above.
The `control listen` command changes the audio frequency within the current
IQ band. `control tune` moves the receiver center; audio follows it unless
an explicit `--listen` or `control listen` selected a fixed station. WFM
defaults to a 200 kHz channel, NFM to 12.5 kHz; use `--bandwidth` to override.
For example, while audio is running: `./build/linux-release/ObriiCli control listen 101.9M`.

Below 50 MHz, `--input auto` selects the direct-sampling HF1 input. Use
`--input hf2` for the second HF input. In this mode `--freq` means the
software listening frequency, while the IQ center stays at zero; keep it
below half the selected sample rate. For example:

```bash
./build/linux-release/ObriiCli run --input hf1 --freq 3M --rate 8M \
  --mode am --audio-stdout | aplay -q -f S16_LE -r 48000 -c 1
```

The direct-sampling channels are not yet compatible with the TCP virtual
complex-IQ receiver. For HF, use local PCM/WAV or raw IQ recording instead.

## Other receiver backends

Choose `--backend hackrf`, `rtl`, `soapy` or `uhd` and run `devices` to inspect
locally installed drivers. These backends use the same wrappers as Obrii SDR;
their native runtime libraries, device drivers and UHD FPGA images must be
installed separately. `rates` lists Soapy rates when the driver provides
them. For HackRF/UHD, choose a supported `--rate` explicitly. Examples:

```bash
./build/linux-release/ObriiCli devices --backend rtl
./build/linux-release/ObriiCli devices --backend hackrf
./build/linux-release/ObriiCli run --backend hackrf --device 0 --freq 145.5M \
  --rate 8M --gain-db 24 --spectrum --serve --bind 0.0.0.0
./build/linux-release/ObriiCli run --backend rtl --device 0 --freq 100M \
  --rate 2.048M --gain-db 20 --serve --bind 0.0.0.0
./build/linux-release/ObriiCli devices --backend soapy
./build/linux-release/ObriiCli run --backend soapy --device 0 --freq 100M \
  --rate 2M --gain-db 20 --serve --bind 0.0.0.0
./build/linux-release/ObriiCli run --backend uhd --device 0 --rx-channel 0 \
  --freq 100M --rate 2M --gain-db 20 --serve --bind 0.0.0.0
```

Native RTL-SDR uses the existing librtlsdr wrapper and requires its local
driver/library. Device indices come from `devices`; they can differ from USB
port order. If `--rate` is omitted for `rtl` or `rtl-tcp`, CLI requests
2.048 MS/s. A gain of zero selects the RTL tuner's automatic gain; positive
`--gain-db` requests manual tuner gain.

Native HackRF RX uses libhackrf and its signed 8-bit IQ callback through a
bounded conversion queue. The accepted `--rate` range is 2..20 MS/s; `rates`
shows this range, not a discrete hardware list. `--gain-db` maps first to LNA
in 8 dB steps up to 40 dB, then to VGA in 2 dB steps up to 62 dB; it is
applied at RX start. `control metrics` reports `hackrfQueueDroppedSamples` if
the CLI cannot drain the callback queue quickly enough. HackRF has one RX
channel and cannot RX and TX simultaneously on the same physical device.
The CLI path currently leaves the RF amp and bias tee off and does not expose
Opera Cake, clock/trigger or explicit IF/LO controls; use the GUI for those.

`rtl_tcp` needs no USB driver on the CLI host; it connects to an existing
rtl_tcp endpoint and converts its unsigned 8-bit IQ into normalized float IQ.
The remote server is responsible for supporting the requested frequency and
sample rate. Bind rtl_tcp only to a trusted network:

```bash
./build/linux-release/ObriiCli run --backend rtl-tcp --rtl-host 127.0.0.1 \
  --rtl-port 1234 --freq 100M --rate 2M --serve --bind 0.0.0.0
```

External backends currently use the RF/complex-IQ path only. `--lna` and
`--vga` apply only to Fobos; use `--gain-db` for HackRF/RTL/Soapy/UHD/rtl_tcp. Exporting
a launch script from the GUI remains Fobos RF-only for now.

## Replay a recording

`--backend file` reads a CF32LE recording as a real-time IQ source. If its
`.json` sidecar is present, the center and sample rate are loaded from it;
otherwise specify both `--freq` and `--rate`. Explicit values that disagree
with a sidecar are rejected. Only RF complex-IQ recordings are supported.
The hardware center and sample rate stay fixed, but `control listen` can
move the demodulator within the recorded band. Playback ends at EOF without
`--serve`; with `--serve` it stops RX but leaves the server available, and
`control start` replays from the beginning. Example:

```bash
./build/linux-release/ObriiCli run --backend file --iq-file ./capture.cf32 \
  --mode nfm --serve --bind 127.0.0.1
```

This uses the same server/client, spectrum, audio and virtual-IQ output paths
as a live receiver, making repeatable tests possible without SDR hardware.

## Recording and virtual receiver

```bash
./build/linux-release/ObriiCli run --freq 100M --rate 8M --seconds 2 \
  --output ./capture.cf32
```

The output is interleaved little-endian float32 I/Q plus `capture.cf32.json`
metadata. Existing files are never overwritten. At 8 MS/s, raw IQ is about
64 MB/s; check available disk space before a long capture.

To view the remote receiver in Obrii SDR, configure its **Virtual receiver**
for **TCP**, listening on the PC's LAN/VPN address and port 17661, then start
the virtual receiver. On the Raspberry Pi:

```bash
./build/linux-release/ObriiCli run --freq 100M --rate 8M \
  --virtual-host 192.168.88.20 --virtual-port 17661
```

This sends exact CF32 IQ, not compressed display frames. At 8 MS/s it needs
roughly 64 MB/s plus transport overhead; a typical VPN cannot carry it.
The sink is optional, reconnects after a disconnect, and drops queued frames
instead of blocking the USB reader when the network cannot keep up. Status
reports `droppedVirtualFrames`; any nonzero value means the remote IQ has gaps.
These receiver streaming commands emit no RF; RF TX is a separate explicitly authorized worker.

For multiple virtual receivers, start each RX on a different TCP port and
point the CLI's `--virtual-host`/`--virtual-port` at `ObriiIqHub` instead of
directly at one RX. For example, with RX instances listening on 17661 and
17663:

```bash
ObriiIqHub --listen 127.0.0.1:17662 --rx student-a@127.0.0.1:17661 --rx student-b@127.0.0.1:17663 --scenario docs/virtual_iq_link_scenario.json
ObriiCli run --freq 100M --rate 8M --virtual-host 127.0.0.1 --virtual-port 17662 --virtual-node-id lecturer
```

In default packet mode the hub supports up to 16 TX streams and 32 RX endpoints
with independent bounded
queues, drop/delay counters, and optional `--delay-ms`, `--noise-sigma`,
`--seed` and `--collision-ms` experiments. It forwards whole frames in arrival
order rather than summing simultaneous IQ waveforms. The opt-in
`--mix-rate 48000 --mix-center 1000000` mode sums sample-aligned CF32 per RX
with bounded buffers and per-link gain/delay. TX must use the exact rate/center;
source clocks need synchronization for accurate inter-host alignment. This is
not a coherent physical RF channel or a routed mesh network, and it has no authentication; use loopback or
a trusted lab network. Add `--scenario docs/virtual_iq_link_scenario.json` to
restrict which TX reaches each RX and set per-link delay/gain. Set a stable
`--virtual-node-id` on each CLI publisher (`run` or `module tx ... --tx-sink virtual`)
so its route survives reconnects. The hub attaches a fresh `hubSessionId` to
each connection's frames and rejects simultaneous duplicate node IDs. Unnamed
legacy TX retain connection-order names `tx1`, `tx2`, ...; IDs are not credentials.
See `docs/open_amateur_radio.md` for the schema and limitations.

On Windows the corresponding executable is `build/runtime/ObriiCli.exe`.
`--help` lists all options. The Linux binary requires Qt Core/Network, FFTW and
the Fobos shared libraries even when selecting another backend. Qt Multimedia
is optional for local CLI playback; the stdout PCM pipe works without it. No
display server is required unless the playback backend itself requires one.

## Live TX, GNSS And Research

`module tx microphone|manual-cw|microphones|control` provides independent live
capture and keyed output. See [cli_live_tx.md](cli_live_tx.md) for commands,
watchdog, file/virtual/RF sinks and platform limits.

`control gnss start FILE.json|status|eof|stop|reset` starts a separate optional
GNSS worker on the actual full-source IQ of local RX, or full-band network IQ
on a CLI client. Request example: `{"backend":"installed","systems":["GPS"]}`.
Tune the source to 1575.42 MHz first; no implicit tuning occurs. Channel IQ and
server-audio modes cannot supply GNSS. The network full-band stream is CI8,
not original ADC precision. Missing sequences or rate/center changes fail the
session. Capture begins only after worker readiness, not at the start command;
finite replay may therefore miss its prefix. For a complete recording use
`module gnss --file FILE ...` directly. EOF is acknowledged before successful
completion; `stop` is asynchronous. See [cli_gnss.md](cli_gnss.md).

`research FILE.json` executes an offline object or a batch of 1..64 objects
(file limit 4 MiB). `control research request FILE.json` operates on retained
snapshots of the running receiver. `control research capture FILE.json` takes
one explicit current IQ block, or the latest exact FFT, according to its
operation; no work is scheduled while inactive. Use `status`, `capabilities`,
`cancel` and `clear` to inspect/cancel/reset. Metadata in a live capture always
comes from the actual receiver. See [cli_research_tools.md](cli_research_tools.md)
for the operations, units and bounded JSON/CSV paging.
