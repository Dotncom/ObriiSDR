# Wireless Laboratory (Beta)

## Available Now

Open **Spectrum measurements > Wireless lab (beta)**, or add the **Wireless lab**
block to the DSP board. This is a separate, resizable window, not a new decoder
inside the receiver callback.

- Open PCAP/PCAPNG captures through an installed TShark.
- List capture interfaces and explicitly start/stop live capture.
- Request Wi-Fi monitor mode when the adapter, driver and OS support it.
- Inspect timestamp, protocol, addresses, SSID/BSSID, frequency, RSSI, subtype,
  retry/protected flags, FCS status, sequence number and protocol summary.
- Aggregate Wi-Fi networks and RSSI; export visible packet history to UTF-8 CSV;
  open the original capture in Wireshark.
- Read Bluetooth/BLE, GSM/GSMTAP and LTE captures with an editable display filter.
  These selectors do not create RF decoders for those standards.
- Observe the current SDR spectrum's mean FFT-bin power and partial coverage
  of nominal 20 MHz Wi-Fi channels. This is not calibrated RSSI, occupied
  channel power, packet detection or an OFDM decoder.

TShark/Wireshark are optional external programs. Windows: install the official
Wireshark distribution and capture driver, then select tshark.exe.
Debian/Raspberry: sudo apt-get install tshark wireshark.
The application does not request elevation or reconfigure capture permissions.
Npcap and monitor-mode support depend on their own installation/licensing and
the adapter; neither is silently redistributed.

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
