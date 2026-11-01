# Open Amateur Radio Protocol Foundation

Obrii SDR keeps open amateur protocols in the optional `ObriiOpenRadio` static
library. Nothing in this library runs from the receiver callback, starts a
worker, opens a network socket, or arms RF merely because the application is
running. The Digital Audio panel's **Open Radio** button opens the manual lab.

## Implemented Now

### AX.25 / KISS

- AX.25 UI-frame address encoding and decoding, including SSID and up to eight
  digipeaters.
- Control `0x03`, no-layer-3 PID `0xF0`, arbitrary information bytes, and the
  reflected AX.25 FCS.
- KISS port/command byte, FEND/FESC escaping, single-frame decoding, and a
  bounded incremental stream decoder.
- Offline AFSK1200 audio WAV generation and decoding for AX.25 UI frames:
  1200/2200 Hz tones, NRZI, HDLC flags/bit stuffing, and FCS validation.
- Explicit live RX in the Open Radio laboratory: a bounded background AFSK1200
  decoder accepts demodulated 48 kHz mono PCM and forwards FCS-valid frames to
  KISS TCP clients on localhost, default port 8001. It does not run when disabled.

KISS transports AX.25 bytes; it is not an RF waveform. The AFSK1200 path runs
only on explicit WAV import/export or explicit live KISS RX. KISS TX injection
is not connected to the transmitter yet; incoming KISS bytes are discarded.
This is not yet a bidirectional TNC or real-air interoperability claim.
G3RUH-style modems still need a separate optional DSP branch. KISS data
frames conventionally omit the HDLC FCS.

### M17

The project vendors the official GPL `libm17` 1.4.1 at revision
`e1f0f5ca5bfe2dd98eeda8e945725d505b7d6aae`. Obrii currently exposes:

- M17 callsign encoding and decoding;
- packet Link Setup Frames with CAN and CRC;
- whole-packet M17 CRC and segmentation into 25-byte frames with the specified
  frame-number/final-byte-count metadata;
- standard preamble, LSF, packet, and EOT 4FSK symbols.
- raw `.m17sym` symbol-file export and import with LSF and packet CRC checks,
  frame sequence validation and recovered source/destination/CAN/payload.

The lab distinguishes raw packet bytes from the M17 SMS application form
(`0x05`, UTF-8 text, terminating null) instead of silently treating every text
box as an application-compatible message.

This is real protocol framing, but not yet a release claim for live RF timing,
symbol recovery from audio/IQ, Codec 2 voice, or interoperability. Those require the optional
RF/audio modem path and repeatable real-air captures.

## Backend Contract

`OpenRadioBackend` is the lazy extension boundary for NPR-70, AREDN/OpenWrt,
FreeDV/Codec 2, ARDOP, and the future open OFDM/TDMA modem. A backend advertises
its protocols, starts only on explicit request, consumes a typed virtual signal
frame, exposes bounded received-frame draining, and reports failure instead of
silently falling back to a different waveform. The registry is thread-safe and
no placeholder backend is registered as working.

- NPR-70: external compatible modem/TAP adapter first.
- AREDN: supported OpenWrt radio plus Ethernet/TAP; not an SDR Wi-Fi MAC claim.
- FreeDV and ARDOP: optional independent modem engines.
- Open OFDM/TDMA: research-only until two implementations interoperate and the
  selected band, bandwidth, identification, power, and duty cycle are legal.

## Virtual Signal Envelope

`VirtualSignalFrameCodec` is the common versioned boundary for the virtual
signal laboratory. Version 1 contains:

- stream and protocol IDs;
- monotonic sequence and nanosecond timestamp;
- protocol bytes, PCM, CF32, CS16, or signed-symbol payload type;
- sample rate, center frequency, symbol rate, discontinuity, and end-of-stream;
- bounded JSON metadata and binary payload lengths.

The transmitter laboratory can send CF32 IQ to the Virtual IQ receiver in
another Obrii instance. For one computer, select Local IPC and use the same
channel name on TX and RX. For two computers, select TCP on both ends, bind RX
to its LAN address, and enter that address on TX. The default TCP bind is
loopback only. TX and RX can start independently. Live TX drops samples while
no receiver is connected and resumes with current samples after reconnection;
finite text, SSTV and laboratory waveforms wait for a receiver before starting
their paced playback. Heartbeat acknowledgements detect stale connections.
The RX source feeds the existing IQ, FFT, waterfall, audio and decoder pipeline;
no RF output is generated. The receiver can follow incoming center frequency
and sample rate, or retain manual settings. Sequence gaps and output re-enabling
mark a discontinuity. A bounded socket queue fails explicitly on overload
instead of growing without limit.

The direct Local IPC/TCP transport remains one TX to one RX. The optional
`ObriiIqHub` TCP relay accepts up to 16 transmitters and fans out to up to 32
configured virtual receivers. Each receiver has an independent bounded frame
and byte queue. Source ID, source sequence, discontinuities, received/delivered
and dropped counts, queue size, and delay statistics are observable. Optional
fixed delay, seeded noise and packet-level collision suppression are intended
for repeatable software experiments. This does not superpose IQ waveforms.
Mixing two independent transmitters into one coherent RF band is a separate
operation requiring a compatible sample clock, center frequency, timing and
sample format. CF32 needs 8 bytes per complex sample: a 50 Msps stream is
about 400 MB/s before framing and fan-out, so the hub and network must be sized
for the sum of all subscriber outputs. Routing loops and authenticated access
control are not implemented; bind to loopback or a trusted lab-only address.
For example, start virtual RX instances on ports 17661 and 17663, then run
`ObriiIqHub --listen 127.0.0.1:17662 --rx 127.0.0.1:17661 --rx 127.0.0.1:17663`;
configure each TX to send TCP to 127.0.0.1:17662. Different TX streams are
time-multiplexed, not mixed into a coherent RF waveform. Matching sample rate
and center frequency are therefore still the experimenter's responsibility.

This is an experimental trusted-network CF32 link, not an authenticated or
encrypted service and not a clock-synchronized replacement for physical SDR.
Payload types other than CF32 remain envelope-level extension points. Full
time-accurate behavior at very high sample rates still requires throughput
validation on the target machines and network.

## Safety And Regulation

The lab only builds bytes and symbols. It neither tunes nor transmits. A future
RF backend must use the normal Obrii arming, watchdog, hardware ownership, band,
bandwidth, callsign/identification, encryption-state, and duty-cycle checks.
Cryptographic-by-default payloads must not be enabled on amateur allocations
where local rules require open content.
