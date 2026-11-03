# LoRaWAN Protocol Adapter

`src/mesh/meshlorawanadapter.cpp` implements
`Mesh::makeLoRaWanAdapter(QObject *)` against `ProtocolAdapter`.
It carries native LoRaWAN 1.0.4 / 1.1 PHYPayload bytes directly, with no Obrii,
Reticulum, Meshtastic, AX.25 or mesh-routing wrapper.

## Supported Mode

NodeService instantiates this adapter for `protocol.type: "lorawan"`; the
shared `ObriiMeshNode` target already links `ObriiLoRaWAN`. It supports one
provisioned EU868 device/network pair, ABP or OTAA, unicast Class A/B/C over virtual DATA
or actual LoRa IQ. This is not mesh routing, a multi-device network server or
a certified radio MAC. Physical LoRaWAN RX/TX is rejected, even with local RF
permissions, until qualified timestamped SDR integration exists.

Use one native interface (default `radio`), with network RX and TX ports sharing
that ID and exactly one outgoing peer on each node. Configure both directed
edges for uplink/downlink. In the designer, select LoRaWAN and a local profile
ID, attach the services, then use the Protocol tab for `join`, `send` and status.
Check the service's advertised `nativeProtocols` and readiness; the UI's backend
label alone is not a readiness or hardware capability guarantee.

The adapter itself opens no sockets or radio devices. One node worker drives
its calls and emits native `transmit(ticket, interfaceId, wire)` requests;
NodeService/LinkSender owns transport and modeled timing.

## Local Profiles

Provision an explicit **local operator profile** through private node options
(`--node-options FILE` or local `mesh.service start` with `optionsFile`). The
project stores only `{"type":"lorawan","profileId":"device-profile"}`.
NodeService resolves `protocolProfiles[profileId]`, strips public selectors and
rejects secret fields from remote topology. Each role needs its own approved
profile and private state; no secret belongs in a scenario/export. The adapter
receives resolved fields, not a profile path, and never returns its configuration.

Common profile fields:

| Field | Values |
| --- | --- |
| `role` | `device` or `network` |
| `activation` | `abp` or `otaa` |
| `version` | `1.0.4` (default) or `1.1` |
| `deviceClass` | `A` (default), `B` or `C`; actual class follows negotiation/clock readiness |
| `interfaceId` | Optional; defaults to the one host interface, or `radio` without a host list |
| `initializeState` | Defaults false; explicit `true` is required to provision new state |

ABP requires `devAddr` (8 hexadecimal digits or a nonzero uint32),
`nwkSKeyHex` and `appSKeyHex` (32 hexadecimal digits each). Optional
`initialFCntUp`/`initialFCntDown` are 0..65535; restored state always wins.
Subsequent full 32-bit counter reconstruction/rollover is handled by `Session`.

For **1.1 ABP**, replace `nwkSKeyHex` with explicit `fNwkSIntKeyHex`,
`sNwkSIntKeyHex`, `nwkSEncKeyHex`; keep `appSKeyHex` and `devAddr`.
`initialFCntDown` initializes NFCntDown; optional `initialAFCntDown` initializes
AFCntDown (0..65535, restored state wins). A 1.1 OTAA profile requires
`nwkKeyHex` **and** `appKeyHex` plus the same EUIs. Root/session key fields
cannot be mixed. Version is bound into private state identity; changing versions
does not reuse an old session or reset its counters. Old 1.0.4 state is readable.

OTAA requires `joinEuiHex` and `devEuiHex` (16 hexadecimal digits each), and
`appKeyHex` (32 hexadecimal digits). The single-device network role additionally
requires `devAddr` and `netId` (6 hexadecimal digits or a uint24). Device OTAA
does not accept an assigned address or imported ABP session fields.

There are no implicit or public demonstration keys. Test keys occur only in
the headless self-test's explicitly constructed local profiles. Credentials,
session keys, key hashes, private state blobs and root paths are absent from
adapter status, command responses and events. Received application payload is
released only after native MIC/counter validation and persistence.

