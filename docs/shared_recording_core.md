# Shared Recording Format Core

`src/core/recordingformatcore.{h,cpp}` is the production container/encoding core
used by GUI `RecordingManager`, `SpectrumIqEventRecorder`,
`SpectrumFrameRecorder`, CLI `CliRuntimeRecorder`, and `CliAudioOutput`.
It requires C++17 and QtCore only. It has no QObject subclass, MOC requirement,
DSP, FFTW, receiver, audio-device, thread, queue, or filesystem naming dependency.
No public GUI recording API or default recording path changes.

## Stable API

All functions are in `RecordingFormatCore`:

- `checkedAppend(QIODevice&, const char*, qint64, QString* = nullptr)` writes a
  borrowed span exactly once. Positive short writes and negative writes fail.
- `beginWave(QIODevice&, quint32 sampleRate, quint16 channels, quint16 bits,
  const QByteArray& fbMetadata, WaveLayout&, QString* = nullptr)` writes PCM16,
  one or two channels, into an empty sink. Optional `fbos` metadata precedes
  `data`, retaining its original JSON bytes and RIFF even-byte padding.
- `appendWave(QIODevice&, const WaveLayout&, quint64& dataBytes, const char*,
  qint64, QString* = nullptr)` checks sample alignment, cursor, and RIFF bounds
  before writing. The byte count changes only for a complete append.
- `finalizeWave(QIODevice&, const WaveLayout&, quint64 dataBytes,
  QString* = nullptr)` trims any incomplete tail, checks flush/seek/write
  failures, and patches RIFF/data sizes using the recorded offsets. It does not
  rewrite the metadata, clamp sizes, close the sink, or extend a missing prefix.
- `waveDataLimit(const WaveLayout&)` gives the largest aligned payload for the
  actual header size. RIFF size includes metadata; both 32-bit fields must fit.
- `encodeIq(const float*, std::size_t components, IqEncoding, QByteArray& scratch,
  EncodedIq&, QString* = nullptr,
  IqNonFinitePolicy = IqNonFinitePolicy::Reject)` produces a borrowed view.
  Encodings are `Cf32Le`, `Ci8`, and `Ci16Le`. CF32 preserves every bit, including
  NaN payloads, infinities, and signed zero. On little-endian hosts it performs
  no input scan or copy. Integer encodings clamp to [-1, 1], multiply by 127 or
  32767, and use the previous CLI `lround` rule. `Reject` or `Zero` handles
  non-finite components inside that same conversion loop, before `lround`.
  `Zero` preserves startup CLI sanitization; the runtime wrapper retains its
  original rejecting source policy, including a CF32 finite guard.
- `beginSpectrum(QIODevice&, const QJsonObject&, QString* = nullptr)` and
  `appendSpectrum(QIODevice&, const SpectrumFrameView&, QString* = nullptr)`
  write the existing FBSPEC1 JSON header and atomic FRM1 records. The view borrows
  bins without reduction/copying; geometry/count checks match the reader.
  Frame headers are 52 bytes, scalar fields and float32 bins little-endian.
- `sigMfMetadata(datatype, sampleRate, dataset, centerFrequency, firstUtcMs,
  globalExtensions = {})` builds the common SigMF 1.0 object. GUI description
  and runtime counts/state/error remain caller-owned extensions. Paths remain
  caller-owned: GUI uses the existing basename sidecar, runtime CLI appends
  `.sigmf-meta` to its reserved data path.

`WaveLayout` contains `headerBytes`, `dataSizeFieldOffset`, and `blockAlign`.
Its default is the ordinary 44-byte mono PCM16 header, allowing the existing
`CliAudioOutput` header to remain untouched. `EncodedIq` contains `data`/`size`;
consume it before modifying its input or scratch. A failed encode returns an
empty view, even if conversion had partially filled scratch.

## Ownership and Failure Boundaries

The core borrows open, writable, seekable sinks. The wrapper must keep the
append cursor at the complete prefix, serialize access, latch failures, and
retain its own counters. Regular successful appends do not query file size,
flush, seek, resize, or allocate container buffers. Header creation checks for
an empty sink; finalization checks actual extent and flushes regular files.

Partial appends roll back the whole attempted record using `QFileDevice::resize`
or `QBuffer` truncation, then restore the cursor. Other seekable QIODevices are
usable, but must support their own truncation policy if a write extends the
sink before failing; unsupported rollback is reported as failure. A sink which
also fails rollback/flush/patching cannot be promised recoverable bytes. Errors
remain visible to the wrapper, never successful count commits. Disk durability
is QFile flush semantics, not fsync or atomic multi-file commit.

GUI scheduling is unchanged and remains synchronous on its existing owner.
CLI runtime reservations (`NewOnly` for data and sidecars), owned-file-only
collision cleanup, asynchronous bounded queue, generations, source guards,
failure states, and notification behavior stay in `cliruntimerecorder.cpp`.
The core never opens, deletes, renames, reserves, or closes any file.
No receive/FFT pipeline changes are part of this migration.

## Memory and Fast Paths

