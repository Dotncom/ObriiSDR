# FobosAPP 4.6.1

FobosAPP 4.6.1 is a stability and usability patch over 4.6.0.

Highlights:

- Added spectrum-frame recording and replay tools for short-lived RF events.
- Improved replay audio/video synchronization and full-IQ replay demodulation.
- Added replay-side tuning controls and cleaned up duplicated replay paths.
- Continued HF interference lab work with baseline visual flattening, reset controls, and audio-side impulse blanking/cancel experiments.
- Improved spectrum/waterfall navigation: changing the listening marker no longer recenters the view, and the waterfall/spectrum view can be panned horizontally.
- Fixed live spectrum update logic that could pull the visible range back to the listening marker.
- Updated Raspberry source packaging without requiring a zip archive.

Privacy/package notes:

- Release packages exclude local `FobosAPP.ini`, diagnostic logs, raw recordings, IQ captures, NMEA/UBX dumps, WAV files, CSV files, and local backup executables.
- GNSS raw logs and QTH/map-provider keys remain local user data and should be backed up separately before replacing a runtime folder.
