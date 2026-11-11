# USRP-compatible analyzer plan

The receiver must be identified by runtime capabilities rather than by a model
name. This is important for B210-compatible clones whose FPGA, USB controller,
clocking or RF front end may differ from an Ettus B210.

## Implemented foundation

- SoapySDR enumeration and RX use are shared by Obrii SDR and Obrii Analyzer.
- The analyzer queries RX/TX channel count, duplex support, frequency ranges,
  sample rates and RF bandwidths and exposes RX channel selection.
- The standalone wide-sweep engine plans overlapped captures, discards RF/FFT
  edges and combines overlap in linear power.
- Obrii Analyzer now has a two-session Multi-SDR spectrum mosaic with
  timestamp-age rejection, edge trimming, level correction and synchronized
  group retuning. It is deliberately separate from coherent IQ joining.
- Existing Fobos, HackRF and RTL-SDR paths keep their native implementations;
  a missing Soapy/UHD module therefore cannot regress those receivers.
- Obrii SDR now has an optional native UHD C-API RX path: channel selection,
  RX gain/antenna, rate/frequency configuration, CF32 streaming and live
  retuning. ObriiCli can receive both B210 inputs in one UHD session. Its
  separate bench mode supports short shared-session RX/TX bursts. General
  shared device ownership across GUI tools remains future work.

## Hardware validation stage

### Dual RX in Obrii CLI

With a two-channel B210 image, start one timed RX stream with `--rx-dual`.
Both inputs use the same center and sample rate, up to 30.72 MS/s per channel.
`--rx-channel` selects the primary input. `--rx-second-gain-db` and
`--rx-second-antenna` set the other input independently.

```sh
ObriiCli run --backend uhd --device 0 --rx-dual --rx-channel 0 --freq 101.287174M --rate 15.36M \
  --gain-db 35 --antenna RX2 --rx-second-gain-db 30 --rx-second-antenna RX2 --spectrum --serve
```

Add `--fpga-image` with a compatible two-channel image for the clone.
On the client, the second channel appears as a separate spectrum and waterfall
in Multi-SDR. Network audio and the existing IQ stream carry the primary
channel. With `--output FILE.cf32`, the CLI writes the other channel to
`FILE.cf32.rxN.cf32` using the same receive-block boundaries.

On ARM Linux, Obrii defaults to 256 in-flight USB receive buffers for UHD
B2xx. `OBRII_UHD_NUM_RECV_FRAMES` can override the default, and an explicit
UHD device argument takes precedence. GUI diagnostic logging reports UHD
overflows, timeouts and the longest pause between reads at Stop. In dual CLI
mode, `uhdRxDiagnostics` progress records also report timestamp gaps.

The vendor GitHub repository supplies a Kintex-7 FPGA binary and a Drive link
to a firmware project, not an in-repository, reproducible HDL build. Keep the
vendor image separate from stock UHD images and retain a known-good copy.
The USRP UHD RX dialog accepts an absolute `.bin` path and passes it as the
UHD `fpga=` device argument on open. Use an empty field for the standard UHD
image. Stop and start local RX after changing this setting. With a custom
image path, the GUI allows up to 75 seconds for UHD to open. With the default
image, it allows 12 seconds. A cold load can take over 20 seconds.
Obrii loads `uhd.dll` from its runtime directory or `wifi-phy/Library/bin`
on Windows and `libuhd.so` from the system loader on Linux. A custom path can
be supplied through `OBRII_UHD_LIBRARY_PATH`. The native code itself is built
into Obrii, but a normal release without these runtime files still needs the
UHD library and compatible images; the current standard Windows package does
not yet bundle a separate lean UHD runtime.
Distribution gate: the repository currently carries a GPLv2 LICENSE text,
whereas the UHD C API sources identify GPL-3.0-or-later. Before publishing a
binary that loads libuhd, confirm Obrii's own and all incorporated code can be
distributed under compatible terms, or keep UHD in a separate process with a
documented protocol. Do not silently add UHD DLLs to a public release.
Before any custom FPGA build, inspect the linked project for HDL, pin/timing
constraints, USB/FX3 protocol compatibility, toolchain version and license.
Do not auto-flash or silently replace UHD images from Obrii SDR.

### Experimental Kintex-7 FPGA images

