# Obrii SDR 8.0.0

This release adds a standalone console frontend, shared GUI/CLI production cores,
on-demand decoder workers and an embedded command window. FT4 and live M17
reception reuse the GUI decoders; optional Codec2 provides clear M17 voice.
The experimental Mesh Designer launches local virtual SDR nodes and an IQ hub,
with reusable device profiles. It is not yet a mesh MAC/routing implementation.

## Downloads

- Windows Full: GUI, Analyzer, FPGA Lab, Mesh Designer, CLI and optional workers.
- Windows Lite: ordinary RX/audio, GUI and CLI without heavy optional engines.
- Raspberry Full/Lite: one source project for the GUI and console frontends.
- Android preview: updated network client, version code 14. This is a debug-signed
  testing APK, not a store/production-signed Android release. Its separate
  `com.fobosapp.networkclient.preview` ID permits side-by-side installation
  without removing an older application or its private settings.

Extract a fresh Windows folder rather than publishing or sharing an existing
working directory. Keep personal INI settings, credentials and recordings private.
For Raspberry, install prerequisites and run `./tools/build_linux.sh`; GUI and
CLI outputs stay together in `build/linux-release`.

## Optional Dependencies

Full Windows includes the redistributable Codec2 runtime, its dependencies,
notices and exact source package. Lite omits it. Debian/Raspberry voice can use
the distribution's `libcodec2-dev`. See `docs/ft4_m17_receive.md`.
UHD and matching vendor FPGA images remain separate; follow
`docs/usrp_b210_setup.md`. Never replace the Kintex-7 clone's image automatically.
The separate Wireless add-on is not included or republished. See
`docs/wireless_addon.md` for installation. Absent decoder engines are reported
and do not load during ordinary reception.

## Qualification

Windows software tests cover PCM/IQ decoding, Codec2 voice, worker restart,
recording, command permissions, exact traces and audio isolation under FFT load.
The new CLI FT4/M17 changes have not been qualified on Raspberry or real RF.
Some specialized GUI tools still have no CLI equivalent; see `docs/cli_parity.md`.
The Agent API is disabled by default, loopback-only and read-only until locally
granted receiver control. It does not grant file or RF TX access or start an AI.

Feedback: mojamajsternya@gmail.com
