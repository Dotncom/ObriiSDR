# CLI Doctor (Schema 1)

`src/cli/clidoctor.{h,cpp}` provides one synchronous QtCore + QtNetwork function:

```cpp
QJsonObject runCliDoctor(const QJsonObject &request = QJsonObject());
```

The caller owns argument parsing, JSON serialization, exit-code policy, and build
integration. This helper does not change main, help text, CMake, or packaging.
Create a `QCoreApplication` before calling for default executable-directory
inspection. Without one, an explicit directory still works; otherwise the report
contains `directory_unavailable` (no fallback to the working/home directory).

## Safety

Default `{}` is passive and creates no socket. Only explicit `checkPorts: true`
with a nonempty `tcpPorts` array enables IPv4 loopback bind/close probes. The
helper never listens, connects, resolves remote hosts, or accepts host/address
options. No physical SDR or audio device is enumerated or opened, no worker/backend
is launched, no transmission occurs, and no profiles, settings, environment variables, home
directories, PATH, or system loader search paths are read. There is no directory
enumeration. Only metadata for fixed adjacent filenames and explicit external
paths is inspected. Existing file contents are not read or changed.

No libraries/plugins are loaded, even FFTW. `QLibrary::load`/`resolve` can run
initializers, so loadability and FFTW symbols are deliberately **unchecked**.
File presence is not proof of ABI compatibility, a complete transitive dependency
set, usable audio, valid executables, or working hardware. The helper never tests
hardware implicitly or explicitly.

Only `checkWritableDirectory: true` authorizes writing, and it requires an explicit
`directory`. One unique `.obrii-cli-doctor-XXXXXX` temporary file is created in the
chosen directory, written with the fixed 23-byte ASCII marker
`obrii-cli-doctor-probe\n`, flushed, read back, and removed. Auto-removal remains
enabled as a fallback. No existing file is overwritten, and no directory is
created. Directory symlinks/junctions, including ancestors recognized by Qt, are
refused for this probe. Cleanup failure is reported as failure, never success.
Directory metadata (such as modification time) can change during a write probe.

Paths must be absolute, at most 4096 UTF-16 code units, without ASCII control
characters or `.`/`..` path components. Explicit UNC, device namespace, Qt resource, URL, and Windows device
basename/alternate-stream paths are rejected. Windows requires a drive-qualified
path. Host-managed mapped drives, mounts, symlinks, and filesystem races cannot be
proven local using portable QtCore metadata. The caller must choose trusted local
directories; do not point this diagnostic at network mounts or untrusted mutable
trees. Bounds limit operation counts, not filesystem latency or OS behavior.

## Request

All fields are optional; unknown fields and incorrect types are rejected. The
entire request is validated before any filesystem checks, writes, or network probes. Rejection
returns one `request` failure with code `invalid_request`; values are not echoed.

| Field | Type / Default | Meaning |
| --- | --- | --- |
| `directory` | absolute string / application directory | Inspect fixed filenames here, not recursively. Does not itself enable writing. |
| `checkWritableDirectory` | boolean / `false` | Opt-in temporary write/verify/remove probe. Explicit `directory` required. |
| `externalPaths` | array / `[]`, maximum 16 | Metadata-only checks of explicitly supplied paths, in input order. |
| `tcpPorts` | array / `[]`, maximum 32 | Distinct integer TCP ports in `1..65535`; no default ports are added. |
| `checkPorts` | boolean / `false` | Explicit opt-in to bind/close each supplied port on IPv4 loopback only. |

Each `externalPaths` element is an object with only these keys:

| Field | Type / Default | Meaning |
| --- | --- | --- |
| `path` | required absolute string | Same path validation as `directory`; no PATH or environment expansion. |
| `kind` | `file`, `directory`, or `executable` / `file` | File checks require a readable regular file. Executables also require Qt's metadata executable flag; Windows does not validate PE headers. Directory checks require existence/type only. |
| `required` | boolean / `false` | Unavailable required paths fail; optional ones warn. |

Without `checkPorts: true`, supplied ports remain `unchecked` and no socket is
created. With the opt-in, each supplied port is probed using
`QTcpSocket::bind(QHostAddress::LocalHost, port, QAbstractSocket::DontShareAddress)`
and immediately closed, including after failure. This checks only exclusive IPv4
loopback binding (`127.0.0.1`), not IPv6, wildcard/remote interfaces, listening,
connectivity, or port ownership. It sends no application data. With no supplied
ports, an explicit opt-in emits one `tcp`/`unchecked` warning without network access.

Bind success reports `passed` / `port_available` / `availability: available`.
The socket is already closed when reported, so the port can be claimed immediately
by another process; this is not a reservation or a promise of later availability.
`AddressInUseError` reports `failure` / `port_in_use` / `in_use`.
`SocketAccessError` reports `failure` / `port_permission_denied` /
`permission_denied` (an exclusive reservation can also produce this on Windows).
All other errors report `warning` / `port_probe_failed` / `unknown`, never an
unsupported claim that the port is occupied. Failed probes include the numeric Qt
`socketError`, not a native error string.

Example request (Windows; use local POSIX absolute paths on Unix):

```json
{
  "directory": "C:/Obrii/package",
  "checkWritableDirectory": false,
  "externalPaths": [
    {"path": "C:/Obrii/engines/decoder.exe", "kind": "executable", "required": false}
  ],
  "tcpPorts": [7355],
  "checkPorts": false
}
```

## Discovery

