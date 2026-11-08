# Native mesh protocol adapters

`src/mesh/meshmeshtasticadapter.cpp` and `meshreticulumadapter.cpp` implement the
fixed `Mesh::ProtocolAdapter` QObject contract. They do not use the Obrii packet
engine, graph routing, min-hop selection or GUI state. Native wire bytes have no
Obrii routing or device TCP/serial API envelope; host DATA/IQ TCP framing remains
transport metadata.
One adapter belongs to one node worker. Reticulum additionally owns exactly one
local Python sidecar process per node; it never connects to a shared external
daemon. NodeService already integrates both adapters, plus the separate
[LoRaWAN adapter](mesh_lorawan_adapter.md). RX/TX ports and per-link DATA/IQ
select transport, not a replacement native router; see
[Hybrid Mesh Ports](mesh_hybrid_ports.md). The old packet-only and fixed-AFSK
laboratories remain separate `lab` experiments.

## Worker integration

- Pass the host-validated, existing absolute local state directory as the third
  `start()` argument. Neither adapter accepts state paths from protocol JSON.
- Connect signals immediately after successful `start()`. Starting either adapter
  never emits a transmission. Meshtastic polls TX only in `step()`; Reticulum
  issues `poll_tx` only after `step()`, and emits its response asynchronously.
- Call `command()` with the unchanged protocol `args` object, including `op`.
  The NodeService envelope is `{"op":"protocol","args":{"op":"send",...}}`.
  Normal endpoint authentication, instance and session fields are also required.
  The host may translate legacy send requests into a native `destination` first.
  LoRaWAN `rx_context`/`tx_started` timing callbacks are host-only and explicitly
  blocked on this remote command surface.
- Supply monotonic milliseconds to Meshtastic, including receive/completion calls.
  The worker's 20 ms `step()` is sufficient; Reticulum internally throttles its
  background polling to at most one cycle per 50 ms of real monotonic time.
  No adapter-owned polling timer runs while inactive. Missing Reticulum runtime
  configuration is a synchronous validation failure and starts no process.
- Every `transmit(ticket, interfaceId, wire)` carries the native protocol module's
  radio bytes. Fan those identical bytes out to all outgoing links bound to that
  interface. Do not choose a graph route or move a frame to another interface.
- Call `complete(ticket, sent, now)` exactly once. NodeService reports success
  at the first successful local emission/acceptance, or failure once all selected
  links fail; a stalled fanout peer does not hold the native interface busy. IQ link
  success means the entire paced waveform was written, not remote FCS/delivery.
  DATA-link success means remote ingress accepted the bytes, not app delivery.
  LoRaWAN instead completes at modeled on-air end, before quiet-tail/TCP
  completion; its timing/authorization contract is documented separately.
  Native Meshtastic ACKs and RNS delivery proofs remain protocol-owned.
- A fast peer can return a Meshtastic ACK before slower fanout links settle. While
  a reliable local unicast TX is active, up to 32 matching peer-to-local frames
  are held and passed to `Node::receive()` after the aggregate completion, using
  that monotonic completion time. This avoids the core discarding an early ACK
  before its TX receipt. It does not fabricate success: failed TX still fails.
  Overflow emits `rx_dropped` with `reason:"completion_backlog"`.
- Adapter tickets are local opaque uint64 values, not native packet IDs. They do
  not reset on stop/start of the same adapter object; late completions are ignored.
- Each emitted event has equal stable string `event` and `status` fields and a
  `protocol` string. The worker can append its `node`, `localMs`, and `serial` and
  preserve events under results without reinterpreting the native address or ID.

## Meshtastic

This directly owns `obrii::meshtastic::Node` and calls `send`, `receive`, `poll`,
`completeTransmission`, `cancel`, and `takeEvents`. Its managed flooding,
duplicate suppression, relay delay, ACK and retransmission behavior are native
core behavior, not replacement host routing. Learned next-hop routing and full
firmware role parity are not claimed by the core or adapter.

Example **host-local operator profile** (the default LongFast key is public,
not confidential):

```json
{
  "nodeNumber": "!12345678",
  "relayRole": "client",
  "hopLimit": 3,
  "channels": [{"name": "LongFast", "pskBase64": "AQ==", "enabled": true}]
}
```

Install this under private node options' `protocolProfiles[profileId]`. The
project selects `{"type":"meshtastic","profileId":"lab-channel"}` and may carry
public node/relay settings, but never channel PSKs, PKI pins or private keys.
NodeService rejects secret-bearing remote topology fields and merges only
locally installed profile material. Match LoRa PHY settings separately on ports
and IQ links; a public channel name does not configure the modem.

