# Obrii SDR

Obrii SDR is a wideband SDR receiver and research workstation, optimized first
for RigExpert Fobos SDR hardware while also supporting additional receiver
backends.
The current packaged release is Windows-first, with active Linux/Raspberry Pi
support and a separately maintained Android USB/network client. Version 6.4
adds protected DMR direct-mode transmission, FPV video auto-scan, a repaired
Windows microphone path, wider SSTV/ATV coverage, quick amateur-band tuning,
and additional 3D waterfall presentation controls. It retains the independent
DSP audio branches, configurable DDC/PFB processing, portable experiment folders,
LAN FFT offload, GNU Radio bridge and connection diagnostics from 6.3.
It retains live Wi-Fi IQ decoding, experimental HT20 support, research
backends for GNSS/LRPT/HF prediction, and expanded measurement tools. It retains
the 6.0.0 DMR, FT8, signal-generation and terrain-propagation features, plus
the graphical DSP path designer, live audio-filter chain, Multi-VFO views, and
hardware-tested native HackRF RX backend. Live I/Q/time 3D constellations and
optional radial surfaces are available in Signal Lab, research and DSP widgets.
It keeps
the high-rate Fobos path, native RTL-SDR/rtl_tcp, optional SoapySDR, external
NMEA/u-blox GNSS, SDR GNSS acquisition diagnostics, 2D/3D waterfall and replay,
Zoom FFT, CW/SSTV decoding, DMR experiments, and a hardware-independent transmit
laboratory with simulator/IQ export, experimental DMR Tier II direct voice generation,
and an experimental, explicitly armed native HackRF output. FFTW/VkFFT selection, exact Hz-per-point FFT sizes, stale-frame
dropping, and multistage channel extraction keep the wideband desktop and
Raspberry paths responsive. Android is released separately only when its
package changes.

Release 6.4.0 also improves HackRF TX buffering and stop handling, signal-lab
generation and decoder self-tests. Protected DMR TX requires the optional
OpenDMR backend and matching authorized keys; repeater wake-up and trunk
signaling are not implemented.
Remote processing is opt-in and experimental; update both computers, use a
trusted LAN and run the connection test before enabling IQ transfer. The receiver
and audio path remain local. See `docs/dsp_stream_manager.md` for limits and setup.

## Feedback

Questions, feedback and bug reports: [mojamajsternya@gmail.com](mailto:mojamajsternya@gmail.com).
Please include the application version and a short reproduction description.
Remove API keys, private coordinates and other sensitive data from any attachments.

## Windows Release Package

Release 6.4.0 provides a Windows x64 core ZIP and a Raspberry/Linux source tar.gz.
No Wireless add-on or Android package is uploaded with this release.
The unchanged optional Wireless add-on from 6.1.1 is also compatible with 6.4.0;
it does not need to be downloaded again for an existing installation.
The add-on contains the portable GNU Radio
Wi-Fi and Wireshark tools; to enable them, run `Install-Wireless-AddOn.ps1` beside
`ObriiSDR.exe` or extract the add-on into that same folder. See
`docs/wireless_addon.md`. Npcap is not bundled.
Extract the Linux source with `tar -xzf ObriiSDR-v6.4.0-raspberry-source.tar.gz`;
the build/run shell scripts have executable permissions. Android packages are
maintained and released separately.

Use the extracted Windows core folder as a self-contained runtime directory.
For local Windows development, the persistent runtime is `build/runtime`;
compiling the Release target writes `ObriiSDR.exe` there directly. The
`release/bin` folder is an older runtime copy, not the development launch path.
Keep `ObriiSDR.exe`, the Qt DLLs, Fobos DLLs, FFTW DLL, libusb DLL, VC runtime
DLLs, and the `platforms/qwindows.dll` plugin together in the same folder tree.

The app stores `ObriiSDR.ini` in the per-user configuration directory (for
example `%LOCALAPPDATA%\ObriiSDR` on Windows or `~/.config/ObriiSDR` on Linux), so it
does not require administrator rights and survives replacement of the release
folder. Existing `%LOCALAPPDATA%\FobosAPP\FobosAPP.ini` and legacy INI files
beside the executable are migrated automatically when no Obrii SDR settings
file exists. Diagnostic logs are still written beside the executable when that
directory is writable.

