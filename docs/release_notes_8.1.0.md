# Obrii SDR 8.1.0

## Highlights

- Offline CPU FFT plan/thread tuning, cached per PC, FFTW binary and FFT size.
  RX/replay must be stopped. Candidates are validated, exported and restored
  before timing; bounded-planning FFTW 3.3.5 wisdom is made reusable. Diagnostic
  logging confirms actual cache use. No background tuning during reception.
- Optional digital-phosphor **Contours** in the main spectrum and DSP board.
  Classic remains available. Amplitude-axis ridge contrast and logarithmic
  brightness expose existing density contours without mixing frequency columns.
- Faster FFT magnitude classification, channelizer finite-value checks and
  display reduction, preserving normalization and measurement contracts.
- Distributed Mesh Designer nodes, LAN discovery/registration, packet routing,
  DATA/IQ bearers, isolated bounded queues and independent RX/TX ports.
  Optional real RX ingress and explicitly authorized, expiring RF egress reuse
  existing receiver/transmitter paths; Fobos remains RX-only.
- Native LoRa PHY and experimental Meshtastic, external Reticulum and modeled
  EU868 LoRaWAN adapters. LoRaWAN physical RF is deliberately disabled pending
  SDR timing qualification; this is not a certified network server or MAC stack.
- Optional authenticated loopback IQ-hub control for live link gain, delay and
  enabled state. The IQ data plane itself remains trusted-network only.
- CLI headless linkage, virtual-IQ restart, audio discontinuity recovery,
  bounded HackRF burst shutdown, credential redaction and expanded help/tests.

## Packages And Optional Engines

Windows x64 and Raspberry/Linux source are supplied in Full and Lite variants.
GUI and CLI share the same source tree. Lite prioritizes basic radio browsing
and omits large decoder engines. Android has no functional changes in this
release and is not repackaged; its existing 8.0 preview remains available.

No private INI files, calibration tables, FFT profiles, keys, logs, recordings
or local roadmaps are included. Existing installations keep their local data.
Rerun FFT tuning for older profiles that report `restore-failed`.

The Wireless add-on is not included. Full Windows retains the redistributable
engines, notices and source materials listed in `THIRD_PARTY_LICENSES.txt`.
Reticulum is optional and **not bundled**: install the pinned RNS environment
as documented in `docs/reticulum_module.md`, then configure its local executable
and private state paths. UHD/vendor FPGA images remain separate; see
`docs/usrp_b210_setup.md`. Never automatically flash the Kintex-7 clone.
Linux dependencies and optional engine setup are documented under `tools/`
and in the corresponding module guides.

## Qualification

Windows synthetic/regression tests and live Fobos 50 MS/s checks were used.
The selected 8M CPU plan was confirmed in the live log. Timing depends on host,
thermal state and concurrent DSP; no fixed FPS or speedup is guaranteed.
New distributed/physical protocol combinations still require real hardware
and Raspberry qualification. Do not treat simulation as proof of RF behavior.

Feedback: mojamajsternya@gmail.com
