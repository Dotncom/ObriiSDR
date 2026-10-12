# Changelog

## 6.0.0 - 2026-10-12

### Added

- Native known-key Motorola ARC4 and AES-256 DMR receive decryption with
  per-slot PI/late-entry synchronization, key-ID checks, and synthetic tests.
- An FT8 station manager with QSO workflow, ADIF logging, optional online/UDP
  services, separate RX/TX routing, callsign filters, all 340 CTY/DXCC entities,
  and explicit journal export to the QTH map.
- A deterministic signal laboratory with analog, PSK/QAM, FSK/GFSK and OFDM
  generation, IQ export, 2D/3D previews, and experimental clear DMR voice TX.
- Local-terrain radio coverage and TX-RX/Fresnel profiles using free-space,
  dominant-obstacle and NTIA ITM models, with manual NOAA HF solar information.
- A beta TShark/Wireshark packet laboratory and future wireless-backend contracts.
- I/Q/time 3D constellation views in Signal Lab, live IQ research and DSP widgets,
  with camera controls and an optional filled radial-surface visualization.

### Fixed And Improved

- Repaired DSD-neo/Gopher transport, burst boundaries and native privacy timing;
  native Motorola ARC4/AES-256 and Gopher ARC4 have user-confirmed RF results.
- Reduced repeated display copies/rebuilds and channelizer work; retained VFO
  waterfall history while changing level ranges and updating neighboring channels.
- Preserved IQ blocks across short FFT snapshot-lock contention with a bounded
  deferred buffer, and made diagnostic waterfall rows opt-in and clearly classified.
- Improved live audio startup/rebuffering and callback wake-up handling.
- Corrected signal-generator RRC normalization, matched-symbol previews,
  short-window RBW/amplitude calculations and FFT planner synchronization.
- Expanded English/Ukrainian help, translations, quiet logging, third-party
  notices and release privacy allowlists. Android is unchanged and excluded.
- Fixed Windows package-manifest evaluation, removed Windows-only HackRF
  binaries from Raspberry source packages, and normalized tar ownership/modes
  so extracted Linux scripts are executable.

### Limits

- DMR remains experimental across vendors and weak RF conditions. Hytera Basic
  is unverified; Gopher DMR AES-256 is unsupported. Only supplied keys are used.
- DMR TX is a clear Tier II direct/simplex beta, without privacy or trunking.
- Wireless captures are not native Wi-Fi/Bluetooth/cellular SDR PHY decoding.
- SDR GNSS, ATV/video, untested Soapy devices and bladeRF remain experimental
  or unavailable. Raspberry assets are source, not a prebuilt ARM executable.

## 5.1.0 - 2026-10-11

### Added

- Added an explicitly armed native HackRF transmit backend with bounded
  sessions, watchdog/error handling, underrun diagnostics, and simulator
  fallback.
- Added live microphone/PTT and manual CW keying, audio-file playback,
  standards-based CW and FT8 generation, SSTV image preparation/transmission,
  and experimental ATV/video waveform sources.
- Added independent TX signal-bandwidth presets and filtering, live FM
  deviation control, hardware-filter and generator/IQ-rate controls, and
  clearer transmit-path diagnostics.
- Added a full-screen alternative DSP workspace with independently split and
  resizable sectors, movable live widgets, saved layouts, binding IDs, and
  embedded spectrum, ruler, 2D/3D waterfall, Multi-VFO, digital, GNSS, TX, and
  research tools.
- Added selected-band density views, 2D/3D persistence controls, scientific
  session helpers, richer pause/measurement overlays, and additional DSP-board
  research widgets.

### Changed

- Expanded native HackRF receive controls, range validation, gain/filter/clock
  handling, diagnostics, and safe live retuning.
- Reworked DSP-board geometry, grouping, zoom/pan, sector assignment, per-view
  settings, and persistence so the board can serve as the primary workspace.
- Reused the production spectrum and 3D-waterfall rendering paths in embedded
  views and reduced unnecessary redraws and background analysis work.
- Improved Multi-VFO display updates, large-FFT throttling, waterfall history,
  replay controls, audio filtering, CW selectivity, and SSTV timing/color
  handling.

### Fixed

- Fixed crashes and stale bindings while adding, removing, moving, or
  reconfiguring DSP-board and Multi-VFO widgets.
- Fixed embedded spectrum/waterfall ranges, color mapping, frequency ruler,
  pause overlays, 3D camera/slice interaction, and saved workspace placement.
- Fixed several HackRF tuning, low-frequency range, sample-rate, microphone
  enumeration, and transmit-control synchronization issues.

### Notes

- HackRF transmission is experimental. Basic RF output was exercised on real
  hardware, but users must verify antenna/dummy-load safety, legal limits,
  occupied bandwidth, level, and spectral purity with appropriate equipment.
- ATV/video transmit and decode, DMR, SDR-only GNSS, SoapySDR, and external
  digital backends remain experimental. bladeRF support remains unavailable.
- Android is unchanged and is intentionally not included in this release.
## 5.0.0 - 2026-10-09

### Added

- Added the graphical DSP path designer with live compact block editors,
  persistent layouts, control blocks, resizable research plots, and independent
  Multi-VFO spectrum/waterfall views.