Optional EU868 settings are `frequencyHz` (default 868100000), `dataRate`
(initial DR0..5, default DR5), `nbTrans` (1..15), `rx1Offset` (0..5),
`rx2DataRate` (0..6), `rx2FrequencyHz` (default 869525000), `receiveDelay`
(1..15 seconds), and boolean `adrEnabled` (default true). All initial uplink
frequency/rate pairs must be enabled module channels. Restored regional state
overrides initial settings; a fixed host uplink frequency disabled by MAC
commands requires host reconfiguration, not an invented channel route.

NodeService's **modeled transport default** is `clockToleranceMs: 250`; an
explicit profile value is preserved and validated (1..250 ms). The direct
adapter API, without NodeService, retains its stricter 10 ms default. Neither
value establishes physical-radio timing accuracy. Other timing settings are
`rxContextMaxAgeMs` (1..120000, default 10000) and
`txCompleteTimeoutMs` (1000..180000, default 120000).
Unknown fields, wrong types and unsupported combinations are rejected.

Class B settings: `pingPeriodicity` 0..7 (default7), `pingDataRate` 0..6
(default3), `pingFrequencyHz`/`beaconFrequencyHz` (default869525000), and
`clockErrorPpm` 0..1000 (default20). The clock tolerance is shared with the
host capture windows. Beacon PHY is fixed to EU868 SF9/BW125; ping PHY follows
the configured data rate. Class C RXC uses `rx2FrequencyHz`/`rx2DataRate`.

## Durable State

`localStateDirectory` must be an existing absolute private directory dedicated
to this one adapter. Relative roots, symlink/junction traversal, nonregular
state files and concurrent ownership are rejected. On POSIX the directory must
have no group/other permissions (0700). On Windows the host must supply an
appropriately restricted ACL; Qt owner permission bits are not a Windows ACL
security guarantee.

`QLockFile` owns `lorawan.lock`. `lorawan-state.json` contains the module's
bounded `saveSession`, `saveDeviceJoin` or `saveJoinServer` image plus
`saveRegion`, `saveDuty` and pending MAC answers in a single checksummed,
profile-bound snapshot. `lorawan-initialized` prevents a missing state record
from silently creating fresh counters in an already initialized directory.

Writes use `QSaveFile`, disabled direct-write fallback, owner-only file bits,
explicit OS file synchronization, atomic commit, and POSIX directory fsync.
Counter/nonce and duty reservations commit **before** native TX bytes escape.
Region/MAC changes commit with the authenticated receive counter before payload
events escape. Storage failure disables the adapter, retains ownership and
requires explicit stop/start reload. An ambiguous committed write is never
retried through the old module instance.

State includes secret keys and is not encrypted. Its checksums detect
corruption, not malicious modification or rollback. Do not restore old
snapshots, delete both state and initialization marker, or reuse a root/key
context after counter exhaustion. Secure storage, durable filesystem behavior,
backups, ACLs and protection against rollback belong to the host/operator.

Duty/cooldown deadlines are saved as remaining intervals and rebased on the
first monotonic call after restart. Offline time is not deducted, conservatively
retaining the previous reservation. Join backoff uses accumulated recorded
active uptime; no wall-clock or regulatory certification is claimed.
Application queues, receive windows and pending encrypted JoinAccept responses
are volatile. Restart may consume a reserved counter/nonce without sending;
it must never reuse it. A device retries a lost join using a new DevNonce.

## Commands

All commands use `op`. Errors return `{ "ok": false, "error": "..." }`.

- `status`: Public state only; `type`/`protocol` are `lorawan`, with role,
  activation, readiness, DevAddr/EUI, queue sizes, counters, PHY templates and
  current receive windows. `ready` is not equivalent to OTAA `activated`.
