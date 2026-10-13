# Obrii SDR 6.1.1

Maintenance update for 6.1.0: online terrain elevations, Windows HTTPS fixes,
and live Wireshark integration. Android is unchanged and is not included.

## Changes

- **Online elevation:** MapTiler can supply TX/RX ground heights and terrain
  for path profiles and coverage without manually downloading HGT files.
  Uses the QTH map's existing key. Enable the separate per-session permission:
  online requests disclose the selected locations/area to MapTiler. Terrain
  tiles stay in memory; missing elevations are not replaced with flat ground.
- **Windows HTTPS fix:** elevations now use the system TLS fallback when Qt
  has no OpenSSL runtime, just like the working base map. Background requests
  are cancellable; error details do not expose keys or coordinates.
- **Live Wireshark:** during live SDR Wi-Fi decoding, the Wireshark button
  starts an extcap capture instead of opening a static file. A bounded local
  packet bus supplies CRC-valid frames; reaching the PCAP file limit no longer
  stops the live stream. This is not a virtual network adapter or Wi-Fi TX.
- **Wi-Fi diagnostics:** separate HT/VHT header CRC and payload CRC counters,
  observed VHT bandwidths and IQ queue-loss statistics. Fixed stale HT Python
  paths after relocating the runtime. HT/VHT remain experimental; recognized
  headers are not evidence of successfully decoded data.
- Local Windows development now uses one persistent `build/runtime` directory.

## Downloads

- `ObriiSDR-v6.1.1-windows-x64.zip`: portable Windows core and science runtimes.
- `ObriiSDR-v6.1.1-wireless-windows-x64.zip`: optional matching GNU Radio Wi-Fi
  and Wireshark/TShark add-on. Extract into the core folder or run the supplied
  `Install-Wireless-AddOn.ps1`, which checks the package SHA-256. Update this
  add-on too when upgrading from 6.1.0. Npcap is not bundled and is not required
  for the SDR-to-Wireshark stream.
- `ObriiSDR-v6.1.1-raspberry-source.tar.gz`: Linux/Raspberry source, not a
  prebuilt ARM executable. Extract with `tar -xzf`, then use `tools/build_linux.sh`.
- `ObriiSDR-v6.1.1-SHA256SUMS.txt`: checksums for the three packages.

Personal settings, API keys, geographic markers, captures and diagnostic logs
are excluded. Settings remain in the per-user configuration directory.

## Зворотний зв'язок / Feedback

[mojamajsternya@gmail.com](mailto:mojamajsternya@gmail.com)

Для повідомлення про проблему вкажіть версію програми та кроки відтворення.
Перед надсиланням логів або знімків приберіть ключі API, приватні координати
та інші чутливі дані.

For bug reports, include the app version and reproduction steps. Remove API
keys, private locations and other sensitive data from attachments.
