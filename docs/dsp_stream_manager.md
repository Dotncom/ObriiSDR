# DSP Stream Manager (Experimental)

## Scope

The manager configures main spectrum FFT placement and optional DDC research
channels without replacing the receiver/audio pipeline. It supports eight DDC
channel streams, local FFT or an external processor, returned channel IQ for
research widgets, and explicit independent CF32LE branch recording. Native Obrii
compute workers and the optional GNU Radio Python adapter share the bounded DDC
protocol. Main FFT offload requires a worker advertising main FFT support; the
DDC adapter alone is not that capability. No receiver is opened by a worker.

The DDC and anti-alias filtering still run on the receiver PC. Only downstream
channel processing/FFT is offloaded by DDC controls. Main wide FFT offload is a
separate, explicit selection. Neither route offloads Wi-Fi/GNSS engines or existing
audio/DMR. A network is not inherently faster:
96 ksps CF32LE costs 768 kB/s each way when returned IQ is requested; 2 Msps costs
16 MB/s each way, before framing. Spectrum-only consumers do not return IQ.

## Independent Audio And PFB

Add and connect `DDC -> Branch demodulator -> Branch audio output`; insert one
or more `Branch audio filters` blocks between demodulator and output as needed.
Enable DDC and audio output separately (both default off). Each DDC can feed one
independent output, plus its FFT/research consumers. Demodulators reuse the
existing AM/NFM/SAM/USB/LSB/DSB/CW/WFM code; DMR/digital decoders are not branch
demodulators. Filters execute in link order, with a total limit of 32 stages.
The demodulation band must fit the DDC band and the returned sample rate.
Processed IQ for audio must be 48 ksps..2 Msps. Output is 48 kHz mono with an
independent device, gain and power-based squelch. A 250 ms pending queue and a
bounded device queue prevent unbounded catch-up; these limits are not a fixed
end-to-end latency guarantee. Display refresh does not clock or drop PCM.

Explicitly enabled branch audio continues while the board is hidden. Receiver
Stop, a disabled branch/output, invalid routing, or Simplified operating mode
stops it. No branch consumes the main audio queue. Output-device indexes are
local to the machine and may need reselection after moving settings to another PC.

Optional `IQ -> PFB channel bank -> DDC` shares an RF analysis bank across
branches. All PFB blocks in the active plan must use the same bank configuration;
there is one active receiver source, not one physical receiver per source block.
Choose 8/16/32/64 bank channels and 8..64 taps per phase. Bands crossing guarded
channel edges fail explicitly; direct IQ -> DDC is always available. FIR controls
on each DDC expose tap count and a transition-width estimate. The status reports
actual FIR taps and PFB processing time separately from downstream branch time.
See pfb_channelizer.md for delay, passband limits and measured performance.

The main Audio filters block's **Apply order from DSP links** action validates a
single complete `Demodulator -> filter blocks -> Audio output` path and reorders
the matching existing stages. Unrelated stages retain their slots and all stage
parameters/enabled states remain unchanged. This is a one-shot apply action, not
automatic rewiring of the main audio path. Independent branch audio links execute
directly; ordinary legacy main-path diagram links otherwise stay descriptive.

## Two Computers

1. On the compute PC open the DSP board, then the computer-icon **DSP stream
   manager** button beside DDC +. Open **Compute on this PC**.
2. Enter that PC's local LAN IP address and port (default 7358), choose the main FFT
   memory budget (64-8192 MiB, default 512), explicitly allow computation and press
   Start. Copy the generated session token.
3. On the receiver PC, in the same manager's **External processor** tab, enter
   the worker IP, port and token. Check **Allow IQ transfer** and Apply.
4. Add DDC +, connect its spectrum/waterfall or research view, enable the branch,
   set its band/output rate and select **External processor / GNU Radio** in
   the DDC editor. Begin with 96 or 192 ksps and one branch.
5. For main FFT offload, select **Main FFT**, choose **Individual IQ windows** or
   **Full continuous IQ**, backend and upload budget, then Apply. DDC selection
   is independent; no DDC branch is required for main FFT.
