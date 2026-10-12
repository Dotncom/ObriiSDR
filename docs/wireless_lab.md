# Wireless Laboratory (Beta)

## Available Now

Open **Spectrum measurements > Wireless lab (beta)**, or add the **Wireless lab**
block to the DSP board. This is a separate, resizable window, not a new decoder
inside the receiver callback.

- Open PCAP/PCAPNG captures through an installed TShark.
- Windows development/runtime folder now includes isolated Wireshark/TShark
  4.6.9 and an optional GNU Radio 3.10.12/gr-ieee802-11 file decoder.
- List capture interfaces and explicitly start/stop live capture.
- Request Wi-Fi monitor mode when the adapter, driver and OS support it.
- Inspect timestamp, protocol, addresses, SSID/BSSID, frequency, RSSI, subtype,
  retry/protected flags, FCS status, sequence number and protocol summary.
- Aggregate Wi-Fi networks and RSSI; export visible packet history to UTF-8 CSV;
  open the original capture in Wireshark.
- Read Bluetooth/BLE, GSM/GSMTAP and LTE captures with an editable display filter.
  These selectors do not create RF decoders for those standards.
- Inspect a saved packet's complete protocol tree and captured bytes by double
  click. Details are read asynchronously, capped at 4 MiB / 15 seconds.
- Select or type a Wireshark profile, enable known Wi-Fi keys from that profile,
  and apply a BPF capture filter separately from the display filter. No key is
  copied to Obrii. Protected frames with dissected LLC payload are identified
  separately; a protected/null/management frame is not proof of decryption failure.
- Observe the current SDR spectrum's mean FFT-bin power and partial coverage
  of nominal 20 MHz Wi-Fi channels. This is not calibrated RSSI, occupied
  channel power, packet detection or an OFDM decoder.
- The Audio checkbox mirrors the main receiver-audio control. Switching it off
  stops the normal demodulated audio; it does not stop RF capture or IQ decoding.
- **Decode live IQ** sends full-bandwidth CS8 blocks from a running RF receiver
  to the same isolated GNU Radio PHY process without an intermediate IQ file.
  CRC-valid legacy OFDM frames appear in the packet/network tables immediately
  and are also written to a PCAP in the selected output folder. **Stop live**
  ends the process. The input queue is limited to 16 MiB, the process pipe to
  2 MiB, and the UI reports dropped IQ samples; overload is not hidden as valid
  continuous reception. A receiver center or sample-rate change stops the
  live decoder for an explicit restart. Stop/close of the receiver or lab also
  stops live decoding. The original RF sample timestamps and calibrated RSSI
  remain unavailable; displayed/PCAP times are decoder wall-clock times.
  Live CS8 uses its own slow adaptive gain before quantization, preserving weak
  RF noise and packets that would otherwise round to zero. The status shows
  samples fed to the process, samples actually read by GNU Radio, queue depth
  and drops. Zero decoded packets does not mean no IQ: the decoder supports
  legacy 20 MHz OFDM only, and a busy 50 MS/s receiver may fail to supply a
  continuous full-rate channel. Prefer the lowest rate covering the complete
  20 MHz channel and tune the SDR center to that channel for the first test.
  The wireless lab duplicates the main spectrum/waterfall update interval
  control; both edit the same persisted setting. Reducing display refresh can
  leave more CPU time for the live PHY, but does not remove IQ drops by itself.

The main center-frequency and listening-frequency preset lists include common
Wi-Fi channels, BLE advertising channels, Bluetooth band center, and selected
cellular downlink/NR band anchors. These are tuning aids, not packet decoders;
availability also depends on the selected receiver's frequency range.

The Packets and Wi-Fi networks tabs show decoded capture files or packets from
a supported OS network adapter. The Capture button uses the selected adapter
through Npcap/libpcap, not the SDR. For SDR Wi-Fi, select a 20 MHz channel at a
supported sample rate, choose **Spectrum + Full IQ short** in the main Spectrum
recorder, and press **Spectrum Rec**. It writes `recordings/spectrum/*_full_iq_s8.iq8`
and a matching JSON sidecar with the actual sample rate and center frequency.
Then open that IQ file in **Wi-Fi IQ / OFDM**. Its format/rate/center are filled
from the sidecar; set **Wi-Fi channel center** separately if it differs from the
receiver center. Press **Decode IQ**, then **Inspect packets** to populate the
Packets/Networks tabs or open the resulting PCAP in Wireshark. Channel IQ WAV
and Spectrum + Channel IQ do not preserve a whole Wi-Fi channel. Full IQ s8
uses approximately 2 * sample_rate bytes/second, so keep captures short.