- Added a reorderable live audio-filter chain with classic filters, FM
  de-emphasis, parametric EQ and shelves, gain/dynamics stages, adaptive notch,
  impulse blanking, CW and sub-tone suppression, spectral denoise, and a custom
  graphical FIR/EQ.
- Added Multi-VFO monitoring and channel selection inside one wide IQ span,
  including separate standard-interface and DSP-board spectrum/waterfall views.
- Added expanded spectrum-research tools for detector modes, traces, markers,
  zero span, triggers, IQ integrity, scientific export, dual-HF analysis,
  constellation, eye diagram, synchronization, and live correction experiments.
- Added hardware-tested experimental native HackRF receive support and bundled
  redistributable runtime libraries and notices. Transmit remains disabled.
- Added individual saved dBFS minimum/maximum controls for every DSP-board VFO
  spectrum/waterfall block and every standard Multi-VFO mosaic channel.

### Changed

- Renamed the desktop and Raspberry application to Obrii SDR, including the
  executable, launcher, help text, recording metadata, and release packaging.
  Existing FobosAPP settings are migrated automatically on first launch.
- Added an embedded Obrii SDR splash screen shown while the main window and
  receiver backends initialize.
- Reworked ordinary audio channel extraction with multistage decimation and
  precise polyphase resampling, with a selectable simplified compatibility path.
- Improved large-FFT responsiveness by dropping stale queued visual frames
  instead of repeatedly calculating history the interface can no longer show.
- Expanded CW, SSTV, DMR, GNSS, scan, calibration, replay, network, help, and
  Ukrainian localization workflows.

### Fixed

- Stabilized FFT switching, Hz-per-point synchronization, waterfall history
  preservation, DSP-board VFO selection, Fobos startup tuning, HackRF tuning,
  and receiver-specific frequency/sample-rate limits.
- Reduced normal diagnostic log noise while retaining detailed profiling behind
  the Logging option.

### Notes

- The native bladeRF RX prototype remains disabled and hidden after physical
  testing failed at device detection. Its source is retained for future work.
- DMR voice and SDR-only GNSS decoding remain experimental.
- Android is unchanged and is intentionally not included in this release.

## 4.8.3 - 2026-10-07

### Added

- Added an audio CW/Morse decoder with adjustable tone, initial/adaptive speed,
  English/International, Ukrainian, and parallel alphabet views.
- Added a simulator-only transmitter laboratory for microphone AM/NFM/WFM,
  DSB/USB/LSB, CW and standards-compatible FT8 baseband generation, waveform
  preview, duration limiting, and CF32 plus JSON export.
- Added SSTV demodulation selection for USB, LSB and NFM reception.
- Added self-tests for CW decoding, transmitter waveform generation and SSTV
  mode/timing behavior.

### Changed

- Expanded SSTV support and timing recovery for Robot 36/72, Martin M1/M2,
  Scottie S1/S2/DX, SC2-180 and the supported PD family.
- Made SSTV VIS acquisition more selective and kept line synchronization
  through short fades without treating arbitrary audio as a valid image.
- Improved Robot 36 chroma recovery and Scottie channel/line sequencing for
  cleaner real-radio image reception.
- Expanded the English and Ukrainian in-app guide and UI translations for CW,
  SSTV and the transmitter laboratory.

### Notes

- The transmitter backend in this release is a simulator and cannot emit RF.
  Real transmitter hardware remains deliberately disabled until dedicated
  safety interlocks and hardware testing exist.
- CW and SSTV decoding remain experimental and depend on signal quality,
  demodulation choice and audio tuning.
- Android is unchanged and is intentionally not published with this desktop
  and Raspberry source release.

## 4.8.2 - 2026-10-07

### Added

- Added a live narrow-band Zoom FFT window driven from the full receiver IQ
  stream, with independent frequency resolution, overlapped updates, spectrum,
  frequency scale, waterfall controls, and persistent settings.
- Added 512- and 1024-point FFT choices plus wider main and extra spectrum
  scaling controls for narrow-band and low-sample-rate receivers.
- Added an experimental native HackRF RX backend ready for hardware testing.

### Changed

- Decoupled FFT production from GUI repaint cadence and queued multiple unique
  waterfall rows between screen updates, allowing sub-frame update intervals
  on high-refresh displays without duplicating frames.
- Preserved 3D waterfall history across ordinary widget resizes and throttled
  auxiliary spectrum tools independently from the hot waterfall path.
- Expanded the English and Ukrainian in-app guide with a dedicated
  Narrow-band Zoom FFT section and clarified acquisition/update timing.
- Reduced normal diagnostic output: FFT benchmarks, spectrum profiling, map
  tile failures, and DMR queue/drop details now require detailed logging.

### Fixed

- Kept fixed FFT length and Hz-per-point controls synchronized in both
  directions and recovered visual updates after live FFT/sample-rate changes.
- Serialized FFTW plan creation/destruction shared by the main and Zoom FFT
  workers to prevent intermittent planner crashes.
- Stored Zoom FFT settings in the per-user FobosAPP settings file and fixed
  UTF-8 text in its Windows dialog and validation messages.
- Reduced the chance of preset/calibration races while live processing is
  active.

### Notes

- HackRF support is experimental and has not yet been verified with physical
  hardware.
- Android is unchanged and is intentionally not published with this desktop
  and Raspberry source release.

