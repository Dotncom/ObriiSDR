# Optional CLI Browser / Multi-User Service

`src/cli/cliwebservice.{h,cpp}` wraps the production `RemoteVfoService` and
`RemoteWebServer` used by the GUI. It runs with `QCoreApplication`; it does not
construct widgets, open receiver/audio devices, or enable itself at startup.
Use `run --web-port 8080 --web-bind 127.0.0.1`, or `control web start IP PORT`,
`control web status` and `control web stop`. The integration details below
describe the shared implementation; native peer hooks remain optional.

## Lifetime and Commands

All methods, including IQ submission and destruction, must run on the object's
owning Qt event-loop thread. Keep the event loop serviced, as the CLI acquisition
loop already does with `QCoreApplication::processEvents()`. Do not call these
methods directly from backend acquisition callbacks. Use the CLI's existing
bounded acquisition queue and submit from its consumer.

```cpp
#include "cliwebservice.h"

CliWebService web(&app); // inert: no gateway, broker, timers, or worker threads
web.setSourceState(currentRadioSettings, receiverRunning);
const bool okay = web.start(QStringLiteral("127.0.0.1"), 8080);
const quint16 actualPort = web.port(); // port 0 requests an ephemeral port
const QJsonObject snapshot = web.status();
web.stop(); // idempotent; destroys gateway, sessions, broker and DSP workers
```

Create it beside the CLI graph/bridge, not in an IQ callback. Cache/update actual
`RadioSettings` and `running` on receiver start, stop, EOF, reconnect, retune,
input-mode changes and sample-rate changes. State set before `start()` is retained.
`stop()` never stops hardware or clears another consumer's IQ history. Repeating
`start()` with the same numeric address and requested/actual port preserves
sessions. Changing the endpoint stops the old service first; a bind failure
leaves it disabled. Invalid address/command input leaves an existing listener
intact. Only numeric IPv4/IPv6 addresses are accepted; blank means loopback, not a
wildcard. `0.0.0.0` and `::` explicitly bind all appropriate interfaces.

For an opt-in local CLI dispatcher, pass these objects to `web.command(request)`:

```json
{"action":"start","address":"127.0.0.1","port":8080,"requestId":"web-start"}
{"action":"status"}
{"action":"stop"}
```

`start` requires an integer port in `0..65535`. Replies include `ok`, `action`,
optional `requestId`/`error`, and status fields `enabled`, `listening`, `address`,
`port`, `status`, `needsIq`, plus the broker's session/VFO/frame counters.
Do not dispatch arbitrary network commands to this local lifecycle API.
There is no hardware-control callback: browser actions exclusively control
listener VFOs. Physical tuning, acquisition and authorization remain in main.

## IQ Hooks and Shared Ownership

Place this after tune-settling samples have been discarded, at the existing
`graph.publishIq(published, publishedCount)` site in `obriicli.cpp`:

```cpp
const bool graphPublishes = graph.running();
if (graphPublishes) {
    graph.updateSource(actualCenter, actualRate);
    graph.publishIq(published, publishedCount);
}
web.submitIq(published, publishedCount, graphPublishes);
```

`publishedCount` counts **complex samples**, so the input has twice as many floats
in raw interleaved I/Q order. Do not pass channelized/demodulated audio IQ here.
Call `setSourceState()` with actual hardware center/rate before these hooks when
state changes. `submitIq()` publishes only while enabled, receiver-running, and
at least one remote VFO is active. It returns false for skipped/invalid blocks.
It uses `IqBuffer::publish(..., queueBlock=false, updateSnapshot=true)` with valid
center-frequency metadata; it does not populate the main audio queue. The
existing IqBuffer publisher's queue behavior is unchanged. Keep exactly one
primary raw-IQ publisher, with this wrapper only filling in when the graph does
not publish. Pass `alreadyPublished=true` only when **this block** was already
published to the same shared destination, not merely because a graph exists.

