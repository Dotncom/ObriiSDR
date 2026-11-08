# Optional Headless Research Helper

The wired executable accepts `ObriiCli research FILE.json`: a bounded **4 MiB**
JSON object, or an array of **1..64** requests. Local control accepts
`control research request FILE.json|status|capabilities|clear|cancel`.
`control research capture FILE.json` requests a one-shot current snapshot:

- Raw IQ for `iq.analyze`, `constellation.analyze`, `eye.analyze` or
  `iq.phase.analyze`. Request `samples` in **16..65536**; capture is clipped to
  one received block and the reply explicitly reports `capturedSamples`.
- The current exact full FFT for `spectrum.analyze`, `density.append` or
  `comb.analyze`. More than **65536** bins is refused, not decimated.
- The parent overwrites metadata with the actual source/rate/center/unit; it
  does not fabricate calibration or units. Unsupported display units are refused.
- `pulse.analyze` currently requires an explicit time series; no live pulse or
  zero-span snapshot is implied. `cancel` cancels parent pending capture, not a
  background helper job.

`CliResearchTools::command(const QJsonObject &request)` remains an optional,
synchronous Qt Core helper. The parent owns routing, capture, transport size
limits and CMake integration. Only explicit calls do work: there
are no QObject instances, subscriptions, worker threads, event-loop requirements,
receiver changes, personal settings access, automatic capture, or submit hooks.
Serialize calls on one thread. One snapshot per tool and one bounded density
history are retained; successful analysis replaces only its own snapshot. Errors
preserve existing data. No live hook is required by this helper.

## Integration

```cpp
#include "cli/cliresearchtools.h"

CliResearchTools research;
QJsonObject reply = research.command(request);
```

Use C++17 and link **Qt Core only**. Compile these sources:

- `src/cli/cliresearchtools.cpp`
- `src/cli/cliiqquality.cpp`
- `src/research/spectrumscienceanalyzer.cpp`
- `src/research/tonalspectrum.cpp`
- `src/research/headlessresearchmetrics.cpp`

Include `src/cli`, `src/research`, and `src/ui` (the latter supplies the
QtCore-only `spectrumoverlaytypes.h`, not a widget). No moc, QtGui, QtWidgets,
FFTW, device, microphone, PTT, GNSS, or networking dependency is needed.
`tools/cli_research_tools_selftest.cpp` uses exactly those dependencies and
`QCoreApplication`; it checks analytic synthetic values, degeneracies, limits,
metadata rejection, stable paging, extracted GUI algorithms and file exports.

## Metadata And Limits

Every analysis requires an explicit `metadata` object with all four fields:

```json
{"source":"synthetic:channel-0","sampleRateHz":8000,"centerHz":100000000,"amplitudeUnit":"normalized"}
```

`source` is an opaque, nonblank string of at most 128 UTF-16 code units, without
CR/LF/NUL. It is never interpreted as a device or file name. `sampleRateHz` must
be a finite number in `[1e-9, 1e12]`; `centerHz` in `[0, 1e15]`. IQ requires
`normalized`; spectra require `dBFS` or `dBm`. dBm is a caller declaration of
already calibrated data, never an implicit conversion. Extra metadata is ignored.
No source, rate, center, axis, or unit is inferred from receiver state.

IQ is capped at **65,536 complex samples**, spectra at **65,536 bins**, and every
read/export at **1,024 rows** (default 256). Oversized/malformed data is rejected,
not truncated. IQ components must be finite and float32-representable; JSON null,
strings, NaN/Inf and incomplete pairs are rejected, not replaced with zero.
Spectrum levels must be finite numbers in `[-200, 100]` dB. These limits bound
helper-owned storage, processing and results, not the parent's JSON parser input.
The parent must separately bound incoming transport/file sizes.

## Supported Operations

```json
{"operation":"capabilities"}
{"operation":"status"}
{"operation":"iq.analyze","metadata":{"source":"test","sampleRateHz":8,"centerHz":0,"amplitudeUnit":"normalized"},"iq":[0.5,0.25,-0.5,-0.25]}
{"operation":"iq.read","offset":0,"limit":2}
{"operation":"iq.read","view":"metrics"}
{"operation":"spectrum.analyze","metadata":{"source":"test","sampleRateHz":1000,"centerHz":400,"amplitudeUnit":"dBFS"},"frequenciesHz":[0,100,200,300,400,500,600,700,800],"levelsDb":[-40,-40,-40,-40,-10,-40,-40,-40,-40]}
{"operation":"spectrum.read","offset":0,"limit":3}
{"operation":"spectrum.read","view":"metrics"}
{"operation":"iq.clear"}
{"operation":"spectrum.clear"}
{"operation":"clear"}
```

