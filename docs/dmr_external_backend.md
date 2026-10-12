# DMR External Decoder Bridge

Obrii SDR can use several DMR paths from the `DMR backend` combo in the
`Digital Audio` dock:

- `Obrii SDR + mbelib` uses the internal DMR parser with the mbelib voice backend
  path.
- `Obrii SDR + OpenDMR/OP25` uses the internal DMR parser with the OpenDMR/OP25
  runtime voice backend preference. The present OpenDMR/mbelib ABI synthesizes
  AMBE voice only. A separate native pre-vocoder module now applies supplied
  Motorola ARC4/AES-256 keys (beta); see dmr_motorola_privacy.md. Clear traffic
  remains supported. DSD-neo remains the RF-tested external alternative.
- `DSD-neo` mirrors the selected DMR discriminator PCM to an external decoder
  process for comparison and optional decoded-audio playback.
- `GopherTrunk bridge` sends complete 132-dibit bursts to the local virtual
  source adapter and accepts decoded voice/audio metadata in return. It remains
  an experimental comparison backend.

DSD-FME is no longer the active integration target for this project. Keep new
DMR external-decoder work focused on DSD-neo and GopherTrunk unless a specific
test case proves that another backend is needed.

## DSD-neo Data Flow

Obrii SDR exports the narrow selected DMR channel after IQ channel selection and
FM/4FSK discriminator processing:

```text
Fobos IQ -> channel select -> FM/4FSK discriminator -> DmrDecoder
                                                     |
                                                     +-> DSD-neo stdin PCM16LE
```

This intentionally avoids sending full wideband IQ to DSD-neo. The external
decoder receives the same mono signed 16-bit PCM stream that feeds the internal
DMR lab decoder.

## GUI Controls

Open `Digital Audio`, select DMR, then set `DMR backend` to `DSD-neo`.
The DSD-neo row controls the external process:

- `Auto start` starts the configured `dsd-neo` executable.
- Program path defaults to `dsd-neo/dsd-neo.exe` in the Windows release
  layout, resolved relative to the Obrii SDR executable folder.
- Managed auto-start sends PCM through the child process's stdin. `UDP in 7355`
  applies only to a manually started external decoder and is disabled during
  auto-start.
- `UDP 23456` is the local decoded-audio input where Obrii SDR listens.

When the bridge is enabled without auto-start, launch DSD-neo manually and point
it at the same UDP input/output ports.

## Recommended Command

The auto-start path uses `-i -` (raw PCM16LE stdin), the selected baseband rate,
and `-mc` for C4FM demodulation optimizations. The default DMR decoding path is
retained, with stereo decoded audio returned over UDP. Do not add `-nm`: it
forces the legacy mono decoder, not just mono output. Obrii already downmixes
the returned audio. For a manually started decoder, use UDP input:

```powershell
.\dsd-neo\dsd-neo.exe -fs -i udp:127.0.0.1:7355 -s 24000 -o udp:127.0.0.1:23456 -mc
```

If the DMR 4FSK baseband rate in Obrii SDR is changed, Obrii SDR passes that
selected value through `-s`.

## Format Contract

- Input to managed DSD-neo: raw PCM16LE mono through stdin, with a bounded
  asynchronous writer. Manual external decoding retains UDP datagrams.
- Input sample rate: current Obrii SDR DMR 4FSK baseband rate, normally 24 kHz.
- DSD-neo output back to Obrii SDR: UDP raw PCM16 stereo, expected 8 kHz DMR voice.
- Obrii SDR downmixes and upsamples returned voice to 48 kHz before playback.

## GopherTrunk Virtual Dibit Source

GopherTrunk already exposes the useful DMR boundary we need: a continuous
stream of decoded dibits plus a monotonic `baseIdx`. For Obrii SDR this is a
better integration point than pretending to be a generic SDR device, because
Obrii SDR owns the Fobos-specific wideband IQ path, channel selection,
discriminator, symbol timing, and slicer.

The maintained adapter source and local Windows build output are:

```text
tools/gophertrunk_adapter
downloads/gophertrunk/bin/fobos-dmr-virtual.exe
```

