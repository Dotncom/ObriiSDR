# Obrii Mesh Designer

`src/mesh` contains a separate Qt Widgets application, model/library persistence
and a process/session manager. It uses the existing Obrii CLI and IQ hub through
their public launch/control interfaces; no receiver/DSP implementation is copied.
The root CMake includes the designer; it also has an independent configure entry
for focused builds that do not replace the main GUI/CLI executables.

## What works

For already running Obrii instances on one or several computers, use the
separate distributed-instances mode described in [mesh_distributed.md](mesh_distributed.md).
Its independent RX/TX ports, per-link DATA/IQ and AFSK/LoRa modems are separate
from the local launcher/packet experiments below. Native Meshtastic PSK/PKI,
Reticulum/RNS and provisioned LoRaWAN use protocol-owned behavior rather than
the laboratory minimum-hop router.

- A canvas with movable virtual-SDR nodes, directed radio links, selection,
  Ctrl+wheel zoom and fit-to-content.
- Versioned project JSON with validation and atomic saves. Profiles live in a
  separate device library and nodes reference stable profile IDs.
- A **Device library** editor: add/edit/delete profiles, choose an installation
  directory, import/export JSON. Each profile records name, type, host address,
  SSH username/port and Obrii installation directory. No password, private key,
  session token or live process/port information is saved.
- Local session launch: one actual `ObriiCli run --backend virtual` per node and
  an `ObriiIqHub` process with the project's directed topology.
- Live listening-frequency, demodulation and bandwidth commands through the
  existing typed dispatcher on each CLI's loopback control port. Actual state
  is displayed separately from desired values; responses pin the instance ID.
- Independent RX start/stop for the selected node, keeping its CLI process,
  control identity, other receivers and medium alive. RX and the node's virtual
  test TX are independent; **Stop test TX** ends only that stimulus.
- A live spectrum dock for the selected node, with frequency/dBFS axes, range
  controls and its confirmed listening-frequency/bandwidth overlay. It reads
  all 1024 existing FFT bins through the CLI's exact `trace` command, not a
  separately computed or decimated FFT. Incomplete/invalid traces are rejected.
- Explicit finite CW stimulus from a selected node, using a directly owned
  `ObriiCliTxModule --tx-sink virtual` process. It cannot select a hardware sink.
- Asynchronous readiness, bounded control queues/timeouts/logs, session-generation
  protection, cancellation during startup, restart and cleanup of owned processes.
  Closing the window stops its session; it never scans for or kills other Obrii PIDs.
- Live gain, delay and enabled-state edits for existing links, with an explicit
  **Apply link** button and confirmed hub values/revision. Amber links have
  unapplied desired values; their line style still follows the last confirmed
  enabled state. No RX or hub process is restarted for these changes.

The initial A–B–C graph describes radio reachability: A's test CW reaches B,
but C needs an explicit relay. The separate Packet network window now supports
both reference packet routing and actual virtual-IQ modem relay, described below.
These local laboratories do not implement a wireless MAC or learned routing.
Native adapters have their own routing/ACK behavior and separate limits.

## Native And Hybrid Nodes

1. With the session stopped, select each node's protocol and independent RX/TX
   ports in the inspector. Use the port/protocol JSON editors for additional
   interfaces. A project carries public settings and a host-local `profileId`,
   never protocol keys, PKI pins, Python paths or RF authorization.
2. Select each edge's DATA/IQ bearer, TX/RX port IDs and modem. `inherit` uses
   the distributed window's session default. Enabled TCP edges require network
   bindings and matching protocols; physical ports are separate local bridges.
3. Start the optional node service on each actual GUI/CLI/NodeHost instance,
   with private node options for protocol profiles, state and permissions. Open
   the distributed-instances window, discover/register or enter endpoints, and
   connect. The designer configures/observes; it does not relay native traffic.