6. Leave the worker dialog open. Closing it stops its service. Credentials and
   transfer permission are not saved; reconnect explicitly next application run.

Only trusted LANs, loopback or an independently secured tunnel are suitable.
The session token authenticates access but does not encrypt IQ or protect against
LAN interception. No automatic discovery, firewall change, Internet listener,
RF control or automatic transmission is performed. Listening on a non-loopback
address is an explicit user action. The native service supports at most two
sessions, allowing main FFT and DDC to use separate connections, with at most one
main FFT session to bound memory use.

## Main FFT Controls

The **Main FFT** tab and the existing **FFT and resolution** block editor use the
same controls and one global preference set. Editing a field does not start work;
**Apply** commits the mode, backend, upload budget and fallback choice. Applying
endpoint/transfer permission on the **External processor** tab is separate.

- **Local** keeps FFT computation on the receiver PC.
- **Individual IQ windows** sends individual FFT input windows to the worker.
- **Full continuous IQ** requests the continuous full-band IQ stream; this can
  require substantially more upload bandwidth than individual windows.
- Backend values are CPU (FFTW) = 1, GPU = 2, Auto = 0. The selected remote backend
  is a compute request, not a change to graph signal routing.
- Upload budget is 1-2000 MB/s, default 100, using decimal MB/s. This is a resource
  limit, not a throughput guarantee. CF32LE requires 8 bytes per complex sample
  before protocol overhead; 20 Msps therefore requires at least 160 MB/s.
- **Allow local FFT if remote processing fails** is off by default. Remote errors,
  backend/capability refusals and memory-limit refusals are reported through the
  main FFT status. Local fallback requires the explicit checkbox.

The worker's main FFT memory budget is editable only while stopped and is passed
to `DspComputeServer::start(address, port, token, mainFftMemoryMiB)`. Its advertised
maximum FFT length is `min(67108864, budgetMiB * 1048576 / 64)` points. The
conservative allowance includes FFT kernel, rings and frame memory; 512 MiB
advertises at most 8,388,608 points. Requests exceeding the advertised capacity
are explicitly refused, not silently reduced. The budget is not a measured
working-set guarantee and does not reserve that amount of physical RAM.

Stop and revoking worker consent call nonblocking `requestStop()`. While the
status is **Stopping**, the service still reports running and its memory budget
remains locked. The current FFT itself is noninterruptible. Closing the dialog
still destroys the service and joins outstanding work, so close may wait for the
current transform to finish.

Board configurations/presets persist only the `spectrumCompute` preference object:
`mode`, `backend`, `uploadLimitMBps`, `fallbackToLocal`. Tokens, transfer permission,
worker consent, runtime status and endpoint details are not written to the board
configuration. Older layouts without this object load Local/FFTW/100/no fallback.
Loading a layout does not grant transfer permission; already granted in-memory
permission remains a session setting. Worker memory budget is dialog-local.

## Board And Integration

Only the explicit `receiver_source` or `iq_source` -> existing `fft` ->
`workspace_spectrum` / `workspace_waterfall` route binds to the main shared frame
path. Sources refer to the currently selected receiver/IQ input, not independent
receivers. The FFT block gains a spectrum output port. Multiple such FFT blocks
are aliases of the same main FFT settings and frames, not extra FFT instances.
The ruler and unwired legacy displays keep their existing shared-frame behavior.

The compiler exposes `DspGraphPlan::mainFftSources` (FFT ID -> source ID) and
`mainSpectrumRoutes` (workspace view ID -> FFT ID). Only complete supported paths
populate the view mapping. A view explicitly wired to an FFT without its supported
source receives no new frames; removing the FFT-to-view wire restores the legacy
implicit display path. Main FFT mappings are separate from DDC `spectrumRoutes`
and do not alter the DDC `executionKey`. A workspace settings binding can coexist
with its spectrum input. Ordinary legacy links still do not execute a DSP graph.

`DspFlowPanel` integration API:

- Read committed state with `spectrumComputeOptions()` and `externalEndpoint()`.
- Set preferences with `setSpectrumComputeOptions(const SpectrumComputeOptions&)`;
  invalid modes/backends default to Local/FFTW and upload limits clamp to 1-2000.
  `spectrumComputeOptionsChanged()` fires synchronously when committed state
  changes (Apply, setter or successful layout restore). It has no arguments;
  integration reads the getter. Unapplied drafts do not emit it.
- `setExternalEndpoint(const DspStreamEndpoint&)` retains the existing DDC
  endpoint behavior and emits `externalEndpointChanged()` on a real change.
  Integration reads the getter; no credential-bearing signal payload is needed.
- `setSpectrumComputeStatus(const QString&)` updates both editors; status is
  runtime-only. `spectrumComputeStatus()` and `spectrumComputeStatusChanged(status)`
  expose the same plain-text status for the block editor.
- `showComputeDialog()` opens/raises the existing nonmodal manager. A new manager
  receives current committed preferences, endpoint and status. Dialog setters do
  not emit Apply signals. Its existing constructor remains compatible.

`SpectrumComputeControls` is the shared draft editor. `options()` reads its fields,
`setOptions()` synchronizes them, `setStatus()` displays runtime status, and
`applyRequested()` is the only user-commit event. Main receiver integration owns
the remote worker and result application; the board does not own a second main
FFT executor or a separate compute-target signal-flow block.

## Connection Diagnostics

- Before enabling external processing, enter the worker IP, port and current
  session token, then open **Connection test** and press **Test connection**.
  The test uses these draft fields without Apply, changing the selected compute
  mode, enabling IQ transfer, opening a receiver or reading live IQ. Both computers
  need the updated native Obrii worker; old workers and the GNU adapter report
  that the test is unsupported. The worker must already be listening.
- The test runs off the GUI thread and can be cancelled. It measures TCP setup,
  application handshake, 12 authenticated request/reply RTTs (minimum, median,
  p95), RTT standard deviation (jitter), then sequential upstream and downstream
  payload throughput. Each direction runs up to 1.5 seconds or 128 MiB; there
  is no per-packet reply in the bulk upload. This measures the application path,
  not the physical Ethernet link rate, one-way latency or UDP packet loss.
- A diagnostic session is exclusive with compute sessions; stop delegated work
  before testing. No live sessions are disconnected to make room. A completely
  occupied two-session server may close the extra connection before handshake.
  Closing the dialog cancels the test; TCP connect itself has a 1.5 s bound.
- The results retain the tested endpoint and mark loopback explicitly. Example
  traffic: full CF32 at 8 MS/s is 64 MB/s; at 50 MS/s it is 400 MB/s. Returned
  spectra cost 4 bytes per FFT bin. Editable Fs, FFT length, update interval and
  overlap are estimates only, initially copied from the receiver controls.
  They do not retune or otherwise change the receiver.
- Window mode requests a fresh sliding observation at the configured interval;
  its estimates are not capped at Fs/N and overlap does not govern its cadence.
  Continuous mode uses max(N * (1-overlap), Fs * interval), with actual emission
  quantized by arriving chunks. Their return-spectrum traffic is shown separately.
  Window mode additionally needs the entire FFT in local history, capped at
  8,388,608 complex samples regardless of the remote worker memory budget.
- The estimate reserves 25% of measured bandwidth and respects the draft upload
  budget on Main FFT. Its per-IQ-chunk ACK estimate remains conservative for the
  negotiated four-chunk pipeline (and applicable to legacy lockstep peers), but excludes
  actual FFT computation, source copying, rendering, protocol overhead, competing
  traffic and receiver load. "Headroom available" is not a real-time guarantee.
  The advertised FFT memory limit is checked separately. No network test alone
  establishes that remote FFT is faster than local FFT.
- The default `127.0.0.1` bind address accepts clients on the same computer only.
  For two computers, stop the worker, select its LAN address, and start it again.
  The worker status shows the bound TCP address/port and warns about loopback.
- Listening without opening a receiver is normal: the worker processes IQ sent
  by the receiver PC. Leave the worker dialog open, and use its current token.
