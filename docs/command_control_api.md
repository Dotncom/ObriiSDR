# Command Control API v1

`CommandControlServer` exposes the current application's `CommandDispatcher`
through an explicitly enabled, authenticated JSON-line socket on IPv4 loopback.
This is `obrii-jsonl-v1`, not native MCP: catalog/status report `mcp: false`.
An MCP adapter would be a separate integration. No cloud connection, shell,
file service, RF transmitter, receiver, or background worker is started by this
transport.

In the GUI, open **Console** in Receiver (or `>_` on the DSP board), then its
Agent API settings tool. Start the service there; port 0 chooses a free port.
Copy the session token only into a trusted local agent. In a CLI receiver or
client host, use `ObriiCli control agent start 0`; this explicit reply contains
the token, so do not save it in a public transcript. `control agent status`
omits the token, `control agent control on|off` grants/revokes receiver control,
and `control agent stop` closes the endpoint. Neither frontend enables this
service automatically or starts an AI model.

## Lifecycle and C++ API

```cpp
CommandControlServer server(&dispatcher, parent);
// Construction creates no TCP server, sockets, timers, or listener.
QString error;
if (server.start(0, &error)) {
    const quint16 actualPort = server.port();
    const QString sessionToken = server.token(); // Explicit local owner access.
}
server.setAllowControl(true); // Only an explicit local UI/owner decision.
server.stop();
```

The exact public interface is:

```cpp
explicit CommandControlServer(CommandDispatcher *, QObject *parent = nullptr);
bool start(quint16 port = 0, QString *error = nullptr);
void stop();
bool isListening() const;
quint16 port() const;
QString token() const;
void setAllowControl(bool);
bool allowControl() const;
QJsonObject status() const;
// Signal:
void statusChanged();
```

The dispatcher must outlive the server. Construct, call, and destroy both on
the same owning GUI/event thread. There is no thread handoff or blocking socket
wait. Starting an already listening server fails without replacing its session.
A failed listen leaves it disabled. Port zero selects an OS-assigned port;
there is no bind-address parameter and no IPv6/wildcard/LAN binding option.
The only listener is `127.0.0.1` and accepted peers must be IPv4 loopback.

Each successful start obtains 256 random bits from Qt's system random generator
and represents them as 64 lowercase hexadecimal characters. The token lives
only in memory: this service never persists or logs it, writes configuration,
or exposes it through status/catalog. `token()` is the explicit local owner's
access point for a manual settings dialog or session handoff. Avoid sharing it
in saved requests, command history, screenshots, telemetry, or logs.

Stop closes the listener and aborts every client, stops client timers, clears
the token, and cancels pending input without dispatching it. Qt objects are
deleted through the event loop. `port()` returns zero and `token()` returns an
empty string while stopped. A restart changes the token and resets session
counters/rate history. The local `allowControl` choice persists for this server
object until the owner changes it; its initial value is false.

## Wire Format

Open one connection to the selected `127.0.0.1:PORT`, send one compact UTF-8
JSON object followed by LF, read one JSON response line, then disconnect.
CRLF is accepted as JSON trailing whitespace plus LF; CR counts toward the
request limit. Partial TCP reads are assembled but never dispatched before LF.
Pipelining is unsupported: extra bytes already received after the first LF
reject the entire request, not a second command.

The envelope must contain exactly `token` and `request`, with no other keys:

```json
{"token":"SESSION_TOKEN","request":{"command":"commands","args":{},"id":"catalog-1"}}
```

`request` must be an object with these fields only:

| Field | Contract |
| --- | --- |
| `command` | Required string matching `[a-z][a-z0-9._-]{0,63}`. |
| `args` | Optional object, default `{}`; dispatcher validates its command schema. |
| `id` | Optional finite number or string of at most 128 UTF-16 code units. |
| `targetInstanceId` | Optional string, at most 128 UTF-16 code units; dispatcher rejects a different instance. |

No envelope/request fields can select origin, permissions, context, remote
frontend, or another process. `version`, `allowControl`, `allowFiles`,
`allowTransmit`, `context`, and `origin` are not request-level grant mechanisms.
Arguments are command data, never context overrides. Use the authenticated
`commands` catalog to inspect the current frontend's actual command names,
schemas, and instance identity.

Successful replies and dispatcher errors retain the dispatcher's v1 response
shape, including command/id/instance identity where the dispatcher supplies
them. The transport does not invent handlers or pretend unavailable commands
exist. A transport rejection looks like:

```json
{"ok":false,"errorCode":"unauthorized","error":"Authentication failed","schemaVersion":1}
```

Bad tokens receive no catalog, instance ID, token hint, request reflection, or
handler call. Token comparison examines all 64 expected characters without
short-circuiting on a matching prefix. This comparison does not make JSON
parsing or the whole network transaction constant-time.

Other transport errors include `invalid_json`, `invalid_envelope`,
`invalid_request`, `request_too_large`, `rate_limited`, `busy`, `timeout`, and
`response_too_large`. Resource exhaustion or an undrainable/disconnected socket
may close without delivering an error. There is never a second reply.

## Permissions

Every admitted request gets a newly constructed, immutable context:

```cpp
const CommandDispatcher::Context context{
    QStringLiteral("agent"), localAllowControl, false, false
};
```

Read is the default. The local UI/owner may call `setAllowControl(true)` to
permit registered Control commands, which can change receiver state. It can
revoke that choice immediately for later requests. The context for an executing
request is not changed by a reentrant UI callback.

