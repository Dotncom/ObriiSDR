# Optional Headless GNSS Worker

`ObriiCliGnssModule` runs an already installed PocketSDR `pocket_trk` against
recorded IQ or explicitly supplied framed full-source live IQ. It uses
`QCoreApplication`, not a local UI or receiver driver.
It is also available through `ObriiCli module gnss ...`, which forwards arguments
to this optional standalone worker without starting the receiver or UI.
Nothing is downloaded, installed or launched automatically by ordinary RX.

## Run

For an already running CLI receiver, use `ObriiCli control gnss start FILE.json`
with `{"backend":"installed","systems":["GPS"]}`, followed by
`control gnss status|eof|stop|reset`. The local source must be running complex RF
IQ with scanning disabled. A CLI network client requires full-band mode (CI8,
not original float ADC precision); channel IQ and server audio are rejected.
The parent uses actual source rate/center and never tunes the source implicitly.
Capture begins at worker readiness. For a complete finite recording, prefer the
file invocation below; a receiver already replaying can have consumed its prefix
before a live start command. RX stop cancels; source changes/gaps invalidate
accepted live IQ. Worker readiness alone is not evidence of GNSS acquisition.

```powershell
.\ObriiCliGnssModule.exe --file 'D:\captures\gps.cs16' --format cs16 `
  --sample-rate 8000000 --center 1575420000 --systems GPS,QZSS `
  --backend 'D:\backends\pocketsdr\pocket_trk.exe'
```

```sh
./ObriiCliGnssModule --file /captures/gps.cf32 --format cf32 \
  --sample-rate 8000000 --center 1575420000 --systems GPS \
  --backend /opt/pocketsdr/pocket_trk
```

Required flags are `--file` (or `--live`), `--systems` and `--backend`. There is no default
constellation or implicit backend selection. `--backend installed` explicitly
opts into the same lookup order used by `GnssSdrPvtDialog`: first
`<worker-directory>/pocketsdr/pocket_trk[.exe]`, then `pocket_trk` on PATH if the
adjacent path does not exist. There is no shared exported resolver in that
dialog, so the worker repeats this small lookup with file/executable checks.
An explicit backend path never falls back to another installation. Supply the
backend's existing dependent libraries too; an executable with missing DLLs
still fails at process startup. See `docs/research_backends.md` and
`THIRD_PARTY_LICENSES.txt` for the existing backend version/license context.

`--systems` accepts comma-separated GPS, GLONASS, Galileo, BeiDou, QZSS and SBAS
(case-insensitive, deduplicated). Unknown/empty entries and signals outside the
recorded band fail before launching the backend. `configureTrackingRate` and
`trackingArguments` select the same power-of-two decimation, supported
2.046..32 MS/s tracking rate and signal/PRN ranges as the GUI adapter. This does
not synthesize missing bandwidth or retune hardware.

Formats are interleaved little-endian I,Q: `cf32` (default), signed `cs16`,
signed `cs8`, or stereo PCM16 IQ WAV. `readInputMetadata` reads the WAV data
extent and existing raw sidecars (`file.ext.json`, then `file.json`), including
`sampleRate`/`sampleRateHz` and `centerFrequency`/`centerFrequencyHz`. Raw
`--sample-rate` and `--center` override sidecars. WAV sample rate comes from its
header; a conflicting explicit rate or non-CS16 explicit format is rejected.
Rate and RF center must be finite and positive, from metadata or flags. Empty
recordings and partial complex samples fail. The underlying stream performs
conversion/filtering; it does not normalize recorded IQ like its live path.

## Output And Lifetime

Stdout is UTF-8 JSON Lines, one object per line. Every object has `module:gnss`
and an ISO UTC `utc`. Backend stdout/stderr go to the null device, never into
the JSON channel; wrapper errors also go to stderr. `--help` prints plain text.

```json
{"event":"status","module":"gnss","state":"running","utc":"2026-11-02T12:00:00.000Z"}
{"event":"nmea","module":"gnss","sentence":"$GNRMC,...","utc":"2026-11-02T12:00:01.000Z"}
{"event":"status","module":"gnss","state":"completed","samplesSent":"2000000","expectedSamples":"2000000","exitCode":0,"utc":"2026-11-02T12:00:02.000Z"}
```

