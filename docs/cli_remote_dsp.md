# CLI Remote DSP And Auxiliary Recording

## Execution Contract

`CliDspGraph` uses the existing `DspBranchEngine` and the shared
`src/core/dspstreammanager.cpp`. The old CLI-only stream manager is retired.
The GUI engine, transport, settings and main audio path are unchanged.

Supported execution is receiver/IQ source -> optional local PFB -> local DDC ->
branch processing. A DDC's `settings.externalProcessing` selects the shared
external worker for **post-DDC channel IQ**, branch FFT and returned IQ.
Demodulation, audio filters and PCM output remain on the receiver host, using
the returned IQ for an external branch. This is not remote hardware control,
remote wideband DDC, remote demodulation, arbitrary graph execution or main FFT
offload. The compute service implements main FFT protocol for existing clients,
but this CLI graph API does not submit main FFT jobs.

`network_input` and `playback` graph source blocks are rejected at start. A file
or virtual source already supplying the CLI receiver's normal IQ path is a
different matter: use an `iq_source` block for that path. Graphs with no enabled
DDC branch cannot start. Display bindings and descriptive legacy wires do not
become executable CLI operations. Branch FFT requires its connected branch
spectrum/waterfall sink, as in the shared compiler.

Primary IQ is snapshotted without popping the main audio queue. Auxiliary IQ
uses `DspIqSourceRegistry`, keyed by the DDC settings `receiverId`/`channelId`;
the existing Multi-SDR lookup must resolve that identity. Source capture and
PFB/DDC stay local. Network waits occur on the shared remote worker, not in
`publishIq()`, the local branch worker, or main audio processing. Pending work
is bounded to two blocks per external branch and `2 * DspMaxBranches` overall;
overflow drops old work and marks gaps. There is no local fallback for a failed
external route. A slow external route can delay other external routes sharing
that worker, but not local branch processing.

## Command API

Call `CliDspGraph::command(request, centerHz, sampleRateHz, receiverRunning)` on
its owning Qt thread, with a running `QCoreApplication` event loop. Poll the
graph to drain PCM and frames. `control graph request FILE.json` loads one of
the JSON requests below on the receiver host. `ObriiCli compute endpoint.json`
starts a standalone hardware-free service; its file contains address/port/token.
The existing receiver-host local control socket dispatches requests
whose `action` is `graph` to this API (newline-delimited JSON over loopback TCP,
default control port 17675). Do not send these to the network-client
mode, which does not run a graph.

Configure the receiver host's external endpoint before starting enabled
external branches:

```json
{"action":"graph","operation":"remote","remoteOperation":"configure","address":"127.0.0.1","port":7358,"token":"REPLACE_WITH_A_RANDOM_32_TO_128_CHAR_SECRET"}
{"action":"graph","operation":"remote","remoteOperation":"status"}
{"action":"graph","operation":"start"}
{"action":"graph","operation":"remote","remoteOperation":"disable"}
```

Every configure request requires all three fields: numeric unicast IPv4/IPv6
address, integer port 1..65535, and a 32..128 character printable non-space
ASCII token. Hostnames, wildcard destinations, multicast and broadcast are
rejected. Invalid changes leave the current configuration intact. Configure
enables the endpoint; disable clears the credential and cancels external work.
An explicitly configured but unavailable server does not prevent graph start:
subsequent branch errors report refusal, authentication failure or timeout.
`running` means armed, not successful remote connectivity.

Endpoint/token changes while running call `setExternalEndpoint()` only. They
invalidate external results and external PCM, without clearing source IQ or
reconfiguring local branches. Affected PCM clients must reconnect after a
reset. To change a DDC between local and external processing, stop the graph,
then use the existing setter API:

```json
{"action":"graph","operation":"set","id":"ddc1","field":"externalProcessing","value":true}
```

The endpoint is in memory, separate from the graph definition and GUI settings.
It survives graph stop/load, but not process exit. Status exposes only
`enabled`, `address`, `port`, and `tokenConfigured`. Commands, graph snapshots,
graph saves and transport diagnostics do not print the configured token.
Do not put credentials in graph block settings, presets, shell arguments or
logged request bodies. The transport authenticates with a token but is plain
TCP, not TLS; use loopback or a trusted private/tunneled connection and restrict
listener access. Shared IQ format negotiation is unchanged (`Auto`).

