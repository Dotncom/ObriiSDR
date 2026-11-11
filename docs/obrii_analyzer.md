# Obrii Analyzer

`ObriiAnalyzer` is the standalone laboratory-instrument interface built from
the same statically linked measurement core as Obrii SDR. Extracting the code
into `ObriiMeasurementCore` does not add a DLL boundary or a per-frame virtual
call to the receiver path.

## Starting it

- Open **Spectrum measurement -> Obrii Analyzer** in Obrii SDR to launch the
  analyzer with `--connect`. The main application keeps ownership of the SDR;
  the analyzer receives a bounded local spectrum stream.
- Run `ObriiAnalyzer` directly to inspect an Obrii spectrum-frame recording.
  Use **Open recording**, then Play/Pause and Replay speed.
- **Connect Obrii** attaches a directly launched analyzer to a running Obrii
  SDR instance on the same computer.
- The analyzer follows the English/Ukrainian interface language selected in
  Obrii SDR's general settings, including controls, sweep states, measurement
  summaries and file dialogs.
- Without Obrii SDR, select a detected Fobos Standard/Agile, HackRF or native
  RTL-SDR device, set center frequency, sample rate, FFT and gains, then press
  **Start receiver**. This is a deliberately small spectrum-only acquisition
  path and does not start Obrii SDR's audio, decoder or workspace subsystems.
- Center frequency can be entered in GHz, MHz or kHz without changing its
  physical value. While the standalone receiver is active, center and gains
  apply live; sample rate, FFT/Hz-per-bin, input and display-reduction changes
  perform a short automatic receiver-session restart. Controls remain locked
  only when a running Obrii SDR instance owns the source.
- **Prefer running Obrii SDR** periodically discovers the local application.
  If it appears while the analyzer owns a receiver, the analyzer first stops
  and releases its USB session and then consumes Obrii SDR's spectrum stream.
- **Multi-SDR...** opens two independent receiver sessions and combines their
  spectra on one absolute-frequency axis. For a Fobos + HackRF test, run
  `ObriiAnalyzer` directly while Obrii SDR is closed, select one physical
  device in each RX panel, place their centers on adjacent or overlapping
  bands, and press **Start both**. Left-dragging the mosaic retunes both
  receivers by the same delta by default; either receiver can be selected as
  the sole pan target.

## Multi-SDR mosaic

- The main Obrii SDR window exposes the same **Multi-SDR...** tool under
  Spectrum measurement. RX 1 is then the already running main receiver, so its
  proven audio, decoders and IQ path remain untouched; RX 2 and later slots are
  independent spectrum sessions. **Add receiver** creates slots up to RX 8 and
  **Remove last** releases them. The standalone Analyzer can instead own every
  slot itself.
- Each RX has independent input/channel, center, sample rate, FFT and gain.
  **Discard each edge** removes unreliable analog/FFT edge regions before
  stitching. **Level correction** compensates a measured gain difference
  between receivers.
- Frames are aligned by timestamp. A source older than **Max frame skew** is
  excluded instead of smearing old data into the current mosaic. Gaps remain
  gaps, while overlapping bins are interpolated and blended in linear power.
- This is a spectrum mosaic, not raw-IQ concatenation. Independent Fobos and
  HackRF oscillators are not phase/time coherent, so their IQ cannot honestly
  become one demodulatable complex stream. Coherent IQ joining is reserved for
  timestamped, shared-clock channels such as a validated multi-channel USRP.
- Receiver sessions own their native Fobos/HackRF/RTL/Soapy handles. The same
  physical device cannot be opened twice, while distinct devices can run in
  parallel if their USB controllers and the host sustain the aggregate rate.
- Multi-SDR runs only while its dialog is open. Its receiver, FFT and mosaic
  workers are inactive otherwise, so it adds no load to ordinary Obrii SDR or
  Raspberry Pi use.

The local stream is inactive while no analyzer is connected. It is reduced to
at most 65,536 display bins with peak preservation so a large source FFT cannot
stall the receiver UI. The status displays the original FFT length and actual
transport-bin width. This live display channel is intended for interactive
instrument use; certified full-resolution export continues to use the existing
recording and experiment-bundle paths.

## Current controls

- explicit upper/lower amplitude limits from -200 to +100 dBFS, persisted per
  user and mirrored by the two vertical sliders beside the plot;
- backend-specific sample-rate lists for Fobos, HackRF and RTL-SDR. SoapySDR
  devices use runtime capability probing for RX/TX channel count, duplex mode,
  tuning range, sample rates and bandwidths; this is the first USRP-compatible
  standalone acquisition path and deliberately does not hard-code B210 limits;
- FFT can be selected directly or derived from a requested Hz/bin value. The
  analyzer shows the resulting even FFT length and actual frequency-bin width;
  both paths support lengths from 512 through 67,108,864 points;
- FFT and Hz/bin are synchronized fields. With Hz/bin checked, its green field
  is editable and FFT shows the exact calculated (possibly non-power-of-two)
  length; without the check, FFT is green/editable and Hz/bin follows it. The
  inactive field is visibly muted. Disabling Hz/bin removes its temporary exact
  FFT entry and selects the nearest standard FFT length;
- a standalone receiver can use automatic output limiting or an explicit
  1:1 through 1:4096 FFT-output ratio. Reduction matches Obrii SDR's Peak,
  dB-average, linear-power average, center-sample and Minimum methods. A point
  count and high-memory warning are shown before acquisition. When attached to
  a running Obrii SDR, its already reduced local stream remains authoritative;
