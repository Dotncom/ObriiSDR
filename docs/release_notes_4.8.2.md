# FobosAPP 4.8.2

FobosAPP 4.8.2 is a desktop and Raspberry source update focused on practical
narrow-band analysis, high-rate waterfall delivery, and release stability.

## Highlights

- Live narrow-band Zoom FFT from a selected range of the current full IQ span.
- Independent Zoom spectrum, frequency scale, waterfall, Hz/bin resolution,
  overlapped update interval, levels, speed, and persistent settings.
- Multiple unique waterfall rows can be produced between GUI repaints, so fast
  update intervals are no longer limited to one row per display refresh.
- 3D waterfall history survives normal window and panel resizing.
- 512- and 1024-point FFT choices plus expanded main/extra scale ranges.
- Experimental native HackRF RX backend for future hardware testing.
- A dedicated English/Ukrainian Zoom FFT help section and quieter normal logs.

## Packages

- `FobosAPP-v4.8.2-windows-x64.zip`: self-contained Windows desktop runtime.
- `FobosAPP-v4.8.2-raspberry-source.tar.gz`: Raspberry/Linux source package.
- `FobosAPP-v4.8.2-SHA256SUMS.txt`: SHA-256 checksums for both archives.

Android is unchanged and is intentionally not included in this release.

## Zoom FFT workflow

Left-drag a range on the main spectrum and click **Zoom spectrum** next to
**Presets**. The new window digitally shifts, filters, and decimates that range
from the live IQ stream. A smaller Hz/bin value increases frequency detail;
initial acquisition still obeys the physical limit of roughly `1 / Hz-per-bin`
seconds, while overlap permits faster subsequent updates.

## Notes

VkFFT and native HackRF remain experimental. FFTW is the safe fallback, and
HackRF requires the appropriate runtime library and driver described in
`docs/hackrf_native_beta.md`.

Settings remain in the per-user configuration directory. Release packages do
not include user settings, diagnostic logs, recordings, map keys, coordinates,
device serials, DMR keys, or API tokens.
