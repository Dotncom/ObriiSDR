# HackRF native RX/TX beta

Obrii SDR contains experimental native HackRF receive and transmit backends. They do not
use SoapySDR. The application loads `libhackrf` dynamically and converts the
native signed 8-bit interleaved IQ stream into the shared Obrii SDR float-IQ
pipeline used by the spectrum, waterfall, demodulators, recording and network
server.

## Runtime

Obrii SDR searches for the Windows runtime in this order:

- `hackrf/hackrf.dll`
- `hackrf/libhackrf.dll`
- `hackrf.dll` or `libhackrf.dll` next to `ObriiSDR.exe`
- the system library search path

The portable Windows package keeps the complete matching runtime in the
`hackrf` subdirectory. Keep these files together:

- `hackrf.dll`;
- `libusb-1.0.dll`;
- `pthreadVC3.dll`;
- `hackrf_info.exe` and `run_hackrf_probe.cmd` for diagnostics.

This separate directory is intentional: it prevents HackRF's libusb runtime
from replacing the libusb DLL used by the Fobos and RTL-SDR paths.

On Linux, install the distribution's `libhackrf` runtime. The source build does
not require HackRF development headers because the backend resolves the stable
C API at runtime.

## Experimental TX

The TX laboratory keeps modulation and waveform generation independent from the
radio backend. Simulator/IQ-file output remains the safe default. Selecting
`HackRF native` resamples that same complex baseband stream to a supported
2–20 Msps DAC rate and feeds signed 8-bit IQ to `hackrf_start_tx`.

TX requires the user to arm RF output for every run. The arm state is never
persisted. TX VGA defaults to 0 dB, the RF amplifier defaults off, a watchdog
limits live transmission, and Stop disables the RF amplifier before closing the
device. Stop RX on the same device first. Initial hardware validation should use
a suitable dummy load and comply with local radio regulations.

Official software and installation instructions:

- https://github.com/greatscottgadgets/hackrf
- https://hackrf.readthedocs.io/

## Initial implementation

- native asynchronous RX via `hackrf_start_rx`;
- signed 8-bit IQ conversion into the shared float-IQ pipeline;
- 2, 4, 8, 10, 12.5, 16 and 20 MS/s presets;
- baseband-filter selection derived from the sample rate;
- live center-frequency retuning and Standard scan support;
- conservative initial gains: LNA 16 dB, VGA 20 dB, RF amp off;
- bias tee off by default;
- live LNA/VGA plus persisted RF amplifier and bias-tee controls;
- automatic or manual MAX2837 baseband-filter bandwidth;
- optional explicit IF/LO tuning with BYPASS/LPF/HPF image-path selection;
- 10 MHz CLKOUT, external CLKIN status, hardware trigger/synchronization and
  RX-overrun safety limit;
- Opera Cake manual, frequency-range and timed-dwell switching modes;
- a separate native-sweep spectrum/waterfall that parses frequency-tagged
  libhackrf sweep blocks rather than feeding them into the raw-IQ path;
- device diagnostics for libhackrf, firmware, USB API, board/revision,
  transfer geometry, CLKIN, Opera Cake addresses and M0 shortfalls;
- local and server-provided receiver selection;
- native device-list enumeration, serial-number labels, and opening the exact
  selected HackRF when the runtime exposes the modern device-list API.

This path is RX-only and ready for hardware-backed testing. If a Windows
computer does not list the receiver, run `hackrf/run_hackrf_probe.cmd`, enable
verbose logging in Obrii SDR, try one start/stop cycle, and keep
`hackrf/hackrf_probe.log` together with `ObriiSDR_diagnostic.log`. A PortaPack
must be in HackRF/USB SDR mode and use the WinUSB driver for the HackRF
interface.

Transmission is intentionally left locked. It must be implemented as a
separate explicitly armed workflow with device and regulatory safety checks.
