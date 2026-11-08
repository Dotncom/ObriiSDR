# FobosAPP 4.7.0

FobosAPP 4.7.0 adds interactive 3D spectrum/waterfall analysis while preserving
the optimized Fobos-oriented IQ path.

Highlights:

- Added 2D, 3D, and 3D-with-mini-waterfall display modes.
- Added selectable 3D frequency resolution with grouped-bin averaging and an
  adjustable history-memory depth.
- Added 3D camera orbit, zoom, and plane-parallel pan controls.
- Added interactive frequency and time/spectrum slices with configurable step,
  width, capture-follow, and fixed-capture modes.
- Added VNC slice control so frequency/time slices can be selected with mouse
  buttons when a remote desktop or Linux window manager intercepts Alt.
- Added the same 3D visualization and slice tools to spectrum-frame replay.
- Added receiver frequency-calibration and displayed amplitude-calibration
  offsets, including persistent settings and settings import/export support.
- Expanded the in-app English and Ukrainian help for 3D controls, replay,
  calibration, Linux Alt handling, and VNC operation.
- Kept Raspberry Pi source packaging on the common CMake/OpenGL path.

Linux/Raspberry controls:

- Right Alt is the most reliable direct modifier for 3D slices.
- If left Alt is reserved by the desktop, use left Ctrl+Alt.
- VNC users can enable `VNC slice control` and use left/right mouse buttons
  without a keyboard modifier. Disable it to restore normal tuning, panning,
  and context-menu actions.

Privacy/package notes:

- Release packages exclude local `FobosAPP.ini`, diagnostic logs, tokens,
  recordings, IQ captures, NMEA/UBX dumps, WAV files, and CSV files.
- GNSS/QTH coordinates, map-provider keys, scan lists, and personal presets
  remain local settings and are not part of the published packages.