Files and Transmit are always false, including when Control is enabled. The
server never calls `Context::human()` and never grants these permissions from
JSON. Permission checks and catalog filtering stay in `CommandDispatcher`:
denied commands do not invoke their handlers, and forbidden commands do not
appear in the caller's catalog. Registry owners must correctly classify their
handlers; a handler mislabeled Read cannot be made safe by a transport.

This endpoint is not the existing SDR LAN protocol or CLI control-port proxy.
It controls only its bound dispatcher/current application instance. The token
is a bearer credential, not operating-system user identity. Do not forward or
proxy the socket to a LAN/Internet address; loopback traffic itself is not TLS.

## Resource and Execution Limits

| Resource | Limit |
| --- | --- |
| Serviced clients | 8 total, including pending input and draining replies; excess clients close without dispatch. |
| Qt pending accepts | 8; OS connection backlog is separate. |
| Request | 65536 bytes before LF, for the entire envelope, including token. |
| Socket read buffer | 65537 bytes, plus one bounded 65537-byte input buffer per serviced client. |
| Response | 1048576 bytes including LF; one bounded write, then disconnect. |
| JSON response nesting | 64 levels in the transport preflight. |
| Deadline | 2000 ms from accept, not refreshed by partial input. |
| Command rate | 20 dispatcher admissions across all clients in any rolling 1000 ms window. |

The rate history is a fixed 20-entry array, not an accumulating request queue.
Malformed or unauthorized messages never reach the dispatcher. Permission and
schema failures returned by the dispatcher still consume an admission.
Reentrant transport requests receive `busy`; the core's independent reentry
guard also remains active for direct local execution.

Replies are size/depth preflighted before allocating their serialized JSON byte
buffer. String escaping is counted; numbers reserve their worst-case width,
so a reply extremely close to the limit can be conservatively rejected.
The final encoded length is also checked. This bounds socket buffers and
serialization, not memory a trusted frontend handler allocates while building
its own result. No raw request, token, or parse detail is logged by this server.

Authenticated dispatch is queued onto the same owning event thread after the
socket's native `readyRead` emission returns. There is at most one queued
request per serviced client. It rechecks session identity, deadline and local
Control revocation before execution: Control requires both the permission at
parse time and the current local permission. A later grant cannot elevate an
earlier request, and stopping the server cancels pending dispatch.

Handlers execute synchronously on the owning event thread. They must be brief:
there is no worker, forced thread cancellation, hard real-time guarantee, or
automatic rollback of an already-running command. A blocking handler delays
timer delivery; its late result is discarded after it returns. If a handler
pumps events, a deadline/stop can close its connection while it is running,
but it does not continue on a background thread. No later result is sent to
a removed client or a restarted session. Slow readers are disconnected when
their connection deadline expires.

## Status

`status()` contains no token or raw request. It reports:

- `schemaVersion`, `transport`, `mcp`, `instanceId`, `listening`, loopback
  `address`, and actual `port`.
- `allowControl`, `allowFiles: false`, `allowTransmit: false`, current `clients`,
  and the fixed byte/client/deadline/rate limits.
- Session `requests` (dispatcher admissions), `authFailures` (wrong token or
  token type), and `rejected` (rejected connections/requests, counted once per
  serviced client). Counters saturate rather than wrapping.

`statusChanged()` has no arguments. It notifies lifecycle, permission, and
client/completion transitions, so owners can read the current redacted status.
Stop retains final counters until the next successful start; constructor and
stopped state have no listening socket.

## Build and Tests

Production sources are `src/network/commandcontrolserver.{h,cpp}`. Include
`src/network` and `src/core`, enable AUTOMOC for the QObject header, use C++17,
and link Qt5::Core, Qt5::Network, and the existing CommandDispatcher
implementation/core target exactly once. There are no Widgets, hardware SDK,
FFTW, or external server dependencies.

`tools/command_control_selftest.cpp` needs these exact sources when not linking
an existing dispatcher/server library:

```text
tools/command_control_selftest.cpp
src/network/commandcontrolserver.cpp
src/network/commandcontrolserver.h
src/core/commanddispatcher.cpp
src/core/commanddispatcher.h
src/cli/clicommandadapter.h
src/core/radiosettings.h
```

The two additional headers exercise trusted-local CLI compatibility without
opening a receiver. Keep their original tree layout for the relative include.
Use the same include directories, QtCore/QtNetwork dependencies, C++17 and
AUTOMOC. Tests use real loopback TCP with synthetic handlers: legacy result
IDs are kept separate from typed request metadata, zero idle
sockets, IPv4-only binding, auth failures without catalog leakage, ID/instance
propagation, read-only and manually enabled Control, permanently denied
Files/Transmit, filtered catalogs and agent audit origin, unknown/type-invalid
envelopes, segmented/missing/oversized lines and exact request boundaries,
8-client capacity, two-second deadlines, global rolling rate limits, bounded
large/escaped/deep replies, token rotation, reentry, and stop during a handler.
They open no hardware, user profiles, recordings, or output files.

Root CMake targets are `ObriiCommandDispatcherSelfTest`,
`ObriiCommandControlSelfTest` and `ObriiCommandConsoleSelfTest`. The PowerShell
`tools/command_cli_integration_selftest.ps1` additionally exercises a real CLI
file receiver, token rotation, permissions and exact trace paging. No RF
transmission is needed for these tests.
