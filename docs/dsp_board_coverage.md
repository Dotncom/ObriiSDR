# DSP Board Coverage Audit

Audit scope: user-visible desktop functions in the current development tree.

## Direct Or Self-Contained Blocks

- Receiver/IQ/network/playback sources, start/stop, enable, center/listening
  frequency, fine tuning, channel filter, resampler, FFT, demodulator, audio
  output, recorder, replay, network output, transmitter, and GPIO.
- The receiver editor mirrors receiver/input/clock/sample-rate/FFT/gain controls
  and provides direct Fobos details and HackRF RX-settings actions.
- Frequency and demodulator/channel-filter editors share the main controls'
  current limits, step lists and value presets, including edits made while the
  editors are open. Changing a preset uses the same receiver/bandwidth action;
  passive synchronization never retunes the receiver.
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
- The USRP software lab launches bounded Wi-Fi PHY and LTE ZMQ loopbacks,
  GSM BCCH/PDCH file decoding and GPRS MAC replay. The same job runner
  serves the local console and CLI. [USRP software lab](usrp_software_lab.md)
- Open radio launches the isolated M17 and AX.25/KISS framing laboratory and
  virtual-signal envelope builder. It remains a launcher until each protocol
  has a bounded, tested IQ modem block; no placeholder is wired into live DSP.

## Board Editing History

Undo/Redo toolbar buttons and Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z operate on canvas
edits: blocks, links, geometry, sectors and board-local settings. Text editors
keep their own Undo. Group deletion/movement is one operation. History is kept
only in memory, up to 100 steps and 16 MiB; loading a layout resets it.
Restoring deleted main-chain filter blocks restores their saved parameters and
position without replacing surviving same-ID stages or unrelated live filters.
RF Start/Stop, TX, network permissions and main receiver settings are not undone.

## Laboratory I/Q on the board

Use **Add → I/Q generation and transmission**. Wire **Carrier → Modulator → I/Q stream**.
The upper modulator input takes carrier offset and amplitude. The lower input takes a
payload. Set the sample rate, duration and RF center on the I/Q stream block.
The carrier's absolute frequency is the RF center plus its offset.

- CW, tone and noise use the carrier input. BPSK, QPSK, 16-QAM, 2-FSK and
  2-GFSK accept UTF-8 text, image bytes or arbitrary file bytes. With no data
  source they generate a repeatable test sequence. The payload must fit the
  selected duration and symbol rate. These modes send raw symbols without a
  framing or file-recovery protocol.
- AM and FM take a mono or stereo PCM16 WAV file and use audio up to the chosen
  duration. The I/Q rate must accommodate the input audio bandwidth, carrier
  offset and FM deviation. A normalized CF32LE file can instead connect directly
  to the I/Q stream block. Set its sample rate and RF center explicitly.
- Connect **I/Q stream → DDC channelizer → Branch FFT → Branch spectrum** and
  enable the DDC branch with simplified mode turned off. **Run I/Q** loops
  one buffer locally, including with the hardware receiver stopped. The DDC can also feed the existing
  demodulator and audio output. Use an enabled receiver source in place of
  the I/Q block to process live reception through those same blocks.
- Connect **I/Q stream → TX output** for a single burst. **Prepare transmission**
  asks for confirmation and uses the configured transmitter and its existing RF
  arming checks. Local I/Q playback and TX are separate actions. The board
  supports one local loop at a time, with up to two million complex samples per buffer.

### Local packet loopback

Choose **Packet QPSK** on the modulator and wire a **Text**, **Image bytes**, or
**File bytes** source to its data input. Connect **I/Q stream → QPSK packet
decoder → Received data** and click **Run I/Q**. The decoder uses the generated
burst's sample clock and carrier offset to recover data from its I/Q samples.
It checks the packet type, length and integrity tag before delivering data.
The output displays text or a supported image preview and saves the original
received bytes using **Save received data**. The latest result can be saved
after stopping the loop.

One packet must fit the current I/Q buffer. At 192,000 samples/s, 64,000
symbols/s and 10 seconds, a packet carries about 160 kB before framing.
### QPSK reception from a receiver

Connect **Receiver → DDC channelizer → QPSK packet decoder → Received data**.
Enable the local DDC and start the receiver while the board is visible. Tune the
DDC to the nominal RF carrier and set **RX symbols/s** to the transmitter's
effective symbol rate. The transmitter rounds to a whole number of samples per
symbol, so its effective rate is the I/Q sample rate divided by that number.
For 1,200 symbols/s, a DDC rate of 12 kS/s and bandwidth of 4.5 kHz leave room
for the signal and carrier error.

