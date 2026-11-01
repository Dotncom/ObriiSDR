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
  retuning. It is not yet hardware-validated on the Kintex-7 clone; TX and
  shared two-channel ownership are still pending.

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
rate, retuning and Stop/Start, before dual-channel or high-rate work. The
native adapter currently selects the first discovered USRP; multiple-device
selection and common RX/TX ownership remain future work.

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