Already-byte PCM/CI8/IQ16 GUI frames and CLI audio are passed directly to the
shared append functions: no copy, conversion, or per-frame heap allocation.
FBSPEC1 uses a 52-byte stack header and directly writes the supplied bins on
little-endian systems. Big-endian serialization uses bounded stack chunks.
GUI frame reduction behavior is unchanged; runtime no longer constructs an
unused copied `SpectrumFrameRecord` or a whole serialized spectrum QByteArray.

Each runtime writer retains one conversion scratch buffer and, only for its
channel format, the channelizer output vector. Capacity follows the largest
accepted worker block; there is no second retained raw IQ vector or extra
queue copy. CF32 scratch remains empty on little-endian systems. Queue limits
and ownership are unchanged; scratch capacities are worker memory, not a new
unbounded queue. This is a structural fast-path check, not a Raspberry Pi
throughput certification; ARM/Pi hardware was not available for this change.

## Build Integration

Build a STATIC `RecordingFormatCore` library from exactly:

```cmake
add_library(RecordingFormatCore STATIC
    src/core/recordingformatcore.cpp
    src/core/recordingformatcore.h)
target_include_directories(RecordingFormatCore PUBLIC src/core)
target_link_libraries(RecordingFormatCore PUBLIC Qt5::Core)
target_compile_features(RecordingFormatCore PUBLIC cxx_std_17)
```

Use absolute `${OBRII_ROOT}` paths for the standalone `src/cli` project.
Link that library wherever any migrated caller is compiled, including static
libraries (PUBLIC for their downstream consumers). At inspection time these
are root `ObriiSDR`, `ObriiCli`, `ObriiMeasurementCore`,
`ObriiCliRuntimeRecordingSelfTest`, `ObriiCliAudioSelfTest`,
`ObriiExperimentBundleSelfTest`, `ObriiScienceSelfTest`,
`ObriiExperimentReplaySelfTest`, `ObriiDmrPipelineSelfTest`, and standalone
`src/cli`'s `ObriiCli`. Existing consumers of `ObriiMeasurementCore` can inherit
the link transitively; do not compile a second copy of the core into a target.
Any new startup `obriicli.cpp` migration uses that same linked library.

New `ObriiSharedRecordingCoreSelfTest` exact sources, in addition to linking
`RecordingFormatCore`:

```text
tools/shared_recording_core_selftest.cpp
src/audio/recordingmanager.cpp
src/audio/recordingmanager.h
src/audio/playbackmanager.cpp
src/audio/playbackmanager.h
src/research/spectrumiqeventrecorder.cpp
src/research/spectrumiqeventrecorder.h
src/research/spectrumframerecorder.cpp
src/research/spectrumframerecorder.h
```

Include directories: `src/core`, `src/audio`, `src/research`. Link only QtCore
and `RecordingFormatCore`. Enable AUTOMOC for RecordingManager/PlaybackManager.
No hardware SDK, Widgets, Gui, FFTW, or network dependency is needed.

The existing runtime-recording self-test keeps its sources unchanged and adds
the core link. Its current complete source set is
`tools/cli_runtime_recording_selftest.cpp`, `src/cli/cliruntimerecorder.cpp`,
`src/research/spectrumframerecorder.cpp`, `src/core/iqchannelizer.cpp`, and
`src/core/channelizerutils.cpp`; includes `src/cli`, `src/research`, `src/core`;
QtCore plus platform thread support. The existing audio self-test also adds
the core link but retains its existing demodulator/FFTW/winmm dependencies.

## Verification

`shared_recording_core_selftest.cpp` covers hardcoded pre-core WAV/FBSPEC1
fixtures, prior QDataStream byte equivalence, PlaybackManager PCM/IQ delivery,
SpectrumFrameReader readback, mono/stereo rates, odd/even `fbos` metadata and
GUI lab/sidecar metadata, event CI8/SigMF, signed zero/NaN CF32 evidence,
integer clipping/rounding/rejection/sanitization, reusable conversion storage,
partial header/payload/frame writes, failed seeks/patches, whole-record prefix
rollback, and exact aligned RIFF boundaries with/without metadata using a
constant-memory logical large sink.
It also asserts direct PCM pointer identity and no per-append file-size query,
and checks GUI wrappers stop on malformed frames/rate changes without losing
their complete prefixes.

The untouched runtime self-test covers simultaneous captures, shared-reader
readback, source/discontinuity failures, queue overflow/drain, new-only
data/JSON/SigMF collisions and owned-file cleanup, and concurrent producers.
The untouched audio self-test checks the real demodulator's WAV output.
All test data is synthetic. GUI default-path test files live beside the
scratch test binary and are removed after verification. No profiles or user
recordings are read.

The shared suite also passed on Raspberry Pi 5 (Debian arm64, GCC 12.2,
Qt 5.15.8). The standalone test project is `tools/shared_core`; it can verify
formats without building the full GUI or requiring receiver hardware.
MSVC x64 Release scratch builds and all three tests passed with a separately
linked STATIC core. No root production build or ARM/Pi hardware run was made
by the recording worker.
