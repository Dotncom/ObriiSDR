# Multi-SDR and compute topology

## Implemented stage

- The main Obrii SDR receiver remains the authoritative RX 1 for the established
  recorder and global IQ history. Audio and its existing demodulators can use a
  selected auxiliary RF stream without replacing those process-global stores.
- Multi-SDR can add RX 2 through RX 8. Every auxiliary receiver owns its native
  device handle, bounded IQ ring, stream thread, FFT plan and FFT worker.
- In the main application the RX 1 button starts and stops the established
  Obrii receiver path. Its hardware controls remain in the Receiver and
  Frequencies sections instead of being duplicated inconsistently.
- Optional span auto-alignment keeps every enabled receiver adjacent to the RX
  whose center was edited. It accounts for sample rate, per-edge discard and a
  user-selectable overlap. Turning it off restores independent center tuning.
- `Include in main window` is independent from receiver execution. Included
  sources form the main spectrum, waterfall and absolute frequency scale;
  unchecked sources remain available in the configuration preview only.
- Combined overview defines 100% zoom as the absolute lowest-to-highest span,
  including uncovered frequency gaps. Focused receiver defines 100% as one
  receiver's usable span, and the lower left/right buttons cycle that focus.
- The shared `SpectrumMosaicEngine` accepts any number of source IDs. It rejects
  stale frames, trims configurable RF/FFT edges, interpolates onto one absolute
  frequency grid and blends overlaps in linear power. Missing coverage is kept
  as a visible gap rather than joined by a false line.
- Main-view group panning applies one bounded frequency delta to every included
  receiver, preserving their spacing even across large uncovered gaps. The
  `Always pan RX together` option extends that rule to focused view; when it is
  off, focused view moves only its selected RX. Individual targets remain
  available in the configuration preview.
- Moving the listening marker into an auxiliary RF span automatically routes
  that receiver's bounded raw-IQ stream to the established audio demodulator,
  with its own center frequency and sample rate. Moving back to RX 1 switches
  to the original low-latency queue without retuning RX 1 merely to chase a
  signal already covered by another receiver. Direct-sampling HF auxiliary
  inputs remain spectrum-only until their real-channel audio path is shared.
- Multi-SDR receiver, input, center, sample-rate, FFT, gain, inclusion,
  alignment and panning settings are written immediately and restored on the
  next application start; they no longer depend solely on dialog destruction.
- Mosaic rendering is capped at 60 updates/s for two sources and 30 updates/s
  for larger groups. Closing the configuration dialog hides its preview but
  keeps the configured sessions available to the main view. Main Start/Stop
  owns all active RX sessions; when Multi-SDR is not created there is no
  auxiliary receiver cost.
- `AnalyzerReceiver` no longer depends on process-global Fobos handles. This
  prevents one receiver session from closing or retuning another and prepares
  the same transport for distinct Fobos devices and Soapy/UHD channels.

This stage is a spectrum mosaic. It does not claim that independent receivers
form one coherent complex-IQ stream. Raw-IQ joining requires shared sample
clock, aligned hardware timestamps, a known phase relationship and measured
sample-delay compensation. A validated two-channel USRP is the first planned
backend for that stricter mode.

## Thread and process assessment

The current hot path is already split into long-lived workers instead of
creating a task for every block:

- the main receiver has its acquisition worker, spectrum worker and separate
  audio DSP/output workers;
- local DSP branches and remote DSP branches are isolated, so a slow network
  worker does not block the local audio path;
- each auxiliary SDR adds one acquisition thread and one FFT thread;
- FFTW planning is serialized through the application-wide planner mutex, but
  execution of independent plans remains parallel;
- auxiliary FFTW thread budgets are divided by the number of enabled receivers
  and capped, preventing multiplication of full-core FFT pools on Raspberry Pi.

The next useful server optimization is topology-aware scheduling, not simply
raising every thread count:

1. Discover processor groups, NUMA nodes and cores at startup. Reserve at least
   one physical core for UI/audio and one lightweight acquisition core per SDR.
2. Keep a receiver's USB acquisition, ring memory, channelizer and ordinary FFT
   on the same NUMA node. First-touch or node-local allocation matters because
   two-socket remote-memory traffic can cost more than the extra cores save.
3. Assign different receiver pipelines to different sockets. Do not split a
   moderate FFT across sockets; allow cross-node FFTW only for very large FFTs
   after a benchmark proves it faster.
4. Replace the fixed four-thread ceiling in the main FFT/post-processing path
   with a shared compute budget and measured profiles: Conservative,
   Balanced, Throughput and Manual. The budget must be shared by main FFT,
   auxiliary FFTs, PFB/DDC and research jobs rather than chosen independently.
5. Keep audio, USB callbacks and local VFO/DDC at high/normal priority. Put
   mosaic generation, recording, network export and remote branches below
   them. Drop stale visual work instead of queuing it.
6. For optional isolation, run one receiver pipeline per worker process and pin
   the process to a NUMA node. This also contains unstable vendor drivers. Use
   shared memory for local full IQ; TCP remains appropriate for remote nodes.

Dual Xeon systems are therefore useful for many simultaneous receivers,
channelizers, decoders and independent FFTs. They will not automatically make
one FFT scale linearly, and an older low-clock dual-socket server can feel worse
than a modern desktop when work is serialized or memory crosses sockets. The
program should choose placement from measured topology and workload, while
Raspberry Pi keeps the present conservative single-path defaults.

## Next functional stages

1. Publish bounded auxiliary IQ blocks to the existing DSP branch manager with
   a source ID, while retaining RX 1 as the default source for old layouts.
2. Add a source selector to VFO/DDC and DSP-board blocks. A branch then binds to
   `receiverId + channelId`, not to the process-global IQ buffer.
3. Add optional time-aligned IQ capture for hardware that reports timestamps.
4. Implement native UHD as one physical-device service owning all B210 RX/TX
   streamers; expose channels to the manager without reopening the device.
5. Add coherent delay/phase calibration and only then enable complex-IQ joining
   and cross-device demodulation.
