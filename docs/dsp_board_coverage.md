# DSP Board Coverage Audit

Audit scope: user-visible desktop functions in the current development tree.

## Direct Or Self-Contained Blocks

- Receiver/IQ/network/playback sources, start/stop, enable, center/listening
  frequency, fine tuning, channel filter, resampler, FFT, demodulator, audio
  output, recorder, replay, network output, transmitter, and GPIO.
- The receiver editor mirrors receiver/input/clock/sample-rate/FFT/gain controls
  and provides direct Fobos details and HackRF RX-settings actions.
- Full audio-filter chain plus one block per filter type.
- Full workspace spectrum, frequency ruler and waterfall blocks that consume the
  already prepared shared spectrum frame, plus settings blocks for the legacy
  main spectrum/2D/3D views, Multi-VFO channel, mini-spectrum and mini-waterfall.
- The existing FFT block and stream manager share Apply-based main FFT placement
  controls: Local, individual remote IQ windows, or full continuous IQ; CPU/GPU/Auto
  backend, upload budget and explicit local fallback. Runtime status is shared.
  Only nonsecret preferences persist in layouts; tokens and permissions do not.
- The board can become the main maximized workspace, use saved presets up to
  five columns, and split each column independently into 1-4 resizable sectors.
  Blocks and live visual widgets can be dragged across sector boundaries while
  retaining their links. The hidden standard window remains the owner of
  receiver, audio and DSP lifetime.
- Self-contained oscilloscope, constellation, eye diagram, and digital
  synchronization plots, each with its matching settings block.
- Digital audio/video settings and independent text/image result windows.

## Exact Tool Launchers

- Agile, standard, and listening scans; DMR/FPV/video hunters.
- Spectrum measurements, zoom spectrum, selected-band density, zero-span,
  spur suppression, spectrum recording, and replay.
- Interference/comb analysis, signal statistics, IQ diagnostics, dual-input HF
  analysis, scientific analyzer, density, masks/baseline, pulse analysis, and
  measurement sessions.
- DMR, CW, SSTV, digital video, GNSS over SDR, serial NMEA/UBX, QTH map,
  presets/calibration, and general settings.

## Intentional Boundary

The board mirrors controls and embeds selected displays, but it is not a
general-purpose runtime graph. A drawn ordinary signal connection does not
automatically replace Obrii's optimized IQ/audio path. Executable links today
include Receiver/IQ source -> main FFT -> workspace spectrum/waterfall bindings,
Source -> DDC -> branch FFT -> spectrum/waterfall and DDC -> research
IQ views, control links, VFO-channel-to-VFO-display links, and matching research
settings-to-research-view links. VFO views can alternatively store a direct VFO
number, and matching research pairs can share a binding ID, so long cross-sector
wires are optional while explicit local wires remain supported. Individual audio-filter blocks create real
AudioFilterChain stages, but stage order still comes from that chain's list,
not from canvas geometry. New executable routing is added only when a block has
a bounded implementation and separate tests; new stream features still need
Raspberry-class hardware qualification.

The optional stream manager routes DDC outputs locally or to a native compute
worker/GNU Radio adapter; independent CF32LE recorders are explicit DDC controls.
It does not load arbitrary GNU binaries in-process. At most two DDC branches and
two FFTs per branch execute, with no audio-queue consumption. Simplified operating
mode disables these branches. Filter ordering and independent demodulator/audio
sinks are still future work; ordinary legacy diagram links remain documentation.
See dsp_stream_manager.md for operation and current limits.

Main FFT links are typed IQ/spectrum bindings to the single existing optimized
pipeline, not a general scheduler or independent receivers/FFTs. The compiler's
`mainFftSources` and `mainSpectrumRoutes` mappings are separate from DDC execution.
Explicit FFT-to-view links require a supported connected source; unwired legacy
views retain implicit shared frames. Display-settings links coexist with spectrum
inputs. Compute placement is a setting, not a signal-flow connection. Other
ordinary legacy links remain diagram-only.

The native worker accepts up to two sessions (separate main FFT and DDC), at most
one of them main FFT. Its 64-8192 MiB main FFT budget defaults to 512 MiB and limits
advertised length to `min(67108864, budgetMiB * 1048576 / 64)` points, including a
conservative allowance for kernel/rings/frame memory. Backend and memory-cap
refusals are explicit. Stop/consent revocation requests shutdown without blocking
the UI; while Stopping, worker resource controls remain locked. Dialog destruction still joins
the noninterruptible current transform. These limits do not qualify real LAN
throughput or Raspberry-class resource use.