`starting` includes absolute input/backend paths, systems, RF center, source
and tracking rates, decimation and temporary working directory. Final state
is `completed`, `failed` or `cancelled`; errors include `text`. Sample counts
are decimal strings to avoid JSON integer precision loss and count complex
samples after decimation (trailing partial decimation groups are not flushed).
`completed` means the stream supplied the expected samples and the backend
exited zero, **not** that a navigation fix exists or every queued byte was
processed by the backend. NMEA is forwarded verbatim subject to the existing
stream's `$` prefix/1024-byte line limit; checksums and fix validity are not
interpreted, repaired or invented. No sentences can be a valid result for a
short/noisy recording. This is not a validated navigation instrument.

The stream binds IQ/NMEA only locally (Windows named pipe for IQ, loopback TCP
elsewhere). Its IQ write queue is limited to about 4 MiB plus one input block.
The JSON writer has a 256-line/256-KiB queue plus one in-flight line. Direct
NMEA callbacks enqueue there, avoiding an unbounded Qt queued-signal backlog;
output failure/overflow stops the run, without silently claiming success.
Consume stdout continuously. Final output gets up to two seconds to drain;
a blocked/closed output is an error and cannot strand the backend. Existing
stream NMEA buffering also caps reads at 64 KiB; malformed/oversized sentences
or bursts exceeding that cap can be discarded by that adapter.

Ctrl+C/SIGINT, SIGTERM, Windows console Ctrl+Break, or creation of an optional
`--cancel-file PATH` requests cancellation. The marker is not deleted; choose a
fresh path per run. `--timeout-ms N` imposes an optional total runtime limit
(default zero: no overall limit). Backend process startup is limited to five
seconds; the existing IQ connection timeout is 30 seconds. After file EOF the
stream drains writes and listens for late NMEA for three seconds. Stream and
backend must finish within five seconds of either finishing. A backend that
exits zero before all samples are supplied is still a failure.

On exit the worker requests stream stop, terminates the backend, escalates to
kill after 300 ms if necessary, joins the stream, and removes its private
temporary working directory including backend-created cache. Cancellation
while a stream socket write is stalled can take about two extra seconds.
Force-killing the worker bypasses cleanup, as with any process. Stdout NMEA and
status paths can contain sensitive coordinates or capture names; nothing is
uploaded or added to the portable application directory.

Exit codes: `0` completed, `2` arguments/input/band invalid, `3` backend missing
or process startup failure, `4` stream/backend/timeout/cleanup failure,
`5` JSON output failure, `130` cancelled. A terminal JSON line may be unavailable
if stdout itself fails.

## Standalone Build And Integration

Requires an **existing** C++17 compiler, CMake 3.16+, Qt 5.15 Core + Network
development packages and platform threads. No Qt Widgets, Gui, Multimedia,
FFTW, SDR device libraries or PocketSDR headers/libraries are needed. PocketSDR
is optional at build time and required only when the command is run.

For a separate build, put the following CMake project in a new scratch/build
directory, not either existing CMakeLists. `OBRII_ROOT` is the absolute checkout
path. This is also the exact optional-target recipe for the build owner to
integrate later; `gnsssdrstream.h` and `playbackmanager.h` require AUTOMOC.

```cmake
cmake_minimum_required(VERSION 3.16)
project(ObriiCliGnssStandalone LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network)
find_package(Threads REQUIRED)
if(NOT IS_ABSOLUTE "${OBRII_ROOT}")
    message(FATAL_ERROR "Set OBRII_ROOT to the absolute source checkout")
endif()
add_executable(ObriiCliGnssModule
    "${OBRII_ROOT}/src/cli/obriiclignssmodule.cpp"
    "${OBRII_ROOT}/src/gnss/gnsssdrstream.cpp"
    "${OBRII_ROOT}/src/gnss/gnsssdrstream.h"
    "${OBRII_ROOT}/src/gnss/gnsssdrdecimator.h"
    "${OBRII_ROOT}/src/audio/playbackmanager.cpp"
    "${OBRII_ROOT}/src/audio/playbackmanager.h"
    "${OBRII_ROOT}/src/core/iqbuffer.cpp")
target_include_directories(ObriiCliGnssModule PRIVATE
    "${OBRII_ROOT}/src/gnss" "${OBRII_ROOT}/src/audio"
    "${OBRII_ROOT}/src/core")
target_link_libraries(ObriiCliGnssModule PRIVATE
    Qt5::Core Qt5::Network Threads::Threads)
# iqbuffer.cpp uses diagnosticlogging.h; playbackmanager.h uses radiosettings.h.
# Both are header-only dependencies in src/core; no other implementation needed.
add_executable(cli_gnss_selftest "${OBRII_ROOT}/tools/cli_gnss_selftest.cpp")
target_link_libraries(cli_gnss_selftest PRIVATE Qt5::Core Qt5::Network)
if(MSVC)
    target_compile_options(ObriiCliGnssModule PRIVATE /utf-8)
    target_compile_options(cli_gnss_selftest PRIVATE /utf-8)
endif()
enable_testing()
add_test(NAME cli_gnss COMMAND cli_gnss_selftest $<TARGET_FILE:ObriiCliGnssModule>)
set_tests_properties(cli_gnss PROPERTIES TIMEOUT 90)
```