`nodeNumber` is required: a uint32 JSON number or 1..8 hexadecimal digits, with
optional `!`/`0x` prefix. Strings are always hexadecimal input; status uses the
decimal string form for both `nodeNumber` and stable `nativeAddress`. Reserved
node numbers 0..3 and broadcast are rejected. The host interface list must have
exactly one member. An empty list defaults to `radio`; optional `interfaceId`
must match the selected host interface. Multiple interfaces are explicitly
unsupported, not silently collapsed or rerouted.

Optional configuration:

- `channels`: 1..8 objects with only `name`, `pskBase64`, `enabled`. Omitting the
  array selects the native public LongFast channel. PSKs use the core's short-key,
  16/32-byte and plaintext-key conventions; maximum encoded key material is
  32 decoded bytes. At least one channel must be enabled.
- `relayRole`: `off`, `client` (default), `router`; `hopLimit`: 0..7 (default 3).
- `relayUnknownChannels` (default false), `relayPki` (default true). Relaying PKI
  ciphertext does not enable local PKI decryption or origination; those require
  `pki:true` and explicit local peer pins as described below.
- `relayDelayMs` (150), `relayJitterMs` (300), `ackTimeoutMs` (30000),
  `retryDelayMs` (500), `maxRetries` (3, range 0..10), `historyTtlMs` (900000).
  Timer bounds and the requirement that history cover the reliable-send window
  are checked by the native `NodeConfig::validate()`.
- `pki`: boolean (default false), `pkiPeers`: explicit local operator-profile pins.
  These must come from the host's locally approved private profile, not remote
  topology fields, received NodeInfo, or unauthenticated network commands.

Commands:

```json
{"op":"send","destination":"!87654321","text":"hello","reliable":true,"channel":0}
{"op":"send","destination":4294967295,"payloadBase64":"aGk=","reliable":false}
{"op":"cancel","nativeId":1}
{"op":"status"}
{"op":"send_native","destination":"!87654321","payloadBase64":"AA==","data":{"port":3,"wantResponse":false}}
```

`send` selects native text port 1. Use exactly one of `text` and canonical padded
`payloadBase64`; payloads are bytes, not a host envelope. `send_native` also accepts
`data.port` (1..511, except unwired admin port 6), `wantResponse`, `dest`, `source`,
`requestId`, `replyId`, `emoji`, and `bitfield`. Those integer Data fields and
`cancel.nativeId` are uint32 JSON numbers. Successful sends return
`{"ok":true,"queued":true,"nativeId":<uint32>}`. Queueing is not TX or delivery.
Cancel cannot recall an already emitted wire transmission; the native event
records cancellation and the host would need its own active-PHY abort support.

Events include `message`, `observed`, `duplicate`, `rejected`, `unknown_channel`,
`ambiguous_channel`, `sent`, `relayed`, `acknowledged`, `negative_ack`, `retrying`,
`tx_failed`, `queue_full`, `identity_conflict`, `cancelled`. ACK timeout maps to
`tx_failed` with `reason:"ack_timeout"`. `relayed` is the native implicit broadcast
relay confirmation, not end-to-end delivery. `nativeId` identifies the original
request for ACKs when available; `packetId` retains the actual event frame ID.
Valid headers add decimal string `from`/`to`; decoded Data adds `payloadBase64`,
`port`, `channel` and valid UTF-8 text when appropriate. Ordinary PSK messages
report `authenticated:false`: syntactic decryption is not sender authentication.
A verified native PKI routing proof can authenticate a PSK-envelope ACK, as
described below.

### Nonce persistence and limits

The adapter takes an exclusive `QLockFile` per node number in the supplied state
directory. Counter filenames contain SHA-256 of the *effective key bytes* plus
the node number, not channel indexes/names or a mutable whole-channel-set hash.
The maximum next counter of all enabled key namespaces is used. Every allocation
commits the incremented high-water mark to every enabled namespace using
`QSaveFile` with direct-write fallback disabled **before** returning an ID to
`Node`, including native ACK allocations. Retrying an existing packet preserves
its original ID and ciphertext. Channel reorder, rename, removal/re-addition and
key rotation therefore retain the relevant old key's high-water mark.