Fixed optional workers are `ObriiCliModule`, `ObriiCliPacketModule`,
`ObriiCliImageModule`, `ObriiCliSpectrumModule`, `ObriiCliM17Module`,
`ObriiCliTxModule`, `ObriiCliGnssModule`, `ObriiCliView`, and `ObriiIqHub`.
Windows adds `.exe`. Missing/unreadable/non-executable workers warn, never launch.

Required adjacent deployment candidates are Qt Core, Network, Gui, Concurrent,
Fobos, Fobos SDR Agile, single-precision FFTW, and (Windows only) libusb. Qt
Multimedia is optional. Windows required-file absence fails; Unix absence warns
because system locations are intentionally not searched. QtCore itself is known
running even if its adjacent deployment file is absent. This is a packaging check,
not a claim that every optional workflow needs every file.

Windows uses `Qt<major><component>[d].dll` (debug suffix follows this translation
unit), `fobos.dll`, `fobos_sdr.dll`, `libfftw3f-3.dll` or `fftw3f.dll`, and
`libusb-1.0.dll`. Linux checks Qt `.so.<major>`/`.so` and Fobos `.so.1`/`.so`,
FFTW `.so.3`/`.so`. macOS checks Qt `.dylib` or adjacent versioned frameworks,
Fobos `.dylib`/`.1.dylib`, and FFTW `.dylib`/`.3.dylib`. No recursive dependency
inspection, binary execution, or system installation lookup is performed.

Audio reports only `winmm_platform_support` on Windows, or a Qt Multimedia
compile-flag hint on other platforms (`FOBOSAPP_HAS_QT_AUDIO` or
`OBRII_CLI_QT_MICROPHONE`). These flags must be applied to this translation unit
for the hint to reflect the parent's build. Runtime audio is always unchecked.

## Report

Top-level keys are stable for schema 1:

- `schema`: `obrii.cli.doctor`; `schemaVersion`: integer `1`.
- `ok`: true exactly when no check has status `failure`. Warnings do not fail it.
- `platform`: `os` (Qt product type), `cpuArchitecture`, `pointerBits`, `byteOrder`.
- `qt`: `buildVersion`, `runtimeVersion`. No machine/host IDs or usernames.
- `policy`: false `hardwareAccess`, `processLaunch`, `libraryLoading`,
  `profileAccess`; true `pathsRedacted`. `networkAccess` and `loopbackPortProbe`
  are true only when a valid request actually attempts at least one opted-in
  loopback port probe; both are false for default/invalid/empty-port requests.
- `checks`: ordered array of `{id, status, severity, code, message, data}`.
- `summary`: `{total, passed, warnings, failures, status}`. Overall status is the
  worst check status: `failure`, then `warning`, then `passed`.

Check `status` is `passed`, `warning`, or `failure`; corresponding `severity` is
`info`, `warning`, or `error`. `data` is always an object. Consume IDs/codes/data,
not English messages. No timestamps, raw input, absolute paths, native error
strings, settings, secrets, or file contents are included. Unchanged fixtures
produce the same passive report (assuming unchanged host/runtime metadata).
Opted-in port probe results also depend on current OS port state/permissions.

IDs are `request`, `platform`, `qt.core`, `directory`,
`worker.<fixed-name>`, `dependency.qt.<component>`, `dependency.fobos`,
`dependency.fobos_sdr`, `dependency.fftw3f`, Windows `dependency.libusb`, optional
`directory.writable`, `fftw.runtime`, `audio.runtime`, zero-based `external.<index>`,
and `tcp.<port>` (or `tcp` for an explicit empty port request).

Codes: `valid_request`, `invalid_request`, `platform_reported`, `qt_core_running`,
`directory_present`, `directory_unavailable`, `worker_present`,
`worker_unavailable`, `file_present`, `file_not_found`, `path_present`,
`path_unavailable`, `unchecked`, `probe_passed`, `probe_unsafe_directory`,
`probe_directory_unavailable`, `probe_open_failed`, `probe_io_failed`, and
`probe_cleanup_failed`, `port_available`, `port_in_use`, `port_permission_denied`,
and `port_probe_failed`.

Library data records `method: file_metadata`, `required`, `foundAdjacent`, and
`loadabilityChecked: false`. Writable data records `attempted`, `cleanedUp`, and
(after opening) `bytes`. FFTW/audio data says `availability: unchecked`. Port data
includes `port`, `requested`, `attempted`, and `availability`. Attempted probes add
`address: 127.0.0.1`, `method: exclusive_bind`, and `reservationHeld: false`.
External data includes only index/kind/required, never its supplied path.

## Validation

`tools/cli_doctor_selftest.cpp` needs QtCore, QtNetwork and `clidoctor.cpp`, C++17. It
uses `QTemporaryDir` and inert fixture binaries, validates schema/counts/severity,
tests missing/complete packages, no-application and no-argument calls, external
path kinds, both port modes, array limits, strict invalid requests, path
redaction, deterministic output, repeated writable cleanup, and failed probe
cleanup/no directory creation. Recursive snapshots are confined to the test's
temporary tree and compare file hashes, permissions, and names. Unix additionally
tests refusal of a writable directory symlink. No real backend is used. The
explicit port fixture reserves an OS-assigned IPv4 loopback port with `QTcpServer`,
proves the opted-in bind is blocked, closes the fixture, verifies the probe passes,
then re-listens to prove the doctor's socket was released. No fixed machine ports
are actively probed; default and invalid requests must report no network access.

An isolated scratch CMake build can compile only these two sources against
`Qt5::Core` and `Qt5::Network`. Main/CMake/parser/help/package integration is deliberately left to
the parent task.
