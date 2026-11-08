# Mesh Discovery Across Routed LANs And VPNs

Discovery locates explicitly started Obrii mesh services. It does not start a
receiver, grant control, arm RF, create firewall rules or alter router routes.
Windows, Linux/Raspberry, GUI, CLI and NodeHost share the same discovery core.

## Saved Addresses

In Mesh Designer, open **Independent instances** and use the network icon beside
Search. Add a name, numeric IPv4 address, mesh TCP port and role for each row:

- **Seed**: probe this node directly using UDP, with TCP fallback.
- **Directory**: query this node's optional address catalogue over TCP, then
  verify the returned endpoints directly before adding them to the found list.

Save the table and press Search. The enabled rows are retained in Mesh Designer's
OS user settings, separately from receiver settings and project/result exports.
Disabled rows are not contacted. Defaults contain no personal addresses.
The endpoint table's manually entered addresses/ports are also probed.

Use the node's advertised mesh TCP port, normally 17690, not the SDR streaming,
SSH or WireGuard port. Up to 48 saved endpoints, including eight directories,
are supported. CIDR ranges, DNS and automatic subnet scans are deliberately not
implemented: a saved endpoint is an explicit operator-approved destination.

Search still sends local broadcast/multicast probes. Those generally remain in
one network segment; a routed WireGuard interface is not an Ethernet bridge.
Direct UDP goes to port 17699. TCP discovery and catalogue requests use the
selected node's existing mesh TCP listener, not a second public TCP port.

The ordinary UDP search lasts about two seconds; pending routed requests can
extend it to a hard five-second bound. At most four TCP probes run concurrently,
with bounded queues and reply sizes. Search completion/cancellation closes them;
there is no perpetual subnet scan. Service-side public requests are rate-limited.

Found-instance tooltips show the time of the last reply and confirmed discovery methods:
UDP, TCP or directory. The Search status tooltip shows response/failure counts.
A retained old entry is not proof of current availability; attach still checks
the actual service identity, credential and ownership before configuring it.

## Optional Directory

Choose one trusted, reachable Obrii instance as a catalogue. Start it explicitly:

```sh
ObriiCli mesh-node --bind 192.0.2.10 --mesh-port 17690 --directory \
  --credentials-file /private/catalogue-startup.private.json
```

The example IP is reserved documentation space; replace it with your own
reachable LAN/VPN address. With a GUI/CLI mesh service already started, its
trusted local console can toggle the catalogue without restarting RX or mesh:

```text
mesh.service directory-on
mesh.service status
mesh.service directory-off
```

The catalogue is disabled by default. It holds at most 64 UUID-indexed public
records: address, port, instance ID, name, bearers and availability. It never
stores protocol keys, packet payloads, process paths or RF authorization.
Records expire after 90 seconds without renewal. Disabling it clears records;
revoking or replacing a registration removes records owned by that credential.

### Register A Node Automatically

Create a dedicated registered credential on the catalogue using its existing
local pairing code. Keep that credential in a private file, not in a project,
release package, chat or public log. A private directory-client configuration is:

```json
{
  "host": "192.0.2.10",
  "port": 17690,
  "token": "<dedicated registered catalogue credential>",
  "advertiseHost": "198.51.100.20"
}
```

`advertiseHost` is optional. Set it when the receiving clients should use a
different reachable address than the node's listener bind address. All example
addresses above are placeholders. The advertised port is the node's mesh port.

```sh
ObriiCli mesh-node --bind 198.51.100.20 --mesh-port 17690 \
  --directory-client-file /private/node-directory.private.json
```

The same settings can be supplied in a private `--node-options` JSON as
`directoryEnabled: true` and/or `directoryClient: { ... }`. A trusted local
typed console can change registration on a running service:

```json
{"command":"mesh.service","args":{"operation":"directory-client","directoryClient":{"host":"192.0.2.10","port":17690,"token":"<private credential>"}}}
```

Use an empty `directoryClient` object to stop automatic registration. It is
optional, renews once per 30 seconds and performs no IQ/FFT work. Its status
reports configuration, registration, last success and a bounded error class,
never the credential. Catalogue failure cannot stop RX, audio or a mesh session.

For initial trusted-lab setup the catalogue's startup admin token also works,
but it has broader permissions; prefer a separate revocable client credential.
On Linux protect private files with mode 0600; on Windows restrict them to the
owning account. Keep them outside the source checkout. Files named
`*.private.json` are also ignored by Git, not intended as public examples.

### Security And Limits

Catalogue reads and TCP discovery intentionally expose only the same public
locator metadata as LAN discovery. Catalogue writes require an existing mesh
credential; another registration cannot overwrite an owner's live record.
There is no open anonymous directory registration or automatic pairing.

A catalogue is an address hint, not a certificate authority. Each returned
endpoint is TCP-probed and its instance UUID checked. This proves reachability
and matching identity, not cryptographic trust. Normal per-node pairing and
session ownership remain mandatory for control; no secrets transfer to found
nodes. Directory lookup is one level only, without recursive catalogue crawling.

TCP discovery/registration has no TLS. Use trusted LAN/VPN networks, preferably
WireGuard; never expose these listeners with public port forwarding. Router
routes and ACLs must allow the relevant addresses and ports in both directions.
Obrii does not silently rewrite MikroTik, Windows Firewall or VPN settings.

## Software Verification

`ObriiMeshRoutedDiscoverySelfTest` covers the editor and strict saved-profile
validation. `ObriiMeshNodeDirectorySelfTest` covers leases, ownership, capacity
and bounded/cancelled renewal. `ObriiMeshRoutedNetworkSelfTest` uses real local
services and controlled TCP peers to test fallback, privacy, malformed replies,
unreachable directory hints, registration/revocation and cancellation/restart.
These tests do not qualify RF hardware or imply every VPN permits broadcasts.
