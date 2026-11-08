# Mesh Ports UI: Phase 1

## Project Compatibility

The project format remains `obrii-mesh-project`, version 1. Old documents keep
their previous JSON representation when no new options are supplied. New fields
are appended to the C++ aggregates:

- `Node.ports`: optional directional port array; absent means network RX and TX
  ports named `radio`. The same ID is valid in different directions.
- `Node.protocol`: optional public native configuration; absent means `lab`.
- `Link.bearer`: `inherit` (the session default), `data`, or `iq`.
- `Link.txPort` / `Link.rxPort`: default `radio`.
- `Link.modem`: `afsk1200` or `lora`. The inspector's LoRa defaults are SF 7,
  BW 125000, CR 1, OS 1, preamble 16, sync word 43.

Basic shape/range checks belong to the project. The service's PortPlan validates
the complete graph, compatible protocol interfaces, modem and backend support.
The local legacy IQ hub and PacketDialog do not implement these extensions; the
constructor disables their launch actions for extended projects.

## Remote Handshake

The controller retains the authenticated instance barrier, session ownership,
lease polling, rollback and incremental result cursor. It refuses advanced
options unless all relevant services explicitly advertise `portBindingsVersion:1`,
`perLinkBearers:true`, `availableModems`, and `nativeProtocols` as appropriate.
The topology snapshot preserves the public port/protocol/link/modem fields.
Discovery, pairing and the process-only credential cache are unchanged.

Physical RX requires `radioRxAllowed:true` and an explicitly allowlisted host
source. The inspector exposes receiver/channel IDs, center frequency and a JSON
port editor including modem settings; a button copies an incoming IQ link modem.
Physical TX can be configured in the inspector but requires
`radioTxAllowed:true`, never implicit transmitter arming. LoRaWAN selection
requires the matching service capability and a local profile. Protocol JSON and
`profileId` are editable, but private keys and backend paths are rejected.
Reticulum requires a locally configured host-sidecar; Python, module and state
paths are never supplied by the remote topology.

## Native Commands

The Protocol tab sends the JSON object as `args` of an owned `op:"protocol"` RPC.
Its public test API is `RemoteDialog::executeProtocolCommand(row,args,error)`;
completion is reported by `protocolCommandFinished`. Adapter commands use `op`:

```json
{"op":"status"}
```

```json
{"op":"send","destination":258,"text":"Hello","reliable":false}
```

Meshtastic configuration uses `nodeNumber` and `relayRole`. Reticulum commands
use native destination/link hashes. Host services must strip the topology's
`type` and unrelated protocol fields before invoking strict adapters. Successful
native send/protocol RPCs use the normal status/identity envelope; raw command
results, including asynchronous `pending`, may be returned under `protocolReply`.

Meshtastic reserves node numbers 0..3 and the broadcast value. New nodes default
to `0x100 + graphIndex` (zero-based). An explicitly configured node number is
preserved without remapping.

Native events retain native IDs, addresses, authentication metadata and payloads.
They do not acquire a fabricated laboratory path or minimum-hop route. All
status, command replies and result exports use explicit public-field projections;
command bodies, keys, tokens and host-local sidecar paths are not exported.

## Verification

`meshremoteselftest.cpp` includes v1/extended JSON roundtrips, production inspector
controls, independent RX/TX, unsupported capability rejection, native result and
pending reply decoding, and export privacy. Existing data/IQ, discovery, pairing,
credential cancellation and late-configure rollback cases remain in the suite.
Tests use an isolated INI settings directory and offscreen widgets. An optional
PNG prefix also captures the ports inspector and native protocol view.
