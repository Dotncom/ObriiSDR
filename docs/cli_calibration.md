# On-demand CLI receiver calibration

The `CliCalibration` Qt Core helper is integrated through `ObriiCli control
calibration select|status|cancel|request FILE.json` and offline `ObriiCli
calibration FILE.json` batches. The helper has no QObject, worker,
timer, event loop, device control, settings access, or automatic application.
No work is scheduled while idle. It does not read personal application settings,
device serials, credentials, location, or API keys.

## Integration API

```cpp
#include "cli/clicalibration.h"

CliCalibration calibration;
QJsonObject reply = calibration.command(request);
if (calibration.capturing()) {
    QJsonObject event = calibration.submitSpectrum(frequenciesHz, levelsDbfs, key);
    // Forward nonempty events through the parent's event channel.
}
```

- `command(const QJsonObject &request)` returns a JSON object with `ok` and,
  on failure, `error`. `operation` chooses the command.
- `submitSpectrum(const std::vector<float> &frequenciesHz,
  const std::vector<float> &levelsDbfs, const ReceiverNoiseBaselineKey &key)`
  returns `{}` immediately when idle, without examining arrays. During capture it
  returns `event: "calibrationCapture"`, `ok`, and `result`: `accepted`,
  `coldCompleted`, `completed`, or `rejected`. Normal events also include `status`.
- `capturing() const` is a constant-time hook gate. Parent dispatch and spectrum
  submission must run serially on one thread. The helper is not thread-safe.
- Sources: `src/cli/clicalibration.cpp`,
  `src/research/receivercalibrationtable.cpp`,
  `src/research/receivernoisebaseline.cpp`,
  `src/research/receiveryfactorcalibration.cpp`; link Qt Core, C++17. No moc, GUI,
  FFT, SDR backend, or network dependency is needed.
- Test source: `tools/cli_calibration_selftest.cpp`, using the same sources/link
  dependencies and include directory `src/cli`. CMake/CLI wiring is intentionally
  left to the parent. No existing help or documentation is modified.

The parent should pass actual current receiver and FFT settings with every frame,
not merely echo the selection key. Feed spectra before baseline/Y-factor display
processing. If amplitude offset/table correction was already applied, describe
that state accurately in the key. The helper never changes receiver settings and
does not automatically apply the loaded table to submitted spectra.

## Selection And Units

```json
{"operation":"profile.select","key":{"receiver":{"backend":"test","input":0,"rate":4000000,"lna":-1,"vga":-1,"rtlGain":-1,"gainState":-1},"centerFrequencyHz":100000000,"fftLength":4,"fftWindowType":1,"rfBandwidthHz":0,"amplitudeOffsetDb":0,"calibrationTableEnabled":false,"calibrationPointCount":0}}
```

`receiver` is the GUI `ReceiverCalibrationContext` JSON model. `rate` is Hz;
`rtlGain` is tenths of dB; other gain/input fields retain the GUI's integer
semantics. Missing backend is wildcard/unspecified, missing integer context fields
are `-1`, and missing rate is `0`. A minimal table-only selection is
`{"operation":"profile.select","key":{"receiver":{"backend":"test"}}}`.
Noise/Y-factor operations additionally require a nonempty backend, positive rate,
and `fftLength >= 2`. Other key fields default to zero/false. They follow
`ReceiverNoiseBaselineKey::matches`, including its frequency/rate/offset tolerances.
Table selection uses the core's most-specific matching profile, never blends
wildcard points with more-specific points, linearly interpolates inside a profile,
and holds endpoint corrections outside it. No matching points means `valid:false`
and null corrections, not an implicit zero correction.

Input levels are **power dBFS**, not amplitude-linear values, dBm, or dB/Hz. An
optional `unit` must be exactly `"dBFS"`; all spectrum-consuming commands reject
other units. Offset-corrected levels are still labeled dBFS with
`absolutePowerCalibrated:false`. Noise and Y-factor application returns relative
dB with an explicit reference unit, never dBm. The helper does not establish an
absolute power reference, RBW, or power spectral density normalization.

## Table Commands

```json
{"operation":"table.load","calibrationTable":[{"frequencyHz":98000000,"frequencyOffsetHz":2,"amplitudeOffsetDb":1,"uncertaintyDb":0.2,"note":"reference","conditions":{"backend":"test"}},{"frequencyHz":101000000,"frequencyOffsetHz":8,"amplitudeOffsetDb":4,"uncertaintyDb":0.4,"conditions":{"backend":"test"}}]}
{"operation":"table.correction","frequencyHz":99500000,"levelDbfs":-80}
{"operation":"table.export","path":"calibration.json"}
{"operation":"table.load","path":"calibration.json"}
{"operation":"status"}
```

- `table.load`: either inline `calibrationTable` or a JSON object file, not both.
  Rows use the GUI settings-export field names. Frequency and amplitude offsets
  are required; uncertainty defaults to zero, note to empty, conditions to
  wildcard. Numeric strings, nulls, nonfinite values and negative uncertainty are
  rejected atomically. Unknown fields are ignored and never exported. Notes are
  limited to 256 characters and backend IDs to 128. The core sorts/deduplicates
  same-profile frequencies within 0.001 Hz and trims notes.
- `table.correction`: requires a selection; returns `valid`, `frequencyOffsetHz`,
  `amplitudeOffsetDb`, `uncertaintyDb`, and optional `correctedLevelDbfs` (input
  plus amplitude offset). It does not retune or mutate a spectrum. The parent
  decides how to apply frequency correction using its receiver convention.