- Open the DDC editor or main FFT controls to read the relevant connection error. TCP errors retain the remote
  endpoint and the operating-system socket reason between retries. A refused
  connection, timeout, or access-denied error is distinct from a rejected token.
- A new native worker explicitly reports mismatched session tokens on both ends.
  Older workers/adapters may only report an unsuccessful handshake.
- With Logging enabled, connection-state changes are logged without tokens or IQ.
  Repeated identical failures and per-block statistics do not flood the log.
- If TCP cannot connect despite the correct listening LAN address, check routing
  and whether the compute PC firewall permits Obrii on the trusted LAN and chosen
  TCP port. Do not disable the firewall globally or expose the service publicly.

## Independent Recording

In a running, enabled DDC editor choose **Record branch IQ** and a user-data
directory; Stop recording finalizes it. Recording is independent of the main
recorder. CF32LE samples and JSON metadata are written in unique segments, never
over existing files. Gaps and changes of source/rate/frequency begin a new segment,
not an apparently continuous file. The default session cap is 256 MiB. These are
relative channel sample positions, not hardware RF/UTC timestamps. Recording is
not automatically armed from a saved layout. Stop RX, disable the branch or enable
simplified mode to stop it. An explicitly armed recorder can keep its branch
working while its visual widgets are hidden.

Stopping is asynchronous: each segment creation and bounded sample write is
admitted against the current generation immediately before I/O. An already
admitted operation may finish after Stop was requested (including creating an
empty segment); the worker then finalizes its files. Disk I/O never holds the
branch-control mutex and cannot delay local audio through that mutex.

## Resources And Failure Behavior

- Local processing calls the shared processor directly, without JSON, sockets or
  wire copies. FFT history uses a fixed-size reusable ring.
- A DDC connection has one outstanding work request, bounded packets, finite deadlines
  and no unlimited backlog. Slow/disconnected workers report a stream interruption.
  No silent switch to another processor or change of sample rate occurs. Main FFT
  local fallback is separately opt-in as described above.
- Local DDC/PFB processing never waits for that connection. Channelized IQ moves
  to a separate external worker with two pending blocks per branch, sixteen total
  (at most 8 MiB pending CF32), plus one in-flight request. Oldest pending data is
  dropped on overflow with an explicit gap, not accumulated into growing latency.
  The worker is created only when external work is submitted and sleeps on a
  condition variable while idle. Local-only use adds no remote thread or IQ queue.
- Source epochs, retuning, stopping and simplified mode invalidate remote jobs
  and cancel transport waits. Endpoint changes do not reset local demodulators.
  Remote branches still share their connection; their own audio can have gaps
  when it cannot keep up. Shared CPU/disk/audio-device limits remain applicable.
- Epoch/generation/sample counters invalidate mixed, interrupted and stale results.
  Source IQ is read via snapshots, never taken from the main audio queue.
- Hidden/offscreen visual consumers stop requesting work unless recording or
  audio was explicitly enabled. No branch worker is created if no branch is active.
- **Simplified operating mode** disables these optional DDC/stream branches and
  recording. Its legacy settings key is preserved. Main RX/audio/DMR and the main
  spectrum/waterfall keep their established shared-frame path with separately
  selected main FFT placement. The separately, explicitly
  started compute service is independent of this receiver-side setting.

## Bounded Main FFT Transport

The native main FFT v1 handshake optionally advertises `mainFftPipelineChunks: 4`.
The client requests `pipelineChunks` in `main_fft_begin`, and requires the same
integer in `main_fft_ready` before uploading. Bounds are 1-4. A missing capability
means explicit legacy one-chunk lockstep: no extension is sent or assumed.
Old clients still receive their unchanged ready reply when they omit the field.
Malformed capabilities or missing/mismatched confirmations fail closed. Framing,
tokens, chunk payloads, job IDs, ACKs and result messages are unchanged; this does
not change DDC protocol or processor APIs.