## 4.8.1 - 2026-10-06

### Added

- Added an optional Vulkan VkFFT spectrum backend with automatic CPU/GPU
  selection for 1M-8M transforms and safe FFTW fallback.
- Added an exact Hz-per-point mode that derives a custom even FFT length from
  the current sample rate while keeping the fixed FFT and resolution controls
  synchronized.
- Added receiver-stream health diagnostics for IQ publication, callback rate,
  discontinuities and audio starvation under detailed logging.

### Changed

- Improved large-FFT planning and post-processing with bounded automatic GPU
  benchmarks and direct multithreaded FFTW use above 8M points.
- Reworked reusable IQ/audio buffering and live sample-rate changes to reduce
  unnecessary copies and avoid applying a rate change under an active reader.
- Balanced the receiver controls into two flexible columns for narrow windows.
- Expanded the English and Ukrainian in-app guide for bladeRF, VkFFT,
  external NMEA/UBX GNSS, SDR acquisition, QTH maps and remote receiver lists.

### Fixed

- Kept fixed FFT length and Hz-per-point displays mutually consistent after
  live mode and sample-rate changes.
- Prevented a network client from sending its local receiver index before the
  server's authoritative receiver list is known.

### Notes

- Android is unchanged and is not published with this desktop and Raspberry
  source patch.

## 4.8.0 - 2026-10-02

### Added

- Added spectrum-research tools covering calibrated amplitude units, detector
  modes, RBW/VBW, FFT overlap, multi-trace analysis, percentile traces,
  automatic bandwidth/channel-power measurements, Zero Span, and triggers.
- Added a dedicated research window with interference-comb analysis, IQ
  waveform/constellation diagnostics, autocorrelation, and dual-HF-input
  comparison tools.
- Added receiver calibration tables with interpolation, uncertainty, external
  path corrections, and preset import/export support.
- Added richer scientific recording metadata and SigMF-compatible IQ metadata.
- Added a normal-interface `Fix plane` 3D mode plus a dense gradient-filled
  front face using the same fixed projection as the Alternative interface.

### Changed

- Reorganized receiver and spectrum-measurement controls for better readability.
- Moved `Capture` and `Fix capture` to their own row in the 3D waterfall panel.
- Stored `FobosAPP.ini` in the per-user configuration directory instead of next
  to the executable. Existing portable settings migrate automatically on first
  launch, so normal use no longer requires administrator rights.
- Updated translations, in-app help, settings backup guidance, release privacy
  checks, and Raspberry source packaging for the new tools.

### Fixed

- Fixed the normal fixed-plane gradient appearing as flickering lines by
  rendering a dense color gradient without depth-buffer conflict.
- Preserved settings, presets, calibration data, scan lists, and map markers
  across release-folder replacement.

### Notes

- Android is unchanged and is not published with this desktop and Raspberry
  source update.

## 4.7.2 - 2026-10-01

### Added

- Added an optional Alternative interface that combines a fixed front-facing
  3D waterfall with a transparent live spectrum along its near edge.
- Added Alternative-interface gradient fill with adjustable opacity, optional
  upper second spectrum, and support for the 3D plus mini-waterfall mode.
- Added frequency-axis labels to the upper spectrum used by the Alternative
  interface.

### Changed

- Reworked the fixed 3D projection so its near edge aligns with the lower
  spectrum while the far edge is taller and wider for better use of the
  available viewport.
- Preserved spectrum tuning, hover inspection, bandwidth measurement, band
  overlays, mouse navigation, and 3D slice controls in the new layout.
- Added settings persistence, settings import/export, translations, and help
  text for the Alternative interface and its gradient controls.

### Fixed

- Fixed upside-down or fragmented lower-spectrum labels in the OpenGL-backed
  Alternative interface by moving dB, hover, measurement, and band text to
  native Qt labels.
- Fixed Alternative-interface spectrum geometry and picking so visible mouse
  positions match the rendered frequency and time slices.
- Improved normal Windows startup activation without changing Linux startup.

### Notes

- Android is unchanged and is not published with this desktop and Raspberry
  source patch.

## 4.7.1 - 2026-09-30

### Added

- Added selectable Rectangular, Hann, Hamming, Blackman-Harris, and Flat-top
  FFT windows with coherent-gain normalization and RBW/ENBW reporting.
- Added optional extended spectrum and 3D-slice information panels showing
  center/listening frequency, sample rate, FFT size, window, bin width, RBW,
  display resolution, peak frequency, and level statistics.
- Added a compact secondary spectrum zoom control for detailed inspection
  beyond the normal zoom range.

### Changed

- Added adaptive FFTW thread selection for large FFT sizes and parallelized
  suitable post-processing stages. The application benchmarks candidate plans
  and keeps the faster plan for the current host.
- Reworked the 3D waterfall upload/render path to reduce CPU work and improve
  frame rate at large FFT sizes on Windows and Raspberry Pi.
- Reduced display data to the visible pixel width before 2D graph/waterfall
  rendering while preserving peak information within each display column.
- Kept detailed FFT profiling behind verbose logging so normal logs remain
  compact.

### Fixed

- Fixed stepped/rectangular spectrum geometry at extreme zoom caused by loss
  of precision in large absolute frequencies stored as floats.
- Kept the primary spectrum, second spectrum, and waterfall aligned when
  reducing very large FFT arrays for display.