IQ scope rows are `[index, timeSeconds, i, q]`, preserving raw components with
time measured from sample zero using the supplied rate. Statistics call
`CliIqQuality`, not a new formula: DC I/Q, **DC-removed AC RMS** I/Q, gain
imbalance, normalized correlation and near-clipping percent. Near-clipping is the
existing CLI threshold `abs(I) >= 0.98f || abs(Q) >= 0.98f`, not the GUI's 0.999
threshold. No wall-clock throughput or timestamp is presented as a signal metric.
Zero-variance imbalance/correlation are explicitly invalid/null rather than the
core's finite placeholder. Phase/frequency and symbol diagnostics have separate
explicit operations described below.

Spectrum rows are `[index, frequencyHz, levelDb]`. Frequencies must be ascending,
uniform within `max(1e-6 Hz, step*1e-6)`, and fit `centerHz +/- sampleRateHz/2`.
They must remain strictly increasing after conversion to the shared core's
float32 axis. Scan mosaics, gaps and float-collapsed high-RF bins are rejected.
Returned frequencies/metrics use that float32 axis; precision is declared in
metadata. This avoids promising Hz resolution the existing API cannot represent.

`SpectrumScienceAnalyzer` supplies single-frame full-span sample-detector
metrics: peak, 40th-percentile noise floor, peak-to-floor SNR, integrated channel
power, power centroid, 90/95/99% occupied widths and 3/6/20 dB widths. A single-bin
width may correctly be zero. There is no fabricated ACPR without measurement
markers, temporal detector history, density, comb family or interference label.

Optionally add a `tonal` object to `spectrum.analyze`:

```json
{"fundamentalHz":20000,"integrationHalfWidthHz":100,"lastHarmonic":5}
```

At least eight spectrum bins are required; frequencies and half-width must be
positive, and `lastHarmonic` is an integer 2..20 (default 5). The existing
`analyzeTonalSpectrum` supplies noise-subtracted SNR, SINAD=S/(N+D), THD/THD+N,
harmonics measured/requested and complete-span status. Its minimum integration
half-width is 1.5 bins; it excludes DC. Fundamental/harmonics use the **absolute
supplied frequency axis**, not offsets from `centerHz`. These estimates do not
establish signal identity. Partial harmonic coverage is explicitly reported;
unresolved signals and nonfinite ratios are invalid/null, not made-up numbers.

Every scalar metric is `{ "value": number|null, "valid": bool, "unit": string }`,
with `reason` on invalid values. `ok:true` means the request was processed, not
that every optional metric is valid. Core names and canonical metadata accompany
snapshots/pages. No unavailable/degenerate measurement is encoded as valid zero.

## Paging And Export

Reads accept `view: "samples"` (default) or `"metrics"`. Pages include `columns`
with names/units, `rows`, `offset`, `total`, `returned`, `nextOffset` (null at end),
metadata and a string `revision`. Pass that revision on subsequent pages to reject
replacement with `code: "stale_revision"`; without it reads target the latest
snapshot. Offset must be a nonnegative integer; offsets past the end return an
empty page. Metric rows are `[name, value, valid, unit, reason]` in sorted order.
`status` returns bounded summaries, never sample arrays.

```json
{"operation":"iq.export","format":"json","offset":0,"limit":256,"revision":"1"}
{"operation":"spectrum.export","format":"csv","view":"metrics","path":"research-metrics.csv"}
```

Export serializes **one page** using identical paging/revision rules. Omit `path`
for inline UTF-8 `content`; provide a nonempty path of at most 4,096 characters
for explicit file creation. Existing files are never overwritten, parent
directories are never created, and incomplete writes are reported and removed.
The parent must authorize/restrict paths before exposing this to remote clients.
CSV uses LF, C-locale 17-digit numbers, quoted/escaped text and unit-bearing
headers; every row repeats source, sample rate, center, amplitude unit and revision.
Metric CSV includes validity, per-metric units and reasons; sample rows are finite
by validation. JSON contains the full page schema. Export replies include byte
and row counts; subsequent pages must use distinct new paths. CSV is plain data,
not an Excel workbook, and does not neutralize spreadsheet formula-like strings.

## Extracted Pure Metrics

`src/research/headlessresearchmetrics.{h,cpp}` exposes bounded, synchronous
computation/data helpers with no QtGui headers, QObject, clocks or I/O. Formulas
are extracted from the actual GUI implementations below; GUI wiring and behavior
are unchanged. The CLI calls these pure helpers rather than maintaining separate
calculation formulas. Direct core calls independently validate bounds and inputs.

