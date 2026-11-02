# Command Console

Open **Console** (`>_`) in the Receiver section, next to Refresh, Network and Settings.
The DSP board also has a `>_` toolbar button, including in the alternative
workspace. This window controls the already running GUI instance; it does not
launch `ObriiCli` or reopen its USB receiver. The receiver keeps running when
the window closes. Start with `commands`, `status` or `metrics`.

`CommandConsoleDialog` is a nonmodal Qt5 console for the existing
`CommandDispatcher`, not a second receiver or DSP runtime. It is initially
720 x 450 pixels and supports a 450 x 260 compact layout.

## Integration

```cpp
auto *console = new CommandConsoleDialog(dispatcher,
    [this](const QString &key, const QString &fallback) {
        return uiText(key, fallback);
    }, this);
console->setControlServer(controlServer);
console->show();
```

`Translator` is optional and receives a stable `command_console_*` key and
English fallback. The application supplies Ukrainian translations as well.
The dispatcher is borrowed and must outlive the dialog. The optional control
server is also borrowed, tracked by `QPointer`, and remains owned by the main
window. Construct and use the dialog, dispatcher, and server on the GUI thread.
Hiding/closing the console cancels a pending remote client request and stops
metrics polling. It does not stop the receiver or the Agent API server.

Build the dialog with Qt Core, Widgets and Network, the existing dispatcher and
control-server implementations, and AUTOMOC for `commandconsoledialog.h` and
`commandcontrolserver.h`. No Qt Test dependency is needed.

## Local Commands

Manual local commands go directly through
`dispatcher->executeText(text, CommandDispatcher::Context::human())` on the GUI
thread. The console does not implement tuning, gain, receiver start/stop, audio,
or FFT itself. Those actions and their permissions remain dispatcher-owned.

The catalog dropdown is populated from `catalog(Context::human())`. Selecting a
command places its name in the input without executing it. The help tool shows
the selected command's description/schema, or requests the current catalog.
Standard commands include `status`, `metrics`, `commands`, `start`, `stop`,
`tune 100M`, `listen 100M`, `gain 1 2`, `demod nfm`, `bandwidth 12000`,
`audio on`, `spectrum`, `trace live 0 128`, and `audit`. Actual availability is
determined by each dispatcher's registered handlers and caller permissions.
JSON command envelopes are also accepted.

The GUI currently exposes primary receiver controls, IQ counters, spectrum
metadata and exact cached RF trace pages. `gain` uses the backend's slider
indices, not universal dB. `status` describes configured settings, not hardware
acknowledgement. A pending start/stop is reported as pending. GUI `trace` accepts
`live` only, rejects direct HF and scan mosaics, and never starts an FFT; enable
the spectrum and wait for a current frame first. Paused data is marked frozen.
Other scientific, recording and module commands of the standalone CLI are not
automatically available in this GUI window. Inspect `commands` for the actual
frontend catalog instead of assuming parity.

The optional Metrics checkbox sends a read-only `metrics` request once per
second, only while the console is visible, local, idle, and the catalog
advertises metrics. It does not enable analysis or manufacture metrics.
Unsupported metrics are disabled. The checkbox is disabled for remote targets.

The input keeps at most 100 commands in memory. Up/Down navigate history and
restore the unfinished draft. Sensitive/redacted input is excluded from
history. The read-only terminal retains at most 1,000 text blocks; each rendered
reply is additionally capped. Clear affects only the transcript. Copy copies
the selection, or the transcript when nothing is selected.

## Remote Requests

Select Remote, enter a host, nonzero port, and password-masked token. Endpoint
values are session-only; changing them clears the old catalog and instance
identity. No connection occurs merely by selecting or editing a target.

The remote client sends one compact UTF-8 JSON object followed by a newline:

```json
{"token":"<session token>","request":{"command":"status","args":{}}}
```

The response must be a flat dispatcher JSON object followed by a newline, not a
nested `response` wrapper. The instance identity comes from `instanceId` in the
reply. The client permits one in-flight request, uses asynchronous `QTcpSocket`
signals, and applies a three-second deadline covering connection and reply.
It never calls `waitForConnected`, `waitForReadyRead`, a nested event loop, or a
blocking network wait. The reply line is limited to 1 MiB; malformed JSON,
oversize replies, connection failures, incomplete lines and timeouts are shown
as structured failures. Hiding the console aborts the client connection; an
already-executed remote action cannot be undone by that cancellation.