TShark/Wireshark are optional external programs. Windows uses the bundled
`wireshark/tshark.exe` or an existing user-selected/system installation.
The official Wireshark distribution was installed on the development machine;
Npcap was installed separately with the user's interactive approval.
Debian/Raspberry: sudo apt-get install tshark wireshark.
The application does not request elevation or reconfigure capture permissions.
Npcap and monitor-mode support depend on their own installation/licensing and
the adapter; neither is silently redistributed.
Npcap Free Edition must not be redistributed with Obrii. Obtain it from
https://npcap.com/ and use its interactive installer on machines needing live
capture. Offline file analysis does not require Npcap.

Wireshark profiles are selected by name. For analysis of one's own protected
captures, configure known keys in that Wireshark profile; Obrii does not copy or
log those keys. Decryption also requires the appropriate captured handshake or
session material. The Protected flag alone does not prove decryption succeeded.

Live capture optionally writes PCAPNG. It stops after five minutes or 100 MiB.
TShark cannot combine a live saved capture with a display filter, so saved live
captures contain all captured traffic; the current display filter is not applied
in that mode. Reopen the saved file to filter it. Without saving, the display
filter applies to live analysis. Choose a user-data directory outside the
project/release folder: captures can contain addresses, SSIDs and private data.
Nothing is uploaded, packet contents are not written to application diagnostics,
and capture does not resume automatically at application startup.

## Performance And Boundaries

TShark runs outside the application; transport/parsing has its own Qt worker.
The handoff keeps at most 2,000 packets; the table keeps the latest 5,000 rows.
UI handoff losses are reported separately and are not RF or driver losses.
RSSI and FCS are blank when absent. Rate statistics describe decoded packets
and exclude filtered traffic and UI handoff losses.

No packet analysis runs when the tool is closed. SDR channel analysis uses the
already prepared spectrum, at most five refreshes per second, and a bounded
number of bins per channel. Scanned/mosaic axes are not treated as contiguous RF.
Live IQ requires a sustained device rate of at least 20 MS/s and a complete
20 MHz channel within the captured band. At 20 MS/s, CS8 handoff is about
40 MB/s plus copies and PHY work; weak hardware may show drop counters.

## SDR IQ To Wi-Fi Packets

The Wi-Fi IQ / OFDM tab processes a recording or live receiver IQ in a separate
process. It does not change the receiver, steal the audio queue, start
TX or pretend that TShark understands raw IQ. Select a CF32/CS16/CS8 little-endian
I+jQ file or standard stereo PCM16 IQ WAV. RF64/float WAV is not supported here.
Specify the actual recording center, Fs and the desired channel center.
The complete nominal 20 MHz channel must fit the capture. Wider files are
explicitly shifted and resampled to 20 MS/s; missing bandwidth cannot be recovered.

The PHY mode selector keeps the tested legacy a/g runtime separate from an
experimental HT20 runtime. The latter uses the pinned
`josephnef/gr-ieee802-11` fork at
`b3dbd2d476407db92f77056df23c166e4274c507`. It has decoded CRC-valid
synthetic HT20 SISO data frames in software; real-air operation and higher MCS
rates still require testing. Windows keeps it in `wifi-ht/`, independent of
`wifi-phy/`; use `tools/prepare_wifi_ht_windows.ps1 -CreateEnvironment` and
`tools/package_wifi_ht_windows.ps1` to rebuild the optional package. Linux/ARM
does not bundle this experimental fork yet and defaults to legacy.

