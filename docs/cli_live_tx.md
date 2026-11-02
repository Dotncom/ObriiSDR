# Live Microphone, PTT And Manual CW

The optional `ObriiCliTxModule` runs independently of RX and the GUI. Start a
session in one terminal, and send control commands from another. Starting a
session arms it but captures no microphone and emits no IQ until PTT/key on.

```sh
ObriiCli module tx microphones
ObriiCli module tx microphone --tx-sink virtual --freq 145.5M --rate 48k \
  --voice-mode nfm --deviation 2.5k --signal-bandwidth 12.5k \
  --virtual-name lab-voice --tx-control-port 17677
ObriiCli module tx control ptt on --tx-control-port 17677
ObriiCli module tx control status --tx-control-port 17677
ObriiCli module tx control ptt off --tx-control-port 17677
ObriiCli module tx control stop --tx-control-port 17677
```

Commands: `status`, `arm`, `disarm`, `stop`, `ptt on|off`, `key on|off` (manual
CW only), and `gain 0..16`. Control is loopback TCP with one JSON line per
request; it is not a public authenticated service. Use a separate control port
for each TX process. Commands do not require keyboard focus on a GUI window.

```sh
ObriiCli module tx manual-cw --tx-sink file --output key.cf32 --freq 1M
ObriiCli module tx control key on
ObriiCli module tx control key off
ObriiCli module tx control stop
```

`--tx-tone 700` sets the CW offset. The key has 5-ms edge ramps. Each activation
is limited by `--tx-watchdog-ms` (100..60000, default 30000); repeated on commands
do not extend it. `disarm`, capture failure, queue overrun, Ctrl+C and shutdown
release PTT. This is a host-software watchdog, not a hardware RF interlock.
Sink/driver latency and a stalled OS can delay release. Ordinary release drains
at most 200 ms; a watchdog/error release skips that drain. No unattended RF.

## Microphone

`--mic-device` selects an exact name from `microphones`; omit it for the system
default. Capture starts only on PTT, stops on release, and can start again in
the same session. `--mic-gain` is software gain; the CLI never unmutes or
changes OS microphone volume. Status reports PCM/IQ counts, RMS/peak dBFS,
clipping, queue length, capture state, last error and release reason.

Windows uses the existing native WASAPI/WinMM capture and streaming conversion.
Linux requires optional Qt Multimedia and a device supporting the chosen exact
mono PCM16 rate. Unsupported formats fail explicitly, rather than interpreting
samples at a different rate. `--rate` supports whole 8000..192000 samples/s.
No arriving PCM for two seconds stops capture. The pending PCM limit is 250 ms.

`--voice-mode am|nfm|wfm|dsb|usb|lsb` uses the shared streaming modulator.
`--deviation` is FM peak deviation; `--signal-bandwidth` is the optional full
transmit-signal filter width. The bandwidth must leave room for deviation and
audio. Wide FM needs a sufficiently high baseband rate. RF device sample rate
is a different setting, not the microphone rate.

For repeatable non-microphone tests, `--pcm-input FILE` explicitly substitutes
a regular mono PCM16LE file at `--rate`, up to 64 MiB, paced in 20-ms blocks.
It rewinds on each PTT activation; EOF releases PTT. Status labels it `pcm-file`.

## Sinks And Limits

- `file --output NEW.cf32`: no RF. New-only CF32LE plus JSON sidecar, at most
  1 GiB and 10000 bursts. Only keyed bursts are concatenated; wall-clock gaps
  are **not** IQ samples. UTC/startSample/samples identify each segment.
- `virtual`: local IPC by name, or TCP with `--virtual-host IP --virtual-port
  PORT`. TX runs without an RX and reconnects when RX appears. Disconnected
  IQ is dropped and counted, never buffered indefinitely or replayed later.
- `hackrf`: requires `--rf-confirm`; `--device-rate` 2..20 MS/s, device index,
  VGA gain and RF filter use the existing hardware backend.
- `uhd`: requires `--rf-confirm`; `--device-rate` must equal `--rate` because
  this path has no implicit UHD resampler. Driver/device constraints still apply.

RF is not opened before PTT. Check a suitable load/antenna and transmission
authorization before selecting an RF sink. Synthetic tests do not qualify RF
output or validate a real microphone. JSON output is bounded and asynchronous
so a blocked stdout consumer cannot itself prevent the software watchdog.