- `join`: OTAA device only, before activation. Enqueues a native JoinRequest.
- `send`: Activated device/network; exactly one of `text` or canonical padded
  `payloadBase64`, optional `port` (1..223, default 1), and boolean `confirmed`.
  Network sends use an authenticated uplink's Class A window, a negotiated
  Class B ping slot, or the active Class C RXC channel.
- `repeat`: Activated device only, idle queue/transport, and only while the
  module permits another NbTrans copy. Reuses ciphertext/FCnt; 1.1 MIC follows
  actual DR/channel. For 1.0.4 all native bytes are identical.
- `send_mac`: Network only, canonical `macBase64`. Currently supports only
  LinkCheckAns, DevStatusReq, DeviceTimeAns and (1.1) ADRParamSetupReq. Regional parameter negotiations
  are rejected because network-side negotiation tracking is not implemented.
- `cancel`: Cancels queued requests, not an already emitted native transmission.
- `set_class`: Idle activated device/network pair; `class: "A"`, `"B"` or `"C"`.
  Optional `periodicity` applies only to B. Device B first returns to A and
  negotiates PingSlotInfo before setting its ClassB uplink flag. For 1.1 A/C,
  DeviceModeInd repeats until matching DeviceModeConf. On the network role this
  command is explicit out-of-band peer provisioning, useful for 1.0.4 peers;
  configure both sides consistently. It cannot create beacon synchronization.
- `beacon`: Network only; aligned `gpsSeconds` (uint32 divisible by128) and
  optional `ttlMs`. Enqueues one native EU868 beacon at the next eligible step.
- `beacons`: Network only; `enabled: true, gpsSeconds: 128` starts a modeled
  GPS/monotonic mapping at the current host time and sends periodic beacons
  every 128 seconds; `enabled: false` stops it. Late slots are skipped, not sent
  under a fresh timestamp. This scheduler and clock mapping are volatile: restart
  requires explicit clock provisioning. No OS clock/GPS accuracy is implied.

For example, use the Protocol tab on an activated device to send
`{"op":"set_class","class":"B","periodicity":0}` and enable network beacons
with `{"op":"beacons","enabled":true,"gpsSeconds":128}`. The device must
receive PingSlotInfoAns and a beacon, then send an uplink announcing ClassB
before the network uses ping slots. Status reports `requestedClass`, actual
`deviceClass`, `beaconLocked`, `pingNegotiated`, `nextPingUs`, `version`, and the
separate 1.1 application downlink counter. Existing `send` queues work in all
classes; Class C can send downlinks without a fresh uplink opportunity.

`rx_context` and `tx_started` are **host-only callbacks**, not application
commands. NodeService explicitly rejects both through remote `op: "protocol"`,
including authenticated requests. Their adapter API forms below are for host
integration, not JSON to paste into the designer's Protocol tab.

`join`, `send`, `send_mac` and `repeat` accept `ttlMs` (1..3600000, default
60000). At most 16 requests and one in-flight transmission are retained.
Data-rate payload limits are checked before native counter reservation. MAC
answers take priority, use FOpts or FPort 0, and do not silently consume an
application request sent later. Pending ACK/MAC-only device uplinks can be
scheduled automatically once cooldown and duty permit. There is no background
timer or automatic confirmed-message retry engine.

## Host Time And PHY

Call `step(nowMs)` with monotonically nondecreasing host time, using the same
epoch for commands, completion and capture metadata. Negative/backwards or
out-of-range time disables the adapter. Call often enough to meet a planned
RX1/RX2 transmission start; missed RX1 can fall back to RX2. Expired windows
cannot manufacture a new downlink opportunity.

Before every receive, the host supplies one-shot capture metadata directly to
the adapter:

```json
{
  "op": "rx_context",
  "frequencyHz": 868100000,
  "dataRate": 5,
  "packetStartMs": 1000,
  "packetEndMs": 1062
}
```

