# GNU Radio DSP Bridge

`tools/obrii_gr_bridge.py` implements [DSP Stream Protocol v1](dsp_stream_protocol.md).
It is an external, optional TCP endpoint, not the older network-IQ/ZeroMQ proposal.
It never opens a radio, controls RF, reads user settings, launches Obrii, or replaces
the receiver/audio queue. The DSP-board client is available through the stream
manager; Windows deployment includes this script as `dsp/obrii_gr_bridge.py`.

## Runtime Check

The inspected Windows bundle is `build/runtime/wifi-phy/python.exe`, with Python
3.12.15, NumPy 2.5.4 and GNU Radio 3.10.12.0. The adapter registers that interpreter's
`Library/bin` and `DLLs` directories before importing native modules; no activation
script or global PATH change is needed. Other Python installations need NumPy;
the default engine additionally needs GNU Radio's `gr` and `blocks` modules.

From the repository root, run these software-only checks:

```powershell
& .\build\runtime\wifi-phy\python.exe -I -B .\tools\obrii_gr_bridge.py --check
& .\build\runtime\wifi-phy\python.exe -I -B .\tools\obrii_gr_bridge.py --check --engine numpy
& .\build\runtime\wifi-phy\python.exe -I -B .\tools\obrii_gr_bridge_selftest.py
& .\build\runtime\wifi-phy\python.exe -I -B .\tools\obrii_gr_bridge_selftest.py --gnu
```

`--check` opens no listener and checks two contiguous synthetic blocks plus a
known FFT. The default GNU block is `multiply_const_cc(1.0)`, an identity. The
NumPy engine is an explicit software-only identity reference, not a silent GNU
fallback. Without `--gnu`, GNU integration cases are reported as skipped.

