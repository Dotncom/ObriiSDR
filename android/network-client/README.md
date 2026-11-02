# Obrii SDR Android USB/Network Client

The 8.0 preview APK uses `com.fobosapp.networkclient.preview` and a debug signing
key. It installs alongside older versions without deleting their settings;
configure its server connection separately. This is not a store-signed build.

This is the Android USB/Network client for Obrii SDR. It is separate from the
desktop Qt Widgets application on purpose: Android can either work as a
lightweight remote client for a desktop/Raspberry Obrii SDR server, or run the
current direct USB/OTG Fobos preview path on compatible phones, hubs, and
Android USB stacks.

## What Works

- Connects to a desktop/Linux/Raspberry Obrii SDR instance running in
  `Network Settings -> Server` mode.
- Uses the same TCP control-channel handshake:
  `FOBOSAPP_HELLO 1`, `FOBOSAPP_OK 1`, `FOBOSAPP_PING 1`,
  `FOBOSAPP_PONG 1`.
- Sends server-side processing commands with the existing JSON control schema:
  `start`, `stop`, `settings`, `requestPriority`, and `priorityResponse`.
- Receives server-side `spectrum` frames and draws a simple spectrum/waterfall.
- Decodes binary display spectra as `f32le` or signed `s16le-db10` (0.1 dB),
  validating the exact primary/reference payload size before allocating it.
  Display frames are limited to 1,048,576 bins; raw IQ is skipped rather than
  decoded. JSON and binary spectra both honor explicit
  `magnitudeOrder: ascending-frequency`; other/legacy frames use FFT-native
  order and are shifted once, including odd lengths with `ceil(N/2)`.
- Lets the user tap the spectrum/waterfall scale to retune the listening
  frequency on the server.
- Receives `audio` frames as base64 `pcm_s16le`, 48 kHz mono, queues them on a
  dedicated audio thread, and plays them with Android `AudioTrack`.
- Supports the multi-client observer/controller role messages at a basic level.
- Requests authoritative `serverState` after the handshake, including for
  observers, and applies `settingsAck` responses. Settings carry request IDs;
  stale acknowledgements are ignored and a missing acknowledgement triggers a
  state refresh. Rejected settings restore the server's actual values.
- Leaves the server's physical receiver selection unchanged. Stop, priority and
  state requests carry no receiver settings. Local server output uses the
  explicit `setServerLocalOutput` command; switching from IQ processing to this
  client's server-side audio/spectrum mode uses `setProcessingMode`.
- Preserves server sample rates, FFT lengths and modulation IDs outside the
  built-in Android presets instead of rounding them on the next Apply.
- Optional automatic reconnect while the client is running, with 1/2/4/8/15-second backoff, a fresh handshake
  and authoritative state synchronization. Disconnect cancels retries. Shared
  receiver Start, settings and controller privileges are never replayed.
- Independent remote VFO A/B: each has its own frequency, bandwidth, demodulator,
  FFT and audio setting. Only the selected VFO's audio and spectrum are rendered.
  Both VFOs can remain active; Stop affects the selected VFO. An unexpected
  reconnect restores only this client's previously active VFOs.
- A small status badge inside the spectrum shows RX, Stop, Wait, No data,
  Retry, Off or Error without adding another panel. A/B is included in VFO mode.
- Uses a collapsible controls panel so the spectrum/waterfall can fill the
  screen, especially in landscape orientation.
- Includes a USB/OTG panel for direct-receiver work. It can list
  Android-visible USB devices, request permission for a likely Fobos device, and
  safely open/close the device to verify Android USB Host access. It also has
  an experimental OTG session probe that claims the likely streaming interface,
  reports bulk endpoints, reads firmware info with a safe vendor control
  transfer, performs short one-shot bulk read tests, and can start an
  experimental live OTG preview with Android-side FFT frames.

## Current Scope

The network path uses server-side processing. Independent VFO mode requires a
server exposing `RemoteVfoService`; shared-receiver mode remains available for
legacy/CLI servers. A VFO can tune only inside the server's captured span and
does not start, stop or retune the physical receiver. The server operator must
start acquisition first. Change between shared and VFO modes while disconnected.

