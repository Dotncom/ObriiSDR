# Obrii SDR 9.0.6

This release consolidates the current Windows, Raspberry Pi ARM64 and Android
worktrees.

- The desktop application includes the refined controls layout, compact Fobos
  Details dialog, Fobos IF and metrics controls, spectrum/analyzer updates,
  FPGA Lab improvements, native UHD operation and the current receive/transmit
  and network improvements.
- Windows Full and Raspberry Pi ARM64 packages contain the Fobos update kit:
  the compiled updater for that platform, validated runtime catalog, all
  images listed in that catalog, and recovery/update instructions. Firmware
  implementation and reverse-engineering material are intentionally not part
  of the release.
- The Android preview now offers direct **HackRF One/Jawbreaker USB OTG RX**
  alongside the existing RTL-SDR, Fobos and network paths. It is deliberately
  receive-only, uses the public HackRF USB interface, and is capped at 8 MS/s
  until it is independently tested on physical Android hardware.

The Android APK is an installable preview build. Android USB host behaviour
varies by phone, USB cable and power supply; use a powered OTG setup when
needed.