Windows example (scratch project above at `build/cli-gnss-project/CMakeLists.txt`):

```powershell
cmake -S build/cli-gnss-project -B build/cli-gnss-standalone `
  -G 'Visual Studio 17 2022' -A x64 `
  -DOBRII_ROOT="$PWD" -DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64
cmake --build build/cli-gnss-standalone --config Release
$env:PATH = 'C:\Qt\5.15.2\msvc2019_64\bin;' + $env:PATH
ctest --test-dir build/cli-gnss-standalone -C Release --output-on-failure
```

Linux example with Qt/compiler already installed:

```sh
cmake -S build/cli-gnss-project -B build/cli-gnss-standalone \
  -DOBRII_ROOT="$PWD" -DCMAKE_BUILD_TYPE=Release
cmake --build build/cli-gnss-standalone --parallel 2
ctest --test-dir build/cli-gnss-standalone --output-on-failure
```

The recorded-file test is a subprocess integration test. It doubles as a
clearly synthetic backend fixture to verify exact IQ bytes/counts, shared
arguments/decimation, raw formats/WAV/sidecar precedence, fragmented NMEA,
JSON-only output despite backend log noise, explicit and adjacent-installed
resolution, invalid inputs, missing runtime, early/nonzero backend exits,
timeout, cancellation, blocked stdout, child reaping and temporary-directory removal. It needs
no real backend, RF capture, receiver, UI, network access or downloads. It does
not validate real PocketSDR acquisition, ephemerides or PVT; run a locally
installed backend against a known capture separately for that qualification.

## Live Stdin Contract (Version 1)

```sh
./ObriiCliGnssModule --live --sample-rate 8000000 --center 1575420000 \
  --systems GPS --backend /opt/pocketsdr/pocket_trk
```

Stdin is **binary framed data**, not JSON Lines or unframed floats. `--live`
and `--file` are mutually exclusive; `--file -` remains invalid. Live input is
CF32 only (interleaved little-endian IEEE float I,Q), at the actual full-source
rate and RF center. CLI gain normalization is disabled: conversion, clipping,
nonfinite-to-zero handling and retained-state decimation match recorded CF32.
The GUI's existing snapshot source and live gain normalization are unchanged.
No publishing into or reading from the global IQ ring occurs in this mode.

Use the existing `CliModuleProtocol::frame(kind, sampleRate, payload, bytes)`:
the header is three little-endian uint32 values (12 bytes). Payloads are at
most 1 MiB. The shared protocol header is unchanged; GNSS-specific meanings
are scoped to `--live`. The sequence is:

1. Launch explicitly with `--backend`, `--systems`, `--sample-rate`, `--center`.
   Rate must be an integer in 1..4294967295 Hz, center finite and positive.
   Shared tracking-rate/band validation runs before any external backend starts.
2. Send `Configure` (kind 3), header rate 0, UTF-8 JSON payload (at most 4096
   bytes), with exactly these five fields:
   `{"version":1,"action":"start","source":"full","sampleRateHz":8000000,"centerHz":1575420000}`.
   Rate/center must exactly match the CLI. Unsupported versions, channel IQ,
   duplicate configuration and retuning fail. No PocketSDR process is started
   until valid Configure/start is received. Configure has a five-second limit.
3. Wait for JSON status `state:"ready",version:1`. `starting` and `running`
   are **not** readiness. Ready means the backend has connected to the IQ
   transport and the injected source is accepting blocks. No pre-ready IQ is
   buffered or silently skipped: sending it is a protocol failure.
