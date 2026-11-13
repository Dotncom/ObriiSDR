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
- Every DDC branch now stores an explicit `receiverId + channelId`. Existing
  layouts default to `main/rx0`; RX 2 through RX 8 are exposed as
  `multi-rx-2/rx0` through `multi-rx-8/rx0`. The identity also crosses the
  network protocol and separates worker history and branch recordings.
- Auxiliary IQ has a bounded per-source history and is not copied unless an
  enabled DDC branch subscribes to that source. Its low-priority worker is also
  created lazily. Thus a layout that uses only RX 1 retains the old hot path.
- Each subscribed source can own one independent PFB bank shared by its DDC
  branches. Banks may use different channel/tap geometry across receivers;
  branches on the same source must agree. Bank history, discontinuity reset and
  selected outputs remain source-local. Direct DDC remains available and a
  layout that does not request an auxiliary bank pays no PFB cost.

The mosaic remains the default mode, but the first coherent-IQ foundation is
implemented behind an explicit opt-in switch:

- every subscribed source publishes stream epoch, monotonically increasing
  complex-sample position, host timestamp, clock-group ID, lock/timestamp flags,
  native ADC bit depth and native sample format together with normalized FC32;
- coherent processing subscribes to and copies IQ only during capture or live mode;
- every RX can declare internal/master/external clock role, a physical clock
  group and optional hardware-start synchronization;
- a bounded one-shot calibration estimates integer and fractional sample delay,
  complex phase, gain and normalized coherence against a selected reference;
- retune, sample-rate change, stop or stream restart clears stale IQ and
  invalidates the calibration;
- receivers of different native precision are never promoted to fictitious
  bits. Their native format remains metadata, while alignment operates on
  normalized float IQ and measured complex gain;
- after successful calibration, **Save IQ snapshot** writes every participating
  source plus one atomic JSON manifest. The session keeps epochs, 64-bit sample
  positions, timestamps, clock metadata, native ADC format and calibration;
- version 2 manifests record a SHA-256 for every CF32LE payload. Replay rejects
  damaged or incomplete sessions before exposing any source. Version 1 files
  still open with the legacy length and metadata checks;
- the shared session core can restore those snapshots and combine them as the
  reference stream, best-SNR source, equal average or noise-weighted MRC. It
  weights measured signal/noise and calibration, never nominal ADC width.

### Live coherent IQ

In **Multi-SDR → Coherent receiver group → Test virtual pair**, two virtual
receivers produce a common broadband calibration signal at 145.5 MHz and
48 kS/s. The second source has an 11-sample delay, 73° phase offset, and 0.65×
gain. The Multi-SDR preview shows the combined spectrum and waterfall. In
**DSP Flow**, select **Coherent IQ (Multi-SDR)** as the DDC's IQ source and
select RX channel 0. For the virtual pair, enter 145.5 MHz as the DDC center
and 48 kS/s as the output rate, and disable following the main VFO.
DSP Flow uses its usual branch conditions: main reception must be active and
the simplified audio channelizer disabled. The Multi-SDR preview can run the
virtual pair on its own.

Select **Stitch IQ bands** before starting the live group or the virtual pair
to combine two adjacent bands. Both receivers need a verified common timeline,
the same sample rate up to 192 kS/s and centers separated by that rate. The
result has twice the sample rate and a center halfway between the inputs.
The filters leave guard bands at the seam and edges. The preview and the
same DSP Flow source carry the joined IQ. The coherent multichannel recorder
requires two aligned streams at one center. The virtual stitch test uses
48 kS/s per input and verifies both signals at 96 kS/s.

**Start live coherent IQ** uses the running receivers in the
Multi-SDR group, with the selected RX as the reference. Live mode requires a
single center frequency, a common rate up to 192 kS/s, and a verified shared
sample timeline. It corrects delay, phase and gain before averaging, with a
live delay search capped at ±512 samples. The
session stops on a discontinuity, loss of clock lock, or loss of coherence.
A receiver that publishes only its own sample counter cannot enter this mode.
The current Fobos API exposes no verified shared sample timeline. Hardware
validation and the corresponding backend support are separate work.

The live status tooltip reports the largest estimated relative phase drift
against the reference receiver in Hz. The estimate uses consecutive coherent
blocks and clears on recalibration or loss of continuity.

For same-center reception, **Weight by noise (MRC)** estimates each channel's
noise floor from short spectra during calibration and gives quieter channels
more weight after gain and phase alignment. The tooltip shows the weights,
starting with the reference receiver. The weights stay fixed until
recalibration. When the calibration spectra cannot separate signal and noise
reliably, the combiner uses equal weights.

