# Obrii SDR 9.0.0

## Receiver and signal processing

- Fobos gains Obrii Dual support, selectable library profiles for compatible
  receivers, and clearer ADC, IF and IQ diagnostic status. Start/Stop and live
  retuning have been revised, with optional retuning while dragging or scrolling.
- Spur calibration can survey the RF range with a terminated input. A separate
  control adjusts the broad noise correction. ADC overload protection is opt-in.
- DMR reception improves stream handling and logs the algorithm and key ID
  selected from the local key store. A manually selected matching key can help
  recover supported late-entry calls.
- IQ recording and video decode can use separate processing paths. The release
  also expands Analyzer instrument planning, DSP Flow, and virtual radio tools.

## Multi-SDR

- Live coherent IQ has a built-in virtual pair and a combined spectrum and
  waterfall. An experimental signal-alignment mode for two Fobos receivers
  accepts Agile or Standard IQ at the same RF center and 8 MS/s and outputs
  125 kS/s. Use a common 10 MHz reference and a common modulated RF signal.
- The virtual coherent path has software coverage. Signal alignment with two
  physical Fobos receivers is pending. See `docs/multi_sdr_architecture.md`.

## Downloads

- The Windows x64 archive contains ObriiSDR, Obrii Analyzer, ObriiCli and its
  modules, the companion tools, and the Fobos HW 3.0.0 updater. Its catalog
  includes the stock Agile image for recovery, the patched Agile r7 image, and
  the experimental Obrii Dual image. See `firmware/fobos/README.txt` for
  supported transitions and installation steps.
- The Raspberry Pi/Linux source archive includes the application, ObriiCli
  and its modules, and the Linux build scripts. Use the official Fobos SDKs
  for the Linux build and follow the setup steps in `README.md`.
