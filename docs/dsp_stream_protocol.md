# DSP Stream Protocol v1

Development contract for optional research streams and the native main FFT
extension. The receiver/audio queue is never consumed or replaced by this service.
Endpoints do not control RF devices. The implementation reference is
`src/core/dspstreammanager.cpp` and `src/core/spectrumcomputesession.cpp`.
All integers below are unsigned unless specified. TCP transport is unencrypted:
use loopback, a trusted private LAN or an independently secured tunnel only.

## Framing

Each message is `OBDSP001` (8 ASCII bytes), uint32 LE JSON byte length,
uint32 LE binary byte length, UTF-8 JSON object, then binary payload.
JSON is limited to 65536 bytes and binary data to 2097152 bytes. Reject invalid
magic, excessive lengths and malformed JSON before allocating a payload.
For compute requests only one may be outstanding on a connection; slow consumers must not
grow a queue. A main FFT run has a multi-message reply, completed by
`main_fft_done`. Timeouts terminate the connection, not the main receive path.

The first request is `{ "type":"hello", "version":1, "token":"..." }`.
An older DDC-only endpoint replies `{ "type":"hello", "version":1 }`.
The native main FFT worker replies, with an empty binary payload:

```json
{"type":"hello","version":1,"mainFftVersion":1,"mainFftMaxLength":8388608,"connectionProbeVersion":1}
```

`mainFftMaxLength` is a number of complex input samples/FFT points, not bytes.
Clients must require `mainFftVersion: 1` before using the extension and respect
the advertised length; a v1 DDC hello alone does not promise main FFT support.
A native worker may send an error with `code: "authentication_failed"` and
`error: "Session token rejected"` before disconnecting when the token differs.
Clients must also support silent disconnects from older workers.

The manager generates a random token per worker dialog/session. Native server
configuration requires 32-128 printable non-space ASCII characters (codes 33-126).
Tokens are never logged or saved in DSP layouts; transfer/worker permission is
session-only as well.

## Connection Probe v1

The authenticated hello may add `purpose: "connection_probe"`. On a supporting
native server this reserves a diagnostic session exclusive with compute sessions.
If processing or another probe is active, the authenticated peer receives
`type: "error", code: "busy"`. Session limits still apply before authentication.
An older server without `connectionProbeVersion: 1` is disconnected by the probe
client without sending any test/compute requests.

All packets use the existing bounded framing. After hello, the exact sequence is:

1. Twelve empty `probe_ping` requests with integer `id` 0 through 11, each answered
   with `probe_pong` and the same ID.
2. Empty `probe_upload_begin`, answered with `probe_upload_ready`.
3. Streaming `probe_upload_data` messages, ID 0 upwards, each with exactly 262144
   bytes of `0x5a`. No per-message ACK. Empty `probe_upload_end` carries total
   integer `bytes`; `probe_upload_done` confirms the validated byte count.
4. Empty `probe_download`. The server streams `probe_download_data`, ID 0 upwards,
   with exactly 262144 bytes of `0xa5`, then empty `probe_download_done` with total
   `bytes`. The client validates pattern, order and total count.

Each bulk direction stops at 1500 ms or 134217728 bytes. Upload duration is also
bounded server-side (8 s), total diagnostic session 20 s plus bounded reads;
reads have a 4 s inactivity bound and writes a 3 s bound. Buffers retain at most
one bounded packet plus socket buffering. No RF data, filenames or receiver
controls exist in this extension. No result or credentials are auto-persisted.
Throughput uses decimal MB/s and end-to-end payload bytes/time, including the
final acknowledgement. RTT is application round-trip time, not a synchronized
one-way timestamp. Transfers are sequential, not a simultaneous duplex benchmark.

## Sessions And Limits

The native listener permits up to **two simultaneous TCP sessions**, including
connections still authenticating. A newly accepted descriptor may wait up to
100 ms for a retiring session during reconnect; no third worker is created.
If both sessions remain occupied, the new connection is aborted, with no busy
reply guaranteed. Main FFT and DDC can therefore use separate connections to the
same endpoint. The per-connection DDC processor still has its separate two-stream
limit; a session is not a DDC stream ID.

At most **one session owns main FFT processing**. The first `main_fft_*` request
on a connection attempts to acquire that lease, after authentication. Another
connection attempting main FFT receives
`{"type":"error","error":"Main FFT worker is busy"}` and closes. The lease
lasts until disconnect, including between runs; `main_fft_done` does not release
it. A new begin on the owning connection reuses that lease. DDC `work` requests
remain independently dispatched, but clients should use separate connections to
avoid serializing DDC work behind a large transform.

The worker memory budget is clamped to 64-8192 MiB, default 512. Its advertised
main FFT cap is `min(67108864, budgetMiB * 1048576 / 64)`. The conservative
64-byte-per-point allowance covers kernel, ring and frame memory, not just the
8-byte CF32LE input. The default cap is 8,388,608 points. This is an admission
limit, not a guarantee that the OS/backend can allocate a given transform.