4. Use the Protocol tab for native `op` commands and inspect native events/state.
   Meshtastic supports PSK and explicitly pinned PKI; RNS uses an isolated local
   runtime with a default 15-second startup/readiness budget. Native IDs, ACKs
   and proofs must not be read as laboratory TTL/visited paths.

LoRaWAN supports one provisioned EU868 device/network pair, ABP or OTAA, Class A
over virtual DATA or actual LoRa IQ, with one native interface and one outgoing
peer per node. Host-modeled timing defaults to 250 ms tolerance;
`rx_context`/`tx_started` are host-only callbacks, not Protocol-tab commands.
Events report `timingSource: "simulated-transport"` and
`hardwareTimingVerified: false`. Physical LoRaWAN RX/TX is blocked; this is not
a certified radio MAC. Meshtastic/RNS physical bridges require explicit local
RX permission or a local TX profile plus bounded RF arming; Fobos is RX-only.
See [ports and RF permissions](mesh_hybrid_ports.md),
[native commands](mesh_protocol_adapters.md) and
[LoRaWAN profiles/timing](mesh_lorawan_adapter.md).

## Device presets

The default library is `devices.json` in Qt's application-config directory for
`Obrii/ObriiMeshDesigner`, separate from the radio's settings and from project
files. Override it with `ObriiMeshDesigner --library PATH`.

Use **local** for an installation on this computer. Set its directory to the
folder containing `ObriiCli`, `ObriiIqHub`, and `ObriiCliTxModule` (with `.exe`
on Windows). Other Obrii applications can remain next to them. Different local
installations may have separate profiles. Prefer absolute installation paths.

Use **SSH / Linux / Raspberry** to prepare a remote device: address, username,
SSH port and absolute POSIX installation directory, e.g. `/opt/obrii/bin`.
Remote profile persistence is implemented, but launching a scenario with an
SSH profile is explicitly rejected before creating any process. No unattended
SSH, credential lookup or remote process is attempted in this version.

Profiles are reusable across scenarios. Import the matching library before
opening a project with its profile IDs; deleting an in-use profile is rejected.
The examples `mesh_devices_example.json` and `mesh_project_example.json` use
placeholder paths and a reserved documentation IP address; edit them first.

## Live changes and current limits

Frequency, demodulation and bandwidth can be applied to a running receiver.
They are individually acknowledged, not an atomic transaction. Failed or
unapplied settings remain visible by comparison with confirmed RX state.
The CW level applies to the next stimulus, not a continuous running transmitter.
Reconfiguration briefly centers the audio listener, applies mode/defaults and
final bandwidth, then restores the desired listener. A short audio discontinuity
is expected; WFM needs a session rate above 200 kS/s for the CLI's mode preset.

Select a node to inspect its spectrum; the spectrum and session log share the
bottom dock tabs. Turn **Live preview** off, close/switch away from the spectrum
dock, or minimize the window to stop trace polling. Only one selected node is
polled, at most twice per second; there is no additional FFT worker. The designer
renews an explicit 1500-ms `cli.preview` lease on that node's existing FFT, with
a 500-ms interval, and cancels it on deselection/disable. If the designer crashes,
the lease expires by itself. Other independently requested FFT consumers remain
unaffected. Existing node status polling continues. The preview clears on RX stop, failed reads or
stale control responses rather than showing an old signal as live. Range
controls affect only this display, not receiver data or measurements. Frame
sequence and computation timestamps prevent repeated reads of an old trace from
being displayed as a fresh signal. Use the updated designer and CLI together.

**Start RX** / **Stop RX** use the node's existing receiver commands. They do
not kill/relaunch its CLI, disable radio links or implement a simulated node
failure; stopping RX does not stop that node's test TX. Session stop still
cleans up every process owned by the designer.