`ObriiSDR.ini` contains user presets, scan lists, QTH map markers, API keys for
map providers, and UI settings. The desktop app provides
`Settings... -> Settings backup -> Export settings... / Import settings...` for
making a separate backup or moving settings to another computer.

DMR voice remains experimental. The transmitter can generate clear or protected Tier II
direct/simplex voice bursts from microphone/PTT/WAV with Color Code, time slot and radio
IDs. Motorola ARC4 40-bit and AES-256 TX privacy are supported with PI headers, MI
late-entry signaling and per-superframe key-stream alignment; repeater wake-up and trunk
signaling are not yet implemented. AMBE+2 encoding, including protected TX, requires the
optional OpenDMR backend. Windows packages may include optional
`dmr_voice_backends/fobos_dmr_voice_*.dll` GPL backend modules. See
`THIRD_PARTY_LICENSES.txt` and `licenses/dmr_voice_backend/` before
redistributing AMBE-capable binaries.

GNSS/QTH has two paths:

- External serial GNSS receivers are the practical path. Standard NMEA modules
  provide live coordinates, time, satellite diagnostics, sky view, QTH map
  updates and raw text logging. u-blox-compatible modules, tested with
  NEO-M8N, can additionally use UBX NAV output, constellation configuration,
  raw UBX/NMEA capture, and module configuration save.
- SDR-only GPS L1 C/A acquisition remains a laboratory workflow. See
  `docs/gnss_preflight_4.1.md` for the recommended test order, RF notes, and
  the generated acquisition report files.

## External GNSS Receiver Quick Start

1. Connect a serial GNSS module such as NEO-M8N and select its COM/tty port in
   the GNSS/QTH section. Most NEO-M8N boards use 9600 baud by default.
2. Press `Connect`. Valid NMEA GGA/RMC fixes update the green current-position
   marker, QTH locator and satellite diagnostics.
3. Press `Raw` only when you want an exact UBX/NMEA binary capture under
   `recordings/gnss_raw/`. These files may contain location data and are not
   included in release packages.
4. For u-blox modules, `UBX sys` first polls `CFG-GNSS` and synchronizes the
   system checkboxes. Adjust GPS/GLO/GAL/BDS/QZSS/SBAS checkboxes and press it
   again to apply. `Save cfg` writes the current module configuration to
   non-volatile memory when supported by the module.

## Optional DMR Voice Backends

Obrii SDR loads optional DMR voice modules from `dmr_voice_backends/` at runtime.
The app itself uses the stable C ABI in
`FobosDMRVoiceBackend-gpl/include/fobos_dmr_voice_backend.h`; third-party AMBE
implementations are kept as optional Git submodules so users can fetch and build
them only when needed.

Optional backend dependencies:

- mbelib-neo: https://github.com/arancormonk/mbelib-neo
- OpenDMR/softdmr: https://github.com/hicaoc/softdmr

When cloning from Git:

```bash
git clone https://github.com/Dotncom/ObriiSDR.git
cd ObriiSDR
git submodule update --init --recursive third_party/mbelib-neo third_party/softdmr
```

If you are working from a source zip instead of a Git checkout, clone the
optional dependencies into the same paths:

```bash
git clone https://github.com/arancormonk/mbelib-neo.git third_party/mbelib-neo
git clone https://github.com/hicaoc/softdmr.git third_party/softdmr
```

Build the backend modules after the submodules are present:

```powershell
cmake -S FobosDMRVoiceBackend-gpl -B build\fobos-dmr-voice-backend-gpl-vs `
  -G "Visual Studio 17 2022" -A x64