The sender permits at most four outstanding 131072-complex-sample IQ chunks
(4 MiB CF32LE plus bounded framing). Each ACK must match the oldest pending job,
64-bit next-sample counter and filled count. One ACK releases one credit. The
fixed-size client queue contains only four counter records, not extra FFT windows.
Socket/read buffers and single-packet write bounds remain unchanged. Server
history, memory-budget limits, one-main-FFT admission and the two-connection cap
are unchanged. Whole framed packets still count toward the upload budget.

TCP orders `main_fft_run` after the final IQ chunk; the client can send it before
draining the last ACKs, allowing server computation to overlap ACK validation.
Upload/chunk ingestion and ACK transit can overlap inside the credit window.
There is still only **one transform and result download at a time**. No next-frame
IQ is uploaded during FFT computation or spectrum download, no second FFT buffer
is allocated for pipelining, and no asynchronous compute scheduler is introduced.
That limit avoids extra large-FFT memory, mutable-history races and full-duplex
write deadlocks. Exact FFT/window/scaling/bin order is unchanged.

Pending ACKs are drained before replacing a job and when continuous input becomes
idle. Cancellation, demand expiry or source epoch/loss changes close the socket
and discard its pending credits. Reconnection never replays unacknowledged IQ
into another job. Continuous transport failures count as gaps; retained-ring
overruns still report skipped samples and restart history. Generation, epoch,
loss, job, token and final result-counter checks remain active throughout.
Runtime status includes `IQ window 1 chunks` or `IQ window 4 chunks`.

Opt-in local fallback retains one local FFT kernel and IQ snapshot across retry
attempts in the same request generation. It does not recompute an unchanged
source window on every retry. Transient connection/busy failures back off by
1, 2, 4, then at most 8 seconds; protocol/authentication/capacity refusals use
30 seconds. A complete remote result or changed configuration resets backoff.
The first complete validated remote result releases the local FFT plan and IQ
storage; generation changes, cancellation and lease expiry also release them.
The cache stays warm only during failure/retry cycles. Its bounded memory can
coexist with a remote attempt, but no parallel local compute
thread is added. Fallback observes the same epoch/loss/history barriers and is
still explicitly labelled. Reconnect attempts can pause local updates: this is
not seamless failover. No local FFT occurs unless fallback was enabled.

## Loopback Measurement

Measured on this Windows host on 2026-10-13, MSVC `/O2`, Qt 5.15.2, CPU FFTW,
127.0.0.1 TCP, native server budget 64 MiB. Each row uses a fresh worker and one
cold plus nine warm frames, deterministic Hann-window IQ and exact-source parity
checks. "Cold" means the first frame on a fresh remote connection, not a cold
machine/process; the reference FFT has already run. The synthetic source advances
one N-sample block after each returned frame, not at a sustained RF sample rate.
Times cover request through frame receipt (1 ms polling), not rendering
or RF acquisition. Baseline was compiled before transport changes with the same
benchmark harness. Values are milliseconds, rounded to one decimal; warm p95 is
the maximum of nine observations. These small samples are descriptive, not a
statistical speed guarantee; other concurrent host work can affect them.

| Mode | N | Cap MB/s | Cold before/after | Warm median before/after | Warm p95 before/after |
| --- | ---: | ---: | ---: | ---: | ---: |
| Windows | 65536 | 100 | 38.8 / 34.1 | 45.6 / 48.7 | 49.8 / 53.7 |
| Windows | 65536 | 2000 | 22.8 / 42.0 | 32.5 / 30.1 | 34.1 / 45.1 |
| Windows | 1048576 | 100 | 225.6 / 263.8 | 233.0 / 215.1 | 249.4 / 253.6 |
| Windows | 1048576 | 2000 | 204.3 / 204.7 | 148.9 / 135.0 | 180.3 / 165.2 |
| Continuous | 65536 | 100 | 45.8 / 58.0 | 48.2 / 46.5 | 52.9 / 51.9 |
| Continuous | 65536 | 2000 | 38.1 / 23.3 | 31.7 / 31.9 | 35.0 / 32.3 |
| Continuous | 1048576 | 100 | 246.0 / 244.5 | 232.8 / 229.7 | 237.2 / 256.2 |
| Continuous | 1048576 | 2000 | 156.9 / 144.7 | 147.6 / 128.5 | 183.1 / 149.4 |