4. Send `ChannelIqF32` (kind 2), header rate equal to the configured source
   rate, payload 8..1048576 bytes, a multiple of 8. Blocks form one contiguous
   source epoch. Hardware/network discontinuities, source replacement, drops,
   rate changes and retunes require cancellation and explicit new start.
5. Send `Configure`, header rate 0, exactly
   `{"version":1,"action":"eof","sourceSamples":"8195"}`, then close stdin.
   The count is the total **source complex samples**, as a canonical unsigned
   decimal string, not a tracking/decimated count. It must equal all accepted
   IQ payloads. EOF drains every accepted block before transport disconnect.
   Closing stdin without this handshake, partial headers/payloads, more frames
   after EOF, or a mismatched count fail. Zero-sample explicit EOF is valid.
6. Observe `event:"eof",version:1,sourceSamples,samplesSent,trailingSourceSamples`.
   It acknowledges the final source cursor and successful socket-queue drain.
   `samplesSent = floor(sourceSamples / decimation)`; the explicitly reported
   trailing remainder is consumed but not zero-padded or filter-flushed. Wait
   for terminal `completed` **and** exit code 0 before declaring success.
   Socket drain does not prove that PocketSDR processed every byte or found PVT.

`Reset` (kind 0, rate 0, empty payload) cancels the session, including before
Configure or while waiting for IQ connection. It is not a successful EOF or
an in-place source reset. After EOF, close stdin and wait for completion;
use the cancellation mechanisms below if the process must stop immediately.
Ctrl+C/SIGTERM, Windows Ctrl+Break, `--cancel-file` and `--timeout-ms` also work.
The stdin thread uses cancellable native I/O; idle stdin cannot strand cleanup.
Windows pipes poll availability at five-millisecond intervals rather than
entering an unbounded read on QProcess's inherited pipe handle; POSIX uses poll.
Protocol/queue/discontinuity errors exit 4; cancellation exits 130. The other
recorded-mode exit codes above are unchanged.

The injected producer queue is bounded by **4 MiB and 64 blocks**, plus one
in-flight payload (1 MiB). The socket queue is bounded to about 4 MiB plus one
converted block (up to 512 KiB). Overruns are fatal and explicit, not dropped
IQ with a false success report. The source uses mutex/condition-variable
synchronization; EOF and cancellation wake consumers. Source read waiting
polls at most 20 ms to allow NMEA servicing. Idle/stdin cancellation is
independent of JSON stdout. `CliJsonOutput` in `src/cli/clijsonoutput.h` is the
same bounded writer as recorded mode, shared with other workers through
`CliJsonOutput("gnss")` / `CliJsonOutput("tx")`, with `send/okay/finish`.
As in the worker, POSIX callers must ignore SIGPIPE before using this writer
so closed stdout becomes `okay()==false` instead of terminating the process.

EOF keeps the existing three-second late-NMEA window. After either the stream
or backend finishes, all parties including physical stdin EOF must finish
within five seconds. An early backend exit, even zero, is not success.
Temp-directory removal and backend terminate/kill/reap behavior are unchanged.

## Parent Integration

`src/cli/clignsssession.{h,cpp}` supplies `CliGnssSession`, an inert QObject
without Q_OBJECT or receiver/backend dependencies. It only launches the optional
worker on explicit `operation:"start"`. All calls, `onEvent` and `QProcess`
work must stay on its owning Qt event-loop thread; keep pumping that loop.
This helper does not register itself with `CliModuleManager` or add commands
to the CLI. The build/receiver owner must wire those separately.

```cpp
#include "clignsssession.h"

CliGnssSession gnss; // keep alive for the receiver session
gnss.onEvent = [&](const QJsonObject &event) { publishEvent(event); };

// Route module/name gnss to this helper, before the generic module manager.
auto reply = gnss.command({{"operation", "start"},
    {"backend", "D:/backends/pocket_trk.exe"},
    {"systems", QJsonArray{"GPS", "QZSS"}}}, actualRxRateHz, actualRxCenterHz);

// Receiver loop: BEFORE channelizer, decimation, demodulation or audio DSP.
// These floats must be the complete raw source block, not a VFO channel.
if (gnss.ready())
    gnss.submitIq(rawIq, complexSamples, actualRxRateHz, actualRxCenterHz, sourceGap);

// For finite replay/network source completion, while ready:
gnss.command({{"operation", "eof"}}, actualRxRateHz, actualRxCenterHz);
// Continue the event loop until status().running == false.

// For retune/rate/source changes, RX restart, disconnect, or ordinary shutdown:
gnss.reset(); // asynchronous cancellation; never implies EOF completion
// gnss.stop() is equivalent. Continue pumping Qt until !gnss.running().
// Only after RX has ended: bounded EOF/cancellation wait, if needed.
bool stopped = gnss.waitForStopped(11000);
// Destruction retains bounded blocking cleanup, also only after RX has ended.
```