It accepts newline-delimited JSON over stdin or one TCP client:

```json
{"baseIdx":0,"dibits":"010310020223332030"}
{"baseIdx":18,"dibits":[0,1,0,3,1,0,0,2]}
{"reset":true}
```

Recommended manual lab command:

```powershell
.\downloads\gophertrunk\bin\fobos-dmr-virtual.exe -interleaved=false -listen 127.0.0.1:7460 -audio-udp 127.0.0.1:23456
```

Obrii SDR now sends a `reset` packet on bridge reset and complete 132-dibit DMR
bursts (`54 payload + 24 center + 54 payload`) in over-the-air order. Packets
carry the monotonic base index, burst/cadence metadata, CACH metadata, AMBE
payload hints, and the current privacy configuration. The managed bridge is
already flattened to one selected channel; it is not a raw interleaved two-slot
RF stream. The adapter uses `-interleaved=false` and explicitly assembles ordered
A-F bursts, retaining center fields for embedded LC and late-entry MI. Missing
or reordered bursts, RF cadence gaps and intervening data headers invalidate a
partial voice superframe. A PI header never counts as another voice burst.

When a privacy mode/key is selected, the managed adapter automatically receives
`-privacy-mode`, `-privacy-key-id`, `-privacy-key-hex`, variant/layout and frame
offset at startup. The same privacy object is sent in reset and live burst JSON,
so changing ARC4/AES settings does not require switching to another backend.
Status and command logs replace the actual key with a redacted byte count.

Only a valid burst-A voice sync can establish voice polarity; embedded B-F
fields must not lock it. Data/PI polarity is normalized independently. The
adapter uses the DMR cyclic Golay wire convention for late-entry MI, checks its
CRC-4, advances the MI LFSR by 32 rounds, and consumes each AMBE frame once.
Skipped frames advance the cipher position rather than shifting later speech.
The native parser's corrected 49-bit hints remain available for voice decoding.

ARC4 with an authorized known key remains experimental. AES-256 voice decryption
is not implemented in this adapter and is explicitly reported as unsupported;
forwarding an AES key is not evidence of decryption. DSD-neo is the implemented
AES path. A correct sync/`applied` flag alone does not establish intelligible
speech or prove that a supplied key matches the transmitter.

## Hytera Basic Beta

Select native `mbelib`/`OpenDMR` or `DSD-neo`, `Hytera Basic (beta)` and an existing KID from the separate
Hytera Basic key table. Supported input lengths are 10/32/64 hexadecimal
characters (40/128/256 bits), not 64/128/256 characters. The table validates
lengths and unique IDs, preserves order/sorting and stores keys with the normal
private settings. ARC4 alignment controls are disabled for Basic.

The managed DSD command uses one `-H` argument, split into 16-character words
where required, plus `-4` to enforce Basic rather than guessing AES from key
length. All-zero keys retain their length. Changing the selected key or mode
restarts only the external decoder through its existing asynchronous path.
Choosing `No privacy` removes both the external key and `-4` and bypasses the
native Basic module. No separate forwarding/decryption checkbox is needed.
Choose `No privacy` for clear calls because Basic enforcement is manual. The
local KID is a table selector, not an RF key-ID validation guarantee.

