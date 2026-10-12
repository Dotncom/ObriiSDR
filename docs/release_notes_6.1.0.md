# Obrii SDR 6.1.0

This release extends the 6.0 receiver and research workstation with optional
external science backends, a Wi-Fi IQ decoding laboratory, and more independent
DSP and measurement tools. Android is unchanged and is not included.

## Highlights

- Live receive-only Wi-Fi IQ-to-packet decoding through an isolated GNU Radio
  process, with bounded queues, visible loss counters, PCAP output, and packet
  inspection through bundled optional Wireshark/TShark tools. Offline IQ-file
  decoding remains available. HT20 is experimental: software fixtures pass,
  but real-air HT data reception has not been confirmed.
- A typed DSP-board model and opt-in DDC/FFT research branches with per-branch
  decimation and output-rate settings. The optimized main RX/audio/DMR path is
  unchanged; drawn links are not a general-purpose executable flowgraph.
- Native BPSK31 reception, condition-bound calibration profiles, up to 64
  persistent markers, tonal SNR/SINAD/THD measurements, and HDF5 spectrum export.
- Optional PocketSDR GNSS tracking/PVT from IQ, SatDump Meteor LRPT offline
  image decoding, and monthly ITU-R P.533/P.372 HF predictions. Live GNSS IQ
  can still overrun on demanding receivers; RF validation remains incomplete.
- Configurable public space-weather, comet and meteor information sources,
  without automatic network uploads of receiver or location data.
- Rapid live sample-rate selections now coalesce before restarting the receiver.

## Packages

- `ObriiSDR-v6.1.0-windows-x64.zip`: portable Windows core runtime, research
  backends, notices, documentation, and a checked wireless-add-on installer.
- `ObriiSDR-v6.1.0-wireless-windows-x64.zip`: optional portable GNU Radio Wi-Fi
  PHY and Wireshark/TShark runtimes. Extract into the core folder or run
  `Install-Wireless-AddOn.ps1` beside `ObriiSDR.exe`; no GNU Radio installer is
  required. Npcap must be installed separately for live network-adapter capture.
- `ObriiSDR-v6.1.0-raspberry-source.tar.gz`: Raspberry/Linux source, not an ARM
  binary. Optional GNU Radio, PocketSDR and SatDump paths depend on local builds
  or separately installed runtimes; see `docs/research_backends.md` and
  `docs/wireless_lab.md`.
- `ObriiSDR-v6.1.0-SHA256SUMS.txt`: SHA-256 checksums of all three packages.

## Known Limits

Wi-Fi decoding is receive-only, not a network interface or a Wi-Fi transmitter.
Only legacy 20 MHz OFDM has confirmed CRC-valid real-air frames in this release;
HT20, modern Wi-Fi, Bluetooth and cellular SDR PHY support are not complete.
GNU Radio is integrated for the specialized Wi-Fi decoder, not yet as a universal
DSP-board bridge. GNSS IQ, LRPT and HF model results require independent field
validation. Personal captures, logs, settings, keys and coordinates are excluded
from release packages.
