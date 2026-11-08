# Obrii SDR 8.2.0

## Highlights

- Saved, named IPv4 discovery endpoints for routed LANs and VPNs, bounded TCP
  fallback when UDP discovery is unavailable, last-reply/method tooltips and
  an optional metadata-only node catalogue. No automatic subnet scan, firewall
  changes or RF activation. See `docs/mesh_vpn_discovery.md`.
- Live mesh node/link append, independent enable/disable and explicit recovery.
  Failed participants no longer release healthy sessions. Acknowledged topology
  revisions, affected-work cancellation, native identity and cumulative counters
  preserve unrelated branches. This is controlled known-graph topology, not a
  learned MANET routing protocol.
- Experimental LoRaWAN 1.1 OTAA/ABP, split counters/keys and EU868 unicast Class
  B/C simulation. Shared native adapters, implicit-header beacon IQ, modeled
  receive opportunities, durable state and independent vectors are covered by
  software tests. See `docs/mesh_lorawan_adapter.md` and `docs/lorawan_module.md`.
- GCC/ARM mesh compilation fixes for Qt IQ containers and 64-bit timing types,
  verified on Raspberry instead of relying solely on Windows compilation.

## Scope

LoRaWAN remains an experimental single device/network pair and simulated
transport. Physical LoRaWAN RF is deliberately blocked until timestamped SDR
TX/RX is qualified. This is not a certified network server or MAC. Automatic
rejoin/roaming, multicast and FUOTA policy are not implemented. Low-level rejoin
frame codecs are not an automatic recovery implementation.

The directory is off by default. It retains public locator metadata only,
requires a credential for registration, renews every 30 seconds and expires
records after 90 seconds. Listing never grants control: every endpoint is
checked directly and normal node pairing/ownership still applies. Use trusted
LAN/VPN networks; these TCP protocols do not provide TLS.

## Packages

Full and Lite Windows x64 packages and Full and Lite Raspberry/Linux source
packages contain the shared GUI/CLI code base. No Wireless add-on or unchanged
Android APK is republished. Android is maintained separately; these optional
desktop/headless mesh changes do not require an Android client update.

Keep user settings, keys, private directory profiles, recordings and calibration
data local. They are not release assets. The Linux dependency helper includes
`libssl-dev` for shared mesh cryptography and `libcodec2-dev` for M17 voice.
Optional engines and redistribution
notices remain listed in `THIRD_PARTY_LICENSES.txt` and module guides; Reticulum
and vendor UHD/FPGA images still require separate setup. Never auto-flash a clone.

## Verification

The release is gated on Windows and Raspberry software builds, bounded mesh
regressions, LoRaWAN core/reference vectors and real two-host VPN discovery and
DATA exchange. Software tests do not qualify RF hardware, precision timestamps,
high-rate IQ throughput or every router/firewall configuration.

Feedback: mojamajsternya@gmail.com