gr-ieee802-11 ad0598e4a874f4b8e1f391a1e0323e80df2b34ff performs synchronization,
FFT/equalization, SIGNAL/FEC, descrambling and CRC checks. Valid packets become
radiotap PCAP and can be opened in the same packet table or Wireshark. All eight
legacy OFDM rates (6..54 Mbit/s) passed a noisy software-loopback fixture.
The PHY status shows short- and long-sync candidates, decoded legacy SIGNAL
headers, CRC-valid frames, management/control/data counts, detected rates and
mean equalizer SNR. The live packet table also accepts short RTS/CTS/ACK
control headers without inventing a sequence number or BSSID. These are
cumulative diagnostics, also saved under `phy` in the PCAP JSON sidecar.
For live runs the sidecar records the decoder's effective input rate and
transport counters (`offered_samples`, `decoded_samples_reported`, and
`queue_dropped_samples`). A PHY rate below the receiver's sample rate or
nonzero queue drops means the live decoder did not keep pace; it does not by
itself identify the reason for a failed HT frame.
Candidates minus valid frames is not an exact CRC-error count: headers may be
invalid, frames incomplete or unsupported by the legacy decoder. If SIGNAL
and valid frames are present but only control traffic appears, compare against
a monitor-mode capture of a known 20 MHz 802.11g test network before assuming
that newer PHY modes are the sole cause.
CS16/CS8/WAV, 50 -> 20 MS/s conversion with a 12 MHz channel offset, and
no-signal rejection also passed deterministic regression tests.
802.11b DSSS, 802.11ax/be, 10 MHz 802.11p and a real-time
MAC/association/ACK engine are not implemented by this adapter. HT20 is
experimental, and neither HT40 nor VHT/ac is validated in Obrii. Live streaming
is receive-only packet observation, not a Wi-Fi interface or link endpoint.

The upstream file graph does not expose original RF sample counters. PCAP times
are explicitly labeled decoder-wall-clock time, not RF event time or accurate
airtime; no calibrated RSSI is fabricated. A JSON sidecar records this provenance.
Pure-zero padding can expose upstream moving-sum roundoff and cause lost frame
tails; noise-only/pure-zero files must not produce valid packets. Real RF tests
remain required. Results stay in a fresh user-data folder, never the runtime.
The cap is 100 MiB / 500,000 packets; cancellation can leave an explicit partial
PCAP. No capture contents or keys are sent to application diagnostics or online.

Windows: tools/prepare_wifi_phy_windows.ps1 records package URLs/hashes and
preserves original notices. Native Linux/Raspberry needs GNU Radio and the two
OOT modules; tools/prepare_wifi_phy_linux.sh builds pinned source with two jobs.
It installs those modules in a private prefix and creates the
`build/linux-release/wireless/python-phy` launcher, selected automatically when
present. The helper checks imports without opening RF hardware. Rebuild the
private prefix if the system Python/GNU Radio ABI changes.
Native ARM execution is not verified on this development host.

## USRP B210 Foundation

`usrpB210PlanningCapabilities()` is an explicitly non-operational hardware
template, not a detected receiver and not native USRP RX/TX support. It describes
two RX and two TX channels, full duplex, independent RX/TX LOs with a shared LO
within each direction, external 10 MHz/PPS, and hardware-timed operations.
Nominal limits are 70 MHz..6 GHz, 56 MHz analog bandwidth, up to 61.44 MS/s in
single-channel mode and 30.72 MS/s in dual-channel mode. These are not USB/CPU
throughput guarantees. A clone must be probed with UHD; actual/coerced parameters,
firmware/FPGA, antennas and measured sustained rates must replace this template.

Descriptors now distinguish per-channel sample counters from device-clock ticks,
serial identity, epoch and lock state. Timed requests require the correct clock,
epoch and a nonexpired lead time. A stopped RX FIFO does not invalidate a verified
running device clock for TX. Clock/MCR changes must increment the epoch and cancel
queued transmissions. The nominal profile cannot authorize transmission, and IQ
TX capability alone does not imply a Wi-Fi/Bluetooth/LTE packet transmitter.

Future UHD service ownership must be shared by RX/TX, not two processes opening
the same device. PHY/MAC engines exchange bounded IQ/PDU queues with that service;
overflow, underflow, late commands and RF sample timestamps must be explicit.
No GUI timer can meet SIFS, Bluetooth hopping or cellular slot timing. Bluetooth,
GSM/UMTS/LTE currently remain packet-inspection tools, not RF modems or networks.
See the primary hardware/timing documentation:
https://files.ettus.com/manual/page_usrp_b200.html
https://files.ettus.com/manual/page_timedcmds.html

## Backend Foundation

src/wireless/wirelesspackettools.h defines:

- A backend capability interface with separate packet-RX, IQ-RX, packet-TX,
  full-duplex and hardware-timed-TX flags.
- IQ descriptors: actual sample rate, usable bandwidth, center frequency,
  sample counter, epoch/discontinuity state and clock identity.
- Explicit packet TX requests and checks for unsupported RX/TX, insufficient
  bandwidth, stale epochs and mismatched hardware timing.

