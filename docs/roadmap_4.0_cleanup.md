# Obrii SDR Live Roadmap

This document contains only current state and remaining work. Completed release
history belongs in CHANGELOG.md; detailed stability notes belong in
docs/release_stability_handoff.md.

## Current Baseline

The v6.2.0 release baseline provides:

1. First-class Fobos Standard and Agile receive paths, native RTL-SDR, and a
   hardware-tested native HackRF receive path. SoapySDR remains optional and
   experimental; bladeRF is hidden until real hardware is available.
2. Low-copy IQ publication, background FFT processing, FFTW multithreading,
   optional Vulkan/VkFFT, exact Hz-per-point FFT lengths, and optimized 2D/3D
   spectrum and waterfall rendering.
3. Native HackRF transmission laboratory with guarded arming and AM, FM, SSB,
   CW, FT8, audio-file, image, SSTV, and experimental video waveform sources.
4. Multi-VFO analysis over one shared wide FFT, channel mosaics, independently
   ranged mini spectra and waterfalls, and a selected audio monitor.
5. Spectrum-frame and IQ pre-trigger recording, synchronized replay,
   replay-side tuning/audio, 2D/3D views, and an area ruler.
6. Scientific spectrum tools: calibrated units, ENBW/RBW/VBW, overlap,
   detector modes, traces, markers, channel metrics, zero-span, masks,
   baseline comparison, triggers, pulse analysis, density/persistence,
   oscilloscope, constellation, eye diagram, synchronization analysis,
   dual-input HF analysis, and measurement sessions.
7. Frequency/amplitude calibration tables with interpolation, uncertainty,
   external-chain correction, and a guided live-spectrum calibration wizard.
8. IQ diagnostics for sequence/epoch/queue state, measured arrival rate,
   clipping, invalid samples, DC/IQ imbalance estimates, and opt-in waterfall
   rows for known internal audio/FFT losses, not asserted USB/driver losses.
9. A configurable audio-filter chain, graphical DSP board, saved DSP layouts,
   interface profiles, scanning/hunting tools, presets, network operation,
   serial NMEA/UBX GNSS, QTH mapping, and experimental digital decoders.
10. Optional native LAN computation for DDC branches and main RF FFT, a bounded
    GNU Radio bridge, branch IQ recording, and a cancellable connection preflight.
    These are experimental; full-rate two-computer performance is not qualified.

## Release-Grade Boundaries

Usable but still requiring explicit qualification:

1. Absolute dBm/dBuV/uV accuracy is only as good as the user's generator,
   attenuator, cables, gain state, and calibration table. The wizard automates
   measurement; it does not create traceability.
2. HackRF TX must be tested into a dummy load with appropriate attenuation and
   local legal limits. Fobos hardware remains receive-only.
3. Vulkan/VkFFT performance varies by GPU, driver, FFT length, and platform.
   Auto and CPU fallback must remain available.
4. Known internal discontinuities can be marked by the optional diagnostic rows.
   A true USB/driver loss marker
   still depends on each backend exposing trustworthy transfer-loss data.

Experimental, not a stable-release claim:

1. DMR voice/metadata, DSD-neo, GopherTrunk, and authorized ARC4/AES key paths.
2. SDR-only native GNSS acquisition and the optional PocketSDR PVT adapter;
   validate real fixes and recorded-IQ replay before a stable claim.
3. FPV/ATV/digital-video decoding beyond current laboratory scaffolding.
4. SoapySDR across untested hardware and all bladeRF support.

## Remaining Priorities

### Wireless Laboratory (Beta)

Portable TShark/Wireshark, capture/import, packet details/profile keys and the
isolated gr-ieee802-11 20 MHz recording-to-PCAP adapter are implemented/tested.
Validate real Wi-Fi captures and monitor-mode hardware. PCAP decoder time is
not RF time. Bluetooth/GSM/UMTS/LTE remain packet inspection, not SDR PHY.
Next: qualified live-IQ/timestamp transport, BLE advertising and GSM broadcast.
The B210 planning template defines multi-channel/full-duplex/device-clock
contracts but cannot authorize TX. Native UHD/clone tests, real-time PHY/MAC,
Bluetooth hopping and cellular links remain future work. See docs/wireless_lab.md.

### P0: Stability And Release Hygiene

1. Keep start/stop/close operations bounded and off the UI thread where device
   APIs permit it.
2. Keep normal logging quiet; detailed FFT, retune, transport, GNSS, and DMR
   diagnostics remain behind the logging option.
3. Preserve the Fobos-specific hot path. Generic integrations must not insert
   adapters, schedulers, or copies into its callback-to-IQ-buffer path.
4. Continue splitting cold UI/feature glue out of main.cpp without changing
   live receive semantics.
5. Build and smoke-test Windows and Raspberry source before each public tag.

### P1: Physical Validation

1. Validate calibration across receiver, input, sample-rate, gain, and
   frequency combinations with a known generator and attenuator.
2. Validate HackRF RX controls and TX modes on real hardware, including
   underrun reporting, guard limits, and dummy-load tests.
3. Reproduce any remaining Fobos Agile startup shift or shutdown delay with
   current firmware and libraries before changing the proven startup sequence.
4. Profile Raspberry audio continuity while optional scientific windows,
   multi-VFO views, and high FFT lengths are active.
   The current display/DSP audit and deterministic test results are recorded in
   docs/performance_and_dmr_audit.md. Known-key Motorola voice now has RF
   confirmation; repeatable weak-signal and broader hardware testing remain.

### P2: Architecture Extensions

