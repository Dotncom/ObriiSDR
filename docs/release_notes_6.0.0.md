# Obrii SDR 6.0.0

This major release expands DMR reception, signal generation and RF research
while keeping the optimized receiver/audio path and modular DSP workspace.

## Highlights

- Known-key Motorola ARC4/AES-256 decryption in the native mbelib/OpenDMR paths,
  with independent slot state, validated privacy headers and late-entry recovery.
  Native ARC4/AES-256 speech and Gopher ARC4 were confirmed on the user's radio;
  this is not a guarantee for every vendor, firmware or signal condition.
- Repaired DSD-neo/GopherTrunk integration and clear backend-capability labels.
  DSD-neo supports ARC4/AES-256; the Gopher adapter supports ARC4, not DMR AES.
- FT8 station manager: standard message/QSO workflow, ADIF journal, optional
  network services, separate RX/TX routing, callsign/340-entity DXCC filters,
  and manual callsign-marker export to the QTH map.
- Signal Lab: reproducible analog, PSK/QAM, FSK/GFSK and OFDM signals, bounded
  I/Q and 2D/3D previews, corrected RRC/FFT normalization, IQ/metadata export,
  and the normal explicitly armed HackRF output path.
- Experimental clear DMR Tier II direct voice generation from microphone/PTT/WAV.
- Terrain-based coverage, link budget and TX-RX/Fresnel profiles using local
  SRTM HGT files and free-space, dominant-obstacle or NTIA ITM models. Map markers
  can be moved, swapped and measured; NOAA F10.7/Kp is fetched only on request.
- Beta wireless packet laboratory using separately installed TShark/Wireshark.
- I/Q/time 3D constellations in Signal Lab, live IQ research and DSP-board widgets,
  with camera controls and an optional filled radial surface.
- Retained VFO waterfall history on level changes, fewer repeated UI rebuilds,
  bounded deferred FFT snapshots, live audio prebuffer/rebuffer improvements,
  clearer opt-in diagnostics, expanded translations and help.

## Packages

- `ObriiSDR-v6.0.0-windows-x64.zip`: Windows x64 runtime with supported optional
  DMR/HackRF libraries, decoder executables and third-party notices.
- `ObriiSDR-v6.0.0-raspberry-source.tar.gz`: Raspberry/Linux source tree.
- `ObriiSDR-v6.0.0-SHA256SUMS.txt`: SHA-256 checksums.

Android is intentionally unchanged and is not uploaded in this release.

## Important Notes

- Only user-supplied DMR keys are used. There is no key search or brute force.
  Hytera Basic is an unverified beta; vendor-specific full encryption is not claimed.
- DMR TX does not implement privacy, repeater wake-up or trunk signaling.
- HackRF TX remains experimental and explicitly armed. The application cannot
  measure SWR; use a suitable antenna/dummy load, attenuation and measurement
  equipment, and observe local operating limits.
- The filled constellation is an optional visual interpolation, not IQ recovery
  or symbol decoding. Axis amplitudes and timestamps retain their documented meaning.
- Wireless selectors inspect captures and SDR channel energy, not native
  Wi-Fi/Bluetooth/GSM/UMTS/LTE PHY decoding or a real-time MAC/link stack.
- Propagation maps are planning estimates. They require appropriate local terrain
  data; absolute levels also depend on calibration, gains/losses and environment.
  The NOAA panel is not an HF ionospheric path predictor.
- SDR-only GNSS, ATV/video and untested Soapy devices remain experimental.
  bladeRF is disabled until direct hardware debugging is possible.
- Raspberry/Linux sources are refreshed, but this release was built and tested
  on Windows; an ARM/Linux build must be performed on the target machine.
- The packages exclude personal settings, logs, captures, screenshots, QTH/GNSS
  positions, API tokens, radio keys and receiver serials. Online services require
  user action/configuration. The supplied crypto fixtures are synthetic test data.
- Detailed information/debug logging is controlled by the Logging option;
  warnings and errors remain available when it is off.

## Upgrade

Replace the extracted program folder, not the per-user settings directory.
Existing Obrii SDR/FobosAPP settings are migrated or reused; export a settings
backup separately before an upgrade. Linux dependencies and run/build commands
are described in README.md and tools/install_deps_debian.sh.
