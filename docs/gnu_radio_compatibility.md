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
changes and gaps reset state explicitly. Eight streams, fixed buffer limits,
deadlines and killable GNU workers keep failure separate from the main RX path.
Tokens and transfer permission are session-only; TCP is unencrypted.

The optional bundled Windows GNU environment can be reused; the base program
runs without it. Software tests cover real GNU identity, FIR history/reset,
hierarchical blocks, rational rates 1/2, 2/1, 3/2, 2/3, 64/33 and 1/64, irregular
packet boundaries, output counters/rates, loopback protocol, invalid outputs,
stalled consumers and cancellation. A standalone C++ protocol regression lives
in `tools/gnu_radio_protocol_selftest.cpp`; CMake integration is separate.
The adapter does not open RF devices or expose hardware-control commands.

## Bounded Rational Phase

Trusted plugins may return `(block, L, M)` or `(hierarchy, L, M, inputReserve)`.
Reduced factors are <=64, output/input <=2, processed rates 1 kHz..2 MHz. The
bridge retains bounded real input history/lookahead and returns whole rate
quanta, including empty startup replies. Input packets stay <=65536; processed
output is <=131072 CF32 samples. Resets discard pending tails. Native processing
remains identity-rate by default. Rate/counter/origin/gap metadata lets branch
rendering, recording and audio consume the processed clock explicitly.

The examples `tools/obrii_gr_resampler.py` and `tools/obrii_gr_hier_resampler.py`
use GNU's existing rational FIR implementation. The latter is a runnable
GRC-style hierarchical adapter, with instructions for exported GRC hierarchies
in [adapter usage](gnu_radio_bridge_usage.md#grc-adapter-integration). It is not
an OOT source/sink installation or a general network-attached GNU scheduler.

## Intentional Limits

- DDC/anti-alias filtering remains on the receiver PC. The GNU adapter does not
  support main wide-FFT, audio, Wi-Fi or GNSS offload or automatic load balancing.
  Main RF FFT offload is available separately through the native Obrii worker.
- Legacy plugins preserve rate. Rational plugins must satisfy the explicit
  fixed-ratio/reserve contract; arbitrary tagged-stream/buffering/variable-rate
  graphs are unsupported. Message ports and stream tags are not carried.
- Returned IQ belongs to its branch and never overwrites the main IqBuffer.
- OOT binary compatibility still belongs to the installed GNU runtime. Obrii
  does not scan for arbitrary libraries or silently load them into its process.
- Real Raspberry-to-laptop throughput and audio continuity require hardware
  qualification. Local tests are not a zero-copy or full-rate RF claim.

## Later Phases

Independent GNU demodulator/audio sinks, a general GRC source/sink OOT package,
variable-rate contracts, explicit hardware-clock metadata and measured local
shared-memory wideband transport remain separate tasks. The older desktop
network transport (packed full/channel IQ) is unchanged; DSP protocol v1 is a
separate authenticated CF32 research endpoint, not a compatible wire format.

Architecture references:
- [GNU Radio source and module layout](https://github.com/gnuradio/gnuradio)
- [GNU rational resampler API](https://www.gnuradio.org/doc/doxygen/classgr_1_1filter_1_1rational__resampler.html)
- [GNU rational FIR history and forecast implementation](https://github.com/gnuradio/gnuradio/blob/main/gr-filter/lib/rational_resampler_impl.cc)
- [Out-of-tree modules](https://wiki.gnuradio.org/index.php/OutOfTreeModules)
- [Stream tags](https://wiki.gnuradio.org/index.php/Stream_Tags)
- [Tagged streams](https://wiki.gnuradio.org/index.php/Tagged_Stream_Blocks)