For an EU868 beacon the host adds `"beacon": true` to `rx_context`, uses data
rate3 and the configured beacon frequency. Its 17-byte payload is handled by
the beacon codec before any JoinRequest classification. `tx_plan`/`tx_started`
advertise `frameKind: "beacon"` and the implicit-header modem configuration.
NodeService validates/preserves these through both DATA and IQ; external
applications still cannot submit host timing callbacks. Common CRC is checked;
beacon events explicitly report `authenticated: false`. Monotonic/GPS continuity
is checked after initial acquisition, including GPS uint32 wrap. A new mapping
must not be invented after receiving an inconsistent timestamp.

These describe host-asserted preamble start and packet-end times. Physical
integration would require actual on-air capture timestamps; the current network
mode supplies modeled times as described below, not decoding-completion times.
The callback's `nowMs`
must be at/after packet end, within the configured context age. The adapter
consumes metadata once, verifies duration against the module's PHY airtime,
and checks direction, channel/rate and device RX1/RX2 packet-start windows.
Missing/stale/mismatched context produces an explicit rejection. A network
uplink's captured end anchors its device's downlink windows.

Before `transmit`, a `tx_plan` event supplies ticket, frequency, data rate,
provisional on-air start/end microseconds and exact modem parameters. It also
includes `airtimeUs` and `startDeadlineUs` as decimal strings. The latter is the
earliest of request expiry, the preparation-time budget, and (for a downlink)
the original RX1/RX2 slot's latest allowed start.

The host can acknowledge a prepared later start once, using the **adapter**
ticket from `tx_plan`, not an unrelated link/job ticket:

```json
{"op":"tx_started","ticket":"1","startMs":1050}
```

Call `command(request, 1050)` on the adapter worker. `startMs` must equal that
call's current `nowMs`, not a future or backdated timestamp. It cannot be earlier
than the original plan, later than request expiry, or more than
`txCompleteTimeoutMs` after dispatch. A downlink additionally must remain in
its **original selected slot** (nominal start through nominal plus tolerance).
It cannot move from a missed RX1 job into RX2, change rate/frequency, or create
a fresh receive opportunity. Unprepared downlink transports must be warmed up
in advance or rejected; setup latency is not a reason to widen the window.

A successful command returns this shape (values are illustrative):

```json
{
  "ok": true, "ticket": "1", "nativeId": "3",
  "startUs": "1050000", "endUs": "1106576", "airtimeUs": "56576",
  "startDeadlineUs": "60000000", "frequencyHz": 868100000, "dataRate": 5,
  "modem": {
    "type": "lora", "sf": 7, "bw": 125000, "cr": 1, "os": 1,
    "preamble": 8, "syncWord": 52, "invertIq": false,
    "payloadCrc": true, "ldro": "auto"
  },
  "hardwareTimingVerified": false
}
```

The actual returned airtime/modem combination is computed from the actual
native frame. A matching event has these same timing/PHY fields without `ok`,
plus `event: "tx_started"`, `status: "tx_started"`, `type: "lorawan"` and
`protocol: "lorawan"`. No second `transmit` is emitted. The adapter durably
merges a later duty/backoff reservation and extends device cooldown before
returning `ok: true`; no counter, nonce or wire bytes are regenerated. Failure
to persist disables the adapter and denies authorization.

**Wait for `ok: true` before releasing the first sample.** A queued
`started(jobTicket)` notification followed immediately by streaming is not a
valid persistence barrier. Pause the sender until the adapter callback reply,
then apply the acknowledged `startUs`/`endUs` to the host job. On rejection,
do not send; cancel that job and call `complete(ticket, false, currentNowMs)`.
Stale tickets and duplicate start acknowledgments are rejected. Existing
immediate-start callers may omit this command only if they actually honor the
original plan; the old completion check remains unchanged for that path.

