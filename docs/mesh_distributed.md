# Distributed Obrii Mesh Nodes

This optional mode attaches a Mesh Designer experiment to independently running
Obrii instances, on one machine or several trusted LAN/VPN hosts. Each node
processes and forwards packets itself. The designer configures directed links
and polls telemetry; it is not the packet relay. Nodes have independent RX/TX
ports, per-link DATA/IQ and AFSK1200/FM or LoRa modems. The laboratory router
retains static known-graph routing; native Meshtastic, Reticulum/RNS and LoRaWAN
adapters use their own protocol semantics instead. LAN instance discovery is
not learned radio-neighbor routing, an OS network adapter or a certified MAC.
See [Hybrid Mesh Ports](mesh_hybrid_ports.md),
[Protocol Adapters](mesh_protocol_adapters.md) and
[LoRaWAN](mesh_lorawan_adapter.md) for configuration and limits.

## Start A Node

No receiver, FFT, audio, GUI display server or RF transmitter is needed:

```sh
ObriiCli mesh-node --bind 127.0.0.1 --mesh-port 17690
```

On a remote Raspberry use its actual LAN/VPN address instead of loopback:

```sh
./ObriiCli mesh-node --bind 192.0.2.20 --mesh-port 17690
```

192.0.2.20 is a documentation placeholder; replace it with the device's address.
`--bind 0.0.0.0` explicitly listens on all IPv4 interfaces; do not expose this
port to an untrusted network. For multiple instances on one PC choose different
ports, e.g. 17690, 17691, 17692. Port 0 requests an OS-assigned port. Startup JSON
contains the actual port, protocol version, instance ID and a private random
token. Copy the token into the corresponding designer endpoint, not into chat,
an experiment export or a public log. It changes when the service restarts.

`--credentials-file PRIVATE.json` writes that startup JSON instead of printing
it; protect and delete the file when no longer needed. It is not a release asset.
`--seconds N` is an optional bounded test lifetime (0 means keep running).
ObriiMeshNodeHost provides the same small standalone endpoint if a full CLI
launch is unnecessary. All variants share the same NodeService implementation.

In the main GUI's command console, or a running CLI's typed dispatcher:

```text
mesh.service start 127.0.0.1 17690
mesh.service status
mesh.service stop
```

The GUI keeps the token redacted in console output. An explicit Start opens a
separate masked connection dialog with a Copy session token button; use that
button to fill the designer's credential field. In typed responses the mesh
endpoint identity is also named nodeInstanceId, since instanceId belongs to the
surrounding GUI/CLI dispatcher.

Use a real LAN bind address for a remote GUI instance. Start/stop require trusted
local command privileges; the generic read/control agent cannot open a LAN port
or retrieve the startup token. This mesh service is separate from the normal
receiver/control ports. It does not tune, stop, replace or seize the user's
existing physical receiver. Starting the node service is explicit and optional;
there is no background mesh DSP while it is stopped.

## Attach From The Designer

### LAN Discovery And Registration

Start the optional node service on each participating GUI/CLI instance first.
Normal Obrii instances with no mesh service enabled do not advertise or open a
network-control port automatically. In the distributed window press Search:
the two-second IPv4 broadcast/multicast probe fills the instance selectors with
names, addresses, ports and capabilities. Assign a discovered instance to each
graph node; its endpoint fields fill automatically. Multiple instances on one
PC have distinct instance IDs and TCP ports, even though they share UDP discovery.
The network locator is advertised, not the process's local installation path.

For first registration, enter the six-digit local pairing code displayed in
the node GUI's connection dialog or the CLI's private startup JSON pairingSecret.
The response grants a separate per-client credential, not the node's primary
admin token. Discovered broadcasts contain neither. Registered credentials stay
in memory, outside project files and exports; a service restart requires new
registration. There are at most 16 registrations per service launch and at most
five failed code attempts per minute. Pair only on a trusted LAN/VPN: TCP pairing
is not encrypted, and this is not a replacement for TLS or a secure public API.

Use the trusted local command `mesh.service registrations` to list client IDs
and names without tokens. Revoke a registration using a typed request:

```json
{"command":"mesh.service","args":{"operation":"revoke","clientId":"CLIENT-UUID"}}
```

Revoking the owner cancels its mesh session and owned mesh RF workers, not
ordinary receiver operation or unrelated radio sessions.
Another registered client can see that a node is busy but cannot read its packet
history, renew its lease, or release its session. Manual address/token setup is
still available. Broadcast discovery stays in its local network segment; it
usually does not cross a routed VPN, VLAN or Wi-Fi client isolation. Known
numeric endpoint addresses can be probed directly during Search, otherwise use
manual setup. Permit UDP 17699 and the chosen mesh TCP port on trusted interfaces
only; no firewall rules or router port forwarding are created by Obrii.

### Connect And Run

Open **Реальні екземпляри** (independent Obrii instances) in Mesh Designer. Associate each graph
node with a reachable address, mesh port and its service token. Select the DATA
or IQ default for links marked `inherit`; explicit per-link bearers override it.
Configure RX/TX port bindings and node protocols in the constructor before
connecting. For remote devices the address must be reachable both from the
designer and from other nodes: each hop connects directly to its next peer.
When hosts differ, use reachable LAN/VPN addresses for every endpoint, including
the one on the designer's PC. Do not advertise 127.0.0.1 to remote peers: that
would point each peer at itself. Loopback addresses are for all-local tests.
The controller first checks every authenticated instance and rejects duplicate
instances, incompatible protocols and nodes already owned by another session.
It then installs a fresh shared session identity/key, pinned peer instance IDs,
the topology, port bindings and per-link bearers. Tokens/session keys are not
part of saved projects or exported results. Stop releases this experiment and
disarms its mesh RF workers; Obrii processes, listener services and unrelated
radio operation remain alive.

For the laboratory protocol, choose source/destination, TTL and payload.
A-B-C with no A-C link requires B to receive and forward; C reports the actual
path and exact binary payload.
Per-node queue, TX/RX, forwarding, delivery/drop and IQ-sample counters distinguish
transport from application delivery. A hop's transport acceptance is not an
end-to-end ACK. Failures do not automatically retry or create routes absent from
the installed graph. Edit/reconnect the experiment to change its static topology.
For native adapters, use the Protocol tab's typed `op` commands and native
addresses, not laboratory TTL/path semantics. Meshtastic PSK/PKI and RNS proofs
remain native; LoRaWAN commands address the provisioned device/network session,
not a mesh destination. Native events do not fabricate visited graph paths.
The first controller is fail-closed: loss of an attached endpoint aborts and
releases its run. Independent degraded-node execution, live topology repair and
resilience experiments without stopping other participants remain future work.
One-second status polls use a per-node result cursor, so unchanged payload history
is not repeatedly transferred across a slow VPN. Eviction counters remain visible.
The cursor is session-scoped and resets after reconfiguration. Result localMs
values are each process's own elapsed clock, not synchronized global timestamps
or end-to-end delay measurements. Do not subtract them between different hosts.

## Ports, Transports And Protocols

Each node has up to eight directional ports. Omitting ports preserves the
legacy network RX/TX pair named `radio`; omitting the protocol selects `lab`.
Each directed edge selects `txPort`, `rxPort`, `bearer` (`data`, `iq`, `inherit`)
and a modem. Enabled TCP edges require network ports and matching protocols.
There is no implicit Meshtastic/RNS/LoRaWAN translator or global hardware bearer.

- **DATA:** bounded JSON/base64 over TCP, without modulation or IQ. Laboratory
  payloads can be up to 4096 bytes with addresses, TTL and path. Native adapters
  send their own wire bytes, subject to their smaller protocol MTUs. Link delay
  applies; IQ gain is not a modeled packet-loss probability.
- **IQ:** sequenced CF32LE bursts with session/peer checks, independent per-link
  pacing, gain and delay. Shared WireModem supports AX.25/AFSK1200/NFM at
  48 kS/s or LoRa CSS at BW * OS. The laboratory packet panel caps IQ payloads
  at 240 bytes; native protocols and modem/sample bounds impose their own MTUs.
  Modem decoding must succeed before protocol receive/forwarding. At 48 kS/s,
  CF32 alone is 384 kB/s; base64 adds about one third plus framing overhead.