This is **not** Hytera Full, ARC4 or AES. The Gopher adapter explicitly reports
Basic as unsupported. No key recovery is performed.
The bundled DSD executable advertises `-H`/`-4`; real HP785 Basic interoperability
remains untested. See the [DSD-neo CLI guide](https://github.com/arancormonk/dsd-neo/blob/main/docs/cli.md#keys--privacy-advanced).

### Native Basic Module

`DmrPrivacyProcessor` is independent of the vocoder ABI. The path is
`RF parser -> AMBE FEC -> Basic known-key transform -> mbelib/OpenDMR -> PCM`.
The module repeats the explicit 40/128/256-bit key across the 882 voice bits
of A-F. It precomputes 18 short masks on key changes, not in the IQ/FFT path.
No code from a third-party decoder is linked into this implementation.

Every corrected payload carries its RF anchor, A-F burst index, frame index
and timeslot. Missing FEC/bursts consume their actual position, rather than
incrementing a counter only for accepted audio. Slot 1, slot 2 and unknown/
simplex have separate alignment contexts. Late entry without A, stale or
duplicate frames, invalid keys and malformed payloads are muted. Unciphered
AMBE silence stays intact and consumes its position. A new key or lost lock
requires a fresh A burst. This is not full dual-slot audio routing.

Raw 72-bit audio candidates are disabled while native privacy is active;
there is no fallback that could accidentally vocode ciphertext. Original
encrypted burst data sent to external backends is unchanged. The DSD-neo
ARC4/AES and Gopher ARC4 paths are unchanged. Native Motorola ARC4/AES are now
implemented as a separate beta path; see dmr_motorola_privacy.md.
The existing ARC4 offset/drop controls do not apply to Basic.

`ObriiDmrPrivacySelfTest` covers independent fixed vectors, all 18 frame
positions, all-zero keys, gaps, duplicate/reordered frames, slot separation,
live key switches, silence, invalid input and raw-fallback suppression.
Verbose diagnostics log only mode, key width and applied/muted counters,
never the key or decrypted AMBE. Algorithm tests do not establish on-air
HP785 interoperability or verify that a supplied key is correct.

## Gopher Late-Entry Timing

Upstream was rechecked on 2026-10-11 at commit
`ab2670602702e7d74d87075b4201b60820a40052`. DMR AES/DES is still not wired into
the voice path: `internal/voice/composer/dmr_ep.go` resolves keys only when
`PIHeader.IsRC4()` is true, and `internal/radio/dmr/voice/ep.go` implements RC4.
The AES cores in cryptolab and the working P25 AES path are not DMR AES support.
The upstream [encryption documentation](https://github.com/MattCheramie/GopherTrunk/blob/ab2670602702e7d74d87075b4201b60820a40052/docs/dmr-encryption.md#what-is-not-implemented)
explicitly lists DMR AES/DES as not implemented. Keep using DSD-neo for DMR AES-256;
updating the upstream version alone will not add it. The now user-verified ARC4
adapter binary is retained.

The MI embedded in A-F is committed after decoding those voice frames and
supplies the next superframe's context. Previously it overwrote the current
PI/LFSR context before voice decoding, causing valid CRC and `applied=true` with
incorrect plaintext. On late entry without PI, the first encrypted superframe
is muted until its verified MI is available for the next one. Regression tests
cover both PI-started and late-entry calls with independently encrypted payloads.

## Decoder Audio Continuity


DSD-neo receives the application's PCM16 discriminator waveform without
per-frame peak normalization. The four symbol levels must retain a consistent
scale; user speech-volume controls belong after decoding, not in this input.
Decoded 8 kHz PCM is interpolated to 48 kHz with persistent state across UDP
packets, and the state is cleared when the managed process or output stream
changes. Live process configuration changes use asynchronous stop/restart and
apply the latest settings. A restart still entails a brief intentional gap and
loss of the decoder's previous synchronization state.

Managed stdin avoids UDP input packet boundaries and no longer discards the
first three 20 ms PCM frames. This preserves an initial PI/header when the child
is ready; frames arriving before startup completes or during a restart are
still unavailable to the decoder. Wrong-rate PCM is rejected while configuration
changes settle. Pending stdin writes are limited to about 200 ms (with a 4096-byte
minimum); a stalled writer or a dropped bridge-queue interval restarts the child
instead of silently splicing discontinuous samples into its cipher/timing state.
This is bounded recovery, not gapless reconstruction of missing data.

Unexpected child exits now log decimal/hex exit code and exit status even when
verbose logging is off. Crash restart backoff is armed once per exit, increases
from 1 to 10 seconds for repeated failures, and resets after a stable 10-second
run. Timers and process shutdown remain on the transport thread. The previous
live log reports a child crash but no native exit code or dump, so its precise
internal cause has not been established. These changes do not claim to fix an
unknown bug inside DSD-neo or its dependencies.

## Capture and RF Processing

Starting or stopping channel-IQ WAV capture does not restart the receiver or
disable live spectrum/audio publication. The UI spectrum update also remains
active throughout Channel IQ recording; recording is not a display-pause mode.
The producer explicitly identifies full int8 IQ versus channel int16 IQ;
the recorder validates the byte count and
stops a WAV if its sample rate changes, rather than mixing incompatible rates.

For local RF DMR with audio enabled, channel capture reuses the audio
demodulator's existing complex low-pass output, before its FM discriminator.
It therefore avoids a second full-rate channelizer in the USB callback. The
WAV header contains the actual channel rate (for example about 769 ksample/s
with the automatic high-rate profile at 50 Msample/s), not the 24/48 kHz
discriminator PCM rate. Capture-only and network-IQ modes retain their separate
producer. Recorder AGC affects only saved channel IQ, not decoder input.

The RF mixer/CIC filter remains unchanged. Regression tests check sample count
and discriminator continuity across non-aligned chunks with and without IQ
capture. These changes address scheduling/capture overhead; they do not imply
that RF noise, symbol errors or decryption failures have disappeared.

The optional `ObriiDmrPipelineSelfTest` target can demodulate a channel-IQ WAV
offline through the production DMR path:

```text
ObriiDmrPipelineSelfTest --inspect-iq capture_channel_iq.wav discriminator.pcm
```

Its output is raw PCM16LE mono at 24 kHz for diagnostic comparison, not a voice
recording. Keep captures and diagnostic output private; they are not release
fixtures.

An optional last argument selects 24000 or 48000 Hz. The NumPy-only
`tools/dmr_recording_compare.py` runs local DSD-neo comparisons with and without
its input filter and several additional FIR filters. It prints aggregate
diagnostics, not keys or voice, and writes private process logs only to the
explicit output directory. Counts of printed FEC/CRC/sync lines are not BER or
an audio-quality measurement. Channel captures have already passed through the
live channel filter; this offline path cannot reconstruct upstream losses and
is not a bit-identical replay of the live discriminator input.

The DMR discriminator no longer substitutes a fixed phase below an absolute
IQ-amplitude threshold. Component scaling keeps the conjugate-product phase
independent of receiver gain; exact zero IQ does not invent a phase jump.
Regression tests compare strong/weak IQ, silence, and arbitrary chunk sizes.
Additional filtering and 48 kHz output were compared on local recordings without
a consistent improvement, so the production filter/rate defaults are unchanged.

The GopherTrunk lab adapter now verifies the complete 96-bit PI header CRC
(CCITT init 0, final complement, mask 0x6969) before updating cipher state.
BPTC correction count alone is not sufficient evidence of a valid MI. Corrupt
headers do not replace a valid MI or reset the stream position. This is separate
from DSD-neo's own PI handling; its bundled executable is unchanged.
The complete adapter and regression tests are preserved in
`tools/gophertrunk_adapter`, rather than only the earlier PI patch. Build inside
a compatible upstream GopherTrunk checkout with Go 1.25.11 or its module-required
toolchain:

```powershell
./tools/build_gopher_adapter.ps1 -SourceRoot C:/path/to/GopherTrunk
```

```sh
bash tools/build_gopher_adapter.sh /path/to/GopherTrunk
```

The Linux script places the adapter in `build/linux-release/gophertrunk`, beside
the application, with upstream license notices. Windows uses the existing
`downloads/gophertrunk/bin` deploy path. The C++ `ObriiGopherDmrSelfTest` exercises
real TCP bridge polarity behavior and can export local PCM into private burst
JSONL with `--export-pcm discriminator.pcm 24000 private-bursts.jsonl`.

## Licensing Boundary

DSD-neo remains an external program. Obrii SDR does not compile it into the core
application and does not require it for normal SDR, scan, GNSS, or internal DMR
metadata tests. Release packages may include instructions or a compatible
external binary only when its license and redistribution requirements are
reviewed for that release.

GopherTrunk remains an external lab tool. The Windows deployment may bundle the
virtual-source adapter as a separate executable; when it does, the deploy script
also places GopherTrunk's Apache-2.0 `LICENSE.txt` and
`THIRD_PARTY_LICENSES.md` beside that executable. It is not linked into the
Obrii SDR core application.
