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
  retuning. Explicit UHD TX uses a separate session. Clone validation and
  shared RX/TX ownership are pending.

## Hardware validation stage

The vendor GitHub repository supplies a Kintex-7 FPGA binary and a Drive link
to a firmware project, not an in-repository, reproducible HDL build. Keep the
vendor image separate from stock UHD images and retain a known-good copy.
The USRP UHD RX dialog accepts an absolute `.bin` path and passes it as the
UHD `fpga=` device argument on open. Empty means the standard UHD image.
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

On first hardware access, run `uhd_find_devices` and `uhd_usrp_probe` with the
vendor-recommended UHD/image combination. Then test one RX channel at a low
rate, retuning and Stop/Start, before dual-channel or high-rate work. Verify
the selected device index against the probed serial when several USRPs are
present.

1. Validate the native UHD RX transport with the clone's vendor image, then
   add firmware/FPGA compatibility reporting and bounded RX/TX queues.
2. Verify every reported sample rate and both RF channels. Record actual USB
   throughput, overflows, timestamps, oscillator source and sensor values.
3. Add timestamped two-channel capture and phase-alignment tests. Do not label
   the clone coherent until repeated measurements demonstrate it.
4. Add TX interlocks, explicit antenna/channel selection, gain limits and a
   user-visible underrun counter before enabling analyzer stimulus output.
5. Make one native UHD device service own every RX/TX streamer for a physical
   USRP. B210 full-duplex and two-channel operation must not be implemented as
   competing per-channel device opens; one service distributes bounded queues
   and hardware timestamps to the selected DSP branches.

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