cmake --build build\fobos-dmr-voice-backend-gpl-vs --config Release
```

Copy the resulting backend libraries into the runtime folder:

```text
build/runtime/dmr_voice_backends/fobos_dmr_voice_mbelib.dll
build/runtime/dmr_voice_backends/fobos_dmr_voice_opendmr.dll
```

On Linux/Raspberry Pi the same backend project builds shared libraries with
`.so` names, usually `libfobos_dmr_voice_mbelib.so` and
`libfobos_dmr_voice_opendmr.so`; place them in the application runtime
`dmr_voice_backends/` directory.

## Optional External DMR Bridges

The Digital Audio dock has a `DMR backend` selector:

- `Obrii SDR + mbelib`
- `Obrii SDR + OpenDMR/OP25`
- `DSD-neo`
- `GopherTrunk bridge` through the bundled virtual-dibit adapter

The optional DSD-neo and GopherTrunk bridges are disabled by default. DSD-neo
receives selected DMR discriminator PCM as raw mono PCM16LE and can return decoded
voice over local UDP. GopherTrunk receives complete DMR dibit bursts through the
local virtual-source adapter. Native mbelib/OpenDMR and DSD-neo support supplied
Motorola ARC4/AES-256 keys; GopherTrunk supports ARC4 only, not DMR AES-256.
Decryption follows the selected mode and saved KID automatically, without a
separate forwarding checkbox. Diagnostic commands redact key material.
See `docs/dmr_motorola_privacy.md` for synchronization and compatibility limits.

Manual DSD-neo command matching the default ports:

```powershell
.\dsd-neo\dsd-neo.exe -fs -i udp:127.0.0.1:7355 -s 24000 -o udp:127.0.0.1:23456 -mc
```

See `docs/dmr_external_backend.md` and `config/dmr_backends.example.json` for
the full bridge contract. Both decoders are optional external tools and are not
required for normal SDR, scan, GNSS, or internal DMR metadata use.

## Main Features

- AM, SAM, NFM, WFM, USB, LSB, DSB, CW, FT8, RTTY, FSK, PSK, ATV, SSTV, APT,
  WEFAX, Meteor LRPT beta, and DMR beta mode selection.
- Digital Audio dock with FT8/RTTY/FSK/PSK decoding work, DMR beta
  sync/activity monitoring, and an optional DSD-neo lab bridge.
- FT8 Station workspace with structured decodes, standard QSO sequencing, true UTC-slot scheduling, local ADIF logbook, optional WSJT-X UDP interoperability, PSK Reporter, QRZ Logbook, and external TQSL/LoTW workflow.
- DMR Lab Capture metadata fields and JSON sidecar files for controlled audio
  and Channel IQ recordings.
- Video dock with analog TV, SSTV, NOAA APT, HF WEFAX, and Meteor LRPT beta
  monitor/test modes.
- Hardware-independent TX laboratory with AM/NFM/WFM/DSB/USB/LSB audio, CW,
  FT8, standard ITA2 RTTY, laboratory OOK/ASK/FSK/GFSK/GMSK/BPSK/QPSK,
  SSTV/ATV, IQ export, and guarded experimental native HackRF output.
- Separate arbitrary-signal laboratory for repeatable carrier/basic-waveform,
  chirp, multi-tone, noise, PSK/QAM/FSK and configurable OFDM generation, with
  I/Q/constellation plus 2D/3D waterfall previews, peak/RMS/PAPR metrics, up to
  250 Msps and 20 million IQ samples per generation, .cf32 plus JSON export,
  and routing through the selected guarded TX backend.
- Concurrent receive and transmit are supported when they use separate devices.
  The native HackRF backend blocks only the same physical HackRF index because
  HackRF is half-duplex; future backends expose their own duplex capability.
- Correct `HF1 + HF2` direct-sampling spectrum layout from `-Fs/2` through
  zero to `+Fs/2`.
- Stable async Fobos streaming path with safer start, stop, and retune handling.
- Live retuning of frequency, sample rate, FFT length, gains, bandwidth, audio,
  and display settings.
- Visual manual-retune scan by cycling through an explicit center frequency
  list on Standard or Agile APIs. Adjacent centers are auto-spaced by at least
  the current sample rate, and the dwell time is user-adjustable because the
  practical scan rate is limited by host/libusb retune overhead, not by the RF
  hardware alone.
- Resizable/dockable UI, zoomable spectrum and waterfall, Y-axis level display,
  level range controls, and frequency widgets with units, presets, and step
  buttons.
- Network server/client mode with remote control, audio/spectrum streaming,
  channel IQ streaming, full IQ client processing, and observer clients.
- Runtime-selectable receiver backend layer with Standard Fobos API, Fobos SDR
  agile API, native RTL-SDR, rtl_tcp, and optional SoapySDR discovery.

## Build Notes

The repository tracks the application source code and release helper scripts.
The local third-party SDK/runtime folders are intentionally ignored:

- `fobos/`
- `fobos_agile/`
- `fftw-3.3.5-dll64/`
- `libusb-1.0.27/`
- `release/`

The current Windows build expects Qt 5.15.2 for MSVC, MSVC 2022, CMake, FFTW,
libusb, the standard Fobos SDK, and the Fobos SDR agile SDK. `CMakeLists.txt`
now uses cache paths instead of hard-coded include/link directives:

- `FOBOS_STANDARD_ROOT`
- `FOBOS_AGILE_ROOT`
- `FFTW_ROOT`

On the existing Windows development machine those default to the local SDK
folders above. On Linux, install Qt5 Core/Widgets/Network/SerialPort/Multimedia, FFTW
single-precision libraries, libusb, and Linux builds of the standard/agile Fobos
libraries, or point the three cache paths at local builds.

Build locally, then launch `build/runtime/ObriiSDR.exe`. The first deployment
of runtime DLLs and documentation, or any later refresh of those files, uses
`-Deploy`. It preserves recordings, logs and existing local config files:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_windows_local.ps1 -Deploy
```