The source center and sample rate are common to the session and editable while
stopped. Link gain, delay, enabled state and frequency-offset edits are staged
until **Apply link** is pressed. In a local **Sum IQ** session, select a link and
set **IQ frequency offset** (`Частотний зсув IQ`) to shift its received carrier.
Use positive Hz for a higher frequency, negative Hz for a lower frequency,
and zero to clear the shift. The magnitude must be below half the sample rate.
The saved project retains the value for its next local Sum IQ session.
The packet lab and remote node sessions use their own channel settings.

Set **IQ clock mismatch** (`Розбіжність тактів IQ`) on each link to model
the source sample clock relative to the receiver. The range is −1000 to
+1000 ppm. For example, +100 ppm at 48 kS/s models a 48,004.8 S/s source
on the shared 48 kS/s output grid. The signal becomes slightly shorter
and a baseband tone rises in frequency. A negative value lengthens the
signal. The fractional-delay FIR uses history across IQ frames and applies
a small filter delay. A live edit changes the rate for new IQ, keeping the
sample position already reached. Set zero for no further drift. A stream
restart or discontinuity anchors the link to its new timestamp.
With sustained drift the mixer still uses its configured playout buffer.
The hub validates both the fan-out and sample-mix paths before mutation, including
the active mixer buffer capacity. Rejected changes leave the previous link and
revision unchanged. Nodes and links can now be appended while running; new nodes
start disabled until their receiver endpoint is registered and explicitly enabled.
Removing/reordering nodes or changing the common medium still requires Stop.
The node's **Enabled** checkbox disables its owned RX/test-TX workers, unlike
**Stop RX**, which pauses only reception. An RX/control/test-TX failure is local
to that node: healthy workers and the hub remain alive. Recovery is explicit;
neither RX failure nor re-enabling a node automatically restarts its test TX.
If live receiver registration was unconfirmed, another enable attempt first
queries the same authenticated hub and reconciles the exact endpoint. The node
remains disabled after reconciliation; enable it again to start RX. A conflicting
or unverified endpoint is rejected rather than silently adopted.
Live link edits are not a routing protocol.
Changes affect newly published frames, not contributions already in the queues
or mixed blocks. The bounded old-condition tail drains normally. Acknowledgment
is a control-plane boundary, not a sample-timestamped scheduled channel change.
Mixing requires the exact same rate/center for all transmitters. The current
backend caps a run at eight mixed-IQ RX nodes or 32 frame-forwarding RX nodes;
the project format/editor does not inherit that runtime node cap. Resource
estimation, distributed clocks, resampling and sharding are future work.

All listeners in this local launcher mode bind to `127.0.0.1`. RX arguments
select a virtual backend; the explicit test transmitter selects a virtual sink. Local
physical receivers and existing GUI/CLI sessions are not attached or modified.

## Hub Control Protocol

The optional hub control listener is disabled unless both `--control-port PORT`
and `--control-key-file PRIVATE.json` are supplied. It always binds IPv4 loopback,
independently of the publisher/data address. The private JSON contains a `token`
string of 64 random hexadecimal characters; do not publish it or pass the token
itself on the command line. This authenticates control only: the IQ data stream
is still unencrypted and unauthenticated.

The designer generates a private per-session key file, removes it after the
authenticated readiness reply, keeps the key only in memory and clears it on
stop. Keys, ports, instance IDs and revisions never enter project/device files.
An old hub lacking this interface fails startup rather than pretending readiness;
use the updated designer and `ObriiIqHub` together.

Each TCP connection accepts one newline-terminated JSON object and returns one
JSON line. Request example (placeholders, not usable credentials):

```json
{"version":1,"id":"request-1","token":"<64 random hex characters>","command":"status"}
```

