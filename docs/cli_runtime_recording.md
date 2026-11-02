# CLI Runtime Recording Helper

`CliRuntimeRecorder` is an independent QtCore-only runtime recorder. It does not
alter startup recorders, radio hardware, tuning, FFT computation, or demodulation.
The parent CLI owns command routing and build/package integration.

The executable exposes `ObriiCli control record start TYPE PATH`, `record stop
[TYPE]`, and `record status` on the RX host. The receiving process stays running
when recording alone is stopped. See `docs/obrii_cli.md` for usage.

## Commands And Lifecycle

```cpp
CliRuntimeRecorder recorder; // 32 MiB shared queue, 64 blocks, up to five types
auto reply = recorder.command({{"operation", "start"}, {"type", "cf32"},
                               {"path", "captures/session.cf32"}}, actualSettings);
auto stopping = recorder.command({{"operation", "stop"}, {"type", "cf32"}}, actualSettings);
auto all = recorder.command({{"operation", "status"}}, actualSettings);
```

Types: `cf32`, `ci8`, `channel`, `audio`, `spectrum`. There is one slot per type,
with all five allowed simultaneously. `stop` without `type` stops all slots.
Start validates source settings synchronously, then returns `starting`; disk open
errors are asynchronous and visible in status. Inputs submitted while starting
are bounded and ordered behind open. States are `idle`, `starting`, `recording`,
`stopping`, `completed`, `failed`. Duplicate starts, including while stopping,
are rejected. Completed/failed slots may be restarted with a different new path.

`command()` and `status()` return JSON containing `recordings` (one entry per
type), `active`, `activeTypes`, `workerRunning`, and queue usage/limits. Each entry
has state, error, paths, generation, actual source settings, pending blocks, and
committed byte/sample/frame counts. Exact byte/sample counts are also decimal
strings. `ok` describes command acceptance, not eventual file-open success.

`active()` and demand queries lock a mutex and inspect at most five slots; they
do not construct JSON or perform file IO. **`active()` remains true through
stopping and failure cleanup, until final metadata is flushed and files closed.**
The parent must prohibit rate/center/gain changes while active, and prohibit
listen/mode/width changes for channel/audio recordings. Receiver operation is
not stopped by a recording error.

Stop is nonblocking: already accepted input drains on the worker before
finalization. Poll status or call `takeNotifications()` for
`recordingCompleted`/`recordingFailed`. Notifications are explicit, with no logs,
and coalesce to one latest terminal result per type (maximum five). Status keeps
the latest result until that type restarts. `command(status)` reaps an already
finished worker; no running disk operation is joined by a command. The worker
exits after the last active slot completes, even if other types stopped earlier.

`shutdown()` drains/stops/joins synchronously; destruction does the same. Call
these **only after RX has stopped**, off the RX callback path. Ordinary file IO
cannot be forcibly cancelled safely: a blocked OS write/flush/open may delay
shutdown indefinitely. There is no hard cancellation or shutdown deadline.

## Source Submission

```cpp
if (recorder.needsIq())
    recorder.submitIq(interleavedIq, complexSampleCount, actualSettings, discontinuity);
if (recorder.needsAudio())
    recorder.submitAudio(existingPcm48kMono16Le);
if (recorder.needsSpectrum())
    recorder.submitSpectrum(exactLevels, firstBinCenterHz, binSpacingHz,
                            actualSettings, discontinuity, frameUtcMs);
```

`needsIq()` is true for **any accepting slot**, including audio/spectrum-only
recordings, to validate actual source metadata/discontinuities on every block.
Only cf32/ci8/channel slots copy the IQ payload; a shared queue block serves all
three. A metadata-only observation is `submitIq(nullptr, 0, actual, discontinuity)`.
IQ `count` means complex samples, requiring `2 * count` readable floats.
The caller retains ownership and may reuse buffers when submission returns.
Inactive submissions start no threads, copy no payloads, and touch no files.

Audio accepts existing **48000 Hz mono signed 16-bit little-endian PCM**, with no
second demodulator, AGC, audio device, or GUI dependency. Odd byte counts fail
the audio recording. Source discontinuities/settings must still be observed
through `submitIq`; PCM alone cannot identify an upstream radio change.

Spectrum receives exact, already computed levels, positive bin spacing, and the
first bin's **center** frequency. Last frequency is `first + (bins - 1) * binHz`.
No FFT or bin reduction runs in the helper. Bin count/geometry and FFT settings
are fixed for a session; changes fail it. `utcMs == 0` uses submission wall time.
Stored frame bin counts are limited to 2,097,152, matching the existing
`SpectrumFrameReader::readNext()` limit. Source FFT lengths may be larger; an
exact vector with more than 2,097,152 levels cannot be recorded in this compatible
format without updating that reader.
The parent must submit each newly produced frame once, not repeatedly resubmit
a stale latest-frame snapshot. Levels, including nonfinite values, are preserved
bit-for-bit; nonfinite IQ instead fails the recording rather than substituting
samples. IQ/audio timestamps are submission times, not hardware sample clocks.