- Preserved the selected FFT window and display-analysis metadata in settings,
  network settings, spectrum recordings, and replay.

### Notes

- Practical Raspberry Pi testing reached roughly 50+ FPS around 131k FFT and
  remained usable around 1M FFT depending on display mode and hardware load.
  Multi-million-point FFT modes are intentionally available as research modes.
- Android artifacts are unchanged and are not published with this desktop and
  Raspberry source update.

## 4.5.2 - 2026-06-22

### Changed

- Reworked network receiver selection so the client uses the server's receiver
  list instead of sending a local device index to the server.
- Updated the Android USB/network client with RTL/Fobos target auto-sync,
  corrected sample-rate option switching, and spectrum half-swap handling.
- Reduced network-server USB probing during client handshakes by using cached
  server receiver state for remote device lists.

### Fixed

- Fixed network-client audio playback/HTTP relay when attaching to an already
  running server.
- Fixed network-client window close hanging while waiting for a local SDR
  reader that does not exist in client-only mode.

## 4.5.1 - 2026-06-22

### Changed

- Restored the legacy waterfall palette/contrast behavior while keeping the
  lower-copy optimized waterfall row path.
- Cleaned the live roadmap so it now contains only current release, stability,
  GNSS, DMR, backend, Android/network and future transceiver priorities.
- Tightened Windows and Raspberry package filtering for nested `recordings`
  folders and local capture formats.

### Fixed

- Omitted exact latitude/longitude from automatic GNSS IQ sidecar metadata and
  acquisition reports. Reports keep QTH locator context plus an explicit privacy
  note.
- Avoided exact UBX NAV-PVT latitude/longitude in verbose diagnostic logs.
- Added extra git ignore rules for local settings, captures, screenshots and
  scratch notes that must not be committed.

### Notes

- This is a privacy/stability/visual-performance patch over 4.5.0.
- Android was not rebuilt for this patch because the changes are desktop/source
  packaging focused.

## 4.5.0 - 2026-06-17

### Added

- Added a full external serial GNSS workflow for NMEA/u-blox receivers, tested
  with a NEO-M8N module: serial port selection, live fix status, satellite
  table, sky view, QTH map overlay, time-zone display, and per-system/per-
  satellite filtering for FobosAPP's own display and analysis.
- Added UBX support for enabling NAV-PVT/NAV-SAT/NAV-DOP output, polling and
  applying `CFG-GNSS` constellation settings, saving module configuration with
  `CFG-CFG`, and capturing exact raw UBX/NMEA serial logs for later replay or
  analysis.
- Added GNSS raw serial capture alongside the existing NMEA text logging and
  SDR IQ acquisition reports.
- Added SDR GPS L1 C/A acquisition diagnostics improvements: per-millisecond DC
  cleanup, GNSS tone-notch diagnostics, stronger report metadata, and clearer
  weak/no-lock status for difficult RF conditions.

### Changed

- Reduced normal diagnostic-log noise from live GNSS serial and IQ-monitor
  telemetry. Frequent NMEA/UBX/IQ detail is still available when verbose
  logging is enabled in Settings.
- Native RTL-SDR starts with tuner auto-gain/AGC by default, which is more
  useful for weak GNSS experiments and avoids the previous zero-gain trap.
- Reorganized the GNSS/QTH controls into a more compact layout so the main
  control panel no longer grows excessively wide.

### Fixed

- Fixed UBX `CFG-GNSS` handling so the first button press polls and synchronizes
  the module state instead of blindly applying unsupported constellation
  combinations.
- Fixed UBX configuration saves by sending `CFG-CFG` and reporting ACK/NAK in
  the GNSS status area.
- Fixed raw serial logging cleanup so active UBX/NMEA captures are closed when
  the serial receiver is stopped.

### Notes

- Generic NMEA GNSS modules should provide most live map, QTH, sky-view and
  satellite-table functionality. UBX configuration controls are intended for
  u-blox-compatible modules and were field-tested on NEO-M8N.
- SDR-only GPS L1 C/A reception remains experimental. The current tools can
  record IQ, monitor RF quality, run acquisition and produce reports, but a real
  position lock from SDR IQ has not been proven with the tested antenna setup.
- DMR voice remains a laboratory feature. The mbelib/OpenDMR, DSD-neo and
  GopherTrunk paths are included for testing and comparison, not as a finished
  commercial-grade DMR receiver.

## 4.4.0 - 2026-06-16

### Added

- Added selectable DMR backend routing for the internal mbelib path, the
  internal OpenDMR/OP25-compatible path, DSD-neo, and the experimental
  GopherTrunk bridge.
- Added DSD-neo bridge controls, UDP PCM handoff, decoded PCM return path, and
  release packaging support for bundled optional DSD-neo runtime files.
- Added the experimental GopherTrunk bridge scaffold for future virtual-source
  tests.
- Added DMR channel-rate control and improved DMR metadata stability filtering.
- Added live USB/serial NMEA GNSS input through Qt SerialPort, including
  NEO-M8N-friendly COM-port controls, fix/satellite status reporting, and
  automatic QTH/map updates when valid GGA/RMC coordinates are received.
- Added QTH privacy controls for clearing the current real position, stopping
  NMEA reception on request, hiding the green current-location marker, and
  removing test/search markers from the map with right-click.

