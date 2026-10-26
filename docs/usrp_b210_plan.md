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

## Hardware validation stage

1. Add a self-contained UHD transport beside SoapySDR, including discovery,
   firmware/FPGA compatibility reporting and bounded RX/TX queues.
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