### Compute Service Helper

The same class can explicitly host the existing hardware-free compute service:

```json
{"action":"graph","operation":"compute","computeOperation":"start","address":"127.0.0.1","port":7358,"token":"REPLACE_WITH_THE_SAME_SECRET_AS_THE_CLIENT"}
{"action":"graph","operation":"compute","computeOperation":"status"}
{"action":"graph","operation":"compute","computeOperation":"stop"}
```

No graph or receiver configuration is required for this helper. A caller can
own a `CliDspGraph` with an event loop solely to dispatch compute commands;
the integrated standalone command is `ObriiCli compute endpoint.json`.
The existing receiver-host socket also accepts these requests. Address and
token validation are as above, except an explicit wildcard bind is allowed
and port 0 requests an ephemeral listener. Default main FFT memory budget is
the shared service's 512 MiB. Start and stop acknowledge asynchronous requests;
check `graph.compute.port != 0` for a bound listener and `status` for bind
failure, and wait for `running == false` before restarting after stop.
`graph.stop()` stops branch processing/recorders, not the compute service;
explicit compute stop or destruction ends that independently owned service.
Final destruction joins its worker threads. A normal compute-stop command
uses `requestStop()` and does not join on the control thread.

Branch status includes `externalProcessing`, source identity and the latest
error/recording/audio statuses. `remoteExecution` reports worker creation,
pending block/byte counts and cumulative dropped blocks for the current engine
lifetime. `workerStarted` is not an authenticated-connection indicator.

## Auxiliary Recording

Both local and external auxiliary DDC branches can use the existing record API:

```json
{"action":"graph","operation":"record","recordOperation":"start","id":"aux-ddc","directory":"C:/captures"}
{"action":"graph","operation":"record","recordOperation":"status"}
{"action":"graph","operation":"record","recordOperation":"stop","id":"aux-ddc"}
```

The directory must already exist and be absolute. Shared engine inspection
confirmed that `runAuxiliary()` assigns `block.source`, selects the local
auxiliary recorder or queues external work, and `consumeOutput()` carries that
identity into the recorded block. `DspStreamRecorder` writes receiver/channel
identity, rate, center, generation, epoch, channel-relative sample positions,
gaps and finalized byte counts in each JSON sidecar next to CF32LE IQ.
The obsolete CLI-only auxiliary-recording prohibition was therefore removed.
For external branches the recording is on the receiver host and requires
successful returned IQ; an unavailable server cannot create valid recorded
samples. Disk errors and segment/byte limits remain the shared recorder's
responsibility. Record start/stop is an intent acknowledgement; inspect
`recordingStatus` and sidecars for the actual outcome. The existing shared
`setRecording()` advances the graph generation and resets branch audio; this
change does not promise seamless local PCM across recording operations.

## Exact Build Integration

No CMake or `obriicli.cpp` files are changed by this work. Apply these source
changes to **both** the root and standalone `src/cli/CMakeLists.txt` `ObriiCli`
targets before building the updated CLI:

1. Remove `src/cli/clilocaldspstreammanager.cpp` (standalone spelling:
   `clilocaldspstreammanager.cpp`). It is now an empty migration placeholder.
2. Add the following sources. In the standalone build prefix every path with
   `${OBRII_ROOT}/`; in the root build these are source-root relative.
3. Add `Qt5::Concurrent` to `ObriiCli`'s link libraries. Both current projects
   already find that Qt component. Keep existing Core/Network/FFTW, C++17,
   AUTOMOC, core/audio include directories, and platform audio/thread links.

```cmake
src/core/dspstreammanager.cpp
src/core/dspconnectionprobe.cpp
src/core/spectrumcomputesession.cpp
src/core/fft.cpp
src/core/fft.h
src/core/gpufftbackend.cpp
tools/fft_test_globals.cpp
```