Successful completion must report the acknowledged (or unchanged original)
**on-air end** within tolerance, excluding modem quiet tails and transport
delivery delay. Device RX windows use that completion end. Timing mismatch or
missing completion fails closed because the radio outcome is unknown. A
successful transport completion does not claim application delivery;
authenticated ACK events are separate. `tx_started` does not relax any of
these completion checks.

NodeService's DATA/IQ transports carry simulated timing provenance in host
LinkJob metadata, not in `tx_started`/`rx_context` (which reject extra fields).
LinkSender prepares modulation/socket state, waits for readiness/link delay,
then synchronously obtains `tx_started` authorization before releasing bytes or
samples. DATA paces native airtime without IQ work. IQ uses the per-frame LoRa
modem and reports modeled on-air completion before quiet-tail/TCP completion.
On receive, DATA's local frame arrival or IQ's first local sample-block arrival
anchors packet start; native time-on-air determines packet end. NodeService
validates directional PHY/airtime and schedules the one-shot context/receive.
No cross-host clock subtraction is used. These are **simulated** contexts,
not hardware timestamps or measured propagation delays. Node status/events
disclose `timingSource: "simulated-transport"`; `hardwareTimingVerified` remains
false. Physical LoRaWAN stays disabled; fixed PHY settings alone do not establish
timing validity.

Configure PHY per direction/link and per selected RX1/RX2 rate. In particular,
LoRaWAN downlinks use `syncWord: 52`, `preamble: 8`, `invertIq: true`,
`payloadCrc: false`; uplinks use normal IQ and payload CRC. Events use OS1 to
fit the shared 2M-sample budget. The generic WireModem accepts
explicit boolean `payloadCrc`, default true. A CRC-less PHY packet has **no**
PHY integrity assurance: the LoRaWAN adapter must still validate native MIC,
counter, address, direction and receive context. Do not treat a PHY decode as
authenticated reception.

The adapter does not tune, resample, timestamp hardware or verify that a host
actually used those settings. `hardwareTimingVerified` is always false. Public
PHY templates are hints; the `tx_plan`/window-specific modem values are the
applicable ones. Physical SDR timing, RF/region legality, real demodulator
metadata and capture provenance require separate host integration and testing.

## Scope And Tests

This is a single-provisioned-device EU868 software adapter, not a full LoRaWAN
network server or certified MAC/radio stack. It uses the actual module Session,
OTAA, regional MAC, RX-window, duty and state APIs. There is no mesh routing,
gateway socket protocol, multi-device database, automatic network
ADR policy, regional negotiation tracking, confirmed retry policy or automatic
RF backend. Receiving supported external regional MAC commands on a device is
delegated to `Eu868::processDownlink`; unsupported network policies are not
simulated as success.

The 1.1 path includes OTAA/ABP, encrypted FOpts with the counter-domain erratum,
three network keys, separate NFCntDown/AFCntDown, confirmed-counter MIC binding,
DR/channel-dependent uplink MIC, Reset/Rekey/DeviceMode exchange, and
ADRParamSetup. Stateless rejoin 0/1/2 air codecs exist in the module; the adapter
does not expose automatic rejoin, ForceRejoin/RejoinParam policy, roaming, dual
old/new security contexts, multicast or FUOTA. Such policies are rejected,
not acknowledged as completed. This is software interoperability support,
not a certification claim.

Class B negotiates PingSlotInfo, waits for a valid beacon, advertises the uplink
ClassB flag, and then schedules unicast ping downlinks independently of Class A
opportunities. Beacons have reserved/guard priority; ppm drift widens detection
windows during the maximum two-hour holdover. Lost lock/restart falls back to A
until beacon reacquisition. Device reception supports PingSlotChannelReq and
BeaconFreqReq; network-side custom regional negotiation is still not exposed.
Class C listens on RXC outside TX and the higher-priority RX1 interval. Delayed
start authorization revalidates Class C RX1 priority and Class B beacon guard;
an expired opportunity never moves into a different slot.

