# Optional Wireless Add-On (Windows)

The 6.2.0 core package works without this add-on. The Wi-Fi PHY file/live IQ
decoder and bundled TShark/Wireshark use the unchanged 6.1.1 Wireless add-on,
which is compatible with both 6.1.1 and 6.2.0. Existing installations can keep it.
It contains isolated GNU Radio runtimes for the tested legacy 20 MHz decoder
and experimental HT20 decoder. No separate GNU Radio installation is required.

## Install

1. Extract `ObriiSDR-v6.2.0-windows-x64.zip` to a writable folder.
2. With Obrii SDR closed, run `Install-Wireless-AddOn.ps1` from that folder:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-Wireless-AddOn.ps1
   ```

   The script downloads the matching GitHub Release asset, verifies its pinned
   SHA-256, checks its paths, and extracts it beside `ObriiSDR.exe`.

For an offline installation, download
`ObriiSDR-v6.1.1-wireless-windows-x64.zip` from the [6.1.1 release](https://github.com/Dotncom/ObriiSDR/releases/tag/v6.1.1),
verify its SHA-256 against `ObriiSDR-v6.1.1-SHA256SUMS.txt`, then extract its
contents into the folder that contains `ObriiSDR.exe`. Alternatively, pass its
path to the same installer: `-ArchivePath C:\path\to\wireless.zip`. The expected directories
are `wifi-phy`, `wifi-ht`, `wireshark`, and `wireless`; do not create another
nested ObriiSDR folder. The 6.2.0 core installer pins this compatible package
and its checksum; the separate version avoids another identical large download.

Npcap is a separate driver, not part of this add-on. It is needed only for
live capture through a supported network adapter, not for SDR IQ decoding or
opening saved PCAP files. Obtain it from https://npcap.com/ if needed.

The SDR Wi-Fi decoder is receive-only. Legacy 20 MHz OFDM has real-air
CRC-valid control-frame evidence; HT20 has passed software fixtures but has
not yet decoded a confirmed real-air HT data frame. This add-on does not make
Obrii SDR a Wi-Fi network interface.

## Live packets in Wireshark (6.1.1 and later)

Start **Decode live SDR IQ** in Wireless Lab, then press **Wireshark**. The
bundled Wireshark opens the `Obrii SDR live Wi-Fi frames` extcap interface and
receives the same CRC-valid radiotap/802.11 frames as Obrii's table and PCAP
file. You can also select that interface in Wireshark directly while live
decoding is running. This path uses a local loopback packet bus, not Npcap or
an emulated Windows network adapter. It cannot carry operating-system network
traffic or transmit Wi-Fi frames.

The bus retains up to 2 MiB of recent frames for a late Wireshark connection
and permits four independent readers. A slow reader is disconnected when its
4 MiB queue fills; IQ decoding and PCAP recording continue. The PCAP file
still has its 100 MiB / 500,000 packet cap, but reaching that cap no longer
stops the live decoder or Wireshark stream. Timestamps are decoder wall-clock
times because the PHY backend does not report an RF sample timestamp.

The local connection descriptor is under `%LOCALAPPDATA%\ObriiSDR\wireless-phy`
on Windows and is removed when live decoding stops. It contains a random
per-session token; treat that file as private. The extcap wrapper and the
decoder must be from the same build. Update both the core and Wireless add-on
when upgrading from 6.1.0, which did not include this live bridge.
