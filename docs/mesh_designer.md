# Obrii Mesh Designer — first working stage

`src/mesh` contains a separate Qt Widgets application, model/library persistence
and a process/session manager. It uses the existing Obrii CLI and IQ hub through
their public launch/control interfaces; no receiver/DSP implementation is copied.
The main application's source and root CMake do not need to change for this stage.

## What works

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
- Explicit finite CW stimulus from a selected node, using a directly owned
  `ObriiCliTxModule --tx-sink virtual` process. It cannot select a hardware sink.
- Asynchronous readiness, bounded control queues/timeouts/logs, session-generation
  protection, cancellation during startup, restart and cleanup of owned processes.
  Closing the window stops its session; it never scans for or kills other Obrii PIDs.

The initial A–B–C graph describes radio reachability: A's test IQ reaches B,
but it does not automatically reach C. Mesh packet forwarding, MAC, neighbor
discovery and routing are not implemented yet. This is a working designer and
virtual-radio launcher, not a completed mesh/MANET simulator.

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

The source center and sample rate are common to the session and editable while
stopped. Links have gain, delay and an enabled flag; edits during a session are
staged and clearly require a restart. The existing hub has no live topology API.
Mixing requires the exact same rate/center for all transmitters. The current
backend caps a run at eight mixed-IQ RX nodes or 32 frame-forwarding RX nodes;
the project format/editor does not inherit that runtime node cap. Resource
estimation, distributed clocks, resampling and sharding are future work.

All current listeners bind to `127.0.0.1`. RX arguments always select a virtual
backend; the explicit test transmitter always selects a virtual sink. Local
physical receivers and existing GUI/CLI sessions are not attached or modified.

## Build and test without disturbing the main build

Windows, using the same Qt SDK as the main program:

```powershell
./tools/build_mesh_designer.ps1 -Test
./build/runtime/ObriiMeshDesigner.exe
```

The script uses `build/mesh-designer`, copies only the new designer EXE into
`build/runtime`, and adds Qt dependencies only when absent. It never rebuilds
or replaces the main GUI/CLI executables. Integration tests copy the existing
CLI/hub/TX binaries into a private test directory so their running processes
do not lock files used by the parallel main build.

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
Windows runtime tests are verified; Linux/Raspberry compilation remains to qualify.

After the main integration work settles, this directory can be included with
`add_subdirectory(src/mesh)` in the root build. It remains one repository and
can link shared modules as its protocol needs grow; the independent configure
entry is an isolation mechanism, not a fork of Obrii's radio code.

## Next implementation steps

1. Remote lifecycle owner/agent over SSH with explicit process ownership,
   readiness, port allocation/tunnels and bounded teardown, using this library.
2. Embedded live spectrum/packet/route inspection and per-node start/stop.
3. Live link/topology updates in the hub, with acknowledgment and session epochs.
4. Traffic/packet endpoints, receive-and-retransmit relay behavior, MAC,
   discovery and routing; demonstrate real A→B→C delivery.
5. Positions/mobility-driven channels, route repair, partitions/merges and
   deterministic scenario/event replay.

None of these later items is presented as already working by the current UI.