An authenticated reply has `ok`, `id`, `instanceId` and a decimal-string
`revision`. `status` includes confirmed `links`, per-RX bounded queue/drop/filter
counters and the current medium mode. Pin `targetInstanceId` on subsequent
requests. A `setLink` request also requires the exact `expectedRevision` string
and an `args` object with all five fields: `tx`, `rx`, `enabled`, `gainDb`,
`delayMs`. In sample-mix mode, optional `frequencyOffsetHz` sets the carrier
shift and `clockPpm` sets the sample clock error for that link. Omit a field
to keep its value or send zero to clear it.
Successful updates increment the revision and return the applied
`link`; conflicts require a fresh status before retry. The API supports gain
-120..60 dB and delay 0..60000 ms, subject to actual buffer capacity; the designer
deliberately limits its delay editor to 500 ms. `addReceiver` accepts exactly
`id`, numeric loopback `host` and `port` in `args`, with the same instance and
revision checks. It appends a disabled node's receiver endpoint without replacing
existing subscribers. Duplicate IDs, capacity limits or host rejection leave the
models unchanged. `getReceiver` with `args: {"id":"node4"}` can reconcile an
unconfirmed registration against the pinned hub. Receiver removal, default
routing, mix geometry and arbitrary process commands are not editable here.

Control is limited to eight simultaneous connections, 64 requests/second,
64 KiB input, 256 KiB output and a two-second socket lifetime. It runs on the
hub event loop and never copies buffered IQ or adds a per-sample mutex. With
control disabled, no control listener or polling task is created.

## Build and test without disturbing the main build

Windows, using the same Qt SDK as the main program:

```powershell
./tools/build_mesh_designer.ps1 -Test
./build/runtime/ObriiMeshDesigner.exe
```

The script uses `build/mesh-designer`, copies the designer, packet simulator
and node-host EXEs into `build/runtime`, and adds Qt dependencies only when
absent. It never rebuilds or replaces the main GUI/CLI executables.
Integration tests copy the existing
CLI/hub/TX binaries into a private test directory so their running processes
do not lock files used by the parallel main build.

`-Test` builds and runs all 19 mesh suites from one shared target list, including
`ObriiMeshPortPlanSelfTest`, `ObriiMeshPortNodeSelfTest`,
`ObriiMeshWireModemSelfTest`, `ObriiMeshProtocolAdapterSelfTest`,
`ObriiMeshRadioEgressSelfTest`, `ObriiMeshLoRaWanAdapterSelfTest` and
`ObriiMeshLoRaWanNodeSelfTest`, both live-topology suites and the existing
designer/packet/IQ/node/remote/discovery suites, plus routed discovery, directory
leases and actual TCP fallback/catalogue integration. The live tests cover node/link
failure isolation, alternate known routes, disabled-node append/rejoin and
rejection of stale updates. The LoRaWAN node suite runs both ABP/OTAA and DATA/IQ,
without subset flags. Reticulum cases use the checkout's local runtime when
present; otherwise the adapter suite explicitly reports them skipped. The
script does not provision that runtime. These software tests do not qualify RF.

Standalone Linux/Qt 5 build:

```bash
cmake -S src/mesh -B build/mesh-designer
cmake --build build/mesh-designer -j2
QT_QPA_PLATFORM=offscreen ./build/mesh-designer/ObriiMeshSelfTest
./build/mesh-designer/ObriiMeshDesigner
```

An optional first self-test argument is a directory with CLI/hub/TX binaries;
it enables the real three-node IQ/command/lifecycle tests. An optional second
argument saves a designer preview and a device-library preview. On Windows,
the native `windows` Qt platform renders text correctly for those previews.
The self-test covers JSON/library/UI validation, direct-link IQ delivery,
absence of fake relay, live applied state, stop/restart and cancelled startup.
It also exercises independent node RX pause/resume with unchanged process and
instance IDs, renewed IQ reception, selected-node trace delivery, no background
polling for unselected/disabled previews and the production spectrum widget.
Live topology tests cut a link, verify filtering of a new TX, restore gain/delay,
resume actual IQ delivery and check unchanged process/session identities. The
production Apply button is tested too. `ObriiVirtualIqHubControlSelfTest` checks
authentication/revisions, bounds, actual gain/delay/filtering in both engines,
queued-tail preservation and atomic capacity rejection.
Windows runtime coverage does not qualify Linux/Raspberry builds or RF hardware.

