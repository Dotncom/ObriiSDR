# Obrii SDR 5.0.0

Obrii SDR 5.0.0 is the first major release under the Obrii SDR name. It turns
the Fobos-focused receiver into a broader modular SDR and research workstation
while preserving the short high-rate IQ path that motivated the project.

## Highlights

- Graphical DSP path designer with persistent layouts, live block controls,
  audio filters, digital/research tools, and resizable result widgets.
- Multi-VFO monitoring with independently assignable spectrum and waterfall
  blocks. Every DSP-board visual and standard mosaic channel stores its own
  dBFS range.
- Live reorderable audio-filter chain, including frequency/EQ filters,
  de-emphasis, dynamics, adaptive notch, blanking, spectral denoise, and custom
  graphical FIR response.
- Expanded scientific spectrum tools: detector and trace modes, markers,
  zero-span and triggers, IQ integrity diagnostics, dual-HF analysis,
  constellation, eye diagram, synchronization, and metadata-rich export.
- Hardware-tested experimental native HackRF receive backend alongside Fobos,
  RTL-SDR/rtl_tcp, and optional SoapySDR support.
- Obrii SDR executable, splash screen, per-user settings migration, help,
  recording metadata, Linux launcher, and release package names.
- Improved wideband processing with multistage channel extraction, exact
  resampling, large-FFT worker throttling, FFTW/VkFFT selection, and exact
  Hz-per-point FFT sizes.

## Packages

- `ObriiSDR-v5.0.0-windows-x64.zip`: self-contained Windows x64 runtime.
- `ObriiSDR-v5.0.0-raspberry-source.tar.gz`: Raspberry/Linux source tree.
- `ObriiSDR-v5.0.0-SHA256SUMS.txt`: SHA-256 checksums.

Android is unchanged and is intentionally not included.

## Important Notes

- HackRF receive and live tuning were tested on real PortaPack/Mayhem hardware.
  HackRF transmit remains disabled until explicit safety interlocks and
  hardware testing are completed.
- The bladeRF prototype is hidden and unsupported because physical testing did
  not reach reliable device detection.
- DMR voice, external DSD/GopherTrunk integration, and SDR-only GNSS decoding
  remain experimental.
- The transmitter laboratory is simulator-only and cannot emit RF.
- Release archives exclude user settings, diagnostic logs, recordings,
  screenshots, coordinates, map/API tokens, DMR keys, and device serials.
