# Hybrid Mesh Ports

The distributed node service is shared by GUI Obrii SDR, ObriiCli, and the
device-free ObriiMeshNodeHost. The ordinary receiver/audio path does no mesh
modulation, decoding or extra copying while this optional service/capture is off.

## Independent RX And TX

Each node has up to eight directional ports. IDs name protocol interfaces; the
same ID may occur once in RX and once in TX. An omitted ports array retains the
old network RX/TX pair named `radio`. For example, an RX-only Fobos observer can
receive from `main/rx0` and retransmit decoded native frames over the network:

```json
{
  "id": "observer",
  "protocol": {"type": "meshtastic", "nodeNumber": 256, "profileId": "lab-channel"},
  "ports": [
    {"id": "radio", "direction": "rx", "backend": "radio", "receiverId": "main",
     "channelId": "rx0", "centerHz": 145500000,
     "modem": {"type": "lora", "sf": 7, "bw": 125000, "cr": 1, "os": 2,
               "preamble": 16, "syncWord": 43}},
    {"id": "radio", "direction": "tx", "backend": "network"}
  ]
}
```

The profile/PHY/frequency must match the experiment, not merely this example.
Fobos remains receive-only. The node neither opens a second RX handle nor
retunes the user's receiver. Start that receiver first, and bind the exact source
identity and frequency in the ports inspector/JSON. The real ingress performs
DDC and resampling, with discontinuity resets and sample/gap counters. LoRa RX
needs OS >= 2 for its anti-alias guard band. `waiting`, `outside_source_band` and
`ddc_rate_mismatch` are explicit states, not successful decoding.

TCP edges require network TX/RX bindings. A physical port is not implicitly
mirrored to a TCP edge. Reticulum can use multiple native interfaces, e.g. one
physical interface and one network interface. Meshtastic currently has one
native radio interface per node. Parallel directed edges between the same node
pair are not supported yet. Enabled links must connect matching protocols;
bindings do not translate between Meshtastic, Reticulum, LoRaWAN and `lab`.
LoRaWAN has one native interface and exactly one outgoing network peer per
node, for a provisioned EU868 device/network pair. Its physical RX/TX ports
are rejected even when the host has granted radio permissions.

## Trusted Local Options

Only the host can grant radio capture access, install protocol secrets or select
a Reticulum executable. Supply a private JSON file using `--node-options FILE`,
or `optionsFile` in the local `mesh.service start` command. Example without keys:

```json
{
  "allowRadioRx": true,
  "radioSources": [{"receiverId": "main", "channelId": "rx0"}]
}
```

`protocolProfiles` maps public profile IDs to host-local adapter configuration.
`protocolStateDirectory` optionally selects an absolute private state directory.
Otherwise each listener uses its own port directory under the user's ObriiSDR
application data. Native identities, counters, locks and keys are not project
content. Do not copy initialized protocol state between simultaneously active
nodes or reset nonce/frame counters under the same identity/key.

`reticulum` may specify host-local `python` and `moduleDirectory`; the sidecar
uses its own persistent identity. Startup/readiness has a default 15-second
budget (`startupTimeoutMs`, locally configurable in 1000..60000 ms); ordinary
post-readiness RPCs retain their separate three-second deadline.
In a development checkout the installed
`build/reticulum-venv` and `src/reticulum` are detected automatically. Missing
runtime/readiness is reported explicitly. See `mesh_protocol_adapters.md` and
`reticulum_module.md` for native commands, installation and limits.

The standalone `mesh-node` mode has no physical RX source of its own. For a real
RX bridge, start the service in the GUI or a normal running CLI receiver using
the shared command console/API. ObriiMeshNodeHost remains useful for device-free
nodes and separately authorized TX experiments.

## RF TX Authorization

Physical TX is disabled by default; a remote topology cannot grant authorization.
It requires a trusted host-local `radioTxProfiles` entry keyed by TX port ID, plus a
separate local transmit authorization. For example:

```json
{
  "radioTxProfiles": {
    "radio": {"backend": "uhd", "deviceIndex": 0, "channel": 0,
              "gainDb": 0, "minHz": 145000000, "maxHz": 146000000}
  }
}
```

This is a schema example, not permission to transmit on those frequencies.
Check the antenna/load, attenuation, equipment limits and applicable rules.
The whole requested occupied band must remain inside the approved interval.
Supported sinks are `uhd` and `hackrf`; an RX-only backend is never substituted.
HackRF also accepts `deviceRate` (2..20 MS/s), integer VGA gain 0..47 dB and
`bandwidthHz`. UHD accepts channel, gain, bandwidth and antenna. Hardware still
validates/coerces its own capabilities; unsupported rates fail explicitly.

In the GUI/CLI command console, enable transmit permission and issue
`mesh.rf arm 60` before attaching a session. `mesh.rf disarm` stops RF workers
without granting any peer permission. The watchdog is limited to 1..3600 seconds.
Release/stop also disarms; reattachment needs a new local arm. The dedicated
host supports `--node-options FILE --rf-confirm --rf-arm-seconds 60`.
Ordinary admin/session tokens cannot arm RF through the mesh network protocol.
Physical TX requires a native Meshtastic or Reticulum adapter; `lab` TX is
rejected. LoRaWAN physical RX and TX remain blocked pending qualified timed
SDR integration, not merely a profile or an arm command.

