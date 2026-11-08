# Digital Phosphor Contours

## Controls and compatibility

Choose **Spectrum persistence**, then **Phosphor: Contours** in the normal
visualization controls. **Classic** remains the default, including for old INI
settings and initial JSON restores without the new key. The existing Classic
amplitude-smoothing behavior is unchanged. Line-spectrum smoothing is independent.
Contours ignores the Classic amplitude-smoothing value; its control is disabled
without discarding that value. Returning to Classic restores that setting.

The DSP board's workspace-spectrum editor and spectrum-settings editor expose
linked copies of these controls. They change the same global phosphor settings,
not per-block waterfall profiles. Each workspace spectrum owns its own history,
including its own pause state and dBFS range. Waterfall and VFO displays are not
converted to phosphor views.

Afterglow, intensity and clutter threshold apply to either style. Style changes
clear history because Classic's additive density and Contours' normalized
occupancy have different units. Level range, frequency domain and raster width
changes also clear incompatible history. Brightness/threshold changes redraw
existing history without accumulating a new sample.

## Accumulation and rendering

The raster is bounded to 1536 frequency columns by 256 amplitude rows. For each
incoming frame and frequency column, normalized raw amplitude selects the two
nearest amplitude rows. Their fractional weights form a narrow kernel K whose
mass is one. No broad brush, frequency-column convolution or cross-gap line
joining is used in Contours.

```text
lambda = exp(-dt / T)
H[x, y] = lambda * H[x, y] + (1 - lambda) * K(rawAmplitude[x], y)
R[x, y] = max(0, H[x, y] + k * (H[x, y] - Gaussian_y(H)[x, y]))
k = 1
sigma_y = 2 rows (13 taps, radius 6)
tone = clamp(log(1 + 40 * visible * intensity) / log(41), 0, 1)
```

The Gaussian samples only amplitude rows in the same frequency column and clamps
at the top/bottom amplitude boundaries. It is used only for contrast subtraction;
it never broadens the occupancy deposit. Clutter gating tests raw H before
contrast or brightness. Above the gate, `visible = max(0, R - threshold) /
(1 - threshold)`. A lookup table quantizes the log-mapped palette input to 1/1024
occupancy units. The existing cyan/green/yellow/red palette is retained.

Explicit mosaic gaps (`preserveGaps = true` and a non-finite level) clear that
column, including earlier coverage, so sharpening cannot paint missing coverage.
Without the explicit-gap flag, missing observations simply decay old occupancy.
Frame-rate invariance follows from using the same T for decay and injection.
Invalid or nonpositive dt does no contour work.

Accumulation and rendering happen on incoming active display frames, never on a
timer or repaint. Line mode allocates no phosphor history; paused/hidden views do
not accumulate. No Contours rendering runs in Classic mode. Pause/resume resets
the display clock rather than treating pause time as newly observed occupancy.

## Persistence and integration

`ui/spectrumPersistenceStyle` in INI and `spectrumPersistenceStyle` in JSON store
`0` (Classic) or `1` (Contours). Values are clamped on restore. Partial JSON
updates retain the current style when the field is omitted, following existing
settings behavior. Histogram history is transient and is not serialized.

The new public widget setter is `MyGraphWidget::setPersistenceStyle(int)`;
`SpectrumPersistence` exposes `Style::Classic` and `Style::Contours`. The existing
four-argument `setParameters` contract is preserved.

The accumulator receives display amplitudes and does not smooth them in Contours.
It cannot reverse preprocessing already applied by an upstream FFT/detector,
baseline correction, display reduction, or transport. Any upstream line-only
smoothing bypass must be wired separately at that owner's processing boundary.
For the normal view, the active Contours condition is
`spectrumDisplayMode == 1 && spectrumPersistenceStyle == 1`.
This visualization is occupancy contrast, not RF source separation or new
measurement resolution.

No CMake additions are needed: the implementation stays in the existing header
and the existing `ObriiSpectrumPersistenceSelfTest` target already links Qt Gui.
Suggested `apphelp.cpp` addition for its owner: document Classic versus Contours,
the disabled Classic smoothing control, shared DSP-board controls, and the fact
that Contours does not separate RF sources. That file is intentionally untouched.

## Verification and optional images

The persistence selftest retains Classic checks and adds reference-formula,
fractional-kernel, boundary, frequency-isolation, two-state, frame-rate, decay,
clutter-gate, parameter-redraw, style-reset, invalid-dt and bounded-storage checks.
The integrated Windows selftest passed and produced the optional deterministic
comparison rasters. Static checks include `git diff --check` and translation JSON
parsing. A live receiver GUI has not been launched by the test.

After an authorized build, the existing first optional output remains the Classic
synthetic preview. A second output saves the matched two-state comparison from
the prototype: Classic with 200 ms smoothing on top, Contours below, with an
explicit gap near the right side. A third output saves the Contours raster alone:

```powershell
ObriiSpectrumPersistenceSelfTest.exe classic.png phosphor_comparison.png contours.png
```

These are deterministic raster smoke fixtures, not screenshots of the live GUI.
A later GUI smoke test should check normal and DSP-board selectors, disabled
smoothing in Contours, pause/resume, INI/JSON restore and legacy defaults. Actual
GUI layout and runtime performance still require a separately authorized launch.