Remote standard text syntax uses `CommandDispatcher`'s parser, even when the
local frontend has no matching handler. Fetching the remote catalog also makes
its custom positional schemas available to that same parser. A temporary
parser-only dispatcher never executes a local handler for a remote request.
Custom JSON envelopes can be sent without fetching a catalog first.

## Agent API Settings

The Agent API tool is enabled only after `setControlServer()` receives a server.
It opens a separate nonmodal settings dialog with a port in the range 0..65535
(0 chooses a free port), start/stop icons, an opt-in receiver-control checkbox,
and a read-only actual bound address/status. The server itself enforces
loopback binding and read-only access by default. Existing server state is
reflected without changing it merely by opening the dialog.

The session token appears only in a read-only password field, with a separate
explicit copy action. Stopping the server clears that field. Server status
changes update visible settings through `statusChanged`; there is no settings
polling timer. Hiding the console hides the settings but leaves the server's
enable/control state unchanged. Destroying or replacing a borrowed server
updates/disables its controls without transferring ownership to the console.

The transport is plain TCP JSON Lines, not TLS or MCP. The built-in server binds
only to loopback. Use a trusted SSH tunnel for access from another machine; the
console does not create tunnels, persist credentials, or expose a LAN listener.
Alternatively, run Obrii's existing GUI or CLI network client locally and use
its local dispatcher to control the connected server, subject to that client's
existing controller/observer rights. The Agent API does not bypass those rights.

There is no embedded model or chat engine. A user-owned local agent can use this
documented API, fetch `commands`, read JSON results, and optionally request
receiver controls after the owner enables them. This is an integration surface,
not an automatically connected AI assistant. See `command_control_api.md` and
`command_dispatcher.md` for wire format and permissions.

## Redaction

Console input echo and replies are sanitized recursively, including objects
inside arrays. Token, password, secret, authorization and API-key fields are
replaced by `[redacted]`. Known client/server token values are also removed
from other strings, rendered output, identity/status text, and copied
transcripts. Invalid JSON input is not echoed verbatim. Redaction is display
only: the original request still reaches the dispatcher/server for validation.
No token is written to settings, history, files, diagnostic logs or the console
transcript. Explicit Agent-token copy is the only clipboard action allowed to
contain the credential.

## Self-Test

`tools/command_console_selftest.cpp` uses an offscreen `QApplication`, real
dispatcher/control-server sources, fake handlers, and a local `QTcpServer`.
It opens no radio hardware and launches no main application. The test checks
GUI-thread human dispatch, frequency parsing, catalog/schema access, bounded
output/history, visibility-gated metrics, remote-only catalog commands,
single-flight requests, fragmented/invalid/oversize/incomplete JSON Lines,
the actual three-second timeout with a live event-loop heartbeat, recursive
credential redaction, Agent API opt-in, explicit token copy, hide persistence,
and borrowed-server destruction.

Standalone test sources:

```text
tools/command_console_selftest.cpp
src/ui/commandconsoledialog.cpp
src/ui/commandconsoledialog.h
src/core/commanddispatcher.cpp
src/network/commandcontrolserver.cpp
src/network/commandcontrolserver.h
```

Include `src/ui`, `src/core`, and `src/network`; link Qt5 Core, Widgets and
Network; use C++17 and AUTOMOC. The optional `--snapshot-dir <directory>` writes
local, compact remote, and Agent-settings PNGs. Snapshot mode checks actual
Latin/Ukrainian glyph pixels. If the Windows offscreen backend has no usable
fonts, the test loads Segoe UI (or Arial) and Consolas from the Windows Fonts
directory. This fallback applies only to screenshot tests; production font
selection is unchanged. `QT_QPA_FONTDIR` remains an optional override. Normal
Windows GUI sessions use their existing platform font configuration.

Stable UI object names use the `commandConsole` prefix. The self-test exercises
the controls by `findChild` and synthesized input events without Qt Test.