The TShark backend is packet receive-only. No RF transmitter is connected here.
Future PHY/MAC backends must implement bounded queues and device ownership,
preserve timestamps and expose measured latency/underruns. A separate RX and TX
can coexist; one half-duplex HackRF cannot be advertised as simultaneous RX/TX.
MAC acknowledgements cannot be scheduled by a GUI timer.
Full-duplex, clock synchronization and hardware timing need separate validation:
enough sample rate does not imply a viable real-time link.

## Recommended Implementation Order

| Family | Practical first SDR step | Later link backend | Constraints |
| --- | --- | --- | --- |
| Wi-Fi | External gr-ieee802-11 a/g/p engine, 20 MHz, packet/PCAP output | Validated PHY/MAC and timed hardware | Not an n/ac/ax/be engine; full bandwidth and bounded latency |
| BLE | Advertising GFSK, CRC, access address, PCAP | Dedicated BLE PHY/link engine | Connected traffic needs channel following, state and timing |
| Bluetooth Classic | Ubertooth/libbtbb captures initially | Frequency-hopping backend | Hopping/link state need wideband reception or following |
| GSM/2G | gr-gsm synchronization, broadcast/system information, GSMTAP | Osmocom/OpenBTS-class lab stack | Approximately 200 kHz channel; slot timing and network roles |
| UMTS/3G | Capture inspection, spectrum and cell-search research | Dedicated WCDMA stack | Approximately 5 MHz, spreading codes and substantially more L1 work; no drop-in decoder claimed |
| LTE | Dedicated cell-search/MIB/SIB engine; Wireshark packet inspection | srsRAN 4G lab UE/eNB | 1.4-20 MHz, duplex mode, scheduling and hardware clocks; 20 MHz LTE commonly uses 30.72 Msps |

HackRF's maximum 20 Msps and half-duplex operation are important restrictions.
It is a marginal whole-channel Wi-Fi analysis platform, not a general real-time
Wi-Fi/LTE modem. A wider Fobos receive stream can help observation, but Fobos
cannot transmit. USRP/bladeRF/other native backends need hardware validation;
packet-engine support does not revive our currently unqualified bladeRF path.

## DMR Changes

The old four length-R moving averages followed by length-R averaging/decimation
become one five-stage fixed-point integrator/comb filter. Its transfer function,
decimation rate and downstream discriminator/filter settings are preserved.
Unsigned modular arithmetic avoids long-run integrator drift. The SIMD path
uses SSE2 on x64 and NEON on ARM64. For decimation factors above 128, the original
floating-point box-filter path is retained to preserve weak-signal resolution
rather than quantizing weak IQ to zero.
Tests compare random input against the old mathematical filter and check DC,
non-power-of-two R and arbitrary chunk boundaries.

DMR baseband is batched into 20 ms PCM frames. DSD-neo transport/process/PCM output
runs on its own thread with a 12-frame bounded input queue. The GUI is no longer
between baseband and UDP or between DSD PCM and audio enqueue.
Internal vocoders are skipped for external voice backends; DSD-neo also skips
the internal AMBE candidate/FEC search while sync/EMB/LC metadata remains active.
Gopher retains its dibit/payload route. Working ARC4/AES arguments are unchanged.
Known IQ gaps reset DMR filter history rather than joining noncontiguous samples.
Health logs measure real PCM duration instead of reporting a fixed ratio of one.

## Primary References

- [TShark](https://www.wireshark.org/docs/man-pages/tshark.html)
- [Monitor capture](https://wiki.wireshark.org/capturesetup/wlan)
- [Wireshark decryption](https://www.wireshark.org/docs/wsug_html/)
- [gr-ieee802-11](https://github.com/bastibl/gr-ieee802-11)
- [Ubertooth](https://github.com/greatscottgadgets/ubertooth)
- [libbtbb](https://github.com/greatscottgadgets/libbtbb)
- [BLE SDR](https://github.com/JiaoXianjun/BTLE)
- [gr-gsm](https://github.com/osmocom/gr-gsm)
- [GSMTAP](https://wiki.wireshark.org/gsmtap)
- [srsRAN UE](https://docs.srsran.com/projects/4g/en/latest/usermanuals/source/srsue/source/1_ue_intro.html)
- [HackRF](https://hackrf.readthedocs.io/en/stable/hackrf_one.html)
