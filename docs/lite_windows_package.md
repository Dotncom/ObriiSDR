# Obrii SDR Lite (Windows x64)

This is the same ObriiSDR executable as the full package, with only the native
runtime needed for ordinary receiver tuning, spectrum/waterfall display, and
analog audio. The UI is unchanged. Native Fobos support is included; small
HackRF and RTL-SDR runtime libraries are copied when present.

The release includes FPGA Lab and all seven Windows interface styles. Choose
a style in Obrii SDR under **Tools → Settings → Interface style**. For FPGA
Lab, see [the build and installation instructions](fpga_lab.md).

Run `ObriiSDR.exe` from this folder. Settings and presets remain in the usual
per-user ObriiSDR settings directory, not in the package. No private INI files,
keys, logs or captures are copied here.

External research backends (PocketSDR, SatDump, ITU-HF, HDF5, DSD-neo and
GopherTrunk) and the optional Wi-Fi/Wireshark add-on are not bundled. Their UI
entries may still be visible but require the full package or separately
installed backends. This is a size-reduced runtime, not a separate feature-
restricted build. It does not include UHD/USRP support.

Rebuild the folder from a deployed `build/runtime` with:

```powershell
.\tools\package_windows_lite.ps1 -Force
```

The script uses an explicit file list and verifies each copied file. It does
not create a ZIP or remove the full runtime.