Optional GPL DMR voice backend modules are built separately after the DMR
submodules are present; see `Optional DMR Voice Backends` above.

Create the Windows zip after deploy:

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_windows_release.ps1
```

The application icon is stored in `packaging/icons` and is embedded into the
Windows executable, the Qt runtime resources, Linux desktop installs, and the
Android USB/network client launcher.

Optional Windows code signing requires a real code-signing certificate from a
trusted certificate authority if you want Windows to show a verified publisher.
After the certificate is available as a PFX file, deploy and sign with:

```powershell
$env:FOBOSAPP_CODESIGN_PFX="C:\path\to\certificate.pfx"
$env:FOBOSAPP_CODESIGN_PASSWORD="pfx-password"
powershell -ExecutionPolicy Bypass -File tools\deploy_windows.ps1 -Sign
```

You can also sign an already-built executable directly:

```powershell
powershell -ExecutionPolicy Bypass -File tools\sign_windows.ps1 `
  -Path build\runtime\ObriiSDR.exe `
  -PfxPath C:\path\to\certificate.pfx `
  -CertPassword pfx-password
```

Initial Linux configure example:

```bash
cmake -S . -B build/linux-release -DCMAKE_BUILD_TYPE=Release \
  -DFOBOS_STANDARD_ROOT=/usr/local \
  -DFOBOS_AGILE_ROOT=/usr/local \
  -DFFTW_ROOT=/usr
cmake --build build/linux-release -j
```

Raspberry Pi OS quick trial:

```bash
unzip ObriiSDR-raspberry-source.zip
cd ObriiSDR-raspberry-source
chmod +x tools/*.sh
sudo ./tools/install_deps_debian.sh
sudo cp packaging/linux/99-fobos-sdr.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
./tools/prepare_fobos_linux.sh
./tools/build_linux.sh
./tools/run_linux.sh
```

For future Raspberry Pi DMR voice/backend testing, start from a Git checkout
with the optional DMR submodules, or clone them into the unpacked source tree:

```bash
git submodule update --init --recursive third_party/mbelib-neo third_party/softdmr
# or, inside a source zip tree:
git clone https://github.com/arancormonk/mbelib-neo.git third_party/mbelib-neo
git clone https://github.com/hicaoc/softdmr.git third_party/softdmr
```

Then build the app and the optional backend modules:

```bash
sudo ./tools/install_deps_debian.sh
./tools/prepare_fobos_linux.sh
./tools/build_linux.sh
cmake -S FobosDMRVoiceBackend-gpl -B build/fobos-dmr-voice-backend-gpl \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/fobos-dmr-voice-backend-gpl -j
```

For a normal runtime layout, copy the built backend `.so` files next to the app:

```bash
mkdir -p build/linux-release/dmr_voice_backends
cp build/fobos-dmr-voice-backend-gpl/libfobos_dmr_voice_*.so \
  build/linux-release/dmr_voice_backends/
./tools/run_linux.sh
```