The eight-chunk, high-cap warm medians improved in this run; small-window and
tail timings were mixed. These are **loopback cycle times, not actual LAN speed**,
and do not establish full-rate continuous streaming, GPU performance or a speedup
over local FFT. Upload caps are configured ceilings, not measured link rates.
An additional repeat measured high-cap 1048576-point warm medians of 121.4 ms
(windows) and 121.6 ms (continuous); timing variability remains material. Raw
repeat output is in `build/remote-fft-selftest/final-loopback.log`.

Reproduce without shared CMake changes or launching the GUI:
`powershell -File tools/build_remote_fft_selftest.ps1` builds and runs the isolated
headless `build/remote-fft-selftest/ObriiSpectrumComputeSelfTest.exe`.
Add `-Benchmark` for timings or `-BuildOnly` to compile only. The existing shared
CMake target remains `ObriiSpectrumComputeSelfTest`. The script does not publish,
package, or replace the sole
application executable.

Deterministic tests withhold ACKs to prove four credits (one for legacy), release
one credit at a time, and prove run-before-ACK-tail ordering with exact FFT parity.
They cover bad negotiation/counters/jobs, disconnect/timeout, full-window
cancel/retune/epoch/lease expiry, fallback duplicate suppression/backoff/recovery,
server stop, concurrent DDC and untouched audio queues. Manager coexistence also
checks that rational output metadata survives consecutive processor replies.

## Remaining Phases

Small FFTs may be faster locally. At 50 MS/s, FFT 65536 and 100 ms window updates
need approximately 5.24 MB/s up and 2.62 MB/s down instead of continuous 400 MB/s
up; transfer volume alone does not include FFT, copy and rendering time.

Continuous joining now seeds from the newest complete retained FFT window,
or all retained samples when N exceeds local history. Later overrun recovery
still clears remote history and reports gaps rather than silently joining across
missing IQ. `IQ lag` is sample-clock distance from the returned window end to the
latest published IQ, measured at frame publication. It is not wall-clock RF age;
when acquisition pauses the sample clock also pauses. The existing `latency`
describes the processing cycle and can include waiting for new IQ.

Future performance work should benchmark full cold/warm FFT cycles on the actual
two computers and evaluate bounded next-frame transfer during processing/results
only with explicit memory and cancellation ownership. The server's
1-second compute-payload flush and 5-second request-idle limits also mean a slow
or paused peer can need reconnection. The preflight tool does not remove those
limits or establish GPU speed from link bandwidth.

On a retune the old remote transform may still be finishing. Admission has a
short bounded grace for retiring sessions; the client retries unavailable/busy
connections rather than adding workers or growing a queue. Already executing
native FFTs are not interruptible. The previous displayed frame can remain while
the status reports the reconnect/failure, and receiver/audio processing continues.

Main FFT software checks include 2,000 exact snapshot parity frames, 300 direct
DFT reference cases, 47 chunk/ring/protocol session frames, native loopback main
FFT plus DDC, bounded pipeline/legacy negotiation, arbitrary FFT lengths, retuning, sampled-center metadata, explicit
fallback, cancellation, mode toggles and preservation of the audio queue.
These checks do not qualify full-rate LAN streaming or GPU performance.

This is a bounded shared-stream system, not a general GNU Radio scheduler in
Obrii. Development after 6.2.0 adds eight independent DDC branches, branch
demodulator/filter/audio sinks, explicit main-filter wire-order application,
FIR controls and an opt-in PFB bank. The GNU adapter accepts persistent one-in/
one-out complex blocks at unchanged or bounded rational sample rates. Arbitrary
rate-changing graphs, automatic load balancing and qualified device-clock
timestamps remain separate work. See gnu_radio_bridge_usage.md,
dsp_stream_protocol.md and pfb_channelizer.md.

Local synthetic/loopback tests do not establish Raspberry-to-laptop throughput.
That requires an actual two-machine test and comparison with the manager disabled.