The root build already includes `add_subdirectory(src/mesh)`. It remains one
repository and can link shared modules as its protocol needs grow; the independent
configure entry is an isolation mechanism, not a fork of Obrii's radio code.
`OBRII_MESH_BUILD_UI` defaults ON for designer/root builds; OFF omits Widgets,
the designer and its UI tests while keeping the shared node service and
headless tests. The standalone CLI selects OFF when adding the shared mesh
targets; its other Qt dependencies remain unchanged. See
[standalone CLI builds](obrii_cli.md#standalone-cli-build).

SSH process lifecycle and hardware qualification are not provided by the local
launcher. Live local additions use an authenticated, revision-guarded loopback
receiver registration; existing socket callback addresses stay stable. A failed
RX/TX process disables only that node; explicitly enable RX to recover it, never
automatically restart TX. Hub failure remains a session-level failure. Attaching already running remote nodes
is a separate supported workflow, not automatic SSH launch or learned routing.

## Packet Network Laboratory

**Packet network** in the designer toolbar opens a separate, modeless packet
simulation. No IQ session, hardware, network socket or external process is
started for it. It snapshots node IDs/names and directed enabled/delay links
from the constructor. When an IQ session is running, the initial links use the
hub's last confirmed values rather than unapplied editor values. An already
open packet lab remains independent: changing its links or node availability
does not stop RX, change IQ links or modify the constructor project. Close and
reopen it for a new constructor snapshot.

Choose source/destination, text and TTL, then **Send**. The default A-B-C graph
has no A-C edge: B receives, checks duplicate identity/TTL, selects its next hop
and queues a new transmission toward C. The results table shows actual visited
nodes, terminal status, hops and virtual latency; selected payloads can be viewed
as plain UTF-8 or exact HEX. The **Nodes** tab includes queues, receive/forward/
delivery/drop counters and explicit simulated failures. **Links** toggles a
directed link or changes its delay. Break B-C to obtain `no_route`; restore it
and send a new packet to resume delivery. A packet already in transit checks
the edge and both endpoints at arrival and can fail as `link_down`/`node_down`.

Routing is a centralized **known-graph, minimum-hop reference model**, with
lexical node-ID tie-breaking. Each relay computes the next hop from the current
graph, and topology changes invalidate route caches. It is not AODV, OLSR,
distributed discovery or a model of their convergence/control traffic. Delay
is one base virtual millisecond plus the directed link delay per hop. The
packet engine uses a single shared channel with no gain, frequency, IQ,
modulation, automatic ACK/retry or random errors. The virtual IQ hub models
RF samples separately.

In **Nodes**, select a row and use **Coordinates and velocity…** to set the
current position in metres and constant velocity in metres per second.
The X/Y values advance with virtual time. A positive **Link radius** limits
the existing directed links to nodes within that distance. The default
**Unlimited** uses the directed graph alone. A relay can move out of range
and later restore a route. The arrival check reports `out_of_range` when a
packet's link exceeds the radius in transit. This model applies in packet
mode and uses the coordinates entered here. Constructor positions are
screen coordinates.

In the same tab, set **Air bitrate** to a positive value (up to 1 Mbit/s)
to add airtime for the payload and eight bytes of simulated overhead per hop.
The transmission occupies its sender for that airtime, rounded up to a
millisecond. Link delay is added after airtime. If another transmission
starts before airtime ends and its sender can be heard over an enabled,
in-range link to the receiver, the affected reception ends as `collision`.
A node transmitting during a reception loses that reception as
`half_duplex`. Attempting to send again from a busy transmitter yields
`tx_busy`. Audibility is evaluated at the start of the overlap. The model
assumes simultaneous arrival on the shared channel and reports per-hop
results with no power capture or MAC backoff. Zero disables the airtime
model. Change the bitrate with an empty queue.

**Pause** stops the GUI clock; **Step** processes one scheduled event and jumps
to its due time. When only node motion remains, **Step** advances by 20 ms.
Automatic mode advances in bounded 20-ms virtual ticks, at most 512 events per
UI callback. It stops its timer when queued work and node motion have ended.
New commands settle events already due at the current time before applying the
command; new-hop delays are at least one millisecond. Wall-clock execution may
slow under load and is not the latency reported by the simulation.

**Scenario** pauses and saves the topology, chronological send/link/node/motion/range/airtime actions
and current virtual stop time. **Results** saves counters, retained results and
hop trace. Export settles currently due events; future deliveries can remain
pending. **Open** validates/replays a packet scenario using the same core and
leaves the GUI paused; pending traffic can continue with Run/Step. Import/replay
is bounded but synchronous; use the console tool for larger batch experiments.
**Reset** discards the experiment and restores its initial topology. Exports
include user-provided packet payloads and node names; treat them as private.
They contain no radio credentials, host profiles or running process identities.

### Headless Replay

```sh
ObriiMeshSim --scenario docs/mesh_packet_example.json --output packet-results.json
```

The included example delivers A-B-C, breaks B-C and repairs it: two successful
deliveries, one `no_route`, two relay forwards, mean successful latency 32 ms.
Without `--output`, JSON results go to stdout. Failures in input/schema/replay
return a nonzero exit status without replacing an existing output file. Packet
delivery failures are experiment outcomes, not CLI syntax errors. The executable
uses only Qt Core and the shared `ObriiMeshPackets` library; no display server,
receiver or network connection is required. It is a companion command-line tool,
not an alias silently added to the main GUI command catalog.
Windows builds place it in `build/runtime/ObriiMeshSim.exe`; the root Linux build
places it in `build/linux-release/src/mesh/ObriiMeshSim`. Installation puts the
executables together in `bin`.

Scenario schema is `obrii-packet-scenario`, version 1. `topology.nodes` contains
`id`, `name`, optional `enabled` (default true) and optional `motion`
(`xM`, `yM`, `vxMps`, `vyMps`). Links contain `tx`, `rx`, `enabled` and
`delayMs` (0..60000). Optional `topology.radioRangeM` and
`topology.airBitrate` set the initial radius and bitrate.
`actions` must be chronological whole virtual
milliseconds via `atMs`. Operations are `send` (`id`, `source`, `destination`,
`ttl`, canonical `payloadBase64`), `link` (same fields as a topology link), or
`node` (`id`, `enabled`), `motion` (`id`, `motion` at that time), or
`range` (`meters`), or `airtime` (`bitrate` in bits per second).
Optional `untilMs` preserves a partial experiment;
without it replay drains the queue. Equal-time events/commands have stable order.
Duplicate identity is `(source, packet ID)`; retries with that ID can be discarded
while present in a node's finite deduplication window. There is no automatic ARQ.

Bounds: 64 nodes, 4096 payload bytes, TTL 0..64, 256 queued events per node,
8192 globally, 4096 recent IDs per node, one hour virtual time, 10000 actions
and 7 MiB recorded action data. Local source=destination delivery is allowed at
TTL zero; remote forwarding decrements TTL per hop. Evidence retains the latest
512 terminal outcomes and 4096 hop events with explicit eviction counters; the
GUI shows the latest 256 outcomes. Global counters include evicted outcomes.
Per-node `received` includes locally injected ingress and relay arrivals;
`forwarded` counts successful non-origin forwarding. `queued` counts scheduled
node events, including in-flight arrivals, not a physical receiver FIFO.
The JSON loader accepts at most 16 MiB. Invalid configuration/replay never
partially replaces the active experiment. No optional packet work occurs when
the dialog/tool is not running. Paused GUI simulations have no active timer.

`ObriiPacketSelfTest` exercises exact binary delivery, per-hop delays, TTL,
duplicates, node/link failure, motion across the link radius, route repair at an intermediate relay, queue
overload, evidence/action bounds, time/ID exhaustion and identical full/partial
scenario replay. `ObriiMeshSelfTest` also drives the production packet dialog,
pause/step/reset and independent failure controls without starting IQ processes.

## Packet Routing Through IQ Modems

In **Packet network**, select **IQ · AX.25 / AFSK1200 / FM**, wait for ready,
choose source/destination, enter UTF-8 text and press Send. The initial A-B-C
topology sends A to B and retransmits from B to C only after B decodes the frame.
The dialog starts its own loopback-only virtual medium, independently of the
designer's process-based CW/receiver session. It does not inject into an already
running external CLI/GUI receiver. It snapshots the constructor's topology and
link gains (confirmed values when its main virtual session is running).
Imported packet-only scenarios have no IQ gains and start at -6 dB per link.

Each hop encodes a versioned binary envelope inside an AX.25 UI frame with FCS,
uses the shared AFSK1200 encoder and NFM CF32 modulation, sends serialized samples
through TCP and the shared sample-mixing VirtualIqMedium, then feeds an actual
CliVirtualIqSource virtual receiver. Shared FM discrimination, AFSK decoding and
FCS checking must recover the envelope before the router accepts the arrival.
Only sender-node routing metadata accompanies IQ frames: packet bytes, hop
tokens and payload are inside the modulated samples, never an IQ metadata bypass.
The receiver also checks session, pending hop, addresses, ID, TTL and payload.
These are experiment identity checks, not cryptographic authentication.

First-version bounds: 1..8 nodes; fixed 48 kS/s, 145.5 MHz virtual center and
3 kHz FM deviation; 1..256 payload bytes; existing directed link gain and
0..500 ms delay; one datagram in flight with serialized bursts. RX uses a fixed
-60 dBFS carrier-power gate, a bounded 10-second burst and a quiet tail to finish
decoding. Missing reception expires after 15 seconds per hop as `modem_timeout`;
worker failure produces `modem_stopped`, not synthetic delivery. Link/node edits
are available between datagrams. Reset cancels the old worker/session before
creating new endpoints; late old callbacks cannot complete the new experiment.

In IQ mode, set **Повторів IQ** to 0–2. A timeout resends the same hop with
a fresh flight ID and the same TTL. Enable **Обходити недоступні зв’язки 30 с**
to route later datagrams around a hop that exhausted its retries. Both
settings default to off. The packet engine observes decoder outcomes locally
and selects a path from the existing topology.

IQ mode follows real elapsed time: Pause/Step and deterministic scenario import/
export are unavailable. Results export retains actual paths/outcomes, modem and
sample counters, and the IQ topology/gains. It is evidence of this run, not a
promise of byte-identical wall-clock replay. ObriiMeshSim remains the independent
packet-only reference runner. The optional worker and network endpoints exist
only while IQ mode is selected; its pump stops while idle. Ordinary RX/FFT is
not involved and no physical SDR or RF transmitter is opened.

ObriiMeshIqSelfTest verifies segmented CF32 demodulation, FCS rejection of damaged
IQ, actual TCP A-B-C modem relay with binary payload, muted-channel timeout,
cancellation and clean worker shutdown. ObriiMeshSelfTest drives the production
mode selector, Send and Reset as well as the packet-only GUI.

### Local IQ Hardware Limits

This fixed-AFSK packet laboratory exposes no physical sink. Its internal OBRII
AX.25 address is a laboratory identifier. On-air acknowledgements, carrier
sensing and exchange of routing advertisements would require an RF link protocol.

The separate distributed node service already has AFSK/LoRa WireModem paths,
locally authorized RX capture and gated native Meshtastic/RNS UHD/HackRF TX
ports. Those do not turn this laboratory into an on-air mode. Fobos still needs
a separate transmitter; physical LoRaWAN remains blocked. Use the
[hybrid port workflow](mesh_hybrid_ports.md), not a hardware bearer switch here.
