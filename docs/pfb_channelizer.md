# Opt-in uniform PFB and DDC FIR quality

## Scope and integration contract

`PfbChannelizer` is a native complex-IQ, critically sampled analysis bank using
the existing single-precision FFTW dependency. It does not replace the precise
per-branch `IqChannelizer`, change main-audio defaults, demodulate, apply AGC, or
clip samples. PFB routing must be explicitly enabled in the board graph.

One bank accepts raw interleaved CF32 at `Config::sampleRateHz`, with power-of-two
`channels` M in 8..64 and even `tapsPerPhase` T in 8..64 (default M=16, T=16).
There is no additional coarse wideband predecimator: the prototype operates at
raw Fs and the output rate is exactly Fs/M. A preceding predecimator would need
its own coverage, alias-rejection, rate, and delay contract; applying the existing
per-DDC front filter globally would not preserve the full set of input bands.

```cpp
PfbChannelizer bank;
if (!bank.configure({inputRateHz, 32, 16})) { /* report configuration failure */ }
const double offsetHz = requestedRfHz - inputRfCenterHz;
const int bin = bank.nearestCenterIndex(offsetHz);
if (!bank.coversBand(bin, offsetHz, occupiedBandwidthHz)) {
    // Reject this PFB route explicitly; do not silently substitute another mode.
}
std::vector<int> selectedBins{bin}; // Deduplicate across branches, at most eight.
std::vector<PfbChannelizer::ChannelOutput> outputs;
auto result = bank.processFloatIq(rawCf32, floatCount, selectedBins, outputs, &cancel);
if (result.status == PfbChannelizer::Status::Ok) {
    // outputs[i] corresponds exactly to selectedBins[i].
    // Residual RadioSettings: sampleRate = outputs[i].sampleRateHz;
    // centerFrequency = inputRfCenterHz + outputs[i].centerOffsetHz;
    // listeningFrequency remains requestedRfHz; inputMode must be INPUT_RF.
    // Each branch retains its own IqChannelizer instance/options/state.
}
```

Pass each unique bin to the bank only once, then fan its output out to independent
residual DDCs. No vectors for unselected channels are produced, although the
small FFT computes the full bank internally. With an empty selection, samples
still advance FIR history and frame cadence, but FFT execution is skipped and
no output is returned. Selecting a bin later does not reset bank history.

Do not route HF-combined, separate real ADC lanes, or a noise-cancellation input
through this complex-IQ API without first performing the intended lane conversion.
Residual target rates must not exceed Fs/M. Validate all requested occupied bands,
including any required filter-transition margin, rather than testing centers alone.

## Frequency convention and coverage

Bin indices use natural FFT order, not fftshift order:

| Index | Center offset |
| --- | --- |
| 0 | DC |
| 1 .. M/2-1 | k Fs/M |
| M/2 | -Fs/2 (Nyquist) |
| M/2+1 .. M-1 | (k-M) Fs/M |

`nearestCenterIndex(offsetHz)` accepts finite offsets in [-Fs/2, Fs/2), returns
-1 otherwise, and resolves exact half-bin ties away from zero. Near positive
Nyquist it can return M/2, whose canonical center is negative Nyquist. This is
only a periodic nearest-bin query, not routing permission.

`binCenterHz()` returns NaN for an invalid bin. `usablePassbandHz()` returns the
FULL width `Fs/M * (1 - 6/T)`, derived from a conservative three-prototype-bin
guard on each side of the Fs/(2M) cutoff. T=8/16/32/64 gives usable widths
0.25/0.625/0.8125/0.90625 times Fs/M. The gaps between usable bands are intentional:
a critically sampled bank cannot transparently cover arbitrary edge-crossing DDCs.

`coversBand(bin, centerOffsetHz, bandwidthHz)` requires a positive finite full
bandwidth, the complete non-wrapped band inside [-Fs/2, Fs/2], and the complete
band inside that bin's usable passband. It rejects edge-crossing and uncovered
bands. It does not wrap a positive-edge band into negative Nyquist. A positive-
width band centered exactly at either raw Nyquist edge is rejected. Nyquist bins
are still available explicitly for analysis/testing.

The guard is an engineering passband policy for this Blackman prototype, not a
brick-wall guarantee. Tests cover passband edges, off-center tones, leakage into
neighbors, gain, and sign. Strong out-of-band interferers still require the usual
system-level dynamic-range assessment.

## Phase, timing, and gaps

The normalized real Blackman-windowed sinc prototype has length L=M*T, unity
DC gain, cutoff Fs/(2M), and linear-phase delay D=(L-1)/2 input samples. FFTW uses
`FFTW_FORWARD`, with no 1/M scaling: unity gain is supplied by the prototype sum.
For an epoch beginning at input index zero, output m is exactly the analysis
operation below, up to float arithmetic:

