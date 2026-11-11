# Obrii SDR 9.0.2 Preview

- In **Network**, set **Max. frames/s** beside **Spectrum level step**. With
  local display disabled on a GUI server, Obrii computes one FFT per network
  update. The selectable limit is 1–60 frames/s, with 20 as the default.
- The local display and the network stream have separate level steps.
  A compatible client can receive one byte per bin when the quantized range
  fits. Older clients continue to receive a compatible spectrum format.
- Large spectrum frames are paced to leave room for audio on the same TCP
  connection. Delayed frames cropped to a previous viewport no longer
  roll back client zoom.
- Raspberry Pi uses the Fusion launcher with a 110% scale. The waterfall
  uses the correct physical viewport, and XWayland handles Qt 5 popup clicks
  where available.
- Multi-SDR can record aligned coherent IQ to segmented files and replay it
  into the spectrum, waterfall and DSP Flow.
- Analyzer Instrument Lab can run and export a simulated filter sweep.

In a Raspberry Pi loopback test with a physical B210 at 50 MS/s, FFT 131072,
and a zoomed 1 dB spectrum, limits of 20, 30 and 40 frames/s produced 20.0,
29.4 and 40.0 frames/s. UHD reported no receive overflows during the
44-second run, and no PCM sequence gaps appeared in the measured intervals.
Full-width frames and different network paths can produce lower rates.