For auxiliary receivers/channels:

```cpp
web.submitSourceIq(sourceIdentity, rawIq, complexCount, actualRate, actualCenter,
                   alreadyPublishedToRegistry, discontinuity, timingMetadata);
```

This rejects the primary `main/rx0` identity and uses the bounded
`DspIqSourceRegistry`; only subscribed auxiliary sources are copied by that
registry. The method also gates on enabled/running/active remote VFO state, and
forwards optional `DspIqTimingMetadata` without logging it. Existing multi-SDR
registry publishers should continue unchanged; pass true (or omit this extra
call) when they already published. An inactive service does not touch either
buffer, even if other DSP consumers have registry subscriptions. Submission has
no private queue/copy and rejects blocks larger than
`IqBuffer::MaxSnapshotComplexSamples` (8,388,608 complex samples).

The acquisition/graph owner must reset shared IQ history at discontinuities,
retunes and restarts, including when the graph is stopped but web VFOs remain
active. Stop/pause the producer, call `IqBuffer::clear(newEpoch)`, update source
state, then resume. `submitIq()` optionally accepts `expectedEpoch` as its fourth
argument to reject stale blocks. The wrapper deliberately does not independently
clear shared state on start/stop/source updates, which could disrupt the graph.
Do not concurrently publish unrelated sources into the primary buffer.

## Optional Native Peer Hooks

To share the broker with existing native multi-user clients without changing
`CliNetworkBridge`, connect at the `NetworkController` owner. The bridge's private
controller is not exposed; these are owner-level hooks, not an assertion that
native routing is already installed in the current CLI.

```cpp
QObject::connect(&network, &NetworkController::controlCommandReceived, &web,
    [&](const QJsonObject &request) {
        if (request.value("type").toString() != "remoteVfo") return;
        const QString peer = request.value("_networkPeerId").toString();
        // Use the ID supplied by NetworkController, never an arbitrary client ID.
        if (peer.isEmpty()) return;
        network.setPeerStreamProfile(peer, QStringLiteral("remote-vfo"));
        web.handleRemoteCommand(peer, request);
    });
QObject::connect(&network, &NetworkController::peerDisconnected, &web,
    [&](const QString &peer, const QString &) { web.closeRemoteEndpoint(peer); });
QObject::connect(&web, &CliWebService::jsonFrameReady, &network,
    [&](const QString &peer, const QJsonObject &frame) {
        network.sendControlCommandToPeer(peer, frame);
    });
QObject::connect(&web, &CliWebService::binaryFrameReady, &network,
    [&](const QString &peer, const QJsonObject &frame, const QByteArray &payload) {
        network.sendBinaryCommandToPeer(peer, frame, payload);
    });
```

Native IDs are internally prefixed to avoid collisions with browser `web-N`
owners. Only native frames are emitted through these signals. All non-VFO
control commands remain with main/its existing authorization. Start the wrapper
explicitly before accepting native VFO commands; the current start hook enables
the browser gateway too, not a separate native-only listener.

## Existing Browser Contract

These are the GUI gateway's actual routes, not a second HTTP implementation:

- `GET /` and `GET /index.html`: identical embedded HTML/CSS/JavaScript, spectrum,
  frequency scale, capture-span tuner, waterfall and browser audio. No asset
  directory, Qt resources, CDN or separate JS/CSS routes are needed.
- `GET /api/status`: production broker JSON. Use HTTP 200 plus parseable JSON as
  a liveness check; `receiverRunning`/`activeVfos` describe receiver readiness.
  It does not include the wrapper's listener/lifecycle fields.
- `GET /ws`: WebSocket upgrade, isolated listener session. There is no `/health`
  route (404), HTTP write/control API, or wrapper-specific browser protocol.