Use these images only for the compatible B210 Kintex-7 clone. Keep the vendor
image available for recovery. Selecting an image in **USRP UHD RX** loads it
for that session. To return to the vendor image, select its path again. For
a stock board, clear the custom path to use the standard UHD image.
If the package includes the images, find them under `fpga/b210/`. You can
also select your own compatible `.bin` by absolute path.

| Image | Profile | Bench coverage |
| --- | --- | --- |
| `b210-siso-strobe.bin` | One RX channel | 30 seconds at 61.44 MS/s with no reported overflow, timeout or timestamp discontinuity. GGA/RMC arrived with no valid fix. |
| `b210-dual-fabric.bin` | Two RX channels | Timed two-channel RX at 15.36 MS/s per channel. The Raspberry GUI also received GGA/RMC/GSV with this image during RX at 10 MS/s. |

Begin on the tested clone with the internal clock, channel 0 and 10 MS/s
in the Raspberry GUI. Use a compatible UHD library. Qualification depends
on the board, host, clock, sample rate and image. Treat these as bench
results for the tested configuration.

On first hardware access, run `uhd_find_devices` and `uhd_usrp_probe` with the
vendor-recommended UHD/image combination. Then test one RX channel at a low
rate, retuning and Stop/Start, before dual-channel or high-rate work. Verify
the selected device index against the probed serial when several USRPs are
present.

1. Extend qualification of native UHD RX to additional board, image and
   driver combinations, with explicit compatibility reporting.
2. Verify the remaining sample rates and both RF channels. Record actual USB
   throughput, overflows, timestamps, oscillator source and sensor values.
3. Add timestamped two-channel capture and phase-alignment tests. Do not label
   the clone coherent until repeated measurements demonstrate it.
4. Add TX interlocks, explicit antenna/channel selection, gain limits and a
   user-visible underrun counter before enabling analyzer stimulus output.
5. Make one native UHD device service own every RX/TX streamer for a physical
   USRP. B210 full-duplex and two-channel operation must not be implemented as
   competing per-channel device opens; one service distributes bounded queues
   and hardware timestamps to the selected DSP branches.

## UHD clock source

Open **USRP UHD RX** on the local receiver or on a client connected to
the Raspberry Pi CLI server. **Auto** preserves the driver's default.
**Internal** and **External 10 MHz** explicitly select the reference.
For local RX, use Stop and Start to apply a changed source. The server
reopens active RX when you click **Apply** and tries to restore the
previous settings if the new source is rejected.

The active reference is read back from UHD. With an external reference,
the dialog shows whether the `ref_locked` sensor reports lock. A missing
sensor is displayed separately from an unlocked reference. The CLI accepts
`--clock-source auto|internal|external` and reports `uhdClock` in
`control status`. This selection controls the frequency reference.

## Onboard GPS coordinates

In **GPS / QTH**, select **UHD GPS** as the position source and start RX.
The QTH status shows whether NMEA data is arriving. With a UHD library that
exposes GSV, it also shows satellites in view, satellites with measured signal,
and the strongest C/N₀. The GGA satellite count shows how many are used for the
fix. Once the module has a valid fix, the status shows coordinates and a
Maidenhead locator, and the QTH map displays the position. The displayed fix
is temporary.
Routine status reports and receiver logs omit the coordinates.

For the Kintex-7 board, use a GNSS-enabled FPGA image and a compatible UHD
library. Set the FPGA image and **GPS NMEA baud** to **38400** in
**USRP UHD RX** before starting local RX. Use **Auto (UHD sensors)** with a
standard UHD GPSDO. When installing a compatible UHD library separately,
set `OBRII_UHD_LIBRARY_PATH` to its absolute path before launching Obrii.

On the tested Raspberry setup, 240 polls returned GGA, RMC and GSV over
four minutes. GSV listed 25 satellites, with no measured C/N₀, and GGA
reported zero satellites used. The position fix remains unverified. GSV's
in-view count can be positive before any satellite signal is measured.

The older Raspberry Pi 9.0 compatibility build takes the baud from its
launch environment. With the B210 free, launch it using the compatible UHD
library, then select the FPGA image in **USRP UHD RX**.

```sh
OBRII_UHD_GNSS_BAUD=38400 \
  OBRII_UHD_LIBRARY_PATH=/absolute/path/to/compatible/libuhd.so \
  ./tools/run_linux.sh
```

Select **UHD GPS** as the position source in **GPS / QTH**. The inline
status updates once RX is running. NMEA data can arrive before the module
has a fix. A valid fix supplies the temporary position on the map.

