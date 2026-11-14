# Obrii FPGA Lab

Launch `ObriiFpgaLab` from the runtime folder or choose **Tools → Obrii FPGA Lab**
in Obrii SDR. Editing and synthesis can run while Obrii SDR receives.

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
- IQ passthrough: preserves the I and Q words and valid strobe with zero added latency.

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

The original vendor B210-Kintex-7 project targets `xc7k325tffg676-2`. Its
active design references two EDIF half-band filters, one Vivado clock-generator
IP (`.xci`), two MIF memory-initialization files, and XDC `set_false_path`
constraints. Those must be replaced or
verified before a complete openXC7 image can be claimed. The two missing
external `edge_detect.v` and `pps_switch.v` references are disabled in `.xpr`
and have active copies in `top/b200`; they are not current blockers. Retain the
vendor's known-working `.bin` as a recovery image. A new `.bit` is not assumed
to be a UHD-compatible `.bin` until its format and hardware behavior are tested.

Open the full clone `.xpr` and HDL sources in FPGA Lab to edit or rebuild
the design. The project browser can inspect a finished `.bin` by size and
SHA-256. Experimental clone images can be loaded for an RX session through
Obrii SDR and UHD, as described in [USRP B210 setup](usrp_b210_setup.md).
Qualifying a vendor image built with the generated IQ pipeline requires a
separate hardware test.

## Editable B210 openXC7 projects

Open your checkout of [B210-Kintex7-openXC7](https://github.com/Dotncom/B210-Kintex7-openXC7)
in Lab. In **B210 Kintex-7**, select the vendor source folder and the SISO or
dual variant, then choose **Create project**. Use a version of the checkout
that includes `project.py`. Lab opens the new `.obrii-b210.json` project and
its staged HDL, XDC and SDC files. Reopen the marker or its folder to resume.
User HDL modules and headers can be placed in the project's `src/user/`.
The project uses the B210 pinout, clocks, and UHD stream interface as its base.

**Build image** invokes the checkout's Linux/WSL build with the edited sources.
Use the toolkit environment described in the checkout's README. If the tool
paths are not exported in the launcher environment, select a trusted `.sh`
file that exports `B210_YOSYS`, `B210_NEXTPNR_ROOT`, `B210_CHIPDB`,
`B210_FPGA_AS` and `B210_PRJXRAY_BIN`. Lab builds in native Linux scratch
storage and copies `.bit`, `.bin`, SHA-256, and timing/log reports to the
selected folder. Select **IQ passthrough** in the sketch and enable its build
option to connect the generated RTL between the DDC and RX framer on each
radio. For this connection the sketch must contain only passthrough blocks.

**Compare through UHD** uses OpenSSH with a trusted host key and a key/agent
login to Raspberry. When FPGA Lab runs on Raspberry itself, leave SSH blank
to use the local radio. Enter the absolute paths to ObriiCli and the
known-good baseline `.bin`. Choose a compatible dual-channel candidate, and
stop other SDR uses of the board before the test. Lab sends the candidate and
checks its SHA-256 on the Raspberry, runs the two images sequentially at
30.72 MS/s per channel, then reloads the baseline through UHD. It reports
rates, counts, UHD overflows, timeouts and timestamp-derived gaps. A CLI
that reports only periodic diagnostics produces a partial result. GNSS and
GPS PPS report separately and require an appropriate UHD library; pass its
remote absolute path in the library field. A validated GPS position needs
independent reference data.

On the tested Kintex-7 clone, the previous Dual openXC7 image and the new
Dual image with the passthrough graph each ran for 15 seconds at 30.72 MS/s
per channel on Raspberry Pi. Both runs had zero UHD overflows, timeouts and
timestamp gaps. The new image delivered GGA, RMC and GSV over UHD and eight
consecutive GPS PPS events. The GNSS module reported a fix. Its coordinates
were outside the scope of this comparison.

## B210 clone data path and block placement

The local project routes the radio-chip sample pins through `b200_io` into two
`radio_legacy` instances in `b200_core`. Each radio already contains a
`ddc_chain`, an RX framer with timing/sequence information, and host-format
conversion. The resulting 64-bit streams are multiplexed into the GPIF2
slave-FIFO bridge to the FX3 USB controller. The top level presents each I and
Q value as 12 radio-chip bits padded to 16, as described by the vendor HDL.

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
