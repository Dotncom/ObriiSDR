# Optional Wireless Add-On (Windows)

The 6.1.0 core package works without this add-on. The Wi-Fi PHY file/live IQ
decoder and bundled TShark/Wireshark need the matching 6.1.0 Wireless add-on.
It contains isolated GNU Radio runtimes for the tested legacy 20 MHz decoder
and experimental HT20 decoder. No separate GNU Radio installation is required.

## Install

1. Extract `ObriiSDR-v6.1.0-windows-x64.zip` to a writable folder.
2. With Obrii SDR closed, run `Install-Wireless-AddOn.ps1` from that folder:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-Wireless-AddOn.ps1
   ```

   The script downloads the matching GitHub Release asset, verifies its pinned
   SHA-256, checks its paths, and extracts it beside `ObriiSDR.exe`.

For an offline installation, download
`ObriiSDR-v6.1.0-wireless-windows-x64.zip` from the same GitHub Release,
verify its SHA-256 against `ObriiSDR-v6.1.0-SHA256SUMS.txt`, then extract its
contents into the folder that contains `ObriiSDR.exe`. Alternatively, pass its
path to the same installer: `-ArchivePath C:\path\to\wireless.zip`. The expected directories
are `wifi-phy`, `wifi-ht`, `wireshark`, and `wireless`; do not create another
nested ObriiSDR folder. Keep the add-on version matched to the core version.

Npcap is a separate driver, not part of this add-on. It is needed only for
live capture through a supported network adapter, not for SDR IQ decoding or
opening saved PCAP files. Obtain it from https://npcap.com/ if needed.

The SDR Wi-Fi decoder is receive-only. Legacy 20 MHz OFDM has real-air
CRC-valid control-frame evidence; HT20 has passed software fixtures but has
not yet decoded a confirmed real-air HT data frame. This add-on does not make
Obrii SDR a Wi-Fi network interface.
