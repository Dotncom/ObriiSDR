# FobosAPP 4.8.1

FobosAPP 4.8.1 is a desktop and Raspberry source patch focused on large FFT
performance, exact spectrum resolution and release documentation.

## Highlights

- Optional Vulkan VkFFT spectrum processing with automatic FFTW fallback.
- Auto CPU/GPU comparison for 1M-8M transforms; larger transforms go directly
  to multithreaded FFTW to avoid a long benchmark stall.
- Exact Hz-per-point mode with synchronized FFT-length and bin-width controls.
- More efficient reusable IQ/audio buffering and safer live sample-rate changes.
- Updated receiver layout and expanded English/Ukrainian in-app help covering
  bladeRF, GNSS/UBX, QTH maps and server-provided network receivers.

## Packages

- `FobosAPP-v4.8.1-windows-x64.zip`: self-contained Windows desktop runtime.
- `FobosAPP-v4.8.1-raspberry-source.tar.gz`: Raspberry/Linux source package.

Android is unchanged and is intentionally not included in this release.

## Notes

VkFFT is experimental. `Auto` is the recommended backend: GPU initialization
or execution failures fall back to FFTW without stopping reception. Very large
FFT lengths still require substantial RAM and may update slowly even on fast
hardware.

Settings remain in the per-user configuration directory. Release packages do
not include user settings, diagnostic logs, recordings, map keys, coordinates,
device serials, DMR keys or API tokens.
