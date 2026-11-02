# Shared Command Dispatcher v1

`CommandDispatcher` is a QtCore-only, statically linked registry used by the GUI
command window and the CLI receiver/client control hosts. It invokes existing
frontend handlers, not a second receiver, FFT, recording queue or DSP thread.
It is created lazily in the GUI. No listener, model, cloud connection or polling
starts merely because the application contains it.

## Discover The Current Instance

Use `commands` (or `help`), `commands tune`, `status`, `metrics`, `spectrum`,
`trace live 0 128`, and `audit`. Availability is frontend- and permission-specific.
The catalog contains only registered, permitted handlers, with argument schemas,
positional fields, access class and a session UUID `instanceId`. The UUID changes
on the next process/dispatcher session; it is not a persistent device identifier.
This is `obrii-jsonl-v1`, not native MCP or an embedded AI model.

GUI entry: Receiver -> **Console** (`>_`), or the DSP board's `>_` button.
CLI entry: `ObriiCli control commands` on a running receiver/client control port.
These are distinct process instances, even when they use the same physical
receiver type. Never assume one instance's identity or catalog applies to another.

## Request And Reply

The dispatcher accepts one JSON object:

```json
{"command":"listen","args":{"frequencyHz":101700000},"id":"tune-1","targetInstanceId":"UUID_FROM_CATALOG"}
```

Only `command`, `args`, `id` and `targetInstanceId` are accepted. `args` defaults
to `{}`; `id` is an optional string/number, generated when absent. An optional
`targetInstanceId` must match the receiving dispatcher. The transport/session,
not the request, supplies permissions. Requests are limited to 64 KiB.

Responses retain the frontend's result fields and add `ok`, `schemaVersion: 1`,
`instanceId`, `command`, `id`, and `durationUs`. Errors include `errorCode` and
`error`. Never treat `ok: true` as a hardware measurement or device acknowledgement
unless the handler explicitly provides one. GUI control replies describe
configured settings and report pending receiver transitions. Frontend-specific
fields and units remain explicit; they are not universally interchangeable.

Text syntax is not an operating-system shell: `tune 100M`, `listen 100.1M`,
`demod nfm`, `bandwidth 12.5k`, `audio on`, `gain 1 2`. Frequencies accept k/M/G
and scientific notation, with a dot decimal separator. There are no pipes,
subprocesses, command substitution or shell escapes. JSON is preferable for
automation and named arguments. Unknown fields, enum values and numeric ranges
are rejected before invoking a handler. Schemas describe the supported subset
of JSON Schema, not a general-purpose JSON Schema interpreter.

## Access And Audit

Commands are classified Read, Control, Files or Transmit. Manual local commands
use a trusted human context with Control and Files, but not Transmit. The optional
authenticated Agent API starts read-only, can be locally granted Control, and
never grants Files or Transmit. Its port/token and limits are documented in
`command_control_api.md`. No command can grant itself permissions.

`audit [AFTER [LIMIT]]` returns at most 100 entries from a 256-entry in-memory
ring. Sequence and large counters are strings to preserve integer precision.
It records command name, origin, outcome, timestamp and duration, not arguments,
result contents, tokens, settings or raw Qt/decoder logs. It is session-local,
not a persistent forensic log. A rejected malformed JSON line that never enters
the dispatcher is not a successful command event. `metrics` samples existing
counters on demand; it does not start an analysis or enable diagnostic logging.

## Frontend Boundaries

GUI handlers control the existing primary receiver slots and controls. Cached
RF traces are paged up to 1024 bins without a new FFT, display reduction or a
full-buffer copy; direct HF inputs and scan mosaics are rejected. Trace replies
include geometry, epoch and frozen/correction flags. dBFS does not become
calibrated dBm because it was read through JSON. GUI tuning follows existing
network-client controller/observer checks. There is no implicit Multi-SDR audio
source selection or arbitrary decoder/module launch in these basic commands.

CLI receiver hosts register the same basic typed commands and route them through
the existing handlers. CLI network clients advertise only what their adapter
supports (no exact local trace handler). Advanced CLI commands remain available
through the trusted-local `cli.legacy` compatibility adapter and original
`control ...` syntax. Original action-based wire replies retain their raw fields
(including graph/Multi-SDR `id`), without dispatcher metadata overwriting them.
A typed `cli.legacy` request instead places that raw object in `legacyResult`,
separate from the dispatcher's own request `id` and `schemaVersion`.
This adapter requires Files and is invisible/inaccessible
to the Agent API, even with receiver Control enabled. The original CLI control
port remains a trusted-local, unauthenticated endpoint, not an OS security
sandbox. Do not forward it to an untrusted network.

## Development And Performance

Construct, register, invoke and destroy a dispatcher on its owner event thread.
Foreign-thread execution and nested execution fail instead of starting another
worker or recursively mutating radio state. Handlers run synchronously and must
be brief; a socket timeout cannot preempt a native device call. There is no
automatic retry, forced cancellation or rollback of completed actions.

Add shared DSP/math to existing common modules, then register the necessary
frontend adapters with accurate access/schema. A new GUI widget alone does not
create a CLI command; a shared algorithm correction reaches both after rebuild.
Keep file names, keys and entire IQ arrays out of status and audit. Use bounded
pages and existing counters for automated diagnostics.

Build/run `ObriiCommandDispatcherSelfTest`, `ObriiCommandControlSelfTest` and
`ObriiCommandConsoleSelfTest`. `tools/command_cli_integration_selftest.ps1` checks
the actual CLI host with synthetic file IQ, control, token rotation and trace
paging; it does not open RF hardware or transmit. Existing audio/recording/FFT
isolation tests remain required when those shared hot paths change.

For a small hardware-free build on Raspberry/Linux (Qt5 base development
packages and C++17 compiler), use the same source tree:

```sh
cmake -S tools/command_core -B build/command-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/command-tests -j2
ctest --test-dir build/command-tests --output-on-failure
```

This is a test harness, not another CLI application project or installation.

Verification on 2026-11-02: dispatcher, control transport and console suites
passed three consecutive Release runs on Windows/MSVC/Qt 5.15.2 and Raspberry
Pi 5 arm64/GCC 12.2/Qt 5.15.8. The transport also passed AddressSanitizer checks,
including timeout-driven socket deletion during a nested handler event loop.
Windows CLI integration passed token/control/trace checks, runtime recording and
the parity suite. File RX with and without a 1M FFT produced identical WAV.
These are software regressions, not physical RF/GUI-control qualification or
certification of concurrent FT4/M17 work.