`tools/gnu_radio_protocol_selftest.cpp` is a standalone QtCore/FFTW codec
regression (build it with `src/core/dspstreamprocessor.cpp` and
`src/core/fftplannermutex.cpp`, using the native self-test's include/library paths).
It covers legacy defaults, eight streams, rational metadata, empty output,
64-bit counters, malformed output and processed FFT axes. No CMake target is
added by this phase. After compiling it, supply its executable to
`obrii_gr_bridge_selftest.py --gnu --cpp-codec <executable>` to check actual GNU
request/result frames with the C++ decoder. Qt and FFTW DLLs must be discoverable
on that executable's runtime PATH. These are synthetic tests, not RF throughput
or audio-quality qualification.

## Local Server

Generate a fresh token for every server launch, keep it out of command lines,
logs, and saved layouts, and pass the same token securely to the client. Tokens
must have at least 32 ASCII characters. This example assigns the generated value
without printing it:

```powershell
$env:OBRII_DSP_TOKEN = & .\build\runtime\wifi-phy\python.exe -I -B -c "import secrets; print(secrets.token_hex(32))"
& .\build\runtime\wifi-phy\python.exe -I -B .\tools\obrii_gr_bridge.py --bind 127.0.0.1 --port 7356
Remove-Item Env:OBRII_DSP_TOKEN
```

The token is accepted only through `OBRII_DSP_TOKEN`; there is no `--token` option.
The process does not log it. `--bind` defaults to `127.0.0.1`, accepts numeric IPv4
or IPv6 addresses, and can also be supplied via `OBRII_DSP_BIND`. The port must be
explicit (`--port 1..65535` or `OBRII_DSP_PORT`); there is no default service port.
Use `::1` for IPv6 loopback. Stop the server with Ctrl+C.

TCP is unencrypted. Binding beyond loopback is an explicit operator choice; use
only a trusted LAN or independently secured tunnel. Authentication does not
provide transport confidentiality. A single connection reserves the client slot
through authentication and cleanup. A newly accepted connection waits at most
0.5 seconds for previous-session cleanup, allowing immediate reconnects; if the
slot is still busy, it is closed without authentication. No extra client worker
or queue is created.

`--timeout` is the absolute per-frame read deadline and total send deadline
(default 5 seconds, allowed 0.05..30). Fragmentation never extends a read deadline.
Idle, partial, slow, and disconnected clients release their session. An invalid
hello disconnects silently; authenticated protocol errors receive a fixed,
non-sensitive error when possible, then disconnect. No request queue is retained.

## Local GNU Plugin

Select a trusted Python file locally with `--plugin C:\path\to\dsp_plugin.py`.
The TCP protocol cannot specify a plugin path, import a module, or execute code.
The file must export:

```python
from gnuradio import filter

def make_block(sample_rate):
    return filter.fir_filter_ccc(1, [0.5, 0.25, 0.125])
```

Returning just a GNU `block` or `hier_block2` preserves the legacy contract: one
CF32 input/output, unchanged rate, exactly one output per input. For a bounded
rational rate change, return `(block, interpolation, decimation)` instead:

```python
from gnuradio import filter

def make_block(sample_rate):
    return filter.rational_resampler_ccf(2, 3), 2, 3
```

The ratio must be reduced, both factors 1..64, output/input at most 2, and output
rate 1000..2000000 samples/s. The direct block's declared relative rate must agree.
GNU's polyphase FIR resampler supplies anti-alias/interpolation filtering;
the adapter does not implement resampling math. `tools/obrii_gr_resampler.py` is
a runnable local example. No hardware, network/file sources/sinks or radio-control
messages belong in a plugin. Plugins are selected by local CLI, never by a peer.

Rational mode retains a bounded input history/lookahead reserve R. By default
`R = (ceil((H-1)/M)+1)*M`, where M is decimation and H is the block's history.
GNU rational resamplers install taps lazily, so H also includes
`ceil(len(taps)/interpolation)`. A hierarchy must explicitly return
`(block, L, M, R)`; R must be a multiple of M, between M and 4096. A direct block
can also declare R explicitly. It must cover all internal forecast/history needs;
an insufficient reserve fails by deadline, never by fabricating padding.

After T contiguous input samples since reset, exactly
`L * floor(max(0, T-R)/M)` samples have been returned. Individual requests can
return **zero** samples; later requests can return up to 131072. Whole
rate quanta make accounting independent of network packet sizes. Direct rational
blocks are scheduled with output multiples divisible by L (maximum 4096).
Hierarchies must arrange compatible internal work multiples themselves. Rate,
taps/history requirements and buffering behavior must remain fixed during a
segment. This is a bounded constant-ratio contract, not arbitrary scheduling.

Each active stream owns a persistent worker process, scheduler, streaming source
and sink. Contiguous packets use the same block and scheduler; a new vector source
and `run()` are not created per packet. Each stream has fixed shared arrays for
65536 input and 131072 output complex samples, plus a 139392-sample output ring.
Scheduler work calls are capped at 4096 output items and exposed output buffers
at 8192 items. The source waits for input and never fabricates samples. The sink
rejects non-finite samples, output exceeding admitted input's rational budget, or
ring overflow. No transaction queue grows while a plugin is stalled.

`--processing-timeout` bounds GNU startup and each transaction (default 5 seconds,
allowed 0.05..30). Missing output, bad signatures, invalid rate contracts, worker
failure, or output-count failure closes the session. Hung workers are terminated;
shutdown first allows a short graceful stop. Server cancellation is checked at
most every 50 ms while waiting for startup/output; teardown escalates from a
250 ms graceful join to terminate/kill joins of 500 ms each. Internal plugin allocations and side
effects are not sandboxed: plugins are trusted local code, and must bound internal
buffers and avoid hardware/network access. Worker isolation supplies a killable
timeout boundary, not a security sandbox for arbitrary Python or native code.

Reset/recreate occurs on generation, epoch, sample rate, center frequency, FFT
configuration (including order, IDs and refresh), explicit `gap`, or
noncontiguous `firstSample`. A plugin is reconstructed with the new rate; failure
to support it is an explicit processing error. Session disconnect destroys all
stream state, including unreturned lookahead/tail: no end-of-stream flush exists.
At most eight stream IDs may exist per connection; a ninth is rejected,
not silently evicted. Reconnect to retire IDs because v1 defines no close-stream
request. Changing only `returnIq` leaves filter and FFT history intact.

## GRC Adapter Integration

There is no installed Obrii OOT source/sink block or generic GRC network endpoint.
The runnable integration is a locally exported **hierarchical** CF32 flowgraph
hosted between the bridge's bounded source and sink. `tools/obrii_gr_hier_resampler.py`
demonstrates the `gr.hier_block2` pattern with GNU rational resampling and gain:

```powershell
& .\build\runtime\wifi-phy\python.exe -I -B .\tools\obrii_gr_bridge.py --check --plugin .\tools\obrii_gr_hier_resampler.py
# With OBRII_DSP_TOKEN already set as above:
& .\build\runtime\wifi-phy\python.exe -I -B .\tools\obrii_gr_bridge.py --port 7356 --plugin .\tools\obrii_gr_hier_resampler.py
```

For a GRC design, export Python with Generate Options = Hier Block, exactly one
complex input Pad Source and one complex output Pad Sink, and a `sample_rate`
parameter. Do not include hardware sources, Throttle, GUI sinks, or an independently
running top_block. Put generated code beside a trusted bridge wrapper; load it
by absolute local path (this also works with Python `-I`):

```python
import importlib.util
from pathlib import Path

def make_block(sample_rate):
    path = Path(__file__).with_name("my_channel.py")
    spec = importlib.util.spec_from_file_location("my_channel", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    block = module.my_channel(sample_rate=sample_rate)
    return block, 2, 3, 192  # Example: declare your actual ratio and validated reserve.
```

Set internal resamplers' output multiples to their interpolation factors; the
runnable hierarchy example shows this. Validate the chosen reserve and output
accounting with synthetic irregular packets before using a generated design.
Tagged-stream lengths, PMT messages, tags, arbitrary multiport graphs and
variable-rate/demand-driven schedulers are not transported by this adapter.

## FFT and Wire Details

Both engines FFT the processed IQ. All frame limits, sample/metadata limits,
uint64 decimal strings, finite input/output requirements, and CF32LE/float32LE
formats follow the contract. Unknown requests/fields, duplicate JSON keys,
non-finite JSON values, duplicate FFT IDs and malformed payloads are rejected.
Reply offsets are byte offsets from the beginning of the entire binary payload,
including returned IQ. Top-level metadata still echoes the **input** request.
Rational replies add `output` with sampleRate, firstSample, sampleCount,
originFirstSample, rateNumerator, rateDenominator, inputReserve and gap. See the
[output metadata contract](dsp_stream_protocol.md#rational-output-v1).

The reference uses symmetric `N-1` windows, matching Obrii's existing FFT code:
rectangular; Hann `(0.5, -0.5)`; Hamming `(0.54, -0.46)`; Blackman-Harris
`(0.35875, -0.48829, 0.14128, -0.01168)`; flat-top
`(0.21557895, -0.41663158, 0.277263158, -0.083578947, 0.006947368)`.
These are cosine-series coefficients starting at harmonic zero. NumPy computes
double-precision windows/transforms and emits little-endian float32 levels.

Normalization is `20*log10(max(abs(FFT(window*x))/sum(window), 1e-10))`, with
`fftshift` for ascending frequency. RBW uses the actual finite-length window:
`Fs * sum(window**2) / sum(window)**2`, not a tabulated asymptotic ENBW. Only the
newest full FFT history is transformed; partial history emits no FFT. Each FFT's
monotonic refresh clock is independent and resets with stream state. No averaging,
one-sided doubling, zero padding or amplitude clipping is applied by the FFT.
FFT bandwidth/RBW use the **processed** sample rate. Small float32-versus-double
numerical differences are expected.

## Contract Clarifications for Integration

- V1 specifies the symmetric finite-N windows used by both endpoints.
- V1 has no stream-retirement message: replacing IDs after eight slots requires
  reconnecting. Changing a generation on an existing ID does not consume a slot.
- Config changes include FFT ordering but not `returnIq`.
  `firstSample + sampleCount` must fit uint64, matching the C++ decoder; positions
  never wrap uint64.
- A 1:1 plugin must produce a complete reply within one work request. Rational
  plugins obey the explicit reserve/quantum contract; generic buffering/tagged
  streams and variable-rate flowgraphs are not automatically adapted.
- `DspStreamResult.sampleRate/firstSample/sampleCount/gap` describe processed IQ.
  Native and older replies default to the input rate/counter; rational counters
  restart at zero per segment. `originFirstSample` is the input-domain anchor.
  `inputReserve` is decoder/accounting metadata; clients do not buffer it again.
  Recorder writers capped at 65536 must split output, advancing firstSample by
  the chunk offset. Audio consumers must validate their supported rate range.
  Empty replies advance input admission only; retain the gap until real output.
- Relative output time is `originFirstSample/inputRate + firstSample/outputRate`.
  This names a stream position, not UTC or an RF timestamp. It does not compensate
  plugin FIR group delay/startup alignment, which depends on the selected graph.
- Tokens are caller-generated per session. The endpoint validates format and
  compares in constant time; it cannot verify randomness or enforce uniqueness
  across independent launches. It provides no loss-free RF guarantee.
- The C++ client currently allows 1500 ms per work reply. Cold GNU process/plugin
  startup and state recreation can exceed that on slower machines; its timeout
  should accommodate startup plus processing, or deliberately report a failed
  external stream. No silent NumPy fallback is performed. C++ token validation
  requires 32..128 printable, non-space ASCII characters; the example's 64 hex
  characters satisfy both endpoints.

## Import-Based Integration Tests

Importing `obrii_gr_bridge` does not start a listener or import GNU Radio. The
public test entry points are `BridgeServer`, `ProcessorFactory`, `DspSession`,
`encode_frame`, `receive_frame`, and `send_frame`. `BridgeServer` binds immediately,
so `server.address[1]` provides an ephemeral port before its serving thread starts.
Port zero is allowed only through this Python API, not the production CLI.

After putting the repository's `tools` directory on Python's module search path:

```python
import threading
import obrii_gr_bridge as bridge

def run_test(token, run_cpp_client):
    server = bridge.BridgeServer(
        "127.0.0.1", 0, token,
        factory=bridge.ProcessorFactory("gnuradio", timeout=10.0),
        timeout=10.0,
    )
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        run_cpp_client(server.address[0], server.address[1], token)
    finally:
        server.close()
        thread.join()
```

Use an `if __name__ == "__main__":` guard around the caller when spawning GNU
workers from a script, especially on Windows. Pass the token to the C++ process
through its environment, not argv. Select `ProcessorFactory("numpy")` explicitly
for protocol-only testing without GNU. `DspSession(factory, clock=...)` also
accepts a test monotonic clock for deterministic refresh/history assertions.