The live decoder finds the 64-symbol preamble, estimates carrier offset and
symbol phase, and tracks carrier and clock drift while receiving the packet.
It accepts 100–96,000 symbols/s and DDC rates from 2.5 samples/symbol up to
384 kS/s. The acquisition range is approximately ±40% of the symbol rate
when the burst fits in the DDC passband. Retunes and sample gaps reset lock.
Up to four live decoders can run, with one per local DDC. New Packet QPSK
transmissions include the preamble. Older bursts can still be decoded with
the local buffer's known boundaries.

VFO plots select a band from the existing shared main FFT; the selected VFO uses
the main audio path. DDC branches instead produce their own filtered, decimated
IQ, suitable for separate FFT, audio, research, recording and external processing.
Thus a VFO plot is inexpensive, but is not an independent IQ channelizer.

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
It does not load arbitrary GNU binaries in-process. At most eight DDC branches and
two FFTs per branch execute, with no main audio-queue consumption. Simplified
operating mode disables these branches. Independent AM/NFM/SAM/USB/LSB/DSB/CW/WFM
demodulator, audio-filter-chain and audio-output blocks have executable typed
links. One audio output per DDC is supported; output is 48 kHz mono and defaults
off. Explicitly enabled audio continues with a hidden board. A bounded audio
worker delivers PCM independently of GUI frame updates. The main filter editor
also offers a one-shot validated wire-order apply action; ordinary legacy diagram
links remain documentation and never automatically rewire the main receiver.

DDC and the optional PFB banks run once per subscribed IQ source on the local
branch workers. Branches on one source share a bank, while different receivers
retain independent bank geometry, history and discontinuity state. External
branches move their already-channelized IQ to a separate, lazily started worker;
socket waits and external-result demodulation no longer block local branch audio.
The queue retains at most two pending blocks per external branch (sixteen total,
8 MiB maximum IQ payload), plus one in-flight request. Overflow discards the
oldest pending block for that branch and marks the next retained data as a gap.
The remote worker sleeps without polling when idle. Stop, retune, source-epoch
changes and simplified mode invalidate queued/in-flight results and cancel socket
waits. Endpoint edits reset only external state. Remote branches still share one
connection, and CPU, disk and device contention can still cause real overruns;
this is isolation from network waits, not a hard real-time guarantee.

The opt-in PFB analysis bank shares RF channelization across connected DDCs.
Its 8/16/32/64-channel configuration must be common to all PFB branches. Each DDC
passband must fit one guarded bank channel; unsupported cross-boundary bands
are rejected. Direct DDC remains available. DDC FIR tap/transition controls do
not alter the normal receiver audio channelizer. See pfb_channelizer.md.
See dsp_stream_manager.md for operation and current limits.

Main FFT links are typed IQ/spectrum bindings to the single existing optimized
pipeline, not a general scheduler or independent receivers/FFTs. The compiler's
`mainFftSources` and `mainSpectrumRoutes` mappings are separate from DDC execution.
Explicit FFT-to-view links require a supported connected source; unwired legacy
views retain implicit shared frames. Display-settings links coexist with spectrum
inputs. Compute placement is a setting, not a signal-flow connection. Other
ordinary legacy links remain diagram-only.

Main FFT also routes through `spectrum_display`, `waterfall_2d` and
`waterfall_3d` settings blocks to their workspace views, by explicit settings
wire or matching binding ID. A single controller may serve several views
without duplicate FFT work. Explicit routes retain their disconnected state
instead of silently falling back to an implicit feed. Conflicting stream and
settings sources are rejected before applying the graph.

The native worker accepts up to two sessions (separate main FFT and DDC), at most
one of them main FFT. Its 64-8192 MiB main FFT budget defaults to 512 MiB and limits
advertised length to `min(67108864, budgetMiB * 1048576 / 64)` points, including a
conservative allowance for kernel/rings/frame memory. Backend and memory-cap
refusals are explicit. Stop/consent revocation requests shutdown without blocking
the UI; while Stopping, worker resource controls remain locked. Dialog destruction still joins
the noninterruptible current transform. These limits do not qualify real LAN
throughput or Raspberry-class resource use.
