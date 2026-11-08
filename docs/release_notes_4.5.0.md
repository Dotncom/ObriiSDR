# FobosAPP 4.5.0

This release focuses on GNSS/QTH work and keeps the current DMR lab backends
available for testing.

## Highlights

- Practical external serial GNSS support: NMEA live position, time, satellite
  table, sky view, QTH map overlay and per-satellite/system filters.
- u-blox/NEO-M8N-tested UBX workflow: enable NAV messages, poll/apply
  `CFG-GNSS`, save configuration with `CFG-CFG`, and capture raw UBX/NMEA
  binary logs.
- SDR GPS L1 C/A diagnostics improvements: IQ monitor, acquisition reports,
  weak/no-lock summaries, DC cleanup and tone-notch diagnostics.
- Reduced normal diagnostic-log noise; frequent GNSS telemetry is now reserved
  for verbose logging.
- Native RTL-SDR defaults to auto gain/AGC.

## Runtime Package Notes

The Windows x64 zip is intended to run from its unpacked folder. It includes
runtime DLLs that can be redistributed with the app, including Qt, FFTW, Fobos,
RTL-SDR, bladeRF beta runtime files, and optional DMR bridge tools where their
licenses allow redistribution.

Optional AMBE/DMR voice functionality remains experimental. See
`README.md`, `THIRD_PARTY_LICENSES.txt`, `licenses/dmr_voice_backend/`, and
`config/dmr_backends.example.json` in the package for the backend contract and
for instructions on fetching/building optional dependencies such as
mbelib-neo or OpenDMR/softdmr when they are not bundled in a specific build.

The package intentionally excludes local settings, diagnostic logs, raw GNSS
serial captures, SDR IQ recordings, screenshots and user test data.

## Known Issues

- SDR-only GPS position lock is not yet proven with the tested antenna setup.
  Use an external serial GNSS module for practical positioning.
- DMR voice is still lab quality. mbelib/OpenDMR, DSD-neo and GopherTrunk paths
  are comparison and debugging routes.
- Some Fobos Agile hardware/firmware/host combinations may show center-
  frequency placement or I/Q mirror issues during tuning. This remains under
  hardware/vendor investigation.