### Fixed

- Reworked stale `DataProcessor` recovery so the Start button no longer silently
  switches the UI to Running when the reader is stuck in an idle-looking state.
- Kept the DMR center offset workaround active so DMR decoding is not run with
  listening and RF center exactly equal on Fobos Agile.
- Bundled `Qt5SerialPort.dll` in the Windows runtime package so serial GNSS
  receiver support works from the release folder.

### Notes

- The attempted Agile startup frequency jog was removed after field testing
  showed it could make the shifted spectrum state persistent. Startup retune
  recovery remains under investigation and should not be released as fixed yet.
- Some Fobos/Agile receivers or host/firmware combinations may still show
  intermittent center-frequency placement, spectrum shift, or I/Q mirroring
  after tuning. This is documented as a known issue until more hardware can be
  tested with the manufacturer.

## 4.3.2-beta - 2026-06-15

### Added

- Added an experimental native bladeRF RX backend that loads `bladeRF.dll` /
  `libbladeRF` at runtime and feeds SC16_Q11 IQ into the existing FobosAPP
  float-IQ pipeline.
- Added bladeRF device enumeration, open/configure/start, live center retune,
  standard scan compatibility, and optional detailed RX statistics logging.
- Bundled the Nuand 2025.10 Windows `bladeRF.dll` runtime and matching
  `libusb-1.0.dll` in the beta Windows package under `bladerf/`.

### Notes

- bladeRF support is RX-only and untested without community hardware.
- TX/PTT support is intentionally not enabled yet; it remains planned for the
  separate future `TransmitterBackend` safety architecture.

## 4.3.1 - 2026-06-15

### Changed

- Added block-level Agile scan IQ metadata so spectrum and waterfall stitching
  use the scan point that actually produced each IQ block.
- Improved scan visual behavior in `Floating/True axis` and `Pass composite`
  modes by carrying scan-point metadata through `IqBuffer` and FFT processing.
- Moved the scan visual mode selector above the scan controls so it is clearly
  shared by all scan display modes.

### Fixed

- Fixed Agile scan waterfall rows being placed with a stale or unrelated scan
  index, which caused irregular sector ordering and visual gaps.
- Skipped tuning-period Agile scan IQ blocks (`scan_index == -1`) instead of
  drawing them as if they belonged to a valid scan center.

## 4.2.2 - 2026-06-14

### Added

- Added the separated diagnostics, help, GNSS/QTH helper, preset helper, and
  preset manager modules to reduce the load carried by `main.cpp`.
- Added SATCOM and expanded FPV/LTE/3G default presets, plus ordered preset
  controls for moving entries up and down.
- Added the built-in bilingual feature guide and refreshed the default desktop
  startup profile for wide FM, 200 kHz bandwidth, larger FFT, color spectrum,
  visible band overlays, and safer scan defaults.

### Changed

- Optimized spectrum/waterfall hot-path handling and exposed faster waterfall
  frame timing/row duplication defaults.
- Moved scan/spectrum measurement and spur controls into a clearer Spectrum
  measurement section.
- Shortened the GNSS/QTH controls and moved acquisition diagnostics into a
  separate plot window so the left control panel no longer grows too wide.
- Kept the RTL-SDR backend wired into Standard scan and kept optional SoapySDR
  support documented as experimental.

### Fixed

- Reduced unnecessary diagnostic noise when verbose logging is disabled and
  tightened start/stop/retune handling after the Fobos IQ offset investigation.
- Kept release packaging focused on runtime files by excluding local settings,
  logs, and real IQ/recording artifacts.

## 4.2.1 - 2026-06-14

### Fixed

- Fixed native RTL-SDR startup when `rtlsdr.dll` enumerates more than one
  device and index `0` cannot be opened. FobosAPP now lists real native RTL-SDR
  indices in the receiver combo and falls back to the next visible RTL-SDR
  device if the selected one fails to open.

## 4.2 - 2026-06-14

### Added

- Added a runtime receiver backend layer with the existing Fobos Standard and
  Fobos Agile paths plus native RTL-SDR, rtl_tcp, and optional SoapySDR
  backends.
- Added Standard scan support for external live-retune backends, so native
  RTL-SDR/rtl_tcp/Soapy paths can use the same explicit-center scan workflow.
- Added ordered preset management controls, LTE/3G default presets, larger FFT
  sizes, faster waterfall refresh options, and a built-in bilingual feature
  guide from General settings.
- Added a 4.2 default configuration snapshot.

### Changed

- Retune/start/stop handling now uses a safer IQ-buffer generation path after
  the Fobos retune-offset investigation.
- Spectrum/waterfall drawing avoids extra row copies in the hot path and can
  duplicate rows for a faster visual waterfall.
- Android is now labeled and versioned as the USB/Network client
  (`4.2-usb-network`) because it includes the direct USB/OTG Fobos preview path
  as well as the network client path.
- Release packaging now documents optional RTL-SDR runtime files and keeps the
  local RTL-SDR source checkout out of Raspberry/source packages.

### Known Issues

- Native RTL-SDR and optional SoapySDR support are new and much less tested
  than the Fobos backends.
- Android USB/OTG is still a diagnostic live preview, not a full Android
  replacement for the desktop receiver pipeline.
- DMR voice and GNSS acquisition remain laboratory features.