The USB/OTG path remains experimental. It includes a native USB/DSP bridge and
local preview/audio paths, but does not replace the full desktop pipeline.
Device, phone, powered-hub and USB-stack combinations still need hardware tests.

## Build

Open `android/network-client` in Android Studio, or build from a terminal with
an Android SDK installed:

```bash
cd android/network-client
./gradlew assembleDebug
# Windows: .\gradlew.bat assembleDebug
# Protocol regression tests: .\gradlew.bat testDebugUnitTest
```

The app has no third-party runtime Android dependencies. JVM protocol tests use
JUnit and org.json. Building requires Java 17, Android SDK/Build Tools 35,
NDK 27.0.12077973 and CMake 3.22.1 for the native USB/DSP bridge.

## Test Flow

1. Start Obrii SDR on the machine with the receiver.
2. Open `Network Settings`.
3. Set `Mode` to `Server`.
4. Set `Processing` to `Server processing (spectrum/audio stream)`.
5. Confirm the control port, default `21090`.
6. Launch the Android client, enter the server IP and port, and press
   `Connect`.
7. After `Control channel ready`, use `Start`, `Stop`, or `Apply`.

For independent listening, enable **Independent VFO** before Connect, select
**VFO A** or **VFO B**, then Start. Frequency, bandwidth and mode apply to that
VFO only. Server span is displayed in Settings. Manual Disconnect releases the
session without automatically restarting either VFO on the next Connect.

For a repeatable test without RF hardware, build `ObriiDspFlowSelfTest` and run
it with `OBRII_REMOTE_VFO_SELFTEST=1` and `OBRII_REMOTE_VFO_SMOKE_SERVER=1`.
This exposes the production transport and VFO broker on **127.0.0.1:21092**,
fed by synthetic IQ at 100 MHz / 2 MS/s. The two test signals are at 100.1 and
100.2 MHz. The server exits after ten minutes. If
`OBRII_REMOTE_VFO_SMOKE_CONTROL` names a local control file, writing `drop`,
`stop` or `start` to that file tests reconnect or source-state transitions.
For Android Emulator use host **10.0.2.2**, port **21092**. Without the smoke
server flag, `OBRII_REMOTE_VFO_SELFTEST=1` runs the headless service regression
tests and exits.

For now, keep the Android client in the same trusted LAN/VPN as the server.
The control protocol is intentionally simple and is not encrypted by itself.

## USB Sandbox Test Flow

Use a powered USB-C/OTG hub or dock. The Fobos receiver can draw up to about
5 V / 1 A, so direct phone power is not a reliable test setup.

1. Connect external power to the hub/dock.
2. Connect the hub/dock to the phone.
3. Connect the Fobos receiver to the hub/dock.
4. Open the Android client and expand the controls panel.
5. Press `USB scan`.
6. Confirm that a `16d0:132e` device appears.
7. Press `USB permission/open` and accept the Android permission prompt.
8. Check the log for `openDevice OK` and the detected `bcdDevice` hint.
9. Press `OTG open`.
10. Confirm that the log says `claimed if...` and lists a `bulk IN` endpoint.
11. Press `OTG info` and check that hardware/firmware/build strings are shown.
12. Press `OTG read`. A `-1` result is acceptable before streaming starts; this
    button only verifies that the Android bulk-transfer path is wired.
13. Press `OTG sample test` for a one-shot `OPEN -> START -> bulk read -> STOP`
    experiment at 100 MHz / 8 Msps.
14. Press the top `Net` button to switch it to `OTG`.
15. Press the normal top `Start` button for a short live spectrum/waterfall
    preview. The log should include `preview first FFT frame 4096 bins`.
16. Press the normal top `Stop` button before unplugging during repeated
    experiments.
17. Press `Tools` only when you need the lower-level USB diagnostics:
    scan, permission/open, raw read, one-shot sample test, or explicit close.

Expected Fobos identifiers from the patched desktop libraries:

- VID/PID: `16d0:132e`
- Standard API generation: `bcdDevice 0000`
- Agile API generation: `bcdDevice 0101`