`fft.h` must be seen by AUTOMOC for `FFTResult`. The existing
`tools/fft_test_globals.cpp` supplies the six legacy main-FFT global symbols
referenced by `fft.cpp`; despite its historical name it is hardware-free.
Use it once in CLI/test executables only. Do not add it to the GUI executable,
which already defines those symbols in `src/main.cpp`. No shared engine or GUI
target changes are needed. On Linux retain `Threads::Threads`, and on Windows
retain `winmm`. Do not link the old local manager definitions as an alternative
to the shared manager.

For the standalone source archive, the allowlist in
`tools/package_cli_source.ps1` additionally needs these files (besides the CLI
graph files it already includes). This packaging script is also left untouched:

```text
src/core/dspstreammanager.cpp
src/core/dspstreamtransport_p.h
src/core/dspconnectionprobe.cpp
src/core/dspconnectionprobe.h
src/core/spectrumcomputesession.cpp
src/core/spectrumcomputesession.h
src/core/fft.cpp
src/core/fft.h
src/core/gpufftbackend.cpp
src/core/gpufftbackend.h
tools/fft_test_globals.cpp
docs/cli_remote_dsp.md
tools/cli_remote_dsp_selftest.cpp
```

### Self-Test Target

This complete root-project target recipe uses existing Qt/FFTW discovery and
`CMAKE_AUTOMOC`. For standalone use, prefix every source/include path with
`${OBRII_ROOT}/`. It does not need receiver libraries, GUI widgets or a device:

```cmake
add_executable(ObriiCliRemoteDspSelfTest
    tools/cli_remote_dsp_selftest.cpp tools/fft_test_globals.cpp
    src/cli/clidspgraph.cpp src/cli/clidspgraph.h
    src/core/dspgraphmodel.cpp src/core/dspbranchengine.cpp
    src/core/dspstreammanager.cpp src/core/dspstreamprocessor.cpp
    src/core/dspstreamrecorder.cpp src/core/dspiqcodec.cpp
    src/core/dspsourceidentity.cpp src/core/dspiqsourceregistry.cpp
    src/core/computebudgetmanager.cpp src/core/pfbchannelizer.cpp
    src/core/fftplannermutex.cpp src/core/dspconnectionprobe.cpp
    src/core/spectrumcomputesession.cpp src/core/fft.cpp src/core/fft.h
    src/core/gpufftbackend.cpp src/core/iqbuffer.cpp
    src/core/iqchannelizer.cpp src/core/channelizerutils.cpp
    src/audio/audioprocessor.cpp src/audio/audioprocessor.h
    src/audio/audiofilterchain.cpp)
target_include_directories(ObriiCliRemoteDspSelfTest PRIVATE
    src/cli src/core src/audio ${FFTW_INCLUDE_DIR})
target_link_libraries(ObriiCliRemoteDspSelfTest PRIVATE
    Qt5::Core Qt5::Network Qt5::Concurrent ${FFTW_FLOAT_LIBRARY})
if(WIN32)
    target_link_libraries(ObriiCliRemoteDspSelfTest PRIVATE winmm)
else()
    target_link_libraries(ObriiCliRemoteDspSelfTest PRIVATE Threads::Threads)
endif()
if(MSVC)
    target_compile_options(ObriiCliRemoteDspSelfTest PRIVATE /utf-8 /DNOMINMAX)
endif()
```

Run `ObriiCliRemoteDspSelfTest` with Qt and FFTW runtime libraries available.
The self-test starts the real shared localhost compute service, checks branch
FFT and PCM, token validation/non-disclosure (including verbose diagnostics),
authentication failure and recovery, absent-server behavior without fallback,
local PCM socket continuity, main audio IQ queue preservation, bounded remote
queue dropping, cancellation during a stalled handshake, async bind failure,
and finalized auxiliary recording identity for local and external routes.
It uses synthetic IQ and temporary recording directories; it is not a test of
real SDR hardware, WAN performance, arbitrary graphs or remote main FFT.

Verified locally with MSVC x64, Qt 5.15.2 and FFTW: six consecutive successful
runs after the final test changes. The focused executable was compiled directly
from the source list above, generating moc output for `fft.h` and
`audioprocessor.h`; no CMake file was edited to run it. Full CLI builds still
require the explicit integration above. Linux/hardware testing was not run.