## Paths And Formats

`path` is the **exact data filename**, not an event prefix. Relative paths resolve
against the working directory. Its parent directory must already exist. Every
type reserves `path` and `path + ".json"` using `QIODevice::NewOnly`; cf32/ci8 also
reserve `path + ".sigmf-meta"`. No existing data/sidecar is overwritten, including
sidecar-only collisions. Partial new-file reservations are removed on open
failure; pre-existing files are retained. Successful/failed captures retain their
complete data prefix and terminal sidecars for diagnosis. Do not rename/unlink
outputs while recording. Concurrent hostile directory replacement is outside
the file-ownership model.

- `cf32`: interleaved I,Q IEEE float32 little-endian, exact finite input values.
- `ci8`: signed interleaved I,Q, `round(clamp(value, -1, 1) * 127)`.
- `channel`: stereo PCM16LE WAV, I then Q, using existing `IqChannelizer` with
  persistent state, default channelizer rate/cutoff, and AGC disabled. Output
  quantization is `round(clamp(value, -1, 1) * 32767)`. Metadata records actual
  output rate, counts, frequency shift, filter delay, and decimation. FIR startup
  delay and incomplete final decimation/filter tails are not padded or fabricated.
- `audio`: standard mono PCM16LE WAV, 48000 Hz, unchanged submitted bytes.
- `spectrum`: existing FBSPEC1 format, readable by `SpectrumFrameReader` and
  `SpectrumFrameRecorder::loadFile`. The shared `makeFrame` builder supplies frame
  conventions without reduction. The embedded header is the start snapshot;
  the `.json` sidecar is authoritative for final frame/bin/byte counts and state.

The WAV/ci8/SigMF layouts follow `SpectrumIqEventRecorder`. Its truncating
path-based writers cannot safely implement new-only ownership, so this helper
uses separate exclusive file handles without modifying either existing recorder.
Raw SigMF includes `core:dataset` naming the exact data filename, source rate,
center frequency, first submitted timestamp, and terminal count/state extensions.
For channel/audio, final JSON sample counts are actual output sample frames;
for spectrum, sample count is the total stored bins. `bytesWritten` excludes
container headers. WAV headers are patched on stop; the RIFF 32-bit limit causes
a visible failure before overflow rather than an invalid oversized WAV.

## Bounds And Failure

One worker performs all open/write/flush/close, quantization, serialization, and
channel decimation. It never holds the submission mutex during IO or DSP. Queue
accounting charges each copied payload once plus fixed block overhead, including
the in-flight block. Defaults: 32 MiB, 64 blocks, five types; constructor arguments
override these for smaller devices/tests (upper bounds: 1 GiB, 1024 blocks, five
types). Each input block is additionally limited to 64 MiB. Writer scratch and
fixed per-slot/channelizer state are separate bounded working memory; this is
not a strict whole-process RSS cap. There are no unbounded deferred snapshots.

Queue exhaustion fails affected stream types, not the receiver or unrelated
stream types. Already accepted blocks drain as a complete prefix; the overflowing
block and all subsequent blocks for that session are rejected. Discontinuity,
source rate/center/actual-frequency/input/device/clock/gain changes, invalid IQ,
channel changes, and write/finalization errors fail closed and are visible in
status and terminal notifications. A disk failure may also prevent persisting a
failure sidecar; in-memory status remains authoritative. No exact-data loss is
silently represented as a successful capture.

## Synthetic Self-Test

Build `tools/cli_runtime_recording_selftest.cpp` with this helper,
`src/core/iqchannelizer.cpp`, `src/core/channelizerutils.cpp`, and
`src/research/spectrumframerecorder.cpp`; include `src/cli`, `src/core`, and
`src/research`. Link Qt5 Core and platform thread support, C++17. No RF hardware,
GUI, FFT library, audio device, or event loop is needed.

The test checks five simultaneous slots, independent stop, exact raw/quantized
readback, analytic mixed channel tone and shared-channelizer equivalence, final
WAV sizes/rates/PCM bytes, actual SigMF metadata, existing spectrum-reader
compatibility, final counts, inactive behavior, new-only collisions, slot caps,
restart, open errors, deterministic tiny-budget overflow, source/channel/geometry
guards, discontinuity, unaligned PCM, nonfinite IQ, and concurrent submission.