New namespaces begin at 1. Values never wrap: reaching the uint32 limit disables
new encrypted transmissions until identity/key rotation. Unreadable, malformed,
symlinked or unwritable counters fail closed; an allocation write failure destroys
the native node and emits `failed`, without releasing its would-be transmission.
State and error exports never include PSKs, PSK key hashes, runtime paths or private
key material. The directory must be a trusted, durable local filesystem. Do not
clone, roll back or delete it while retaining the same sender/key; do not run the
same sender/key from two different directories. Local directory ownership and
backup rollback protection are host responsibilities, not an AES-CTR feature.

Bounds: one active TX, 32 queued/active transmissions, 16 reliable pending sends,
128 queued native events, 2048 entries each in received/local packet history,
256 node records. Native wire MTU
is 255 bytes; encrypted Data is at most 239 bytes **including protobuf overhead**,
so 239 raw application payload bytes are not guaranteed to fit. Admin client/server,
PKI admin execution, and device management are not wired; status explicitly
reports `admin:false` and `pkiAdmin:false`, even when local PKI is enabled.

### Local PKI identity and explicit peer pins

Enable PKI in a locally approved operator profile:

```json
{
  "nodeNumber": "!12345678",
  "pki": true,
  "pkiPeers": [
    {"nodeNumber": "!87654321", "publicKeyHex": "<64 hexadecimal characters>"}
  ]
}
```

Initially start each node with `pki:true` and no peers to provision its identity.
Read its public `status.publicKeyHex` and exchange/verify that key out of band.
Then add each peer explicitly to the local profile and restart the adapter. The
host's local `protocolProfiles[profileId]` merge is the trust boundary: only the
operator-approved profile may supply `pki`/`pkiPeers`. The adapter rejects unknown
configuration fields and accepts no private-key import, runtime key path, dynamic
pin command, remote add-peer command, or trust-on-first-use operation.

`platformCrypto()`, `generatePrivateKey()` and `PkiContext` provide all native
key generation, X25519/AEAD, key validation and authenticated routing ACK proofs.
The adapter does not implement protocol cryptography. A versioned, checksummed
binary `meshtastic-<nodeNumber>.pki` file holds the private key, its derived public
key and up to 256 explicit peer pins together. It is protected by the same
per-node exclusive `QLockFile` as the nonce counters, and atomically committed
with `QSaveFile` (direct-write fallback disabled). Owner-only permissions are
requested; permissive Unix group/other access is rejected. Windows filesystem
ACLs and durable directory ownership remain the host's responsibility. Owned
temporary private-key buffers are wiped with the module's `secureErase()`.

Pins in a profile are add-or-confirm. Omitted peers remain pinned across restarts;
an empty array does not erase established trust. Reusing a node number with a
different public key fails closed **without** overwriting the old pin or private
identity. Low-order keys are rejected by the native crypto provider. No implicit
unpin/re-pin or identity rotation is exposed; changing established trust requires
an explicit out-of-band operator migration of the local state. Received NodeInfo
public-key claims never become trusted pins.

`status.pki` reports the active PKI state; `publicKeyHex` is the 32-byte public
key as lowercase hex; `nativeFingerprint` is lowercase SHA-256(public-key bytes),
not a new radio node address. `nativeAddress` remains the decimal node-number
string. `pkiPeerCount` is the stored pin count; private keys and pin file paths
never appear in status, events or error messages. All administrative execution
remains explicitly unsupported.

```json
{"op":"send_pki","destination":"!87654321","text":"hello","reliable":true}
{"op":"send_pki","destination":"!87654321","payloadBase64":"AA==","data":{"port":3}}
```

`send_pki` invokes native `Node::sendPki`, requires a pinned remote unicast peer,
and uses native channel hash zero. A PSK `channel` argument is rejected. Optional
`data` fields are the same typed non-admin fields as `send_native`; admin port 6
is rejected. PKI plaintext Data is limited to 227 bytes including protobuf
overhead. The module supplies its CSPRNG extra nonce and AEAD tag. Reliable
delivery uses native authenticated routing proofs: the outer ACK may be a PSK
packet, but an unverified PSK ACK cannot clear a pending PKI send. Authentication
is reported by `authenticated`, not inferred solely from the outer `security`
string. When present, `senderPublicKeyHex` and `senderFingerprint` identify the
native authenticated/pinned sender public key.