```text
t_m = (m+1) M - 1
y_k[m] = sum(l=0..L-1) h[l] x[t_m-l] exp(-j 2 pi k (t_m-l)/M)
x[n] = 0 for n < 0
```

History residues are reversed before the forward FFT to implement this global
negative-sign mixer. A positive center-bin tone becomes constant positive-real
baseband with no arbitrary bin-dependent phase. A residual positive frequency
stays positive. An off-center tone has the prototype's group delay D; the API
reports this via `delayInputSamples()` (seconds = D/Fs).

The first output is available after M input samples, with a zero-padded startup
transient. There is no startup suppression or automatic end-of-stream tail flush.
For fully settled history, discard frames with `t_m < L-1`. To flush a finite
capture explicitly, feed L-1 zeros and account for the added synthetic samples.
`Result::firstOutputInputSample` is t_m of the first emitted frame, relative to
the last reset, not the delay-compensated signal time. Subsequent frames are M
input samples apart. The delay-compensated time is `(t_m-D)/Fs`.

Arbitrary complete-IQ chunk boundaries preserve phase, history, and cadence
exactly. `configure()` with an identical configuration preserves that state;
changed valid configuration starts a new epoch. Invalid configuration preserves
the previous working configuration and history. `reset()` clears history/cadence
without replanning. After a dropped input block, RF retune, or epoch change, the
parent must reset BOTH the shared bank and affected residual DDCs and mark the
output discontinuity. Missing samples are not inferred or interpolated. A newly
attached residual DDC also needs its own fresh state. Changing selection alone
does not reset the shared bank.

## Ownership, limits, cancellation

- One owner must serialize configure, reset, process, move, and destruction on
  each instance. Independent instances may execute concurrently.
- FFTW plan creation and destruction use the same `fftwPlannerGlobalMutex()` as
  `fft.cpp`. Execution has no planner lock. Plans use bounded 8..64-point
  `FFTW_ESTIMATE`; this component never calls global FFTW cleanup or alters
  FFTW threading/wisdom policy. Existing FFT code restores its planner thread
  count to one under the shared mutex.
- Plans/buffers are RAII-owned and movable, not copyable. Failed planning or
  allocation during configuration leaves the previous instance usable.
- Input is at most 1,048,576 COMPLEX samples per call, or 2,097,152 floats.
  Split larger raw blocks into complete-IQ chunks. Selected indices must be
  unique and there can be at most eight. Odd float counts and null nonempty
  buffers are rejected without consumption. Null with count zero is allowed.
- Prototype/history are at most 4,096 coefficients and 4,096 complex samples
  (48 KiB), plus 1 KiB FFT buffers and FFTW's small plan. Logical output is at
  most 8 MiB per call. Caller vectors may retain capacity; reuse them between
  calls. Input storage belongs to the caller and must not alias any output.
- Per-component NaN/Inf input values are replaced by zero. Ordinary CF32 is
  assumed to have sane finite amplitudes; no automatic gain limiting is applied.
- Output capacity is reserved before consuming input. Validation/resource/
  allocation failures return empty outputs and consume nothing.
- Cancellation is checked before allocation and every 256 input samples.
  `Cancelled` may return a valid prefix with matching output metadata and
  `inputSamplesConsumed`. Resume at `samples + 2*inputSamplesConsumed`, retaining
  that prefix and bank state, for exact parity. Alternatively discard the prefix,
  reset both bank and residual DDCs, and declare a gap. Never retry the full input
  after a partially consumed call. `outputSamplesPerChannel` is zero for an empty
  selection, even though input/history have advanced.

## DDC FIR options

`IqChannelizer::Options` adds:

| Field | Meaning |
| --- | --- |
| `firQuality` | Legacy=32, Balanced=64, High=128 taps |
| `firTapCount` | 0 uses preset; explicit even 16..256 overrides preset |
| `transitionHz` | 0 keeps existing design policy; positive requests a full Blackman transition-width estimate |

All presets retain 64 fractional phases and the existing Blackman-windowed sinc
design. With positive transitionHz, required taps are `ceil(6*coarseRate/width)`,
rounded up to even. Automatic tap selection uses the larger of this and the
preset. An explicit tap count must already meet that minimum. Width must fit
below output Nyquist: `cutoffHz + width/2 <= outputRateHz/2`. Nonfinite/negative
width, insufficient explicit taps, or requirements exceeding 256 return invalid;
there is no silent clamping. For example, coarse/output rate 192 kHz, cutoff
15.36 kHz, and width 18 kHz needs 64 taps; an explicit 32-tap request is rejected.

Transition width is an approximate design constraint, not a new passband-edge
definition or an equiripple attenuation guarantee. `cutoffHz` remains the sinc
cutoff midpoint. This quality control changes only the final resampling FIR;
it cannot undo aliasing or droop from the existing earlier front/coarse decimation
stages. It is not a quality guarantee for arbitrary large decimation ratios.