For network use, start the UHD server with `--uhd-gnss-baud 38400` and
the corresponding `--fpga-image` and UHD library. Use `--uhd-gnss`
for a device with standard UHD GPSDO sensors. On a trusted network the
client with receiver control can select the same **UHD GPS** source.

## Remote UHD RX and TX test

On the Raspberry Pi, enable duplex test bursts with
`--duplex-tx-channel 0` or `--duplex-tx-channel 1`. Set the RX rate
to `15360000` or `30720000` per channel. RX and TX use one UHD
session and a fixed 30.72 MHz master clock.

- For **2 RX + 2 TX**, include `--rx-dual` and set both RX antennas
  to `RX2`. The TX option selects the default output in the client.
  Connect **both** TX/RX ports to 50-ohm loads or suitably attenuated test
  paths. UHD keeps both TX lanes enabled during a burst, including when
  Obrii submits zero IQ on one lane.
- For **1 RX + 1 TX**, use a matching pair such as
  `--rx-channel 0 --antenna RX2 --duplex-tx-channel 0`. Connect the
  corresponding TX/RX port to a 50-ohm load or attenuated test path.

From the Raspberry Pi, issue an arm code over the server's loopback
control port:

```sh
./ObriiCli control uhd-tx-arm --control-port 17675
```

Open **TX…** from the controlling Obrii network client. Select TX 0, TX 1,
or both outputs when available. Enter the common TX center, gain (0–10 dB),
duration (50–500 ms), and single-use code. The code expires in 60 seconds.
The test sends a ramped tone 1 kHz above center on a single selected port.
With both outputs selected, TX 0 is 1 kHz above center and TX 1 is 2 kHz
above center. Each TX lane runs at 1.92 MS/s. **Stop TX** cancels the
burst. Use this test mode over a trusted network.

RX keeps running during the burst. The `uhdTx` event reports the selected
output, samples per channel submitted to UHD, and the RX overflow and
missing-sample counters for the burst window. RF output, TX underruns, and
RX continuity need hardware verification.

## Instrument stages

1. Frequency sweep with settle time, dwell/averaging, bidirectional passes and
   per-segment calibration.
2. Tracking-generator and scalar network analysis using separate TX/RX paths,
   a through reference and magnitude normalization.
3. Return-loss measurements only with a directional coupler or bridge and
   open/short/load calibration metadata.
4. Vector network analysis only after coherent reference routing, timestamp and
   phase stability are verified. The UI must distinguish raw phase from a
   calibrated complex S-parameter.

All large IQ queues and optional measurement workers stay outside the basic
receiver/audio path, so Raspberry Pi operation remains unaffected while these
features are disabled.

## UMTS RX qualification with the B210 clone

1. Identify the USB device with `uhd_find_devices`. Run `uhd_usrp_probe`
   with the vendor-recommended UHD and FPGA image. Record the actual
   serial, reported channels, clock source and antenna names. Keep the
   vendor image and local Vivado project together for comparison. This
   checkout has an XC7K325T project at
   `B210_Project_Firmwire/B210_Project_Firmwire.xpr` and an existing
   `B210_Project_Firmwire/B210_Project_Firmwire.runs/impl_1/b200.bin`.
   Confirm the image against the delivered clone before using it.
2. Select one RX channel and a known W-CDMA downlink carrier. Record a
   short capture with the native UHD backend. Use the rate and center
   frequency from the recording metadata when replaying it. Start with
   a low RX gain and inspect clipping, overflows and actual sample rate.

```powershell
# Set these to the verified carrier and clone image before running.
ObriiCli --backend uhd --rx-channel 0 --rate 7680000 --freq $CarrierHz --gain-db 10 --seconds 1 --fpga-image $VendorFpgaImage --output b210-umts.cf32
ObriiCli usrp umts-decode b210-umts.cf32 7680000
```

3. Record the synchronization outcome separately from BCH CRC and MIB.
   Repeat on disjoint portions of a capture and after Stop/Start. The
   console and DSP-board job can replay the same file for comparison.
   [USRP file replay](usrp_software_lab.md)
4. For bidirectional qualification, use a conducted, attenuated setup
   or a shielded test enclosure with an independent UMTS peer and lab
   subscriber context. Qualify timed RX/TX and channel ownership before
   measuring RRC/NAS or IP interoperability.
