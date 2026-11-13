# Installing Obrii SDR

Download the archive for your system from [Obrii SDR releases](https://github.com/Dotncom/ObriiSDR/releases).

## Windows x64

Extract the entire ZIP into a new folder. Run `ObriiSDR.exe` for the GUI
or `ObriiCli.exe --help` for console use. Mesh Designer, MeshSim, Analyzer
and the optional decoder and transmitter workers live in the same folder.
The package includes the Qt runtime, Codec2 for M17, and the Fobos USB
libraries. For supported Fobos firmware and recovery instructions, see
`firmware/fobos/README.txt` inside the package.

## Raspberry Pi OS Bookworm, 64-bit

Extract the ARM64 archive and run these commands from its directory.

```sh
sudo apt update
sudo apt install libqt5widgets5 libqt5concurrent5 libqt5network5 libqt5serialport5 \
  libqt5multimedia5 libqt5multimedia5-plugins libfftw3-single3 libusb-1.0-0 libcodec2-1.0
./start-obrii.sh
./bin/ObriiCli --help
```

The launcher uses Qt Fusion at 110% by default. If a Fobos is accessible
only as root, install the included udev rule and reconnect it.

```sh
sudo install -m 0644 fobos-sdr.rules /etc/udev/rules.d/60-fobos-sdr.rules
sudo udevadm control --reload-rules
```

For an ARM64 source build, clone with `git clone --recurse-submodules`
or use the release's Raspberry source archive. Run the following
from the source directory.

```sh
sudo ./tools/install_deps_debian.sh
ALLOW_UPSTREAM_FOBOS_CLONE=1 ./tools/prepare_fobos_linux.sh
./tools/build_linux.sh
./tools/run_linux.sh
```

## Optional components

- **M17 voice.** The Windows ZIP includes Codec2. On Debian or Raspberry Pi
  OS use the distribution's Codec2 package. For a separate Windows build,
  follow [Codec2 runtime setup](ft4_m17_receive.md#codec2-runtime-and-redistribution).
  See [M17 voice and PTT](open_amateur_radio.md#m17).
- **USRP B210.** Install a compatible UHD library and FPGA image for the
  actual board. See [UHD and GPS setup](usrp_b210_setup.md).
- **SDR GNSS PVT, satellite images and other research engines.** Follow
  [the backend installation instructions](research_backends.md).
- **Wi-Fi research tools.** See [the optional Wireless add-on](wireless_addon.md).

## Building on Windows

Install the Visual Studio 2022 C++ desktop tools, CMake, Qt 5.15 (MSVC
x64), and the FFTW single-precision development files. Download the
Standard and Agile SDKs from [RigExpert Standard releases](https://github.com/rigexpert/libfobos/releases)
and [RigExpert Agile releases](https://github.com/rigexpert/libfobos-sdr-agile/releases).
Keep each header with its x64 import library and pass those directories as
`FOBOS_STANDARD_ROOT` and `FOBOS_AGILE_ROOT` when configuring CMake.
Point `Qt5_DIR` to Qt's `lib/cmake/Qt5` directory and `FFTW_ROOT` to
the FFTW headers and import library. Build the Release configuration.
Place the corresponding x64 runtime DLLs beside the executable and
deploy Qt with `windeployqt`.
