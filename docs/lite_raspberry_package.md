# Obrii SDR Lite (Raspberry/Linux source)

This is the same Obrii SDR source and user interface as the Full Raspberry
package. Lite excludes optional mbelib-neo, ITU-HF model source/data, and VkFFT
source. Its build disables those engines; regular Fobos reception, FFT,
waterfall, and analog audio remain available.

Install the normal dependencies and Fobos libraries as described in README.md.
Build the extracted Lite tree with:

```sh
BUILD_LITE=1 ./tools/build_linux.sh
```

The GUI build installs **Obrii SDR (Fusion 110%)** in the application menu and
on the desktop when a desktop directory is configured. It uses Qt Fusion and
110% scaling. In a Raspberry Pi Wayland session the shortcut uses XWayland
when available so Qt 5 drop-down items can be selected with the mouse.
An explicit `QT_QPA_PLATFORM` setting takes precedence. To recreate the
shortcut after moving the source tree, run `python3 tools/install_linux_shortcut.py`.

The built executable is in `build/linux-release/ObriiSDR`. This is a source
package, not a prebuilt ARM binary. The build still compiles the main program,
Analyzer, FPGA Lab, and IQ Hub. Optional external DMR, Wi-Fi, GNSS and satellite
engines are not installed by this package. The corresponding UI actions may be
visible but need those separate backends. Settings stay in the user's normal
ObriiSDR configuration directory; no personal settings are shipped.

If the package includes experimental B210 Kintex-7 FPGA images, find them
under `fpga/b210/`. To use one, select a compatible `.bin` in **USRP UHD RX**.
The UHD library and its standard images are installed separately. See
[`usrp_b210_plan.md`](usrp_b210_plan.md) for the image profiles, GPS / QTH
setup and bench coverage.