During development, Obrii SDR also searches `build/fobos-dmr-voice-backend-gpl/`
directly, so the copy step is optional for quick local tests.

On Windows, create that source zip with:

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_raspberry_source.ps1
```

The archive is written to `release/ObriiSDR-raspberry-source.zip`.

If `libfobos` or `libfobos_sdr` are not installed under `/usr/local`, pass their
roots explicitly:

```bash
FOBOS_STANDARD_ROOT=/path/to/libfobos \
FOBOS_AGILE_ROOT=/path/to/libfobos_sdr \
./tools/build_linux.sh
```

`tools/prepare_fobos_linux.sh` builds the vendored Fobos source trees shipped
with the project and installs both Linux libraries into
`third_party/fobos-linux`:

- `third_party/patched/libfobos`
- `third_party/patched/libfobos-sdr-agile`

The standard tree is the patched `libfobos` source used by Obrii SDR, so Linux
builds do not silently fall back to the official upstream version. If the
vendored trees are missing, the script stops with an error. For a deliberate
upstream experiment only, run:

```bash
ALLOW_UPSTREAM_FOBOS_CLONE=1 ./tools/prepare_fobos_linux.sh
```

That clone path is intentionally opt-in because it may not contain the stability
fixes expected by this application.

Optional local install after a successful Linux build:

```bash
sudo cmake --install build/linux-release --prefix /usr/local
```

## Ready Audio Relay

The Network Settings dialog includes an optional ready-audio UDP relay. Enable
`Send ready audio by UDP` on the machine that demodulates audio and enter the
target IP/port. On the listening machine, run another Obrii SDR instance and
enable `Receive ready audio by UDP` on the same port. The relay carries 48 kHz
mono PCM frames and is intended for low-latency Raspberry Pi monitoring without
an external VLC loop.

For VLC or another generic player, enable `Serve VLC-compatible HTTP/WAV audio`
on the machine that demodulates audio. Open
`http://<raspberry-ip>:21092/audio.wav` from the player, replacing the IP and
port as needed. This mode also carries 48 kHz mono PCM, but uses an ordinary
HTTP WAV stream instead of the Obrii SDR UDP framing.

## Android USB/Network Client

An Android USB/network client lives in `android/network-client`. It is a
separate Android Studio project that can talk to the existing Obrii SDR network
server over the TCP control channel or run the current direct USB/OTG Fobos
preview path. The current Android client supports remote
start/stop/settings, observer/controller role handling, spectrum/waterfall
display, zoom/pan, tap-to-tune, frequency presets, band-marker overlays, and
48 kHz mono ready-audio playback in network mode. The USB/OTG path can open a
Fobos receiver through Android USB Host, apply basic receiver settings, and
show an experimental live spectrum/waterfall preview on compatible hardware.

Android debug APKs are signed by the normal Android debug key. For a signed
release APK, set these environment variables before running
`./gradlew assembleRelease` from `android/network-client`:

```bash
export FOBOSAPP_ANDROID_KEYSTORE=/path/to/release.keystore
export FOBOSAPP_ANDROID_KEY_ALIAS=fobosapp
export FOBOSAPP_ANDROID_KEYSTORE_PASSWORD=...
export FOBOSAPP_ANDROID_KEY_PASSWORD=...
```

## Current Limitations

- DMR clear voice and supplied-key Motorola ARC4/AES-256 reception have been
  tested on real signals. Compatibility and weak-signal quality still vary;
  Hytera Basic remains unverified, Hytera Full and complete Tier III grant
  following are not implemented. DMR TX supports clear, Motorola ARC4 40-bit
  and AES-256 Tier II direct/simplex calls through the optional OpenDMR encoder;
  repeater wake-up and trunk signaling are not implemented.
- The new WEFAX, LRPT, and analog video paths are beta features and need more
  testing with real signals.
- Meteor LRPT currently provides a QPSK IQ monitor, not a final decoded image.
- Full IQ client processing needs a fast LAN and can be too heavy for slower
  network links.
- Linux/Raspberry Pi support is usable for current test builds, but still needs
  broader hardware and distribution testing.