All tools below support `.status`, `.read`, `.export` and `.clear`. Reads/exports
use the same `view: samples|metrics`, revision, paging, JSON/CSV and create-only
file rules above. `status` includes every tool, without raw arrays. Numeric table
boolean columns are 0/1. Null table entries have companion validity information.

### Density

```json
{"operation":"density.start","metadata":{"source":"test","sampleRateHz":100,"centerHz":50,"amplitudeUnit":"dBFS"},"columns":2,"historyFrames":2,"retainPercent":50,"minimumDb":-100,"maximumDb":0,"mode":"count"}
{"operation":"density.append","metadata":{"source":"test","sampleRateHz":100,"centerHz":50,"amplitudeUnit":"dBFS"},"frequenciesHz":[0,100],"levelsDb":[-100,0]}
{"operation":"density.read","offset":255,"limit":2}
{"operation":"density.export","format":"csv","offset":0,"limit":512}
```

`DensityHistogram` follows `SignalDensityWidget::appendSpectrumFrame`, not the
colored `SpectrumPersistence` raster. `density.start` explicitly configures and
clears accumulation. Defaults: 512 columns, 256 history frames, 98% retention,
-140..-20 dB, count mode. Limits: columns 1..1024, history 1..4096, their product
at most **1,048,576**, retention 0..100%, finite minimum/maximum in -200..100 with
maximum > minimum. The histogram always has **256 amplitude rows**.

Each explicit append decays every cell by `retainPercent/100`, evicts the oldest
retained frame's weights multiplied by `retain^historyFrames`, then adds a frame.
Incoming levels are linearly interpolated **in dB** to the configured columns.
Row is `clamp(lround((maximum-level)*255/max(1,maximum-minimum)),0,255)`.
Count weight is 1; `mode:linearPower` uses `10^((level-maximum)/10)` without
clipping that weight. Thus units are **decayed frame count** or **decayed
relative-power sum**, never probability, seconds, watts or pixel brightness.

Append requires the actual source/rate/center/unit to match the selection exactly.
Grid endpoints changing by more than `max(1 Hz, span*1e-4)` clear old contributions
as in the GUI, with `rangeReset:true` reported. No automatic timer/interval gate
is applied. A read/export does not decay history. Before the first frame, the
frequency axis is unknown and the sample table is empty.

Rows are `[column,row,frequencyHz,levelDb,density]`, ordered column first, then
amplitude row 0..255 (maximum down to minimum). Frequencies interpolate the actual
first/last FFT bins. All cells, **including zeros**, are paged, giving stable
offsets and at most 262,144 total cells. Unlike GUI sparse CSV, this deliberately
avoids content-dependent page offsets. The core stores only bounded contribution
rows/weights, not input FFT history.

### Pulses

```json
{"operation":"pulse.analyze","metadata":{"source":"test","sampleRateHz":8000,"centerHz":100000000,"amplitudeUnit":"dBFS"},"timeBase":"observation","timeMs":[0,10,20,30,40],"levelsDb":[-100,-60,-20,-60,-100],"thresholdMode":"absolute","thresholdDb":-70,"hysteresisDb":10}
```

`analyzePulses` follows `PulseAnalysisWidget::Impl::analyze/effectiveThreshold`.
Supply **3..65536** paired observations, finite levels -200..100 dB and strictly
increasing nonnegative **integer milliseconds <= 2^53-1**. Explicit `timeBase`
is `observation` or `sample`; the supplied sample rate is source metadata, **not**
the observation cadence. No timestamps are read from a clock or invented from
array length. Threshold defaults to -80 dB, hysteresis to 3 dB, with accepted
bounds -300..300 and 0..300 dB respectively. `thresholdMode:relative` adds the
ordered level at index `n/5` to `thresholdDb`; default mode is `absolute`.

A pulse starts at the first observed level >= threshold and ends at the first
subsequent observation <= threshold-hysteresis. No crossing interpolation is
performed. Rise/fall are sampled 10/90% crossings of the **dB excursion** from
release threshold to pulse peak, not linear-voltage rise time. Rising-edge search
looks back at most 12 observations, exactly as in the GUI. Width/PRI/rise/fall
are ms; repetition is `1000/meanPRI` Hz; duty is total completed widths divided
by the first-to-last observation interval, in percent.

Unfinished final pulses are omitted and `trailingActive` is explicit. A pulse
already high at the first observation is retained, with `leftCensored:1`; its
width is the observed width, not a claim about the unknown earlier edge. No-event
count/duty are valid zero; width/rise/fall are invalid/null without completed
pulses, PRI/repetition without two. Event rows:
`[startMs,endMs,widthMs,riseMs,fallMs,peakDb,leftCensored]`.

### Constellation And Eye