**Track phase drift** uses cross-correlation against the reference to rotate IQ
continuously across block boundaries. It corrects slow relative phase changes
while the streams remain aligned. A sudden phase jump, weak correlation or IQ
gap still invalidates the calibration. Both options start disabled and work
with the virtual pair. They apply to same-center reception, including **Align
by signal (Fobos)**. Band stitching uses its own combiner.

**Align by signal (Fobos)** is a separate software mode for two receivers
using the existing Agile or Standard IQ APIs. Set both RF inputs to the same
center and 8 MS/s, switch off scanning and span auto-alignment, connect the
shared 10 MHz reference, and feed the same modulated signal within about
±47 kHz of the center. Select the reference receiver, start both, and click
**Align by signal (Fobos)**. The spectrum, waterfall and DDC source use the
combined 125 kS/s channel. Set the DDC output rate to 125 kS/s or lower.

The software compares a short window against about ±98 ms of relative
arrival skew. A pure tone can measure phase drift but cannot establish a
unique delay. If correlation, phase or stream continuity fails, the
combined output pauses and starts a fresh epoch after reacquisition.
The status **Aligned by signal** describes an estimate from received IQ.
The stock USB stream can conceal a loss that is too small to detect from
the signal. Hardware clock lock and shared sample numbering are separate.

### Continuous coherent IQ recording

Start a live coherent group or **Test virtual pair**, wait for the preview,
then click **Record coherent IQ…** and choose a directory. The recorder writes
each delay-, phase- and gain-corrected RX as a separate CF32LE channel at the
coherent output rate. All channels in a segment have the same 64-bit sample
range and epoch. File writes and SHA-256 run on a separate worker with a
bounded queue. Each segment holds roughly two seconds of samples, and a
recording can contain up to 4,096 segments.

Click **Stop IQ recording** to save the final segment and commit
`recording.json`. An IQ gap, a loss of alignment, or a full disk queue ends the
recording and indexes the valid prefix when it can be saved. The status shows
the reason and its tooltip shows the index path. Keep the segment folders
with the index. After stopping the live preview, click **Replay IQ recording…**
and select that index. Playback verifies the segment hashes and timeline and
combines the stored aligned channels at the recorded rate. It uses the saved
weights when present and equal weights for older recordings. The combined IQ
feeds the spectrum, waterfall and **Coherent IQ (Multi-SDR)** source for DSP Flow.

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

The shared compute-budget and first topology-aware scheduling stage are now
implemented rather than raising every independent thread count:

1. Startup discovery records logical threads, physical cores, Windows processor
   groups and NUMA nodes (Linux uses sysfs node/cpu topology).
2. Conservative, Balanced, Throughput and Manual profiles reserve realtime
   capacity and provide one budget to main FFT, auxiliary FFT, DDC branches and
   research work. Auxiliary FFT allowance is divided between enabled receivers.
3. Main/auxiliary FFT and local/remote DDC workers receive stable topology roles.
   On multi-group Windows this is an ideal-processor hint; on multi-node Linux
   it is a thread affinity mask. Single-node machines are unchanged.
4. The stream manager's Local resources tab shows detected topology, selected
   profile and resulting limits. Changes persist through QSettings. An existing
   FFTW plan adopts a changed thread count when that plan is next rebuilt.
5. Simplified operating mode still disables optional DDC/stream-manager work;
   it does not secretly reduce the user's selected main FFT or receiver rate.

Future refinement may add first-touch/node-local buffer allocation and optional
per-receiver worker-process isolation. Those need measurement on real dual-socket
hardware; forcing them today could make a single-socket desktop or Raspberry Pi
worse.

Dual Xeon systems are therefore useful for many simultaneous receivers,
channelizers, decoders and independent FFTs. They will not automatically make
one FFT scale linearly, and an older low-clock dual-socket server can feel worse
than a modern desktop when work is serialized or memory crosses sockets. The
program should choose placement from measured topology and workload, while
Raspberry Pi keeps the present conservative single-path defaults.

## Next functional stages

1. Implement native UHD as one physical-device service owning all B210 RX/TX
   streamers; expose channels to the manager without reopening the device.
2. Qualify delay/phase calibration with a shared reference signal, then enable
   live complex-IQ stitching, MRC and cross-device demodulation.
3. Qualify the new per-source PFB banks on Raspberry Pi and a multi-receiver
   desktop, including CPU/memory budget, teardown and simultaneous audio.