PKI adds a durable counter namespace keyed by node number and the public-key
fingerprint to the existing PSK namespaces. Every new local packet/ACK ID is
reserved in **all active namespaces** before encryption/TX. Thus enabling,
disabling or re-enabling PKI cannot reset a PSK or PKI sequence; IDs remain a
strictly increasing uint32 ledger rather than randomly colliding packet IDs.
The native provider's CSPRNG is used for the private identity and native PKI
extra nonces. An existing identity requires its PKI counter file. Missing or
corrupt private state, missing/corrupt counters, pin mismatch, unavailable crypto
or failed persistence refuses startup. A missing private identity alongside an
existing PKI ledger is not silently regenerated. Interrupted first provisioning
can leave a partial identity/ledger pair; this also fails closed and requires
operator recovery of the complete local state, not an automatic nonce reset.

## Reticulum

The local runtime must contain exactly `rns==1.5.5`, as enforced both by the pinned
`src/reticulum/obrii_reticulum/node.py` service and the adapter readiness check.
The development runtime is `build/reticulum-venv/Scripts/python.exe` on Windows
or `build/reticulum-venv/bin/python3` on Linux. NodeService detects supported
local checkout/packaged paths when no explicit `reticulum` options are supplied.
Use `src/reticulum/requirements-lock.txt` to provision an isolated runtime; the
adapter does not install dependencies or modify a user's global Python packages.

Example private **node options** (use actual absolute paths on the node host):

```json
{
  "reticulum": {
    "python": "C:/Users/Svyat/Desktop/FobosAPP/build/reticulum-venv/Scripts/python.exe",
    "moduleDirectory": "C:/Users/Svyat/Desktop/FobosAPP/src/reticulum",
    "framing": "rnode",
    "transport": false,
    "bitrate": 1100,
    "mtu": 255,
    "startupTimeoutMs": 15000
  }
}
```

NodeService strips remote `python`, `moduleDirectory` and `stateDirectory`,
then supplies locally approved runtime options. `QFileInfo` checks require
absolute existing executable/module paths and all seven service package files.
Paths are canonicalized. These checks verify path shape/existence, not operator
approval or package authenticity; approval belongs to the local host. The adapter
uses `QProcess` with the executable and fixed argv, never a shell or user-provided
arguments. A constant Python `-I -u -c` bootstrap imports only the approved package
directory and invokes `obrii_reticulum.service`. Configured paths are argv data,
never interpolated source. Python environment overrides/user-site imports are
disabled. State comes exclusively from the third `start()` argument. Both the
Qt adapter and native service exclusively lock the persistent state directory.

Supported configuration is limited to `python`, `moduleDirectory`, `interfaces`,
`framing`, `transport`, `bitrate`, `mtu`, `startupTimeoutMs`. Interfaces are 1..8 distinct ASCII IDs,
1..32 characters from letters, digits, `_`, `.`, `-`; default `radio`. Optional
configuration `interfaces` must exactly match the host list if both are supplied.
Framing is `obrn` (default) or `rnode`, bitrate 5..10000000 (default 1100), MTU
64..255 (default 255); RNode requires 255. The service's own fragmenter owns these
radio formats. No host envelope is added around its emitted bytes. RNS owns
routing, identities, encryption, links, delivery proofs and packet fragmentation.
IFAC credential-file provisioning is not exposed by this adapter.

`start()` returns accepted, not ready. Only a successful, version-checked status
RPC sets `status().ready=true` and emits `ready`. Stable `nativeAddress` is the
32-hex-digit RNS destination hash, populated on readiness and restored from the
service's persistent identity on restart. No TX is polled before worker `step()`.

Typed public operations are `send`, `announce`, `request_path`, `path`, `link_open`,
`link_close`, `link_send`, `request_echo`. The native service argument names
`destination`, `expected_identity`, `link`, `data` are supported. Payload-bearing
commands additionally accept `payloadBase64` or `text` in place of `data` (one
payload field only; `announce` defaults to empty app data). Hashes must be exactly
16 bytes represented by 32 hexadecimal characters. A successful submission returns
`{"ok":true,"pending":true,"id":"<adapter-rpc-id>"}` followed by an event:

```json
{"event":"reply","status":"reply","protocol":"reticulum","id":"7","op":"send","ok":true,"result":{"packet":"..."},"nativeId":"..."}
```

`status` returns a sanitized local snapshot synchronously. Public commands cannot
invoke internal `rx`, `poll_tx`, `tx_done`, `online`, `shutdown`, or arbitrary
operations. `cancel` is explicitly unsupported by the native sidecar API.