- `table.export`: returns `document` when path is absent, or creates a new JSON
  file. The document contains only `schema: "obrii.cli.calibration.table"`,
  `version: 1`, and the calibration rows. It is not a general settings export.
  Loading also accepts an object with only `calibrationTable`; supplied schema
  and version must match. No automatic settings-file discovery occurs.
- `table.clear`: empties the table. Successful load/clear also clears all noise,
  Y-factor, and pending cold data, because the shared profile key contains a
  point count but not a table-content hash. Failed loads leave all state intact.

## Y-Factor

For batch computation use `yfactor.compute`, `frequenciesHz`, `coldFramesDbfs`,
and `hotFramesDbfs`. Both frame fields are arrays of 8..512 actual measured
spectra, each spectrum an array matching `frequenciesHz`. Frames are averaged
in linear power by the shared module; they are not averaged in dB. For example,
four-bin cold frames `[-100,-100,-100,-100]` and hot frames approximately
`[-92.596373,-92.596373,-92.596373,-92.596373]`, each measured eight times, with
290 K cold and 2900 K hot produce about 290 K equivalent noise temperature and
3.0103 dB noise figure. Supply real frames, not duplicates to imply more samples.

Source fields, shared by `yfactor.compute` and `yfactor.hot`:

- `sourceMode: "temperatures"` (default): `coldTemperatureKelvin` defaults to 290;
  `hotTemperatureKelvin` must exceed it.
- `sourceMode: "enr"`: `enrDb` defaults to 15, range `(-20,100]`; the core uses
  hot temperature = cold temperature + 290 K * `10^(enrDb/10)`.

Batch calculation is atomic: a bad frame or entirely invalid Y-factor result
leaves previous profiles intact. Partially valid results retain invalid bins.
The GUI module computes `Te = (Thot - Y*Tcold)/(Y-1)` and
`NF = 10*log10(1 + Te/290 K)`, rejecting bins with invalid ratios/negative Te.

For live capture, issue `yfactor.cold` with optional `frames` (default 8), deliver
frames through `submitSpectrum`, wait for `coldCompleted`, physically switch the
source without changing settings, then issue `yfactor.hot` with source fields
and deliver hot frames. Cold and hot grids must match exactly after float
conversion. The hook does not switch sources or infer which state is present.

`yfactor.read` returns the selected table, source settings, frame counts,
`validBins`, and per-bin `valid`, hot/cold levels, `yDb`,
`equivalentNoiseTemperatureKelvin`, and `noiseFigureDb`. Invalid scientific
values are JSON null, never zero. `yfactor.export` creates the same paged report
or returns it as `document`; it is a report, not a reloadable profile store.

`yfactor.apply` takes `frequenciesHz`, `levelsDbfs`, and optional `targetFloorDb`
(default 0). The shared module performs input-referred cold normalization;
the cold state maps to `targetFloorDb`. Returned `levelsDb` have unit
`"dB re cold input"`, with the reported target offset added. Application requires
a matching profile covering the entire requested span. It rejects interpolation
touching any invalid temperature bin, since the core otherwise leaves such bins
unchanged. It never returns a mixture of dBFS and corrected relative levels.

## Noise Reference

`noise.capture` with optional `frames` arms linear-power reference averaging via
`submitSpectrum`. `noise.read`/`noise.export` return frame count, capture time,
spread in dB, and per-bin `referenceLevelDbfs`. `noise.apply` takes
`frequenciesHz`, `levelsDbfs`, optional `targetFloorDb` (default 0), and
`mode: "relative"` (default) or `"excess"`. The shared formulas are respectively
target + `10*log10(P/Preference)` and target +
`10*log10(max(P/Preference - 1, 1e-12))`. Output unit is
`"dB re noise reference"`, not an absolute floor in dBm. `noise.clear` and
`yfactor.clear` remove only the selected matching profile and pending capture
state. Profiles otherwise stay in memory until object destruction or replacement.

## Bounds And Lifecycle

- Maximum table: 4096 rows; file input: 4 MiB. Frequency/rate bounds: 0..1e12 Hz;
  offsets/uncertainty: amplitude +/-300 dB, uncertainty 0..300 dB, frequency
  offset +/-1e12 Hz. Source bounds: cold `(0,1e9]` K, supplied hot `[0,1e12]` K
  (ENR-derived hot temperature follows the ENR formula above).
- Spectra: 2..32768 bins, equal array lengths, finite nonnegative ascending
  uniformly spaced frequencies (float-precision tolerance), levels -300..300
  dBFS. Live capture grids cannot change even if endpoints remain the same.
  Apply spans may be subsets but cannot extend beyond the captured range.
- Captures: 8..512 frames per stage, one stage at a time, automatic stop at the
  target. Batch total: at most 1048576 hot+cold level samples. The core retains
  at most 16 profiles for each of noise and Y-factor, evicting oldest entries.
- `noise.read`, `yfactor.read`, and their exports accept `offset` (0..32768,
  default 0) and `limit` (1..4096, default 256). Reports include total and
  returned bin counts. Exports are paged too; request further pages explicitly.
- Exports require a new path and never overwrite existing files. Paths are
  explicit caller input; parent dispatch is responsible for access policy.
- `capture.cancel` drops active and pending captures, preserving completed
  profiles. Key/grid/level mismatch rejects and cancels capture and pending cold.
  Profile selection, table replacement, and clearing are rejected while actively
  capturing. Selecting a different key also discards pending cold data.
- An armed capture with no submitted frames consumes no background CPU and
  waits until explicit cancellation or submission. No timeout thread is created.

This is bounded calibration parity, not a scientific-tool dispatcher: there is
no streaming auto-correction, absolute dBm conversion, source control, FFT/RBW
calculation, profile-settings import, network fetch, or personal persistence.