Current native socket deadlines are 1500 ms for the initial hello, 5000 ms to
receive each subsequent request, and 1000 ms to drain each outbound packet.
Reads/writes check shutdown in short waits. FFT computation itself has no
interruptible deadline: the next-request receive timer is not running while a
transform executes. Stop/consent revocation calls nonblocking `requestStop()`;
`running()` stays true while sessions wind down. Destruction/`stop()` still joins
the outstanding transform. There is no wire-level cancel command or heartbeat.

## DDC Work

The client sends `{ "type":"work", "stream":"block-id", "generation":"1",
"epoch":"1", "firstSample":"0", "sampleCount":4096, "sampleRate":96000,
"centerHz":100100000, "gap":false, "returnIq":true, "ffts":[
{ "id":"fft-id", "length":2048, "window":1, "refreshMs":100 }] }`.

Input is interleaved IEEE-754 little-endian float32 I,Q (CF32LE); binary length
must equal sampleCount*8. Sample count is 1..65536; sampleRate is 1000..2000000
complex samples/s. All floats must be finite. centerHz is finite, 0..8e9.
Identifiers are 1..128 characters. Generation, epoch and firstSample are decimal
uint64 strings, not JSON floating numbers. firstSample counts CHANNEL samples,
not receiver samples. They are relative stream positions, NOT UTC/RF timestamps.

At most two FFTs, each with length in {512,1024,...,65536}, window 0..4 and
refreshMs 20..2000. Window codes: rectangular, Hann, Hamming, Blackman-Harris,
flat-top. A changed generation/epoch/rate/center/config, a gap, or a noncontiguous
firstSample resets all channel history and GNU Radio filter state. Changing only
returnIq does not reset history. Windows use the symmetric finite-N convention
(phase denominator N-1), including exact finite-N ENBW.

The reply echoes stream, generation, epoch and firstSample and has type=result,
sampleRate, centerHz, sampleCount, processingMs and an array spectra. Each spectrum
has id, length, binWidthHz, rbwHz and offsetBytes. The payload starts with optional
returned CF32LE IQ when returnIq=true (iqBytes=sampleCount*8, otherwise iqBytes=0),
followed by each FFT's float32 LE dBFS levels in ascending frequency order.
FFT normalization is abs(FFT(window*x))/sum(window), floor -200 dBFS; RBW is
Fs/N * N*sum(window^2)/sum(window)^2. Frequency at index i is centerHz+(i-N/2)*Fs/N.
No FFT is emitted until its full history is available, and refreshMs limits
outputs by elapsed monotonic time. The reply is still sent for every work block.
Maximum retained IQ history is 65536 complex samples per stream, two streams.
A third stream ID is rejected until the connection/session is reset.

Errors use `{ "type":"error", "error":"bounded non-sensitive reason" }`.
Reject unknown requests; never execute commands or deserialize executable code
received over the connection. Successful replies do not assert loss-free RF.

## Main FFT Begin

The extension uses the same framing and authentication, but not the DDC
`work`/`result` schema. It processes one full-band complex stream as `INPUT_RF`;
there is no RF control, HF-combined mode, returned IQ, DDC, or resampling here.
Configuration is a JSON-only request:

```json
{"type":"main_fft_begin","job":"fft-job-1","mode":"windows","length":1048576,"rate":20000000,"center":100000000,"window":1,"backend":1}
```

- `job`: nonempty string, at most 80 characters. Use a new job identifier when
  changing source/configuration so stale replies cannot be mistaken for new data.
- `mode`: exactly `windows` or `continuous`; Local is a client choice, not a wire mode.
- `length`: integer FFT points, 512 through the advertised cap. Unlike the DDC
  FFT grammar, this handler does not enforce radix-2 lengths.
- `rate`: finite complex sample rate, 1000 through 1,000,000,000 samples/s.
- `center`: finite center frequency, 0 through 8,000,000,000 Hz.
- `window`: 0-4, using the same rectangular/Hann/Hamming/Blackman-Harris/flat-top codes.
- `backend`: Auto = 0, CPU FFTW = 1, GPU VkFFT = 2. This is passed to the worker's
  `FFTResult::setBackendPreference`, not to a client-local transform.

Success returns `{"type":"main_fft_ready","job":"fft-job-1"}`, without
binary data, and clears sample history/continuity state. A subsequent begin
reconfigures and clears the same session. Invalid fields, a nonempty begin
payload, or length beyond the memory cap fail explicitly with
`Invalid main FFT parameters or worker memory budget exceeded`; length is never
silently clamped. Ready acknowledges accepted configuration/history allocation,
not successful FFT planning/execution, which happens at run time.

Backend is a preference, not a guarantee of GPU execution: the current shared
FFT implementation can log a GPU failure/unavailability and execute FFTW on the
**worker**. Auto applies its existing backend selection policy. Neither hello
nor result reports the backend actually used. This is distinct from the client's
explicit opt-in fallback to computation on the receiver PC. Upload MB/s budget
and receiver-local fallback are client policies; they are not wire fields.