1. Qualify the new optional stream manager on Raspberry-to-laptop LAN. Native
   compute workers and the external GNU Radio adapter now process DDC IQ and
   return FFT/IQ, with bounded queues and explicit gaps. Main RF FFT now supports
   individual windows or continuous full-band IQ on the native worker. DDC and
   audio remain local; Wi-Fi/GNSS offload is not implemented. See
   docs/dsp_stream_manager.md and docs/gnu_radio_compatibility.md.
2. Independent bounded DDC recorders are implemented. Independent demodulator/
   audio sinks and a measured PFB stage remain future work. The existing
   Multi-VFO path still shares one FFT and one proven audio monitor.
3. Add backend-specific USB/driver discontinuity counters only where the
   underlying API can report them reliably.
4. Streaming HDF5 spectrum export, condition-bound calibration, up to 64
   markers and single-tone SNR/SINAD/THD are implemented. Validate RF results
   with a known generator. CSV/JSON/SigMF remain the dependency-light baseline.
5. Separate exact full-resolution measurements from optional overview/selected-
   band FFT modes. First evaluate linear-power display reduction before dB
   conversion; preserve calibration, detectors, recording and RBW semantics.
   Do not silently replace a requested long FFT with a shorter display FFT.

### P3: Deferred Signal Work

The optional ITU-R P.533/P.372 monthly HF planner is implemented and tested
against published fixtures/all months. It is not a real-time storm model.
NOAA/comet/meteor/fireball source URLs are editable in General settings; network
refresh is explicit or independently opted in while the event feed is visible.

1. Validate Hytera Basic on HP785 and extend repeatable weak-signal/timeslot
   coverage. Native Motorola ARC4/AES-256 speech is RF-confirmed;
   cipher/PI/late-entry fixtures pass. Gopher DMR AES remains unsupported.
2. Validate the optional PocketSDR live/file GNSS PVT adapter and SatDump
   Meteor LRPT file-to-image path with repeatable RF captures. The native
   LRPT live view remains a QPSK monitor, not a complete live image decoder.
   Native BPSK31 reception has synthetic tests; validate real interoperability.
3. Resume video-decoder work only with repeatable captures and expected output.
4. Revisit multi-receiver display and synchronization after one-receiver
   operation remains stable on Raspberry-class hardware.

## DSP Board Coverage

The DSP board has direct blocks or launchers for receiver/network/playback
sources, tuning, channel filtering, resampling, FFT, demodulation, every audio
filter stage, audio output, record/replay, TX, scanning/hunters, Multi-VFO,
2D/3D displays, digital text/image/audio/video, GNSS/QTH, presets,
calibration, GPIO, backend-specific Fobos/HackRF tools, and application
settings.

Dedicated measurement blocks now open the exact tool for interference/comb
analysis, statistics, IQ diagnostics, dual-input HF analysis, the scientific
analyzer, density, selected-band density, masks/baseline, pulse analysis, and
measurement sessions. Oscilloscope, constellation, eye diagram, digital
synchronization, VFO spectrum, and VFO waterfall also have self-contained
visual blocks.

The board now also executes an explicit Source -> DDC -> branch FFT/display
grammar alongside the unchanged main pipeline. Two DDC branches and two FFTs
per branch are supported; research views can consume DDC IQ. Processing uses
one bounded worker, snapshot reads (never audio-queue consumption), generation
checks and gap counters. Hidden/offscreen consumers stop requesting work unless
an independent recorder is explicitly armed. Optional external FFT/IQ processing
uses the stream manager, not a replacement of the main pipeline. Simplified
operating mode suppresses all these optional DDC branches and recordings.
Typed links distinguish data, controls, bindings and legacy diagram-only links.
New board audio filters start disabled and deletion removes their matching
table stage. Existing saved stages retain their activation state.

Remaining graph work: independent demodulator/audio sinks, an explicit
opt-in audio-filter order compiler, configurable FIR quality/transition bands,
and a measured PFB stage for many channels. There is no arbitrary IQ scheduler
and no graph-controlled replacement of the main RX/audio/DMR pipeline.

## Release Checklist
Radio propagation planning is implemented as an independent, manually started
tool: local SRTM HGT, dominant knife-edge / NTIA ITM / free-space models,
coverage raster, link budget and terrain/Fresnel profile. The independent
ITU-R P.533/P.372 HF planner adds monthly MUF/SNR/reliability predictions;
NOAA F10.7/Kp and public space-event feeds remain separate observations, not
real-time corrections to the propagation model.
FT8 filters now use the complete 340-entity CTY/DXCC catalog, with local CTY
import and explicit callsign-labeled journal export to the QTH map.
See docs/radio_propagation_lab.md for limits and privacy behavior.


1. Build Windows x64 and smoke-test build/runtime; use clean packages for release.
2. Package Raspberry/Linux source and verify build/run/dependency scripts.
3. Do not publish Android unless it was rebuilt and tested on its Android
   development machine.
4. Parse both translation JSON files and inspect new English/Ukrainian labels.
5. Exclude settings, logs, screenshots, tokens, map keys, DMR keys, GNSS raw
   logs, coordinates, device serials, and real IQ/audio/video captures.
6. Preserve user settings and presets during upgrades; document import/export.
7. Verify optional third-party binaries and notices are complete and licensed.

## Privacy Rules

1. ObriiSDR.ini and legacy FobosAPP.ini are always local user data.
2. Exact GNSS coordinates, QTH markers, map keys, DMR keys, tokens, logs, and
   recordings must never enter release archives or commits.
3. Diagnostic logs should avoid exact coordinates unless the user explicitly
   exports them.
4. Generated reports should identify whether personal location or receiver
   serial metadata is included before export.