`packet_received`/`link_data` become `message`; `delivered` becomes `acknowledged`;
`delivery_failed`/`tx_rejected` become `tx_failed`. Other allowed native event names
remain intact, with `backendEvent` retaining the service name. `nativeId` is the
native packet/request hash or interface-local ticket when available. Received
Data becomes `payloadBase64`; valid native metadata is preserved. SINGLE packet
ciphertext does not identify its sender, so no fabricated `from` is attached;
link events can include a known `peer_identity`. RNS peer discovery/controller
association and graph addressing remain outside this adapter.

### Sidecar bounds and failure handling

- One in-flight RPC; at most 32 queued plus in-flight RPCs total. Ordinary
  requests cap at 24, leaving eight slots for interface TX completion receipts.
  Full RX queues emit `rx_dropped`; full public queues reject synchronously.
- JSON lines, including newline, are limited to 32768 bytes in both directions.
  Output is read in 4096-byte chunks with bounded per-callback processing.
  Oversized/unmatched/malformed responses fail closed rather than parsing tails.
- Cold backend startup and its initial status/readiness RPC share a separate
  15000 ms elapsed budget. A locally approved profile may set `startupTimeoutMs`
  to an integer in 1000..60000 ms for slower hosts. This does not alter any native
  ACK, proof, link or application-delivery timer.
- After readiness, every accepted RPC has a fixed three-second elapsed deadline,
  checked by worker `step()` and reply arrival, including queue time. Initial
  readiness is also deadline-checked on reply arrival; no late response can mark
  an expired backend ready. Failed launch, startup stderr,
  unexpected exit, protocol violations and timeout emit `failed`; errors are
  generic and never forward traceback paths, environment, or credentials.
- Status is polled once per second, events in batches of four per polling cycle,
  and every free interface is independently polled for TX. An interface remains
  busy until its actual `tx_done` RPC succeeds, not merely until it is emitted.
  The service's large status/peer list can exceed the response limit; that is a
  fail-closed backend error, not a truncated or falsely successful snapshot.
- Stop affects only the owned process. It requests graceful shutdown, then uses
  terminate/kill if needed, with waits bounded to 200 + 200 + 300 ms. No worker
  `waitFor*` call occurs during start, command, receive, step or completion.
- The service has its own bounded queues, event/peer/link/receipt caches and
  fragment reassembly limits. Adapter payload validation allows up to 4096 decoded
  bytes (128 for announce); actual SINGLE/link MDU is enforced by RNS. Large echo
  requests use the existing service resource handling; no new fragment protocol
  is implemented here. Service status exposes `single_mdu` after readiness.

## Build And Verification

`ObriiMeshNode` already includes the adapters and links `ObriiMeshtastic` with
its platform crypto backend (`bcrypt` on Windows, OpenSSL Crypto elsewhere).
RNS uses the local pinned runtime; the shared LoRa modem supplies PHY work.
`ObriiMeshProtocolAdapterSelfTest` is already registered in CMake. After building:

```powershell
./build/mesh-designer/Release/ObriiMeshProtocolAdapterSelfTest.exe `
  --python ./build/reticulum-venv/Scripts/python.exe --modules ./src/reticulum
```

The suite uses QCoreApplication, temporary private state and native byte
loopback, without GUI or RF. Without runtime arguments Meshtastic still runs
and Reticulum explicitly reports skipped. `tools/build_mesh_designer.ps1 -Test`
builds/runs all 19 mesh suites and supplies the local Reticulum runtime when
present; it does not install RNS or silently replace a missing runtime.

Coverage includes Meshtastic relay/ACK/retry, early ACK and aggregate-completion
handling, durable IDs across channel/key changes and restarts, locks and
fail-closed persistence. PKI cases cover explicit pins/no NodeInfo TOFU,
authenticated messages/ACK proofs, forged PSK ACK rejection, persisted identity,
pin conflicts/bounds, low-order keys and missing/corrupt identity or nonce ledgers.
Real RNS cases exercise readiness, encrypted packets/proofs, native links/echo,
persistent destination, path commands and queue bounds with one/eight interfaces.
Fault fixtures exercise redaction, malformed/oversized output, launch failure
and separate startup/RPC deadlines: initial readiness may exceed three seconds
within the default 15-second budget, but ordinary RPCs still expire after three.
Status exports `startupTimeoutMs` and `rpcTimeoutMs` for diagnosis.

See [LoRaWAN tests](mesh_lorawan_adapter.md#scope-and-tests) for its direct
adapter and paired DATA/IQ node suites. None of these software suites qualifies
physical RF interoperability, certified MAC timing, Meshtastic admin execution,
learned routing or full firmware parity.