`command` supports start/status/eof/stop/reset. Start requires an explicit
`backend` path or `installed`, and `systems` as an array of strings or CSV.
`worker` optionally overrides the default adjacent `ObriiCliGnssModule[.exe]`;
`timeoutMs` optionally sets the worker runtime limit. Optional `sampleRateHz`
and `centerHz` must match the **actual** arguments; they never retune a source.
The worker validates constellation bandwidth before launching PocketSDR.
Replies use `ok`; status includes state, ready, running, source/center rates,
decimation, accepted sourceSamples, rejectedBlocks, queuedBytes, error and
eofAcknowledged. JSON sample counters are decimal strings.

`submitIq` returns false before readiness/after EOF/inactive, incrementing
rejectedBlocks. Callers must gate normal capture on `ready()`, explicitly
report the capture start boundary, and must not treat rejected data as accepted.
For finite sources that must replay the entire recording, pause the producer
before its first block until ready; simply skipping pre-ready blocks explicitly
excludes that prefix from the capture and from the counted EOF contract.
Once ready, mismatched actual rate/center, `discontinuity=true`, invalid buffers
or a pipe-queue overrun fail the session and require explicit restart. Blocks
are split into 1-MiB frames; the entire submission is capacity-checked before
any part is enqueued. The helper's QProcess write queue is at most 4 MiB plus
small control frames. It never creates an unbounded queued signal per IQ block.
Little-endian hosts copy complete native float blocks with memcpy; only
big-endian hosts require per-float byte conversion. The injected consumer uses
the same fast path. Nothing is allocated or copied from source IQ before ready.
EOF is counted from accepted submissions, is acknowledged/validated against
tracking decimation, and completion requires acknowledgment and worker exit 0.
Public `stop()`, `reset()`, and their command forms never wait on RX: they send
Reset/close input (or defer Reset until process startup), mark `stopping`, and
return immediately. This also invalidates a session while it is still starting.
`ready()` immediately becomes false; `running()` and `status().running` stay
true until the worker is reaped. A Qt watchdog sends terminate after seven
seconds and kill after another 300 ms; it never blocks the owning event loop.
Successful cancellation ends in `cancelled`; existing source errors stay failed.
Do not start another session until running is false.

`waitForStopped(timeoutMs=11000)` is an optional bounded, event-pumping wait for
EOF/cancellation shutdown **after RX has ended**, not a receiver-loop command.
It returns false on timeout and does not treat a timed-out EOF as completion.
Check terminal state/eofAcknowledged as well. Destruction requests asynchronous
stop, then retains blocking fallback cleanup: at most seven seconds graceful,
300 ms after terminate, three seconds after kill (about 10.3 seconds total).
Keep destruction/waitForStopped off RF watchdog paths. `onEvent` must itself
remain bounded and nonblocking.

For optional network full IQ, use the same `submitIq` at the full-IQ callback
with its actual source rate/center and discontinuity flag. Do not use audio,
spectrum or per-VFO IQ callbacks. Reset on source identity/epoch changes even
when numerical rate and center stay the same; framing cannot infer missing
upstream frames without the receiver/network discontinuity indication.

### Exact Build Additions

Parent `ObriiCli` target: add **only** `src/cli/clignsssession.cpp` and its
header; the helper depends on Qt5::Core and the existing `climoduleprotocol.h`.
It needs no GNSS stream, playback, global IQ ring, QtNetwork, Widgets, SDR
drivers, PocketSDR library or extra MOC. If including `clijsonoutput.h`, also
link platform threads (`Threads::Threads` on POSIX) and use C++17. `finish()`
can wait up to two seconds: keep it out of RF watchdog critical paths.

