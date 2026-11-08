# FobosAPP 4.7.2

FobosAPP 4.7.2 is a 3D visualization and interface patch over 4.7.1. It adds an
optional research-oriented layout while preserving the existing receiver, IQ,
audio, spectrum, replay, and network paths.

Highlights:

- Optional Alternative interface selected from General settings.
- Fixed front-facing 3D waterfall with the live spectrum aligned to its near
  edge and time moving away from the viewer.
- Optional colored or solid gradient fill below the spectrum with adjustable
  opacity.
- Support for both fixed 3D and fixed 3D plus a top-left mini waterfall.
- Optional full second spectrum above the 3D view with its normal interaction
  tools and a denser frequency axis.
- Preserved tuning, panning, hover inspection, bandwidth measurement, band
  overlays, frequency/time slice selection, and extended information panels.
- Wider and taller far perspective for easier inspection of waterfall history.
- Native Qt labels for lower-spectrum dB scale, hover values, bandwidth
  measurements, and band names, avoiding fragmented OpenGL text.
- Persisted settings, translations, and updated in-app help for the new mode.

Package and privacy notes:

- The Windows archive excludes local settings, diagnostic logs, recordings,
  IQ/audio captures, GNSS dumps/reports, screenshots, tokens, and API keys.
- Raspberry Pi uses the tagged source tree or the prepared source folder; no
  separate Raspberry archive is required.
- Android is unchanged and is not included in this release update.
