# Shared GUI and CLI Core

GUI and CLI are separate frontends, not processes forwarding data through each
other. Shared algorithms are statically linked into each executable. There is no
new runtime DLL, intermediate IQ queue, receiver thread or network hop.

The normal Raspberry package is one source tree. `tools/build_linux.sh` builds
GUI and CLI from the root CMake project into `build/linux-release`; even
`BUILD_TARGET=ObriiCli` selects targets in that same project/output directory.
`cmake --install` puts both frontends and the on-demand CLI workers in one `bin`.
There is no separate user installation or source checkout for the console.

## Implementation Boundaries

- `src/core/fobosreceivercore.h` is an allocation-free native-session view used by
  GUI primary RX settings/readout, the Multi-SDR/Analyzer configuration path and
  CLI Fobos RX. It selects Standard/Agile through the same safe backend wrappers.
  It borrows the handle: destruction never cancels or closes another owner's RX.
  The owning frontend must preserve stop, join, then close. Opening and closing
  are explicit; USB buffer lengths, thread priority and cancellation scheduling
  remain with the existing workers. Other native receivers already use their
  shared backend adapters; this is not a new universal hardware driver.
- `src/core/ffttransformcore.*` owns FFTW buffers, CPU plans, window coefficients,
  input staging, normalization primitives and bin ordering used by GUI `FFTResult`
  and CLI `CliSpectrumMeter`. GUI retains GPU selection, HF processing, FFTW thread
  budgets and its existing parallel policy. CLI retains an in-place buffer and
  its bounded asynchronous spectrum worker. Different windows, modes, sample
  coverage or trace detectors can still yield different results intentionally.
- `src/core/recordingformatcore.*` implements checked seekable appends, PCM16 WAV
  chunks/finalization, IQ byte encodings, FBSPEC1 frames and SigMF metadata. GUI
  recorders and CLI startup/runtime recorders use it. File ownership, async queues,
  filenames, experiment metadata and error handling belong to their adapters.

See `shared_fft_core.md` and `shared_recording_core.md` for numerical and binary
compatibility details. Internal C++ APIs are not plugin or binary ABI promises.

## How To Develop

Put corrections to hardware dispatch, FFT math or recording formats into the
shared core and rebuild **both** frontends. Keep widgets, settings persistence and
command parsing in their adapters. A newly exposed setting still needs GUI/CLI
parameter wiring and tests; a GUI-only change does not create a CLI command.
Do not duplicate the algorithm into a console-specific source file.

Avoid logging, JSON serialization, extra whole-buffer validation passes, virtual
per-sample calls and implicit subscribers in the hot path. Optional analyses
remain opt-in. Blocking file systems and FFT scheduling are not made real-time
merely by sharing their implementation.

## Nonvisual Verification

Build and run `ObriiSharedReceiverCoreSelfTest`, `ObriiSharedFftCoreSelfTest` and
`ObriiSharedRecordingCoreSelfTest`. The receiver test uses stubbed safe APIs, not
physical RF. FFT tests compare CPU paths and numerical references. Recording
tests check bytes and existing readers, including write failures and size limits.
Also run the existing snapshot, asynchronous FFT, recording and audio suites.

`tools/cli_fft_isolation_selftest.ps1` compares the exact demodulated WAV with and
without a 1M FFT workload. `tools/cli_runtime_integration_selftest.ps1` exercises
live recording commands against deterministic file IQ. Neither test transmits RF.
These tests do not replace Raspberry throughput or physical-device qualification.

Verified on Windows/MSVC and Raspberry Pi 5 (Debian arm64, GCC 12.2, Qt 5.15.8,
FFTW 3.3.10): all three shared-core suites passed. The Raspberry CLI built from
the source archive; four seconds of synthetic FM IQ (4194304 complex samples)
produced byte-identical WAV with no spectrum and with a 1048576-bin background
FFT. Windows integration suites also passed recording restart, audio isolation,
remote DSP, browser VFOs, calibration, experiment restoration and help checks.
A Windows Fobos Agile smoke test at 8 MS/s received over 16M samples and stopped
normally. This is not qualification of every RF backend or Raspberry live RF.

A small standalone test project is included with the CLI source archive. On a
Raspberry with Qt Core/Concurrent and FFTW development packages installed:

```sh
cmake -S tools/shared_core -B build/shared-core -DCMAKE_BUILD_TYPE=Release
cmake --build build/shared-core -j2
ctest --test-dir build/shared-core --output-on-failure
build/shared-core/ObriiSharedFftCoreSelfTest --benchmark
```

The receiver stub test additionally needs the Fobos headers, not the driver
libraries or a physical receiver. This project does not replace running binaries
or read private settings.

For scripts and AI-assisted diagnostics, use `ObriiCli capabilities`, `doctor`,
requested JSON `control metrics`, paged traces and research results instead of
screen scraping. `capabilities.sharedCores` reports the internal core contract
revisions (currently 1). Read named fields, check `ok`/errors and output schema,
and retain sample rate, center, FFT/window and sample counters with evidence.
Power in dBFS is not calibrated dBm, and a displayed/reduced spectrum is not IQ.

No cloud service or AI integration is enabled by this change. Control is intended
for a trusted local environment. Hardware TX still requires explicit arming and
confirmation; diagnostic commands must not silently key a transmitter. Avoid
exporting personal settings or keys with diagnostic artifacts.

`CommandDispatcher` is the common typed command boundary for the GUI console
and CLI receiver/client hosts. It validates schemas, permissions and session
identity before invoking existing frontend handlers. The GUI **Console** button
and DSP-board `>_` open a modeless command window for that GUI instance; they do
not launch another receiver. An opt-in loopback Agent API exposes its catalog
and bounded JSON responses without a background model. Commands currently cover
primary receiver controls, counters and cached traces, not every advanced GUI
tool. The CLI retains its larger trusted-local command surface through a
compatibility adapter. See `command_dispatcher.md`, `command_console.md` and
`command_control_api.md` for scope, permissions and tests.

New commands still require a frontend adapter when they operate on different
application state. Register only real handlers, classify their access correctly,
keep them brief, and implement DSP/math in the shared modules rather than in
the command UI. This unifies the command contract, not every worker lifecycle.