Null options and old options with the new fields left at defaults retain the
original 32-tap/64-phase coefficients, output schedule, AGC, and clipping.
The selftest compares every legacy coefficient and output sample bit-for-bit
against a frozen reference. No routing or main-audio call sites are changed here.
The FIR bank/history have bounded fixed storage, approximately 70 KiB per DDC.

`Result::firTapCount` reports the effective count. `firDelayCoarseSamples` reports
the existing resampler scheduling delay, taps/2, in samples AFTER the front/coarse
stages, not total chain delay. FIR option changes that alter the tap count reset
the resampler history. The parent should reset the whole branch on plan changes
for an explicit epoch boundary. Validation occurs before mutating DSP state.

## Build integration

CMake includes the bank in `ObriiSDR` and `ObriiDspFlowSelfTest`. It uses the
existing FFTW float dependency and shared planner mutex. Enable
`FOBOS_BUILD_CW_SELFTEST` for the test targets. The standalone bank has no Qt
dependency; the combined DDC test uses Qt Core for `RadioSettings`.

```powershell
cmake --build <build-dir> --config Release --target ObriiPfbChannelizerSelfTest
& <build-dir>/Release/ObriiPfbChannelizerSelfTest.exe
& <build-dir>/Release/ObriiPfbChannelizerSelfTest.exe --benchmark
```

The integrated CMake targets were built and passed on Windows with MSVC 19.44,
FFTW 3.3.5 float and Qt 5.15.2. `ObriiDspFlowSelfTest` additionally checks actual
bank-to-DDC routing, independent demodulation/filter state, volume changes and
simplified-mode suspension. This is not Raspberry or audio-device qualification.

## Verification and measured benchmark

On the development Windows x64 machine, the test passed direct mixed-convolution
and impulse/delay comparisons for M=8/16/32/64 and T=8/16/32/64, positive/negative/
DC/Nyquist tones, off-center passband-edge tones, unclipped amplitude 1.25, bit-exact
chunk and cancellation/resume parity, empty selection, gaps, reconfiguration,
coverage/input/resource guards, concurrent FFTW plan lifecycles, legacy FIR
reference parity, all supported FIR tap extremes, and transition validation.
Worst tested PFB relative amplitude error was 0.0005763 (0.05763%); worst tested
adjacent rejection was 75.82 dB. These describe the fixtures, not an exhaustive
sweep or a universal dynamic-range guarantee. The 128-tap DDC FIR improved the
chosen off-bin rejection fixture by 61.14 dB relative to 32 taps.

The unchanged IQ-channelizer selftest passed, including 2.048/20/50 MS/s chunking
and independent-queue/snapshot tests. High-quality DDC chunking at 2.048/50 MS/s
and reset-after-gap parity also passed. The combined PFB/FIR test passed an
additional MSVC /O1 /Zi /fsanitize=address build in
`build/pfb-sidecar-selftest/asan`, with no reported memory errors. Bundled FFTW
and Qt DLLs themselves were not rebuilt with instrumentation. No GUI or
hardware-radio test was run.

`--benchmark` compares independent DDCs against shared PFB PLUS all independent
residual DDCs, using M=32/T=16, 262,144 complex input samples per block, three
timed blocks after warmup, equal final requested rate Fs/128, and equal residual
cutoff settings. Planning is excluded. Output responses differ; this is not an
equal-selectivity or equal-total-delay comparison. The synthetic input is a
single tone; every requested branch is still processed.

A repeat using the integrated Release target, with no application build running,
produced the following mean ms/block on 2026-10-13:

| Fs | Branches | Individual DDC | PFB + residual DDC |
| --- | ---: | ---: | ---: |
| 2.048 MS/s | 1 | 3.10 | 8.56 |
| 2.048 MS/s | 2 | 6.31 | 8.84 |
| 2.048 MS/s | 4 | 12.60 | 9.50 |
| 2.048 MS/s | 8 | 25.23 | 10.66 |
| 50 MS/s | 1 | 3.28 | 8.54 |
| 50 MS/s | 2 | 6.53 | 8.85 |
| 50 MS/s | 4 | 13.25 | 9.57 |
| 50 MS/s | 8 | 24.79 | 10.66 |

Small three-block timings are descriptive, not a statistical guarantee; earlier
measurements varied with concurrent work. Rerun on the target device under its
real workload. Neither
speedup nor real-time operation is assumed: 262,144 samples at 50 MS/s represent
only 5.24 ms, shorter than the measured PFB path in these runs. More prototype
taps cost more work, even if only one bin is selected. Opt-in routing and explicit
coverage checks remain necessary independently of performance.