- **Native protocols:** Meshtastic supports PSK and locally pinned PKI; RNS runs
  in an isolated local Reticulum sidecar with a default 15-second startup budget.
  LoRaWAN supports one provisioned EU868 device/network pair, ABP or OTAA,
  Class A over virtual DATA/IQ. Its host-modeled timing defaults to 250 ms
  tolerance and reports `timingSource: "simulated-transport"` and
  `hardwareTimingVerified: false`. `rx_context` and `tx_started` are host-only
  callbacks, rejected as remote protocol commands. This is not a certified radio MAC.
- **Physical bridges:** separate `radio` ports can capture an already running,
  locally authorized RX source. Native Meshtastic/RNS TX requires a host-local
  UHD/HackRF profile and separate bounded local RF arming. These physical paths
  are experimental and hardware-untested. Physical LoRaWAN
  RX/TX is blocked pending qualified timestamped integration. Fobos is RX-only;
  generic serial LoRa module drivers are not provided by these ports.

Distributed IQ is point-to-point per link. Simultaneous
incoming connections have independent decoders, not a shared superposed RF
medium: collisions, hidden-node MAC, carrier sensing, frequency-offset simulation
and distributed clock synchronization are not implemented. The local Packet
network IQ experiment retains its separate sample-mixing medium. Neither mode
should be mistaken for a physically faithful multi-radio mesh yet.

## Limits And Security

At most eight nodes per experiment, 16 queued/current TX packets per directed link,
32 inbound sockets and a baseline 5-second input deadline (modeled LoRaWAN
bursts extend it by native airtime). Outgoing link deadlines are at least
15 seconds, extended to IQ burst duration plus 5 seconds when longer. The
laboratory router has no automatic ARQ; native ACK/retry rules are protocol-owned.
Laboratory deduplication retains the last 4096 source/packet IDs. Evidence
retains at most 128 outcomes and 48 KiB, with an explicit resultsEvicted counter.
The service checks message sizes, canonical base64, TTL/path, session, previous hop and
instance identity. Output queues are bounded. An inactive worker does no FFT
or waveform processing; network work is independent from the ordinary RX path.

Authenticated status polling renews a 15-second session lease; loss of the
controller expires ownership and closes its mesh connections. A shared random
session key permits configured peers to exchange only mesh frames, not general
GUI commands or local RF arming. Admin tokens permit endpoint configuration,
but cannot install host-local secrets or grant radio permissions. A locally
armed native radio port can transmit protocol traffic until disarm/expiry.
TCP is **not TLS**: credentials and outer transport metadata are not encrypted;
native payload protection is protocol-specific. Use
only a trusted private LAN/VPN; no public Internet listener or port forwarding.
Stable node IDs and instance checks are not substitutes for authentication or
confidentiality. The topology is a model of permitted links, not a physical
security boundary against another authorized participant.

## Verification

ObriiMeshNodeSelfTest launches three independent node-host processes and tests
binary A-B-C data forwarding without IQ, real CF32 demodulation/retransmission,
ownership protection, authentication and session release. The same test accepts
an ObriiCli executable path to run the nodes as `ObriiCli mesh-node` instead.
PortPlan, mixed-port, WireModem and native-adapter suites cover the extended
service. `ObriiMeshLoRaWanAdapterSelfTest` covers direct native state/timing;
`ObriiMeshLoRaWanNodeSelfTest` exercises ABP/OTAA Class A DATA and actual LoRa
IQ round trips, dynamic directional PHY, host-only callback rejection,
physical-port rejection and persistent reconfiguration. The isolated build
script's `-Test` builds and runs all mesh suites; see
[build and test](mesh_designer.md#build-and-test-without-disturbing-the-main-build).
Cross-device Windows/Linux/VPN behavior still requires testing on those devices;
the Windows process test is not an ARM or real-RF qualification.