- the bottom Scale slider crops the visible span without changing receiver or
  FFT settings; left-drag pans a zoomed spectrum inside the captured span;
- Sample, Positive/Negative peak, RMS, Average, Median and Quasi-peak detectors;
- VBW, detector frame count and replay speed;
- independently visible current, Max hold, Min hold and Average traces, so the
  current green line can be hidden while inspecting accumulated traces;
- optional P50/P90/P99 calculations in the shared analyzer core;
- ten ready-to-use markers (A/B and M3-M10) with click placement, peak and
  adjacent-peak navigation. The Pairs table starts with A/B and permits adding
  arbitrary parallel delta pairs; its first row is the detailed range;
- Shift + left-drag selects the current primary-pair measurement range. The status line and its
  tooltip report span, bin count, peak, noise, SNR, channel power, -3/-6/-20 dB
  widths, OBW 90/95/99 and adjacent-channel power ratios;
- current-frame CSV export.
- a standalone wide sweep across a user-defined start/stop range. The sweep
  plans overlapping receiver captures, discards configurable edge regions,
  optionally averages multiple frames per segment and blends overlaps in
  linear power. The displayed stitched result is capped at 4,194,304 bins to
  bound RAM use; this changes display sampling, not RF capture. One-shot mode
  leaves the result on screen and stops the receiver. Continuous/live mode
  updates each completed segment immediately and starts another pass without
  stopping RX. Fobos Agile automatically uses its native indexed scan API for
  2-256 centers and FFT lengths up to 4,194,304; other cases use the universal
  live-retune path. **Fobos sweep mode** can override this choice: Agile native
  forces indexed hardware scanning, while Normal retune forces the universal
  sequential path for direct comparison and troubleshooting. **Start wide
  sweep** now opens and starts the selected receiver itself; stopping or
  completing the sweep also stops and releases that receiver. Sample-rate,
  FFT/Hz-per-bin and display-reduction changes made during a sweep restart the
  same sweep with the new parameters instead of dropping into ordinary RX.
- two-stage display zoom matching the main application: the primary slider
  covers 0.1-100 percent of the selected span and the second slider divides it
  by 1-100. This permits sub-pixel-Hz inspection of a 50 MS/s capture when the
  source FFT has sufficient resolution; left-drag pans inside the source span.

## Instrument laboratory

The **Instrument lab...** button opens a hardware-gated workspace for every
planned analyzer instrument stage:

- tracking-generator stimulus sweeps;
- scalar network analysis with through/reference normalization;
- return loss and SWR with a characterized directional bridge and Open/Short/Load;
- coherent VNA planning with two RX channels, simultaneous TX, timestamps,
  shared reference, thru and phase calibration;
- editable external-generator/receiver SCPI command profiles;
- reproducible measurement-plan and session JSON files with DUT, operator,
  calibration identity, capabilities, event log and result-point schema.

The shared measurement core now owns the capability model, validation rules,
frequency-grid generation, return-loss/SWR formulas and JSON round trips. A
small backend interface is the future boundary for native UHD, so RF handles do
not leak into the UI and capability/safety checks cannot be bypassed.

Set the frequencies and timing under **Measurement plan**, then open
**Simulation → Run simulation**. The virtual DUT is an ideal one-pole low-pass
with a 10 MHz cutoff. The table shows forward level, S21, return loss, SWR
and S21 phase. The run uses the plan's settle, dwell and averaging values.
A bidirectional run starts the return pass at the next lower frequency.
**Stop** keeps the completed points. **Save results…** writes a session JSON
with the simulation backend ID and a completed or cancelled outcome.

**Execute RF** requires a qualified hardware adapter. Physical SNA,
return-loss and VNA measurements require their corresponding normalization
or fixture calibration.

Plan validation rejects non-finite source/safety levels, invalid hardware
frequency ranges and reference impedance, and available RX/TX devices without
declared channels. Return-loss measurements require simultaneous RX/TX or an
external generator. Frequency-grid generation rejects invalid or oversized
inputs before allocating memory. Plan import checks the format version and
instrument mode; plan and session-template saves use atomic replacement and
report write failures.

For existing complex reflection measurements, use **Calibration → Correct
recorded S11 (Open / Short / Load)**. The input is a JSON object with a `points`
array sorted by `frequencyHz`. Each complex value is `[real, imag]` and each
standard array is ordered Open, Short, Load. An ideal-standard example is

```json
{"points":[{"frequencyHz":150000000,"standards":[[1,0],[-1,0],[0,0]],"measuredStandards":[[0.7,0],[-0.5,0],[0.1,0]],"measuredDevice":[0.25,0]}]}
```

Use the characterized standards at each frequency for physical measurements.
The saved JSON contains complex `s11`, `returnLossDb` and `swr` per point. A
non-finite metric is `null`. The operation uses the supplied measurements and
does not start RF or capture calibration data.

The current Windows bundle contains UHD command-line/runtime components used by
the wireless toolchain, but not a SoapyUHD device module. A future USRP can be
used through this acquisition path once a matching SoapyUHD module is deployed;
native UHD transport is tracked separately in `usrp_b210_plan.md` so the
analyzer does not silently depend on Wi-Fi tooling.
