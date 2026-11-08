# FobosAPP 4.8.0

FobosAPP 4.8.0 is a desktop and Raspberry source update focused on scientific
spectrum analysis, 3D inspection, calibration, and reliable per-user settings.

Highlights:

- Spectrum-analyzer detector modes, RBW/VBW, FFT overlap, finite and
  exponential averaging, max/min hold, and percentile traces.
- Automatic signal measurements including peak data, SNR, channel power,
  occupied bandwidth, bandwidth at several level offsets, and ACPR.
- Zero Span with threshold/edge triggering, pre-trigger and post-trigger data,
  single-shot capture, timestamps, and CSV export.
- Research views for interference combs and harmonics, IQ waveform and
  constellation, autocorrelation, and comparison of the two HF inputs.
- Frequency-dependent receiver calibration tables with interpolation,
  uncertainty, external path corrections, and preset editing.
- Expanded scientific metadata for recordings and SigMF-compatible IQ data.
- `Fix plane` for the normal 3D waterfall, matching the Alternative interface
  projection while preserving the normal surrounding UI.
- Dense gradient fill for the visible fixed-plane front face with adjustable
  opacity and the active spectrum palette.
- Cleaner 3D controls with capture options on a separate row.
- Per-user settings at `%LOCALAPPDATA%\FobosAPP\FobosAPP.ini` on Windows and
  `~/.config/FobosAPP/FobosAPP.ini` on Linux. A legacy INI beside the executable
  is migrated automatically, removing the need to run as administrator.

Performance notes:

- Adaptive multithreaded FFT and the GPU-backed 3D renderer remain enabled.
- Multi-million-point FFT sizes are research modes. Their practical frame rate
  is dominated by CPU FFT work and memory bandwidth; GPU FFT is planned as a
  separate experimental backend after this stable release.

Package and privacy notes:

- The Windows archive excludes local INI files, diagnostic logs, recordings,
  IQ/audio captures, GNSS data, screenshots, tokens, and API keys.
- Raspberry Pi uses the tagged source tree or prepared source folder.
- Android is unchanged and is not included in this release update.