```json
{"operation":"constellation.analyze","metadata":{"source":"test","sampleRateHz":8000,"centerHz":100000000,"amplitudeUnit":"normalized"},"symbolRateHz":2000,"phaseFraction":0.5,"iq":[0.5,0.25,-0.5,-0.25,0.5,0.25,-0.5,-0.25]}
{"operation":"eye.analyze","metadata":{"source":"test","sampleRateHz":8000,"centerHz":100000000,"amplitudeUnit":"normalized"},"symbolRateHz":2000,"phaseFraction":0.5,"traces":32,"iq":[0.5,0.25,-0.5,-0.25,0.5,0.25,-0.5,-0.25]}
```

`buildIqViews` extracts the **2D ResearchAnalysisDialog** algorithm. IQ must have
1..65536 pairs with components in [-1e12,1e12]; this additional numerical bound
keeps float interpolation safe. DC I/Q are removed and reported; amplitudes remain
normalized amplitudes, not auto-gain-scaled coordinates. `symbolRateHz` is required,
positive, and must yield >=2 finite samples/symbol. Phase is 0..1 (default 0.5).

Constellation samples linearly interpolate at `phase*samplesPerSymbol`, then each
symbol, while `position+1 < inputCount`, capped at **3000 points**. Rows:
`[index,timeSeconds,samplePosition,i,q]`. This is the GUI's bounded first-symbol
sampler, not its separate 3D stratified-jitter trajectory preview or a decoder.

Eye traces start one symbol apart at that same phase, each spanning **two symbols**
with **192 inclusive points**. Only complete traces for which
`start+2*samplesPerSymbol+1 < inputCount` are included. `traces` is 1..128 (default
32), so at most **24,576 points** are retained. Rows:
`[trace,point,phaseSymbols,timeSeconds,i,q]`; `phaseSymbols` runs 0..2 and time is
absolute within this supplied IQ block. Read paging may cross trace boundaries.
`limited` reports when the point/trace cap omitted available data; insufficient
input yields zero points/traces, never a fabricated eye. No EVM, eye-opening,
automatic synchronization, decoding or lock metric is asserted.

### IQ Phase And Comb

`iq.phase.analyze` takes explicit normalized metadata and 2..65536 IQ pairs in
[-1e12,1e12], like the view input. `analyzeIqPhase` extracts the GUI's DC-centered
adjacent phase-step mean/std in Hz, pseudo-power image estimate in dB, and
129-lag I autocorrelation. The latter uses **every fourth pair** but full-block
variance for normalization, so periodic inputs can produce values beyond +/-1
(including lag zero != 1); it is intentionally not redefined. Rows are
`[lag,autocorrelation,valid]`; unobserved lags/zero I variance are invalid/null.
Pseudo-power excludes the first sample as in the GUI. Its singular 99 dB GUI
sentinel is reported invalid/null, not a measured image ratio. Zero AC power
invalidates frequency metrics. These are block diagnostics, not carrier recovery.

`comb.analyze` takes explicit spectrum metadata, uniform `frequenciesHz/levelsDb`
as above (at least 5 bins), `prominenceDb` (default 6, 0..300),
`minimumSpacingHz` (default 100, >0), `maximumSpacingHz` (default 1e6, >=minimum,
<=2e15), and optional matching `referenceLevelsDb` on **the same supplied axis**.
`analyzeComb` extracts `ResearchAnalysisDialog::updateInterference`: 40th-percentile
noise floor, asymmetric adjacent-peak ties, four-neighbor local prominence,
**96 strongest peaks**, spacing-vote resolution `max(10 Hz,2*binWidth)`, membership
tolerance `max(3*binWidth,1.5%*spacing)`, >=4 members and at most **3 families**.
This identifies candidate spacing patterns, not emitter identity or certainty.

Rows are `[bin,frequencyHz,levelDb,prominenceDb,referenceDeltaDb,family,harmonic,referenceValid]`.
Reference deltas are null without a reference; harmonic is null without family
membership, where family is 0. Family summaries provide `familyN.stepHz`,
`familyN.anchorHz` and `familyN.members`. `stepDriftHz` and `amplitudeDriftDb`
compare the last detected primary family for the same source, grid and options;
non-detections retain that prior detection, as in the GUI. First observation or a
changed source/grid/options invalidates drift rather than reporting fake zero.

Only genuinely unavailable coherent-hardware operations (`coherent.*`) return
`code:"unsupported"`; widget access modifiers are not an unsupported condition.
Unknown operations return `code:"unknown_operation"`; no data returns
`code:"no_data"`. The helper itself has no `capture`/`cancel` job: those are parent
control operations, keeping the reusable computation API inert while idle.
