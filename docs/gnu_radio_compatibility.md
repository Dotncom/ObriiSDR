# GNU Radio Compatibility

## Implemented Development Path

Obrii uses an optional external adapter, not GNU libraries in its main executable.
The DSP board can route a DDC channel through `tools/obrii_gr_bridge.py` and receive
processed IQ for research widgets and calibrated FFT spectra. The native LAN
compute worker speaks the same protocol. See [stream manager](dsp_stream_manager.md),
[protocol](dsp_stream_protocol.md) and [adapter usage](gnu_radio_bridge_usage.md).

GNU blocks are selected in a trusted local Python plugin exposing
`make_block(sample_rate)`. One-in/one-out CF32 blocks retain filter history and
their scheduler across contiguous packets. Generation, epoch, rate/frequency
changes and gaps reset state explicitly. Two streams, fixed buffer limits,
deadlines and killable GNU workers keep failure separate from the main RX path.
Tokens and transfer permission are session-only; TCP is unencrypted.

The optional bundled Windows GNU environment can be reused; the base program
runs without it. Software tests cover real GNU identity, FIR history/reset,
hierarchical blocks, loopback protocol, invalid outputs and stalled consumers.
The adapter does not open RF devices or expose hardware-control commands.

## Intentional Limits

- DDC/anti-alias filtering remains on the receiver PC. The GNU adapter does not
  support main wide-FFT, audio, Wi-Fi or GNSS offload or automatic load balancing.
  Main RF FFT offload is available separately through the native Obrii worker.
- Plugins must preserve rate and produce one output for each input within each
  request. Arbitrary tagged-stream/buffering/rate-changing graphs are unsupported.
- Returned IQ belongs to its branch and never overwrites the main IqBuffer.
- OOT binary compatibility still belongs to the installed GNU runtime. Obrii
  does not scan for arbitrary libraries or silently load them into its process.
- Real Raspberry-to-laptop throughput and audio continuity require hardware
  qualification. Local tests are not a zero-copy or full-rate RF claim.

## Later Phases

Independent demodulator/audio sinks, a general GRC source/sink OOT package,
rate-changing contracts, explicit hardware-clock metadata and measured local
shared-memory wideband transport remain separate tasks. The older desktop
network transport (packed full/channel IQ) is unchanged; DSP protocol v1 is a
separate authenticated CF32 research endpoint, not a compatible wire format.

Architecture references:
- [GNU Radio source and module layout](https://github.com/gnuradio/gnuradio)
- [Out-of-tree modules](https://wiki.gnuradio.org/index.php/OutOfTreeModules)
- [Stream tags](https://wiki.gnuradio.org/index.php/Stream_Tags)
- [Tagged streams](https://wiki.gnuradio.org/index.php/Tagged_Stream_Blocks)
