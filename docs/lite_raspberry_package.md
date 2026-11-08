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

The built executable is in `build/linux-release/ObriiSDR`. This is a source
package, not a prebuilt ARM binary. The build still compiles the main program,
Analyzer, FPGA Lab, and IQ Hub. Optional external DMR, Wi-Fi, GNSS and satellite
engines are not installed by this package. The corresponding UI actions may be
visible but need those separate backends. Settings stay in the user's normal
ObriiSDR configuration directory; no personal settings are shipped.