The gateway handles masked JSON VFO actions `open`, `list`, `status`, `create`,
`update`, `activate`, `deactivate`, `delete`, and injects `type=remoteVfo`.
Spectrum is JSON; WebSocket binary messages are raw 48 kHz mono signed-16-bit PCM
without the native transport's JSON header. Existing quotas, supported AM/NFM/
SAM/USB/LSB/DSB/CW/WFM modes and captured-span checks come from `RemoteVfoService`.
The remote broker is local-compute in this wrapper; external DSP endpoint
configuration is not added by this change.

## Exact Build Dependencies

No CMake files were edited. Use C++17, `CMAKE_AUTOMOC=ON`, Qt5 **Core, Network,
Concurrent**, the float FFTW3 header/library (`fftw3.h`, `fftw3f` or
`libfftw3f-3`), and include directories `src/cli`, `src/core`, `src/network`,
`src/audio`, and the FFTW include directory. No Qt Widgets, Gui, WebSockets,
WebEngine, receiver driver, TLS library, or browser-asset package is needed for
the standalone self-test. Windows also links `winmm`; other platforms link
`Threads::Threads`. Qt Multimedia is optional for other local-audio uses and
is not required for remote PCM delivery. Do not define `FOBOSAPP_HAS_QT_AUDIO`
unless also finding/linking Qt5 Multimedia. Optional VkFFT/FFTW-thread macros
retain their own existing dependencies and are not enabled here.

For optional build-time inclusion, the recipe below uses an `OBRII_CLI_WEB`
CMake option defaulting to `OFF`. Guard main-level hooks with `OBRII_CLI_WEB`.
Runtime opt-in still requires an explicit `start()`; compiling support alone
does not enable networking or IQ work.

Add this source group to `ObriiCli` (paths below are repository-relative):

```cmake
set(OBRII_WEB_ADDITIONS
    src/cli/cliwebservice.cpp src/cli/cliwebservice.h
    src/network/remotevfoservice.cpp src/network/remotevfoservice.h
    src/network/remotewebserver.cpp src/network/remotewebserver.h
    src/audio/dspbranchaudiooutput.cpp
    src/core/dspstreammanager.cpp src/core/dspconnectionprobe.cpp
    src/core/spectrumcomputesession.cpp
    src/core/fft.cpp src/core/fft.h src/core/gpufftbackend.cpp
    tools/fft_test_globals.cpp)
```

The current standalone CLI already has the following dependency sources. Keep
exactly one copy. The standalone self-test needs this entire group as well:

```cmake
set(OBRII_WEB_SHARED
    src/core/iqbuffer.cpp src/core/dspgraphmodel.cpp src/core/dspbranchengine.cpp
    src/core/dspstreamprocessor.cpp src/core/dspiqcodec.cpp
    src/core/dspstreamrecorder.cpp src/core/dspsourceidentity.cpp
    src/core/dspiqsourceregistry.cpp src/core/computebudgetmanager.cpp
    src/core/iqchannelizer.cpp src/core/channelizerutils.cpp
    src/core/pfbchannelizer.cpp src/core/fftplannermutex.cpp
    src/audio/audioprocessor.cpp src/audio/audiofilterchain.cpp
    src/audio/remoteaudioplayer.cpp)
```

Resolve these paths against `${OBRII_ROOT}` when used from `src/cli/CMakeLists.txt`:

```cmake
list(TRANSFORM OBRII_WEB_ADDITIONS PREPEND "${OBRII_ROOT}/")
list(TRANSFORM OBRII_WEB_SHARED PREPEND "${OBRII_ROOT}/")
find_package(Qt5 REQUIRED COMPONENTS Core Network Concurrent)
option(OBRII_CLI_WEB "Build optional CLI browser/multi-user support" OFF)
if(OBRII_CLI_WEB)
    target_sources(ObriiCli PRIVATE ${OBRII_WEB_ADDITIONS})
    target_include_directories(ObriiCli PRIVATE "${OBRII_ROOT}/src/cli")
    target_link_libraries(ObriiCli PRIVATE Qt5::Concurrent)
    target_compile_definitions(ObriiCli PRIVATE OBRII_CLI_WEB=1)
    if(WIN32)
        target_compile_definitions(ObriiCli PRIVATE NOMINMAX)
    endif()
endif()

add_executable(ObriiCliWebServiceSelfTest
    "${OBRII_ROOT}/tools/cli_web_service_selftest.cpp"
    ${OBRII_WEB_ADDITIONS} ${OBRII_WEB_SHARED})
target_include_directories(ObriiCliWebServiceSelfTest PRIVATE
    "${OBRII_ROOT}/src/cli" "${OBRII_ROOT}/src/core"
    "${OBRII_ROOT}/src/network" "${OBRII_ROOT}/src/audio" "${FFTW_INCLUDE_DIR}")
target_link_libraries(ObriiCliWebServiceSelfTest PRIVATE
    Qt5::Core Qt5::Network Qt5::Concurrent "${FFTW_FLOAT_LIBRARY}")
if(WIN32)
    target_compile_definitions(ObriiCliWebServiceSelfTest PRIVATE NOMINMAX)
    target_link_libraries(ObriiCliWebServiceSelfTest PRIVATE winmm)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(ObriiCliWebServiceSelfTest PRIVATE Threads::Threads)
endif()
if(MSVC)
    target_compile_options(ObriiCliWebServiceSelfTest PRIVATE /utf-8)
endif()
enable_testing()
add_test(NAME cli_web_service COMMAND ObriiCliWebServiceSelfTest)
```

`list(TRANSFORM)` requires CMake 3.12 or newer; on the standalone project's 3.10
minimum, prefix the paths with a `foreach()` instead. `tools/fft_test_globals.cpp`
supplies the legacy `FFTResult` global symbols missing from the standalone CLI;
omit it if the integrating executable already defines those symbols. Link the
real `src/core/dspstreammanager.cpp`, not replacement stubs; the current empty
`clilocaldspstreammanager.cpp` can remain but supplies no implementation. If a
parallel CLI remote-DSP integration already added any of this group, do not add
duplicate sources/global definitions. The existing CLI's hardware-driver and
other module dependencies are unchanged.

The self-test uses ephemeral loopback ports, synthetic IQ, `QCoreApplication`,
and production services, with no RF/audio device. It covers disabled/idle IQ
gating, invalid/busy binds, idempotent start/stop, browser assets and status,
WebSocket handshake/masked commands, two isolated VFO sessions, rejected tuning,
FFT and PCM delivery, per-session quota, auxiliary registry-to-browser delivery, duplicate suppression,
native peer-ID isolation, disconnect and active shutdown. It does not render a
real browser or measure a CPU benchmark; disabled idleness follows from having
no allocated service, timer or worker and no IQ publication.

Local verification: MSVC 2022, Qt 5.15.2 Core/Network/Concurrent and float FFTW3
were available; no required libraries were unavailable. The isolated headless
build and self-test passed without Qt Gui/Widgets/Multimedia/WebSockets. A second
isolated CMake build using the source groups and self-test recipe extracted from
this document passed CTest three consecutive times.
Linux and real-browser rendering were not tested.

## Exposure and Logging

This gateway is **LAN-oriented, not Internet-hardened**. It has no built-in
authentication, TLS, Origin enforcement, or comprehensive connection limits.
Binding a LAN/wildcard address does not enforce LAN-only reachability. Keep
loopback by default; use trusted network/firewall boundaries, or an authenticated
TLS reverse proxy/VPN for remote access. No Internet safety claim is made.
The wrapper never logs request bodies, settings, credentials or IQ; status only
reports listener/broker state. Do not add settings/credential dumps when wiring
CLI diagnostics. Browser/native session labels can still be returned by the
existing broker protocol; treat them as untrusted display data.
