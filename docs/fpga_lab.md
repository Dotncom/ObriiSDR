# Obrii FPGA Lab (first version)

`ObriiFpgaLab` is a separate Qt application. It can run while Obrii SDR receives;
it does not open the SDR or alter its live FPGA image. Launch it directly from
the runtime folder, or use **Spectrum measurement -> Obrii FPGA Lab** in the
main application.

## Opening a project

- Open a folder containing HDL, XDC and Tcl sources, or select its Vivado
  `.xpr` file. The source tree excludes generated output, caches and symlinks.
  Select a source file to edit it; **Save code** writes it atomically. The editor
  refuses to overwrite a file changed by another program since opening it.
- A selected `.xpr` is view-only in the text editor. If a folder has more than
  one `.xpr`, open the desired file explicitly before building.
- A `.bin` opens as a finished-image preview with size and SHA-256 only. Binary
  images cannot be edited back into HDL.
- Work on a copy of the vendor project. Vivado may regenerate project files or
  outputs even though FPGA Lab only saves the source you explicitly edit.

## Graph and build

The **IQ pipeline sketch** tab lets you arrange DDC, FIR, decimator and
level-trigger blocks and enter their input sample rate, processing clock and
parameters. It checks the sample-rate/clock contract and Nyquist limits.
**Save graph** creates `.obrii-fpga.json` in the project folder; named user
presets can also be saved and restored. Each stage has a signed IQ16 input and
output, one declared `clk` domain and a `valid` strobe. The analyzer reports
input/output sample rates and rejects a stream faster than the declared clock,
invalid Nyquist settings, and a trigger placed before the final stage.
**Synthesize RTL** generates a separate `obrii_iq_pipeline.v` and runs Yosys.
The current block prototypes are:

- DDC: signed complex mixer, 32-bit phase accumulator and quarter-wave Q15
  sine table. Its increment is calculated from the *input sample rate* and
  advances on valid samples, not every clock.
- FIR: generated 17-tap Hamming-windowed low-pass with a user-specified cutoff
  in Hz, Q15 coefficients and saturated IQ16 output. Symmetric taps are paired
  before multiplication. This short filter has a broad transition band,
  especially at a narrow cutoff.
- Power-of-two boxcar decimator (2 to 1024). It averages each group, but does
  not guarantee adequate anti-alias rejection; add an appropriate FIR before
  decimation and verify the actual passband/stopband response.
- Level trigger: IQ16 passthrough with a separate, valid-aligned `out_trigger`
  pulse based on I²+Q² relative to a full-scale 16-bit reference. It must be
  last in the chain.

The standalone DDC/FIR/decimator/trigger chain has a numerical Icarus Verilog
test using a known complex tone and a quiet interval, and passes Yosys
`synth_xilinx` structural checks. These checks do not establish timing closure,
RF performance, spectral purity or correct mapping to the clone's USB/FPGA
interface. Generated RTL is not inserted into the vendor project or live
receiver. Integration still requires a verified top-level stream interface,
clock/reset domains, timing constraints, resource budget and hardware tests.
For the one-megasample test chain (DDC, FIR, decimate by two, trigger), Yosys
estimated 22 DSP48E1 cells and 555 logic cells after symmetric-FIR pairing,
versus 36 DSP48E1 and 931 logic cells before it. These are synthesis estimates
for a standalone module, not a routed resource or timing report for B210.

### User-defined logic blocks

**Custom block** opens a separate graphical IQ16 editor from the pipeline tab.
Place constants, arithmetic and bitwise nodes, Q15 multiply, bitwise LUT4, and
registers. Select a node and choose its input sources in the right inspector;
the directed links update on the canvas. Drag nodes to arrange the diagram.
Choose the I/Q output sources independently. The editor stores
`.obrii-logic.json` in the project folder and can export a standalone
`obrii_user_logic.v`. **Check Yosys** exports the module and starts an openXC7
module check. The **Custom** stage in the IQ pipeline loads a saved logic JSON
file and embeds its validated snapshot in `.obrii-fpga.json` and named presets.
Select that stage and use **Load block** to replace its snapshot. Multiple
stages may use the same block; generated Verilog gives each instance a unique
module name. Changes to the source JSON do not silently change an existing
graph. The combined pipeline passed an Icarus valid-gap test and Yosys
`synth_xilinx` checks. This does not build or flash the B210 image.

