# FobosAPP 4.5.1

FobosAPP 4.5.1 is a privacy, stability, packaging, and waterfall-visual patch
over 4.5.0.

## Highlights

- Restores the useful legacy waterfall contrast/palette behavior while keeping
  the optimized low-copy waterfall row path.
- Removes exact latitude/longitude from automatic GNSS IQ sidecar metadata,
  acquisition reports, and UBX verbose diagnostics. GNSS reports keep QTH
  locator context and an explicit privacy note instead.
- Tightens release packaging so nested `recordings` folders, local logs, raw
  captures, GNSS reports, local settings, tokens, API keys, and scratch files do
  not enter release archives.
- Cleans the roadmap into a current live plan instead of a historical task log.
- Keeps the large source split/refactor work current for desktop and Raspberry
  source builds.

## Known Status

- Fobos Standard and Fobos Agile remain the primary supported paths.
- RTL-SDR remains a secondary/test backend.
- Serial GNSS modules through NMEA/UBX are the practical GNSS path.
- SDR-only GNSS acquisition remains experimental.
- DMR voice and external DMR decoder bridges remain lab features.
- Android was not rebuilt for this patch.

## Privacy Note

`FobosAPP.ini` is local user data. It may contain QTH coordinates, map API keys,
DMR privacy keys, serial ports, presets, and UI settings. Keep or export it
before replacing a runtime folder, but do not publish it with release packages.
