# Obrii SDR 7.0.0

Full and Lite packages for Windows x64 and Raspberry/Linux source. Android and
the large optional Wireless add-on are not republished.

## What's New

- **Obrii FPGA Lab:** a separate graphical HDL/IQ-pipeline workbench. It can
  inspect and edit a local project, generate standalone DDC/FIR/decimator/
  trigger RTL, and check small custom logic blocks. It does not yet build or
  flash a working USRP clone image.
- **Virtual IQ Hub:** several software transmitters and receivers can share a
  bounded relay. Per-receiver queues and counters expose losses and delay;
  seeded noise, delay and packet-level collision controls support repeatable
  experiments. This is not coherent RF superposition or a mesh MAC.
- **Network DSP:** display-only spectrum reduction is separate from exact IQ.
  Client/server processing modes can be changed remotely; server labels now
  follow the effective mode. Full-rate IQ still requires a fast local network.
- **Open radio lab:** experimental live AX.25/AFSK1200-to-KISS receive and
  file-based M17 packet/symbol framing. KISS transmit injection and live M17
  RF interoperability are not yet implemented.
- **Hardware groundwork:** a native UHD receiver backend and coherent multi-SDR
  metadata are present, but the B210 clone and shared-clock operation still
  need hardware validation.

## Packages

- `ObriiSDR-v7.0.0-windows-x64.zip`: full Windows runtime, including the main
  application, Analyzer, FPGA Lab and IQ Hub plus selected optional research
  backends and their notices.
- `ObriiSDR-v7.0.0-windows-x64-lite.zip`: same main executable and small native
  device libraries, without large external research engines.
- `ObriiSDR-v7.0.0-raspberry-source.tar.gz`: full Linux/Raspberry source. Run
  `./tools/build_linux.sh` after installing dependencies.
- `ObriiSDR-v7.0.0-raspberry-lite-source.tar.gz`: smaller source package. Run
  `BUILD_LITE=1 ./tools/build_linux.sh` after installing dependencies.

Lite is a smaller distribution, not a separate restricted UI. Existing personal
settings are not in any package. No USRP FPGA image, UHD runtime, Vivado,
openXC7 toolchain, Npcap, or user API keys are included. See README.md,
`docs/lite_raspberry_package.md`, `docs/lite_windows_package.md` and
THIRD_PARTY_LICENSES.txt for prerequisites and component sources.

## Optional Downloads

- For the experimental native USRP RX path, obtain a compatible UHD runtime
  and FPGA image from the official Ettus UHD project or the clone vendor. On
  Windows, place `uhd.dll` beside `ObriiSDR.exe`, or set
  `OBRII_UHD_LIBRARY_PATH` to its absolute path. On Linux, install a matching
  `libuhd.so`. Select a vendor `.bin` by absolute path in the USRP RX dialog;
  keep a known-good image. UHD is not bundled pending license-compatibility and
  hardware verification. Source: https://github.com/EttusResearch/uhd .
- FPGA Lab's openXC7 checks need an independently installed Linux/WSL
  openXC7/Yosys toolchain; FPGA Lab itself is included in Full. See
  `docs/fpga_lab.md` and https://github.com/openXC7 .
- Live Wi-Fi capture requires Npcap from https://npcap.com/ . The unchanged
  optional Wireless add-on is available under the v6.1.1 GitHub release; it is
  not part of the four 7.0.0 packages.
- The Full Windows package includes selected optional DMR, GNSS, LRPT, HDF5
  and HF prediction tools with notices. Lite omits these external engines;
  use Full rather than copying individual DLLs without their dependencies and
  license files.

## Limits

The browser interface and virtual IQ transport have no built-in TLS or account
authentication. Use only on a trusted LAN or behind a VPN/authenticated proxy.
Do not assume lossless full-IQ streaming over a slow link. FPGA and UHD features
are preparatory until tested on the actual B210 clone. Raspberry packages are
source archives; Windows build verification does not replace ARM testing.
The IQ hub, network, display codec, virtual signal, open radio, UHD and FPGA
Lab/pipeline/logic self-tests passed on Windows. The FPGA Lab GUI test needs
the Qt platform-plugin path when run from the MSVC build directory.

Feedback: [mojamajsternya@gmail.com](mailto:mojamajsternya@gmail.com).
Remove API keys, DMR keys, coordinates and other private data from reports.