## 4.1 - 2026-06-13

### Added

- Added the GNSS/QTH laboratory workflow with map window, Maidenhead/QTH grid
  overlay, online/offline map layers, provider selection, marker editing,
  coordinate search, NMEA paste, and QTH copy support.
- Added GNSS L1 tuning presets, GNSS IQ monitor, raw GNSS IQ save, listening
  scan presets, GPS L1 C/A live/deep acquisition, offline replay, synthetic GPS
  acquisition self-test, synthetic multi-satellite position solver test, NTP
  time query, Doppler search controls, and spur-watch diagnostics.
- Added repeatable GNSS acquisition artifacts under `recordings/gnss_reports`:
  JSON summary reports plus PRN/Doppler heatmap and correlation-profile CSV
  dumps.
- Added `docs/gnss_preflight_4.1.md` and expanded the roadmap/IQ audit notes
  for future GNSS-SDR/reference comparison.

### Changed

- Standard-firmware scan now includes explicit-center controls, range fill,
  safe spacing, presets, adjustable dwell/settle timing, and stitched scan
  display improvements.
- Android network client version is bumped to `4.1-network`.
- Release deployment now includes the 4.1 default configuration snapshot.

### Known Issues

- Real GPS L1 C/A lock is not proven yet. The GNSS tools are meant for saved IQ,
  replay, RF diagnostics, and future active-antenna testing.
- DMR voice remains experimental/lab quality and is not a stable voice receiver
  in this release.

## 4.0 - 2026-06-11

### Added

- Added optional GPL DMR voice backend modules under
  `dmr_voice_backends/`, with mbelib-neo and OpenDMR/OP25-backed paths.
- Added Ukrainian UI coverage for the main control sections, DMR/digital
  controls, preset manager, network settings, and scan/hunter panels.
- Added release-facing default configuration snapshot in
  `config/fobosapp_defaults_4.0.json`.
- Added DMR backend licensing and attribution notes for GitHub/binary release
  preparation.
- Added a standard-firmware visual scan prototype with an explicit center
  frequency list, sample-rate-safe auto-spacing, and adjustable dwell time for
  slower hosts or libusb-heavy retune paths.
- Added persistent collapsible control-section state so rarely used panels can
  stay closed between launches.

### Changed

- DMR remains experimental, but the 4.0 package can expose the current DMR
  metadata/voice lab state instead of hiding the feature.
- Detailed DMR logging is user-controlled through the settings dialog, with
  log rotation to reduce runaway diagnostic files.
- Standard scan retune timing now uses a small dedicated timer instead of being
  quantized by the spectrum/waterfall refresh interval.

### Known Issues

- DMR voice quality is not beta-stable yet. Real Motorola DMR voice may still
  alternate between intelligible speech, artifacts, and silence.
- External DMR decoder mirroring is documented as the next bridge target but is
  not implemented in the desktop UI yet.

## 3.6 - 2026-06-04

### Added

- Added agile scan measurement tools for broad coverage checks, including
  peak/baseline tracking, CSV export, and graph overlay support.
- Added calibrated spur suppression for stable receiver spurs, with a 50 ohm
  load calibration flow and persisted spur masks.
- Added spectrum hover readout and drag bandwidth measurement on the desktop
  graph.
- Added experimental extended RF tuning range up to 7.75 GHz for receivers and
  firmware that expose useful operation above the official range.
- Added Android network-client controls for FFT length, full/truncated spectrum
  frames, presets editing, fine tuning, and band markers.

### Changed

- Improved DMR laboratory monitoring using controlled Motorola test captures,
  stronger burst/CACH/EMB reporting, and more conservative sync diagnostics.
- Improved auto-center tuning so middle-click/double-click lands on sensible
  rounded frequency steps instead of raw Hz-level estimates.
- Improved desktop persistence for center/listening frequency widgets, unit
  selections, frequency steps, presets, and waterfall/graph level sliders.
- Improved Raspberry Pi/Linux operation with safer Fobos USB refresh/recovery
  behavior and source packaging that excludes the Android project.
- Reworked desktop controls into collapsible sections and refined compact band
  marker display.

### Fixed

- Fixed listening-frequency resets caused by startup/UI refresh paths and
  direct-sampling mode changes.
- Fixed zoomed spectrum/waterfall frame ranges so network/server-side FFT views
  follow the selected visible span.
- Fixed graph/waterfall alignment issues caused by the right-side graph margin.

### Known Issues

- DMR remains a beta sync/activity monitor, not a complete DMR voice decoder.
- Video/image modes remain beta-quality and still need real-signal validation.
- Android is still network-client focused; direct Android USB/OTG receiver
  operation is not production-ready.

## 3.5 - 2026-06-01

### Added

- Added Raspberry Pi/Linux network-server and client hardening around high
  sample-rate Fobos operation, including binary spectrum frames, ready-audio
  relay, and VLC-compatible HTTP/WAV audio streaming.
- Added an Android network client with spectrum/waterfall display, audio
  playback, zoom/pan, tap-to-tune, remote setting controls, preset editor, and
  optional band-marker overlays.
- Added desktop and Android fine-tuning controls, including a horizontal
  mouse-wheel scale mode and an alternate dial mode on desktop.
- Added editable frequency presets and editable spectrum band markers for
  general ranges and amateur/HAM ranges.
