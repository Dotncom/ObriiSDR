# Obrii SDR 9.0.1 Preview

This preview extends the B210 and Raspberry Pi workflows from 9.0.0.

- Select **UHD GPS** in **GPS / QTH** to see NMEA availability, fix state and,
  with compatible UHD and FPGA support, the GSV satellite and signal counts.
  Coordinates appear after the module reports a valid fix.
- Use dual UHD RX on the CLI server and configure the remote channel, gain,
  and antenna in Multi-SDR. The secondary channel has its own spectrum and
  waterfall. Network IQ and audio carry the primary channel. Shared UHD RX/TX
  is available for explicitly armed test bursts with compatible hardware.
- On Raspberry Pi, the UHD receive path uses more in-flight USB buffers.
  Custom FPGA images have a longer startup deadline. The Linux build creates
  an Obrii SDR shortcut using Qt Fusion at 110% scale.
- USRP Lab exposes software loopbacks and file-based decoder jobs. See the
  [USRP Lab guide](usrp_software_lab.md) for runtime requirements.
- Network WFM uses a separate processing path and a 160 ms playback prebuffer.
  DSP Flow shows sample-rate mismatches, and Y-factor tables and CSV exports
  include a repeatability estimate.

On the test B210, UHD GPS delivered GGA, RMC and GSV throughout a four-minute
measurement. No satellite had a measured signal and no position fix was
available during that measurement. The updated Raspberry GUI showed GSV and
completed a 10 MS/s receive test with zero UHD overflows or timeouts.

See the [B210 guide](usrp_b210_setup.md) for compatible-image setup and the
scope of the single- and dual-channel tests. The normal package uses a
separately installed compatible UHD runtime and an image supplied by the
operator. Redistribution of the tested custom B210 images remains pending
verification of the generated FPGA components and their corresponding source.
The application menu includes updated English and Ukrainian help.