## Main FFT IQ Chunks

After ready, send each chunk and await its acknowledgment before the next request:

```json
{"type":"main_fft_iq","job":"fft-job-1","first":"0","gap":false}
```

Binary input is finite CF32LE, 1-131072 complex samples (8-1048576 bytes) per
message, always a multiple of 8 bytes. This main FFT chunk limit is smaller than
the enclosing frame's 2 MiB payload cap. There is no `sampleCount` field: the
payload length determines sample count. `first` is a decimal uint64 string
(1-20 digits, no sign), counting complex source samples, not floats, bytes or DDC
channel samples. Overflow of `first + chunkSamples` is rejected. These positions
are relative stream counters, not hardware-clock/UTC timestamps. `gap` defaults
to false; it or a mismatch with the previous `next` declares discontinuity.

The JSON-only acknowledgment is:

```json
{"type":"main_fft_ack","job":"fft-job-1","next":"131072","filled":131072}
```

`next` is the exclusive next sample counter as a decimal string; `filled` is a
numeric count of complex samples currently retained, capped at FFT length.

In `windows` mode, exactly one complete window must be assembled. Appending
beyond its length fails with `FFT window exceeded`. A discontinuity after any
samples have been collected fails with `IQ gap inside FFT window`; a gap on the
first chunk is allowed. After a run, the window remains filled, so use a new
`main_fft_begin` to upload the next independent window.

In `continuous` mode, IQ accumulates in a fixed-length ring and only the newest
FFT-length samples are retained. A discontinuity resets history before admitting
the new chunk. A chunk larger than the FFT window retains its tail, while `next`
advances across the entire chunk. The client must refill a full window after a
reset before running. The server does not schedule FFTs automatically or impose
a refresh interval; the client explicitly requests every run.

## Main FFT Results

Run is JSON-only and must name the configured job:

```json
{"type":"main_fft_run","job":"fft-job-1"}
```

An incomplete window fails with `Main FFT needs a complete IQ window`. Otherwise
the ring is put in chronological order and computed with the main FFT kernel.
The response consists of a header, one or more bin chunks, and a completion marker:

```json
{"type":"main_fft_result","job":"fft-job-1","length":1048576,"next":"1048576","dspMs":12.3}
{"type":"main_fft_bins","job":"fft-job-1","offset":0}
{"type":"main_fft_bins","job":"fft-job-1","offset":262144}
{"type":"main_fft_bins","job":"fft-job-1","offset":524288}
{"type":"main_fft_bins","job":"fft-job-1","offset":786432}
{"type":"main_fft_done","job":"fft-job-1"}
```

Only each bins message has binary data: at most 262144 float32 LE levels
(1048576 bytes). `offset` is a zero-based **float/bin index**, not a byte offset.
Chunks are contiguous, and together contain exactly `length` bins. Header/done
payloads are empty. `next` echoes the exclusive end of retained source history;
the window covers `[next-length, next)`. `dspMs` measures `computeSnapshot`,
including any planning/backend selection performed there, excluding ring
linearization and network output. There is no returned frequency array, RBW,
epoch, generation, actual-backend field, or DDC-style `spectra` array.

Main FFT levels are **unshifted FFT output order**, with DC at index 0, unlike
the DDC reply's ascending-frequency bins. For an even length N, shift by N/2
before displaying against `center + (i-N/2)*rate/N`. They use the main kernel's
window-sum amplitude normalization and **-160 dBFS floor**, not the DDC -200 dBFS
floor. Main frame processing/FFT shift remains the receiver application's job.
All chunks belong to the same job; validate job, counters, advertised length,
offsets, payload sizes and finite levels, and only publish after a matching done.
Connection loss or a malformed/partial response must not publish a partial frame.

Run does not consume or clear the window: repeated runs without more IQ can
return the same window. In continuous mode, subsequent chunks advance the ring.
In windows mode, begin is required to replace its full input window. There is no
separate end/release command; disconnect releases the main FFT lease.

Native main FFT errors use the same JSON `error` envelope and terminate the
session. Besides the parameter, busy, gap and window errors above, current
reasons include `Stale or missing main FFT job`, `Invalid main FFT IQ chunk`,
`Unknown main FFT command`, `Main FFT computation failed`, and
`Main FFT resource limit; reduce FFT length or worker memory budget`. Unknown
`main_fft_*` requests also go through session validation and close on failure.
The service never retries a failed request by changing length, rate or mode.

## GNU Radio Adapter

The external Python adapter implements the same endpoint, optionally processing
IQ through a locally selected GNU Radio block before FFT/return. Version 1 allows
one-in/one-out complex streams at the unchanged sample rate only. A local plugin
provides `make_block(sample_rate)` returning a GNU Radio block/hier_block2.
The block and scheduler persist across contiguous packets. Unsupported rate
changes, unbounded output, or missing output fail explicitly. This is not an
arbitrary graph scheduler inside Obrii and does not load plugins from the network.
Its DDC v1 support does not imply `mainFftVersion: 1` or any `main_fft_*` support.