`ObriiMeshLoRaWanAdapterSelfTest` uses two real adapters, private temporary
roots, local explicit test credentials and native wires. It checks ABP/OTAA,
persist-before-TX counters/nonces/duty, ACKs, repeats/dedup, Class A context,
MAC-answer priority, reboot/profile binding, exclusive locks, bounded queues,
expiry, fail-closed I/O, corrupt/missing state and monotonic clocks. With
`OBRII_LORAWAN_WITH_LORA`, wires additionally cross the module's actual LoRa IQ
bearer in irregular 137/1024-sample chunks, including a forged-MIC inverted
CRC-less downlink that PHY accepts and the protocol rejects. It does not use
the generic WireModem, RF hardware, registry or network. Start-handshake
tests cover delayed uplinks and in-slot downlinks, unchanged counters/airtime,
durable duty/cooldown extension, wrong/duplicate/stale tickets, expired/late
starts, storage failure, and still-strict completion after an acknowledged start.

`ObriiMeshLoRaWanNodeSelfTest` exercises actual paired NodeService endpoints:
ABP and OTAA over DATA and IQ, confirmed uplink/downlink payloads and native
ACKs, per-frame directional PHY overriding deliberately wrong static settings,
one plan/start/completion per native ticket, and durable session reconfiguration.
It also checks modeled provenance, the host's 250 ms default, explicit invalid
tolerance rejection, remote callback/secret rejection, one-peer enforcement and
physical-port blocking. Without arguments it runs both bearers; `--data-only`
and `--iq-only` select subsets. The build script's `-Test` uses no subset flag.

Both suites are already registered in `src/mesh/CMakeLists.txt` and are built
and run by `tools/build_mesh_designer.ps1 -Test`. They use test-owned local
services/state and no physical RF; software success is not hardware timing,
interoperability, regulatory or certification evidence.

Additional scenarios cover unicast B/C for 1.0.4 and 1.1, implicit beacon IQ,
periodic beacon timing, PingSlotInfo and DeviceMode negotiation, spontaneous
Class C downlink, ping-only Class B downlink, ACKs, class switch/reboot, and
OTAA 1.1 Rekey. Paired NodeService tests run 1.1 OTAA and B/C on both DATA and
IQ without RF. The module's `ObriiLoRaWANBC11SelfTest` independently checks
2076 external 1.1/Class B vectors and published beacon CRC.

For a Linux/ARM validation snapshot (no RF), build the native module tests and
these three integration targets. Qt5 Core/Network and the existing OpenSSL
dependency for the shared mesh library are required; Go is not needed:

```sh
cmake -S src/lorawan -B build/lorawan-check -DCMAKE_BUILD_TYPE=Release \
  -DOBRII_LORA_BUILD_TESTS=OFF -DOBRII_LORA_BUILD_TOOLS=OFF
cmake --build build/lorawan-check -j2
ctest --test-dir build/lorawan-check --output-on-failure
cmake -S src/mesh -B build/mesh-lorawan-check -DCMAKE_BUILD_TYPE=Release \
  -DOBRII_MESH_BUILD_UI=OFF -DOBRII_LORA_BUILD_TESTS=OFF -DOBRII_LORA_BUILD_TOOLS=OFF
cmake --build build/mesh-lorawan-check -j2 --target \
  ObriiMeshLoRaWanAdapterSelfTest ObriiMeshLoRaWanNodeSelfTest ObriiMeshWireModemSelfTest
ctest --test-dir build/mesh-lorawan-check --output-on-failure \
  -R '^(ObriiMeshLoRaWanAdapterSelfTest|ObriiMeshLoRaWanNodeSelfTest|ObriiMeshWireModemSelfTest)$'
```

The node suite runs all ten paired scenarios without subset flags. It has a
600-second aggregate CTest timeout to accommodate real modeled airtime/duty
waits; individual RPC/window deadlines remain bounded. Each suite creates its
own temporary credentials/state and stops only its own services.