- Added collapsible desktop control panels and a compact band-marker display
  mode that draws markers in the lower graph strip instead of tinting the full
  spectrum.
- Added initial Fobos SDR agile compatibility and scan-range UI preparation.
- Added HF1/HF2 noise-cancel laboratory controls and reference spectrum overlay
  tools for local-interference experiments.

### Changed

- Reworked the desktop control layout to reduce vertical space, move less-used
  options into the settings dialog, and add English/Ukrainian UI text loading.
- Improved network command reliability for Android and desktop clients,
  including setting retransmission/debouncing and safer controller/observer
  state updates.
- Improved waterfall persistence and resize behavior so display-history is not
  cleared during routine UI layout changes.
- Improved Linux/Raspberry Pi packaging so vendored patched libfobos sources are
  used instead of silently cloning upstream libraries.

### Fixed

- Fixed Android HF input ordering and direct-sampling tuning synchronization.
- Fixed server/client priority transfer and several network disconnect/restart
  edge cases.
- Fixed client-side frequency and listening-frequency sync in Full IQ and
  server-side spectrum modes.
- Fixed desktop USB/LSB remote display behavior and direct-sampling spectrum
  layout issues found during network testing.

### Known Issues

- DMR remains a beta sync/activity monitor, not a complete DMR voice decoder.
- Video/image modes remain beta-quality and still need real-signal validation.
- Android is still network-client focused; direct Android USB/OTG receiver
  operation is prepared for future work but not production-ready.

## 3.2.1 - 2026-05-29

### Added

- Added an Android USB sandbox for the network client: visible USB device
  listing, Fobos VID/PID highlighting, Android permission request flow, and a
  safe open/close probe that reports raw descriptor `bcdDevice` hints.

### Fixed

- Fixed Android network-client input selection order to match the desktop/server
  Fobos input IDs: RF, HF1+HF2, HF1, HF2.
- Fixed Android HF tuning sync so zero/negative direct-sampling frequencies are
  accepted from spectrum frames and direct inputs always send center frequency
  as 0 Hz.

## 3.2.0-beta - 2026-05-29

### Added

- Added `android/network-client`, an initial Android Studio project for a
  lightweight FobosAPP network client. The skeleton connects to the existing TCP
  control channel, sends basic server-side processing commands, receives
  spectrum/audio frames, draws a simple spectrum/waterfall, plays 48 kHz mono
  PCM audio, and handles basic observer/controller roles.
- Improved the Android client skeleton with a collapsible controls panel,
  explicit light edit-field styling for dark Android themes, server metadata
  syncing from spectrum frames, debounced settings apply, and tap-to-tune on the
  spectrum/waterfall frequency scale.
- Added Android network-client spectrum zoom/pan, tap-to-tune scale handling,
  waterfall level min/max sliders, and a desktop-style waterfall palette.
- Embedded the FobosAPP icon into the Windows executable, Qt runtime resources,
  Linux desktop install rules, and Android launcher resources.
- Added Windows signing helper scripts and optional Android release signing
  configuration driven by environment variables.

### Known Issues

- The Android client is still network-only; direct USB/OTG Fobos receiver access
  is not implemented yet.
- Android audio/display latency and remote-control responsiveness still need
  wider testing on different phones and tablets.

## 3.1.0-beta - 2026-05-29

### Changed

- Started Linux/Raspberry Pi preparation by replacing Windows-only CMake paths
  with platform-aware dependency discovery for Qt5, FFTW, standard libfobos,
  and agile libfobos_sdr.
- Guarded WinMM/Win32-only audio, diagnostics, thread-priority, and device
  enumeration code so non-Windows builds can progress to dependency and runtime
  integration work instead of failing immediately on Windows headers.
- Added an initial non-Windows Qt5 Multimedia PCM playback path for local,
  network, and playback audio frames.
- Added Raspberry Pi OS/Debian helper scripts for dependency installation,
  Linux configure/build, Linux launch, and a Windows source zip packager.
- Added `tools/prepare_fobos_linux.sh` to build the vendored patched libfobos
  and libfobos-sdr-agile source trees into a private project-local Linux prefix.
- Added `third_party/patched/` source trees so Linux/Raspberry Pi builds use
  the FobosAPP-compatible libraries instead of silently cloning official
  upstream code. Upstream cloning is now an explicit opt-in test path only.
- Disabled root-level udev rule installation in the vendored Fobos CMake builds;
  Linux rules are installed by the documented packaging step instead.
- Switched network IQ frames from base64 JSON blobs to binary payload frames with
  compact JSON headers to reduce bandwidth and Raspberry Pi client CPU load.
- In Full IQ client-processing mode, audio now falls back to server-demodulated
  PCM while the client still receives Full IQ for the wide spectrum/waterfall.
  This avoids forcing low-power clients to demodulate audio from 16+ MHz IQ.
- Reverted aggressive Full IQ rate throttling after Raspberry Pi tests showed
  lower traffic did not guarantee lower audio jitter when IQ continuity was lost.
- Made the live IQ audio queue duration-based instead of a fixed float count, so
  low-rate Channel IQ cannot build seconds of stale audio backlog on Raspberry Pi.
- Increased the Qt audio output buffer on Linux to tolerate more network and
  scheduler jitter during remote playback.