This first editor uses one fixed signed 16-bit IQ data type and one `clk`
domain with `reset` and `valid`. Add/sub wrap at 16 bits; Q15 multiplication
saturates. The validator rejects missing references, cycles (including
registered feedback for now), mismatched register depths between data paths,
and I/Q output latency mismatch. Multi-width ports, clock-domain crossings,
BRAM, nested reusable subgraphs, and timing closure remain
future work. Placement/routing of actual FPGA resources stays with openXC7.
This is not a shortcut to a working Wi-Fi FPGA image: the RF/USB interface,
timing, interoperability and legal TX controls need independent validation.

**Build in Vivado** invokes the installed `vivado` executable in batch mode on
an opened `.xpr`, runs `impl_1` through `write_bitstream`, and shows its log.
For a project distributed as Tcl, right-click its `.tcl` file in the tree and
choose **Use as Vivado build script**; this runs the selected vendor script
instead of the generated `.xpr` build script.
Vivado is not included in Obrii. This button does not produce a vendor-specific
UHD `.bin` automatically and never flashes the device. Run only trusted project
files: Vivado projects and their Tcl scripts may execute commands.

## OpenXC7 without Vivado

The **openXC7** tab reads the `.xpr` file as XML and reports the exact part,
active HDL files, constraints, and build blockers. It can:

- Check that Yosys, nextpnr-xilinx, fpga-as and the Kintex database variables
  are available in Linux/WSL.
- Run `synth_xilinx` on the selected standalone Verilog/SystemVerilog file.
  Enter its actual top-module name; success proves synthesis of that module,
  not a complete working SDR image.
- Generate and run the openXC7 `yosys -> nextpnr-xilinx -> fpga-as` flow for a
  compatible pure-Verilog Kintex-7 `.xpr`. The first build generates the chipdb
  for that part if needed. The output is `.obrii-openxc7/full.bit`.
  This command stays disabled when an active Vivado IP, EDIF netlist, VHDL file,
  memory initialization file, missing source, external source or unverified XDC
  command is present.

On Windows, the tool uses the default WSL Linux distribution. Install openXC7
there and expose its commands and `PRJXRAY_DB_DIR`; `KINTEX7_CHIPDB` is optional
and selects a directory with prebuilt chip databases. An
`/opt/openxc7/export.sh` environment file is sourced automatically when
present. The optional **Nix through WSL/Linux** checkbox runs the upstream
Nix development environment. Its first use may download about 10 GB; FPGA Lab
never installs this silently. On Linux the same tab runs the tools natively.
See the [openXC7 getting-started guide](https://github.com/openXC7/getting-started)
for supported installation paths. No generated image is programmed into an SDR.

The examined B210-Kintex-7 clone project targets `xc7k325tffg676-2`. Its
active design references two EDIF half-band filters, one Vivado clock-generator
IP (`.xci`), two MIF memory-initialization files, and XDC `set_false_path`
constraints. Those must be replaced or
verified before a complete openXC7 image can be claimed. The two missing
external `edge_detect.v` and `pps_switch.v` references are disabled in `.xpr`
and have active copies in `top/b200`; they are not current blockers. Retain the
vendor's known-working `.bin` as a recovery image. A new `.bit` is not assumed
to be a UHD-compatible `.bin` until its format and hardware behavior are tested.

The B210-Kintex-7 GitHub repository currently provides a ready `.bin` and a
separate download link for the full firmware project. The `.bin` alone is not
enough for editing or rebuilding. FPGA Lab can open the full clone project once
its actual `.xpr` and HDL sources are available locally, but that integration
has not been hardware-tested.

## B210 clone data path and block placement

The local project routes the radio-chip sample pins through `b200_io` into two
`radio_legacy` instances in `b200_core`. Each radio already contains a
`ddc_chain`, an RX framer with timing/sequence information, and host-format
conversion. The resulting 64-bit streams are multiplexed into the GPIF2
slave-FIFO bridge to the FX3 USB controller. The top level presents each I and
Q value as 12 radio-chip bits padded to 16. These are findings from the HDL,
not measurements of the as-yet-unavailable clone hardware.

This favors small, deterministic FPGA work ahead of the USB boundary: optional
selected-band streams, timestamped events, burst capture and trustworthy
overflow/gap reporting. Flexible FFTs, demodulators, decoders and exploratory
algorithms can remain on the host. The existing DDC/decimator should be reused
or benchmarked before replacing it. An unchanged UHD stream is the safest
first integration target; a new packet format requires matching FPGA, FX3/UHD
and Obrii changes and cannot be presumed backward-compatible. Reducing sample
width on the wire can save bandwidth but increases quantization noise and must
be an explicit, measured mode, never a silent replacement for full-resolution
IQ. Clamping or omitting out-of-band samples is not a substitute for proper
filtering and decimation when reconstructable IQ is required.
