# USRP software lab

Open **Wireless lab → USRP lab** or add **USRP software lab** from the DSP
board's Digital category and open the block. Both open the same window.
The block launches an explicit job and has no streaming ports. Loading a
layout or opening the window leaves the lab idle.

The local GUI console accepts `usrp open`, `usrp status`, `usrp cancel`,
and the job names below. Use a JSON command for paths with spaces or
for MAC replay. The command is available to the local operator.

```json
{"command":"usrp","args":{"action":"gsm-decode","input":"C:/captures/channel 62.cf32","arfcn":62,"timeslot":1}}
```

UMTS file replay uses the same console and DSP-board worker. The carrier
offset is the carrier frequency minus the recorded center frequency.

```json
{"command":"usrp","args":{"action":"umts-decode","input":"C:/captures/wcdma.cs16","rate":16000000,"offset":-2600000,"format":"cs16","start":32000000}}
```

The standalone CLI waits for completion and prints the result as JSON.

```text
ObriiCli usrp status
ObriiCli usrp contracts
ObriiCli usrp wifi-loopback
ObriiCli usrp zmq-loopback
ObriiCli usrp lte-zmq
ObriiCli usrp gsm-demo
ObriiCli usrp gsm-decode "channel 62.cf32" 62 1
ObriiCli usrp gsm-replay assignments.jsonl 62 path/to/gsm
ObriiCli usrp umts-decode capture.cf32 7680000
ObriiCli usrp umts-decode capture.cs16 16000000 -2600000 cs16 32000000
ObriiCli usrp umts-demo ipv4
ObriiCli usrp umts-demo rlc-recovery
```

| Job | Input and result |
|---|---|
| `contracts` | Worker ownership, IQ framing and software radio contract checks. |
| `wifi-loopback` | Windows GNU Radio runtime. Tests the existing 20 MHz legacy OFDM PHY across the 26 synthetic cases. |
| `zmq-loopback` | Bundled localhost transport check. Python with PyZMQ verifies byte-identical CF32 samples in each test direction and confirms a bounded send timeout without a peer. |
| `lte-zmq` | Linux srsRAN build with ZMQ, TUN and SCTP. Runs attach, IP transfer and orderly shutdown inside the lab's owned namespaces. |
| `gsm-demo` | Generates a noisy synthetic IQ file and verifies seven CS-1 control blocks. |
| `gsm-decode` | Centered CF32LE GSM IQ, 3250000/3 complex samples per second, up to 32 MiB. ARFCN is supplied by the caller. Omit the timeslot for BCCH only, or choose 1–7 to also decode PDCH CS-1. |
| `gsm-replay` | MAC JSONL up to 128 KiB and the verified `gsm` directory from a same-recording IQ job. Resolves packet assignments against the SI13/SI1 context at the packet frame. |
| `umts-decode` | Closed CF32LE, CS16LE or CI8 capture. Use centered chip-grid IQ at 3.84 MS/s or raw IQ at 5–32 MS/s. Reads at most four overlapping windows from the requested sample index and writes `umts.json`. |
| `umts-demo` | Synthetic 3.84 MS/s IQ exchange. Select `prach`, `rrc`, `attach`, `ipv4` (default) or `rlc-recovery`. |

GSM writes `decoded.json`, the evidence manifest and, for IQ jobs, a BCCH
PCAP. The JSON includes supported SI/SoLSA/GPRS fields, packet assignments
and their physical provenance. External MAC replay retains its own
provenance. Check `summary.status` for the decode outcome and
`summary.packet_fec_verified` for physical packet validation.

For UMTS, `summary.status` distinguishes `NO_CELL_SYNC`, `CELL_SYNC`,
`BCH_DECODED` and `MIB_DECODED`. The job status `PASS` means the file
analysis completed. The full report includes scrambling codes, carrier
tracking, paths selected from CPICH and CRC results for BCH blocks.

In the window, choose **UMTS synthetic IQ demo**, then select the scenario.
The presets progress from PRACH/AICH through RRC, NAS attach and IPv4 over
DTCH. The RLC AM preset simulates a lost downlink block and verifies recovery.
The worker checks the recorded stage outcomes and the size and SHA-256 of
every artifact. The result folder contains the synthetic IQ, stage reports
and the `umts/manifest.json` index. The lab uses synthetic identities and
exchanges packets only within the generated IQ.

The app detects the local `sandbox/usrp-u0-u2` adapter from the checkout
containing its executable. For another local checkout, set
`OBRII_USRP_LAB_ROOT` to that lab's absolute path. Set
`OBRII_USRP_LAB_PYTHON` if the Python 3.11+ executable is outside PATH.
Keep the tested GSM lab beside the USRP lab as `gsm-bcch-u3`. The runner
expects the locally prepared engines described in those labs. It reports
a missing dependency without installing software. On Linux, an alternate
prepared srsRAN lab may be selected with `OBRII_USRP_LTE_ROOT`.

The bundled `zmq-loopback` works independently of the local lab checkout.
Use `OBRII_USRP_LAB_PYTHON` to select a Python executable with PyZMQ when
the default environment lacks it. A missing runtime yields `BLOCKED`.
The result is stored in a per-run system temporary directory shown by the job.
The transport check uses synthetic IQ and loopback sockets on the same host.
On Linux the app also detects the lab's `.venv/bin/python`.

Keep `umts-sync-u5` beside the USRP lab and use Python with NumPy. On
Windows UMTS runs natively with a 4096 MiB free host RAM gate, one logical
CPU, a 1400 MiB job cap and a 75-second worker deadline.

On Windows, GSM and LTE use the controlled WSL Ubuntu profile. Start with
WSL stopped, no existing `.wslconfig` and at least 4 GiB free host RAM.
The owned VM uses 3 GiB RAM and 2 vCPUs, with a 1 GiB swap file. On native
Linux use systemd with cgroup v2. LTE requires a local root session there.
With the memory controller disabled, Linux runs the finite `contracts`
and UMTS jobs with a 1400 MiB per-process address-space limit,
the same CPU quota and task limit, and a bounded lifetime.
The GSM worker is capped at 1 CPU and 1400 MiB, and the LTE worker at
1.5 CPUs and 2048 MiB. The Windows Wi-Fi worker requires 2048 MiB free host
RAM and is limited to one logical CPU and 1400 MiB. Jobs run one at a time
from the same lab.

Use **Cancel** in the window or Ctrl+C in the CLI. Each attempt keeps its
own result directory under the local lab's `runs` folder. The CLI returns
0 for success, 2 for a blocked dependency or environment, 130 for
cancellation and 1 for failure. The software lab covers finite IQ and
simulations. Live USRP capture, RF transmission and operation with
hardware peers require a hardware-qualified backend.