Worker target: retain the exact recipe above, including AUTOMOC for
`gnsssdrstream.h` and `playbackmanager.h`; add `src/cli/climoduleprotocol.h` and
`src/cli/clijsonoutput.h` to the target's header inventory if desired. Both
headers resolve alongside the worker source; no extra include path/library is
needed. Deploy the worker and Qt Core/Network runtime separately from the
optional PocketSDR executable/dependencies. Nothing is downloaded by this code.

Add the live self-test alongside the existing recorded test in the scratch
project/build-owner recipe (no repository CMake files are changed here):

```cmake
add_executable(cli_gnss_live_selftest
    "${OBRII_ROOT}/tools/cli_gnss_live_selftest.cpp"
    "${OBRII_ROOT}/src/cli/clignsssession.cpp"
    "${OBRII_ROOT}/src/cli/clignsssession.h"
    "${OBRII_ROOT}/src/gnss/gnsssdrstream.cpp"
    "${OBRII_ROOT}/src/gnss/gnsssdrstream.h"
    "${OBRII_ROOT}/src/audio/playbackmanager.cpp"
    "${OBRII_ROOT}/src/audio/playbackmanager.h"
    "${OBRII_ROOT}/src/core/iqbuffer.cpp"
    "${OBRII_ROOT}/src/openradio/virtualsignalframe.cpp"
    "${OBRII_ROOT}/src/openradio/virtualsignaltransport.cpp")
target_include_directories(cli_gnss_live_selftest PRIVATE
    "${OBRII_ROOT}/src/cli" "${OBRII_ROOT}/src/gnss"
    "${OBRII_ROOT}/src/audio" "${OBRII_ROOT}/src/core"
    "${OBRII_ROOT}/src/openradio")
target_link_libraries(cli_gnss_live_selftest PRIVATE
    Qt5::Core Qt5::Network Threads::Threads)
if(MSVC)
    target_compile_options(cli_gnss_live_selftest PRIVATE /utf-8)
endif()
add_test(NAME cli_gnss_live
    COMMAND cli_gnss_live_selftest $<TARGET_FILE:ObriiCliGnssModule>)
set_tests_properties(cli_gnss_live PROPERTIES TIMEOUT 120)
```

Run both suites. The live executable doubles as its own explicitly selected
synthetic backend. It checks exact CF32-to-CS16 bytes at 4/8/16/50 MS/s,
fragmentation, decimator continuity and final tails, invalid Configure/rates,
pre-ready rejection, source changes, block/byte overruns, EOF sample mismatch,
truncation/physical EOF, Reset cancellation, immediate helper cancellation,
late NMEA, child reaping and private-directory removal. It opens no real
hardware or real backend. The recorded suite continues to cover formats/WAV,
resolver behavior, blocked stdout, cancellation and existing file behavior.
The live test prints per-case progress with elapsed milliseconds to stderr and
includes total elapsed milliseconds in its final result. Its deliberately
stubborn stall/slow backend fixtures self-expire after 30 seconds if the
worker fails to reap them; they no longer wait forever. Keep the external
CTest timeout too: cooperative thread joins/native cleanup are not a hard
whole-process deadline if an OS I/O operation cannot be interrupted.

An optional second argument enables main-CLI control/feed integration, while
the original single-worker argument still runs the same standalone suite:

```powershell
.\cli_gnss_live_selftest.exe .\ObriiCliGnssModule.exe .\ObriiCli.exe
```

This starts only `run --backend virtual --input rf` at 8 MS/s, 1575.420 MHz,
with independent loopback virtual-IQ/control TCP ports and `--spectrum --fft
1024` (no GUI/network server). It primes the first virtual discontinuity before
explicit GNSS start through a request JSON file, waits for ready, feeds wrapped
contiguous CF32 frames, and verifies exact post-ready IQ bytes including a split
RX block tail, counted EOF/completion/acknowledgment, zero source drops, private
backend-directory removal and process reaping. It also checks active GNSS stop
is asynchronous/cancelled, then exits the receiver through control `quit`.
The backend is always the explicitly selected synthetic test executable, never
real PocketSDR or receiver hardware. The test target additionally compiles
`virtualsignalframe.cpp` and `virtualsignaltransport.cpp` with the `src/openradio`
include directory; no additional libraries are required. A build owner can add
a separate CTest command with both executable arguments and a 120-second timeout.
