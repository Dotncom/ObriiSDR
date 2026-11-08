# FobosAPP 4.7.1

FobosAPP 4.7.1 is a spectrum-analysis and performance update over 4.7.0. It
keeps the existing Fobos-oriented IQ path and 3D tools while making large FFT
and high-refresh visualization substantially more practical.

Highlights:

- Adaptive single/multithreaded FFTW plans for large FFT sizes, selected by a
  short benchmark on the current computer.
- Parallel FFT post-processing where the data size justifies its overhead.
- Faster 3D waterfall data upload and rendering on Windows and Raspberry Pi.
- Pixel-width spectrum/waterfall reduction that preserves local peaks instead
  of drawing every off-screen FFT point.
- Rectangular, Hann, Hamming, Blackman-Harris, and Flat-top FFT windows with
  amplitude normalization.
- Optional detailed spectrum and 3D-slice panels with frequency, sample rate,
  FFT size, FFT window, bin width, RBW, display resolution, peaks, and level
  statistics.
- A secondary fine zoom control for detailed spectrum inspection.
- Corrected frequency geometry at extreme zoom, including large absolute RF
  frequencies where float precision previously produced visible steps.
- FFT-window and analysis metadata preserved in settings, network settings,
  spectrum recordings, and replay.

Raspberry Pi test note:

- Practical testing produced approximately 50+ FPS near 131k FFT and usable
  frame rates through roughly 1M FFT, depending on display mode and hardware.
  The largest multi-million-point FFT settings remain deliberate research and
  measurement options rather than normal live-view defaults.

Package and privacy notes:

- The Windows archive excludes local settings, diagnostic logs, recordings,
  IQ/audio captures, GNSS dumps/reports, screenshots, tokens, and API keys.
- The Raspberry package is source-only and excludes Android, local captures,
  build output, private settings, and Windows-only packaging tools.
- Android is unchanged and is not included in this release update.
