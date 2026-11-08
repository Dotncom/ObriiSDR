# FobosAPP 4.8.3

FobosAPP 4.8.3 is a desktop and Raspberry source update focused on CW audio,
practical SSTV reception, and a safe simulator-first transmitter laboratory.

## Highlights

- CW/Morse audio decoding with adjustable tone and speed, adaptive timing, and
  English/International, Ukrainian, or parallel text views.
- SSTV demodulation through USB, LSB or NFM, with improved VIS validation,
  timing recovery and short-fade holdover.
- Expanded SSTV handling for Robot 36/72, Martin M1/M2, Scottie S1/S2/DX,
  SC2-180 and the supported PD modes.
- Simulator-only AM/NFM/WFM, DSB/USB/LSB, CW and FT8 IQ generation with
  waveform preview, duration limits, and CF32 plus JSON export.
- New synthetic self-tests for CW, transmitter waveforms and SSTV timing/mode
  behavior.

## Packages

- `FobosAPP-v4.8.3-windows-x64.zip`: self-contained Windows desktop runtime.
- `FobosAPP-v4.8.3-raspberry-source.tar.gz`: Raspberry/Linux source package.
- `FobosAPP-v4.8.3-SHA256SUMS.txt`: SHA-256 checksums for both archives.

Android is unchanged and is intentionally not included in this release.

## Important Notes

The transmitter laboratory is deliberately simulator-only. It cannot key or
drive RF hardware. Real transmitter output remains disabled until a dedicated
backend, hardware interlocks, watchdogs and dummy-load testing are available.

CW and SSTV decoding remain experimental. For SSTV use USB on HF, LSB for an
inverted sideband, or NFM when SSTV audio is sent through an FM handheld radio.

Settings remain in the per-user configuration directory. Release packages do
not include user settings, diagnostic logs, recordings, map keys, coordinates,
device serials, DMR keys, screenshots, or API tokens.