Each RadioEgress TX port has a separate worker, a four-burst queue and an
independent `steady_clock` watchdog; expiry does not rely solely on the node's
Qt timer/event loop. Without an explicit positive bounded authorization, no
egress worker starts. Disarm/stop raises cancellation on **all** ports before
joining any worker, so one driver's teardown does not delay notifying the others.
Queued work is cancelled; an in-progress native call may still take its bounded
driver timeout to return. This is not a hardware-timed RF stop guarantee.

Finite native HackRF mesh bursts require the official `hackrf_enable_tx_flush`
API. An older library without it must fail before RF starts. Completion means
the API's successful flush-complete notification, not merely an empty source
queue or a USB write acceptance. This finite-burst path does not alter existing
PTT/microphone transmission behavior. UHD/HackRF reuse the existing backend
code; no generic LoRa module driver is implied.

These RF paths and safety changes are **experimental and hardware-untested**,
not a qualified hardware-timed MAC. Open/setup latency, cancellation, actual
flush/RF completion, duplex behavior, RF quality and duty-cycle compliance still
need controlled hardware qualification. Software self-tests use non-RF sinks.

## Per-Link Transports And Protocols

Select DATA or IQ for each edge; `inherit` retains the session default. DATA
avoids waveform work. IQ transmits bounded, paced CF32LE sample bursts with
independent decoders per incoming link. AFSK uses AX.25/FCS and FM; LoRa uses the
shared CSS modem with SF/BW/CR/OS/preamble/sync/IQ polarity settings. Match the
actual peer's settings; decoding bytes alone does not prove RF interoperability.
For example, these two directed edges can coexist in one matching-protocol graph:

```json
[
  {"tx": "A", "rx": "B", "txPort": "radio", "rxPort": "radio", "bearer": "data"},
  {"tx": "B", "rx": "C", "txPort": "radio", "rxPort": "radio", "bearer": "iq",
   "modem": {"type": "lora", "sf": 7, "bw": 125000, "cr": 1, "os": 2,
             "preamble": 16, "syncWord": 43}}
]
```

Omitted modem settings select AFSK1200/FM, not an inferred native radio profile.
LoRaWAN is the exception: each frame's native TX plan supplies the directional
PHY, frequency and RX1/RX2 rate, overriding static link modem settings.

The laboratory protocol retains known-graph minimum-hop routing, TTL and visited
paths. Meshtastic instead handles its own native addresses, forwarding, duplicate
suppression and ACK/retry state, with PSK channels or explicitly pinned local
PKI peers. Reticulum uses actual isolated RNS sidecars for
its native identities, paths, encrypted packets, proofs and links. Native frames
are not wrapped in the laboratory router and do not receive fabricated paths.
Transport completion means local emission/acceptance, not an end-to-end ACK.

LoRaWAN supports ABP/OTAA Class A virtual DATA and actual LoRa IQ for one
provisioned EU868 device/network pair, not mesh forwarding or a multi-device
network server. NodeService supplies modeled timing with a default
`clockToleranceMs` of 250 (explicit profiles may use 1..250); direct adapter use
defaults to 10 ms. DATA paces native airtime without IQ; IQ marks modeled
on-air completion separately from quiet-tail/TCP completion. Receive context
is derived from local arrival and known airtime, not remote/hardware clocks.
Status/events disclose `timingSource: "simulated-transport"` and
`hardwareTimingVerified: false`. `rx_context` and `tx_started` are host-only
callbacks, never remote commands. This is not a certified radio MAC. See
[LoRaWAN adapter](mesh_lorawan_adapter.md) for provisioning and timing rules.

Each directed TCP link has a queue of at most 16 frames and its own socket,
pacing and timeout. A stalled neighbor cannot hold the node's other outgoing
links. Packet generation is bounded, but active IQ modulation/decoding still
consumes CPU; DATA is preferable for constrained/VPN experiments. Current IQ
receivers do not superpose simultaneous incoming links into one RF collision
domain. The separate local IQ-medium experiment remains the collision model.

The constructor configures and observes actual separate instances; it is not the
data relay. The current controller still stops the experiment if an endpoint
fails, and its 15-second ownership lease requires keepalive. Live graph structure
changes, autonomous long-lived runs and learned neighbor discovery remain later
work. TCP has authentication but no TLS: use only trusted LAN/VPN networks.

## Verification

`ObriiMeshWireModemSelfTest` tests native bytes, segmented IQ, CRC/FCS, inversion,
gaps and allocation bounds. `ObriiMeshPortNodeSelfTest` exercises mixed bearers,
queue isolation, native LoRa relay and synthetic registered radio ingress.
`ObriiMeshProtocolAdapterSelfTest` checks native protocol and persistent-state
behavior, with optional real Reticulum sidecars. `ObriiMeshRadioEgressSelfTest`
uses an injectable non-RF sink and checks local authorization/expiry.
`ObriiHackRfFiniteBurstSelfTest` verifies finite-buffer/EOS ordering, flush and
USB failure handling, cancellation, and unchanged continuous/PTT zero-fill without
opening a HackRF.
`ObriiMeshPortPlanSelfTest` checks protocol metadata preservation and default
`lab` selection. `ObriiMeshLoRaWanAdapterSelfTest` covers direct LoRaWAN state
and timing; `ObriiMeshLoRaWanNodeSelfTest` covers ABP/OTAA DATA/IQ services,
directional PHY, native ACKs, durable reconfiguration and host-only/physical gates.
`tools/build_mesh_designer.ps1 -Test` builds and runs all 14 mesh suites. The older
three-process, GUI, discovery, pairing and ownership suites remain regressions.
Passing software tests is not physical SDR, cross-host or Raspberry qualification.
