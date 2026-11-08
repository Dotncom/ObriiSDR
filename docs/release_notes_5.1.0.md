# Obrii SDR 5.1.0

Obrii SDR 5.1.0 expands the project from a wideband receiver and analyzer into
an experimental transceiver and configurable DSP workstation. The release
keeps the optimized Fobos receive path while adding hardware-tested HackRF RX,
initial native HackRF TX, a much richer DSP board, and additional scientific
visualization and measurement tools.

## Highlights

- Experimental native HackRF transmission with explicit arming, bounded
  sessions, watchdog/error handling, underrun diagnostics, and simulator
  fallback.
- AM, NFM, WFM, DSB, USB and LSB microphone/audio-file transmission, manual and
  generated CW, FT8, SSTV image preparation/transmission, and experimental ATV
  and video waveform sources.
- Independent TX signal-bandwidth presets and filtering, FM deviation,
  generator rate, HackRF IQ rate, hardware-filter and amplitude controls.
- Full-screen alternative DSP workspace with resizable sectors, live embedded
  spectrum/ruler/waterfall widgets, production 3D rendering, Multi-VFO views,
  saved layouts, and direct access to digital, GNSS, TX, and research tools.
- Expanded density/persistence visualization, selected-band analysis,
  scientific sessions, pause and area-ruler overlays, replay improvements, and
  broader DSP-board coverage.
- More complete native HackRF RX controls and diagnostics plus stability and
  performance work across large FFTs, waterfall rendering, Multi-VFO, CW, SSTV,
  audio filtering, and workspace persistence.

## Packages

- `ObriiSDR-v5.1.0-windows-x64.zip`: self-contained Windows x64 runtime.
- `ObriiSDR-v5.1.0-raspberry-source.tar.gz`: Raspberry/Linux source tree.
- `ObriiSDR-v5.1.0-SHA256SUMS.txt`: SHA-256 checksums.

Android is unchanged and is intentionally not included.

## Important Notes

- HackRF transmission is experimental. Basic output was tested on real
  hardware, but Obrii SDR cannot measure SWR. Use a correct antenna or dummy
  load, suitable attenuation and external measurement equipment; observe local
  regulations and verify occupied bandwidth, output level and spectral purity.
- TX signal bandwidth limits generated baseband content; HackRF IQ rate is the
  device sample stream rate and is intentionally a separate setting.
- Experimental ATV/video transmit and decode paths are included for laboratory
  work and are not claimed as broadcast-quality implementations.
- DMR voice, DSD-neo/GopherTrunk integration, SDR-only GNSS, SoapySDR and some
  scientific workflows remain experimental. bladeRF remains disabled until
  physical hardware is available for direct debugging.
- Release archives exclude user settings, diagnostic logs, recordings,
  screenshots, coordinates, map/API tokens, DMR keys, device serials and local
  test data.