- Added a lightweight UDP ready-audio relay: any instance can transmit
  demodulated 48 kHz mono PCM to another FobosAPP instance for low-latency
  playback without an external VLC loop.
- Added a VLC-compatible HTTP/WAV ready-audio stream server so a Raspberry Pi
  client can expose demodulated audio at `http://<host>:<port>/audio.wav`
  without running a second FobosAPP instance on the listener.
- Added Linux packaging drafts for udev USB access and a desktop launcher.

### Known Issues

- DMR remains a beta sync/activity monitor, not a complete DMR voice decoder.
- Video/image modes remain beta-quality and still need real-signal validation.
- The Qt5 Multimedia audio path still needs real Linux/Raspberry Pi testing and
  may need latency/buffer tuning.

## 3.0.0-beta - 2026-05-28

FobosAPP 3.0 beta starts the DMR work as a laboratory/test build. DMR is not a
finished voice decoder yet; this release exposes a DMR sync/activity monitor and
recording metadata tools so controlled test captures can drive the next stage.

### Added

- DMR beta mode in the Digital Audio dock.
- DMR 4FSK sync/activity monitor with frequency-tagged lock and burst reports.
- DMR Lab Capture fields for known test values such as Color Code, timeslot,
  source ID, target/talkgroup ID, radio model, and notes.
- JSON sidecar metadata written next to audio and Channel IQ recordings.

### Fixed

- Corrected the `HF1 + HF2` direct-sampling display model: the spectrum,
  waterfall, and scale now span `-sampleRate/2 .. 0 .. +sampleRate/2`.
- `HF1 + HF2` audio/channel-IQ selection now follows the negative-frequency
  side for HF1 and the positive-frequency side for HF2.

### Known Issues

- DMR is currently a beta sync/activity monitor, not a complete DMR decoder.
- DMR symbol/frame synchronization still needs controlled lab captures before
  payload and voice decoding can be considered reliable.
- Video/image beta limitations from 2.9 remain.

## 2.9.0-beta - 2026-05-28

FobosAPP 2.9 beta is an intentionally experimental video/image decoding build.
The new decoders are present so users with suitable signals can help test them.

### Added

- Video dock modes for ATV, SSTV, NOAA APT, HF WEFAX, and Meteor LRPT beta.
- SSTV decoder support for Robot, Martin, Scottie, Wraase, and PD-family modes,
  plus an internal SSTV test pattern.
- NOAA APT image decoder with a real decoder-path test pattern.
- HF WEFAX beta decoder with a real decoder-path test pattern.
- Meteor LRPT beta QPSK IQ monitor for signal inspection and future image work.
- Analog video VSync option and PAL/NTSC line-rate presets.

### Changed

- Moved video/image modes into the Video dock and digital text/audio modes into
  the Digital Audio dock.
- Added video/image bandwidth presets for SSTV, APT, WEFAX, and LRPT.
- Extended audio filtering and FT8 decode work from the 2.x development builds.

### Known Issues

- WEFAX, LRPT, and analog video are beta-quality and need real-signal testing.
- LRPT currently shows a QPSK/IQ monitor rather than a finished satellite image.
- Video decoding can still be CPU-heavy on weak clients or wide IQ streams.

## 2.0.0 - 2026-05-27

FobosAPP 2.0 is the first broadly usable Windows release after the audio,
stability, and network-control rebuild.

### Added

- Network server/client operation with control-channel handshake.
- Multi-client networking: the first client gets control, later clients become
  observers, and observers can request control transfer.
- Server-side spectrum/audio streaming, channel IQ streaming with client
  demodulation, and full IQ client processing mode.
- Persistent `FobosAPP.ini` settings for receiver, display, audio, and network
  options.
- Support code for both standard `fobos.dll` and agile `fobos_sdr.dll` APIs.
- Device listing with multiple receiver support.
- New frequency controls with units, presets, and step buttons for center
  frequency, listening frequency, and audio bandwidth.
- Right-click spectrum tuning actions for signal center, USB lower edge, and
  LSB upper edge.
- Resizable main window and dockable control panel.
- Spectrum Y-axis level labels and min/max level controls.
- Zoom range extended down to 0.1 percent.

### Changed

- Reworked Fobos start/stop lifecycle around async streaming and safer device
  close/reset handling.
- Improved live retuning while running, including sample-rate and FFT changes.
- Reworked AM/FM/SSB/CW/FT8/FSK/RTTY/PSK audio path and mode bandwidth presets.
- Split NFM and WFM modes.
- Optimized waterfall drawing and sparse network spectrum rendering.
- Reduced server load when streaming to clients by allowing local visual/audio
  suppression.
- Reduced dependency footprint by dropping unused Qt OpenGL and Multimedia
  modules from the link list.

### Fixed

- Frequent crashes on stop, restart, FFT length changes, and sample-rate changes.
- Client stop-button state after remote start.
- Network disconnect detection.
- Client visual freezes after processing-mode changes.
- Missing scaling propagation in network visual modes.
- Waterfall/spectrum gaps when zooming networked spectrum frames.
- Scientific-notation frequency display in normal UI fields.

### Known Issues

- Digital protocol decoding is not complete yet.
- Full IQ network mode can fragment audio on weak links or slow clients.
- Multi-client mode needs more outside testing.
