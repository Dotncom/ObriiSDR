# Shared GUI / CLI FFT Core

`FftTransformCore` in `src/core/ffttransformcore.{h,cpp}` is the production CPU
transform implementation used by both `FFTResult` and `CliSpectrumMeter`.
It owns aligned FFTW buffers and the forward complex plan, caches the window,
stages sanitized IQ directly into FFT storage, executes the CPU transform, and
writes magnitude levels through one precision-parameterized kernel.

## Preserved Contracts

| Concern | GUI `FFTResult` | CLI `CliSpectrumMeter` |
| --- | --- | --- |
| Storage | Out of place, GPU-compatible input/output pointers | In place |
| Input admission | Newest complete pairs; short snapshots zero padded | Newest complete full window; short windows rejected |
| Window | Rectangular, Hann, Hamming, Blackman-Harris, flat top | Hann |
| Normalization | Float dB arithmetic; sum of the copied window prefix, clamped to at least 1 | Double dB arithmetic; full Hann amplitude sum |
| Output levels | Natural FFT order | Shifted order, source `(i + ceil(N/2)) % N` |
| Level floor | -160 dB | -160 dB |
| Execution policy | Existing budgeted range scheduling and FFTW thread setup | Existing single-consumer execution |

The GUI frequency vector remains the separate centered display axis:
`center + (i - floor(N/2)) * sampleRate/N`. It is not an assertion that the
natural-order magnitude at index `i` has that frequency. Existing renderers
retain responsibility for arranging natural-order magnitudes for display.
For odd `N = 513`, the CLI trace starts with natural bin 257 (offset -256 bins),
and ends with natural bin 256 (offset +256 bins). Peak JSON uses those same
signed offsets, including the extreme bins.

Input NaN/infinite components are independently replaced with zero before
windowing. Float GUI and double CLI output arithmetic intentionally keep their
previous finite/overflow semantics. These are explicit precision policies, not
two independent FFT engines.

## Boundary And Ownership

The core has no receiver, global IQ buffer, global radio settings, JSON, or GUI
rendering dependencies. `radiosettings.h` provides the existing window constants
and normalization helper; that header currently depends on Qt Core.

`FFTResult` still owns snapshot acquisition, metadata handling, GPU selection,
automatic GPU/CPU benchmarking, FFTW wisdom, thread-count configuration,
profiling, HF channel composition, and adaptive HF noise cancellation. The HF
branches use shared real-channel input staging and CPU plans without changing
their two-transform, bin remapping, reference, or adaptive-state algorithms.
VkFFT still operates on the same out-of-place buffers, followed by shared
window normalization and magnitude writing. GPU hardware execution is not
covered by the CPU self-tests.

`CliSpectrumMeter` still owns admission/validation, exact median noise
estimation, full-resolution peak selection, and JSON. It compacts natural-order
levels forwards into the first half of its FFT buffer, and uses the remaining
half for median workspace, as before. The shifted output is written only when
requested. `CliSpectrumAccumulator` and streaming/drop policies are unchanged.

Core instances are single-consumer and noncopyable. Callers must serialize each
instance, and execute only a ready plan. `allocateBuffers()` reuses matching
storage and otherwise releases the old plan/buffers. `createPlanLocked()` must
be called while holding `fftwPlannerGlobalMutex()`, including any surrounding
FFTW wisdom/thread configuration. Plan destruction takes that same mutex.
There is no global FFTW cleanup, new execution mutex, or thread-count change.

Window and staging/writer range calls are synchronous. Disjoint ranges can be
scheduled by the caller; no worker pool, virtual per-bin call, or type-erased
per-bin callback is introduced. In-place magnitude compaction must run forwards
over the whole buffer sequentially. Shifted output must not alias its source.
An interrupted window dispatch is not cached; the next request rebuilds it.

## Memory And Performance

The proportional storage is unchanged: GUI CPU FFT buffers use `16*N` bytes and
CLI FFT storage uses `8*N` bytes; a nonrectangular window uses `4*N` bytes.
Existing output vectors, GPU resources, and HF cancellation scratch/state are
additional and unchanged. The core introduces no second IQ copy, per-bin heap
allocation, or per-frame FFT/window allocation after warmup. Existing CLI JSON
and GUI parallel-dispatch allocations are outside that guarantee.

Input mode dispatch is once per range with compile-time-specialized inner
loops. Window generation keeps the GUI's existing parallel thresholds and
the CLI's sequential setup. GUI large-bin scheduling, FFTW threaded-plan
thresholds, and GPU thresholds have not changed. HF staging now writes the
copied prefix and zero tail once rather than clearing the prefix before writing
it again. ARM/Pi performance still needs measurement on actual target hardware;
Windows timings are not a substitute for that check.

## Build Integration

Compile `src/core/ffttransformcore.cpp` once in a static shared-core target, or
add it directly to every target compiling `src/core/fft.cpp` or
`src/cli/clispectrummeter.cpp`. Link Qt Core and float FFTW, expose `src/core` and
the FFTW include directory, and use C++17. Keep one existing
`src/core/fftplannermutex.cpp` definition per final program. The core requires no
new compile definitions, Qt Concurrent, AUTOMOC, or FFTW threads library itself;
the GUI wrapper retains its existing additional build requirements.

The combined self-test source set is:

```text
tools/shared_fft_core_selftest.cpp
tools/fft_test_globals.cpp
src/cli/clispectrummeter.cpp
src/core/fft.cpp
src/core/fft.h
src/core/gpufftbackend.cpp
src/core/iqbuffer.cpp
src/core/computebudgetmanager.cpp
src/core/fftplannermutex.cpp
```

Link that test to the shared-core target, Qt Core/Concurrent, and float FFTW.
Enable AUTOMOC for `fft.h`; include `src/core`, `src/cli`, and FFTW. On
non-Windows platforms also link `Threads::Threads`.

## Verification

`shared_fft_core_selftest.cpp` covers GUI/CLI matched-Hann RF traces, even/odd
frequency mapping (including the extreme positive/negative bins), independent
direct DFTs, every window, short zero-padded GUI snapshots versus CLI rejection,
trailing incomplete pairs, finite sanitation, in-place/out-of-place storage,
normalization/floor and float/double overflow policies, warm-plan reuse,
interrupted window dispatch recovery, and concurrent mixed GUI/CLI
planning/destruction. Its optional
`--benchmark` reports warm GUI and CLI timings at 65,536 and 1,048,576 bins.

Also run `fft_snapshot_selftest.cpp` and `cli_spectrum_selftest.cpp`: together
they exercise all GUI HF modes, adaptive cancellation state and references,
metadata/historical snapshots, large parallel/profile paths, CLI narrow peaks,
accumulation, allocation failure, and concurrent planners.

Verified with Qt 5.15.2, MSVC Release and MSVC AddressSanitizer in separate scratch
build trees: 24 matched GUI/CLI frames and 92 independent DFT frames in the
combined test; 2,000 exact snapshot parity frames and 300 DFT frames in the
existing GUI test; the existing CLI suite also passed. The shared suite also
passed on Raspberry Pi 5, Debian arm64, GCC 12.2, Qt 5.15.8 and FFTW 3.3.10.
File RX there produced identical WAV bytes with and without 1M background FFT.
Compiled VkFFT hardware execution was not covered by these CPU tests.
