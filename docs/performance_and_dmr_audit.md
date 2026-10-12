# Performance And DMR Audit

Release 6.0 status: the notes below describe successive investigations, not
remaining failures in the final build. Native Motorola ARC4 and AES-256 receive
decryption are now RF-confirmed on the user's tested setup; Gopher ARC4 is also
RF-confirmed, while Gopher DMR AES remains unsupported. See
dmr_motorola_privacy.md for the final native path and limits.

## Scope

This audit separates display work from signal-processing correctness. It does
not change the receiver startup sequence, FFT length/window/RBW, privacy keys,
DMR demodulation filters, or the full-spectrum data supplied to other tools.

## Implemented Resource Changes

1. Spectrum display reduction now evaluates only the selected detector. Peak,
   minimum, nearest sample and average-dB modes no longer calculate an unused
   power-domain average for every source bin. Average-power mode keeps its
   original power-domain mathematics. A one-element input no longer passes
   reversed bounds to std::clamp.
2. DSP workspace/VFO display routes are cached until the scene changes. The
   per-frame path no longer enumerates every scene item for every VFO widget.
   VFO JSON is parsed only when its content changes. This does not cache the
   main window's configuration serialization.
3. Hidden/minimized main displays do not render a second copy of the DSP
   workspace output. Screen reduction uses the visible workspace width rather
   than a hidden main window's width. The independent DSP output still runs.
4. Hidden/minimized DSP boards stop requesting visual frames. Embedded research
   widgets check scene/view visibility and viewport intersection before copying
   IQ. Receiver, recording and demodulation paths remain independent.
5. Multi-VFO UI context is rebuilt only when receiver context changes. Hidden
   table/mosaic views do not reduce each incoming spectrum. DSP VFO views retain
   their independent feed.
6. FFT waterfall batches swap reusable frequency buffers instead of copying the
   full axis for each row. Scan-axis conversion for the scientific display runs
   only when that display is due for refresh.

## Verification And Limits

The deterministic spectrum test compares all five reduction modes against an
independent reference, including regular/irregular axes, overlays, narrow zoom,
odd FFT lengths, non-finite levels and one-element input. The DSP test covers
route-cache invalidation after reload/removal, VFO reassignment, unchanged
receiver context, frame throttling and hidden/minimized boards.

On this Windows host, a Release microbenchmark reducing 4,194,304 input bins to
1,600 display bins took about 129 ms in Peak mode before the detector change
and about 23 ms after it. This is the display-reduction stage, not FFT execution,
an end-to-end FPS measurement, or a Raspberry performance guarantee.
Average-power mode retains the more expensive conversion deliberately.

Native Raspberry execution and real-device audio continuity still require
hardware testing. Network-client display handling remains a separate path;
these changes do not imply a complete network rendering audit.

## Long FFT Versus Display Resolution

The main pipeline still calculates the full configured FFT. Its zoom/reducer
then maps the visible frequencies to screen columns. A screen column can cover
thousands of actual FFT bins; it is not an FFT bin or an RBW measurement.

Reducing FFT length just because the screen is zoomed out changes acquisition
duration and RBW. A weak narrow carrier may become less visible in the wider
noise bandwidth even when both plots have the same pixel dimensions. Computing
only one frequency per screen column can miss a carrier between those points.
Neither is an exact substitute for full-resolution Peak reduction.

There are three distinct future options:

1. Exact display-only postprocessing: for constant normalization/correction,
   max(log(power)) equals log(max(power)). Peak/minimum/nearest-sample displays
   can reduce linear powers first and calculate dB only for display columns.
   Average-power can reduce linear powers too. Average-dB still requires the
   original per-bin logarithms. Frequency-dependent calibration, spectral
   suppression, VBW/traces, recording, hunters, scans and scientific consumers
   must be applied correctly or force the full-resolution output path. This
   reduces logarithms, allocations and memory traffic, not the full FFT itself.
2. An explicitly selected overview FFT can be smaller, with independently
   computed precise measurements/selected-band views. This is a multiresolution
   instrument, not a lossless replacement of the requested FFT. It needs honest
   RBW/bin/time labels and must not silently change recorded/measurement data.
3. A selected-band IQ path can frequency-shift, anti-alias and decimate before
   FFT. ZoomSpectrumProcessor already does this for the separate narrow-spectrum
   tool, not automatically for the main zoom. It uses a fixed Hann window and
   its own filtering/normalization; calibrated equivalence to the wide FFT has
   not been established. Its input filtering also costs CPU and must stay opt-in.

For example, 50 MS/s and 1 Hz bin spacing need approximately 50 million samples
over one second. A selected 100 kHz band at 250 kS/s needs about 250,000 samples
for the same bin spacing and observation duration. Actual anti-alias rejection,
passband response, window ENBW and measured runtime determine whether this is
an acceptable instrument mode. Overlapping one-second windows can produce more
rows but do not create independent one-hertz measurements every few milliseconds.

Pruned FFTs can evaluate selected DFT outputs using all input samples, but FFTW
has no general built-in pruned transform. Its authors report modest practical
gains and recommend considering it mainly for very small output subsets. A
full-band display preserving each group's peak still needs information about
all frequencies in that group, not just selected DFT output samples.

References: [FFTW pruned FFTs](https://www.fftw.org/pruned.html),
[SciPy ZoomFFT and acquisition-time limits](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.ZoomFFT.html).

## DMR Test Findings

The latest live test does not establish improved speech quality. During stable
intervals the audio path generally keeps up without IQ loss, but the log reports
about 0.7-1.0 seconds of demodulation work per second of stream, leaving limited
CPU headroom. There is a PCM queue discard, and DSD-neo reports FEC/signaling
errors even in otherwise continuous intervals. A transport gap around a retune
must be distinguished from steady reception. Heterogeneous log-line counts are
not a measured voice-frame error rate.

Display resource savings may reduce scheduling pressure; they do not repair
incorrect symbols, damaged privacy metadata or failed FEC. Do not change keys,
RC4 alignment or DMR filters on the basis of these log counts alone. The next
signal-quality gate is a repeatable saved IQ fixture with known clear speech,
reference decoded PCM/metadata, and per-stage timing/error counters.

### DSD Bridge Follow-Up

The following live test kept the IQ sequence continuous during steady reception,
with demodulation taking roughly 710-740 ms per second at 50 MS/s. The 71- and
163-frame PCM queue discards coincided with privacy changes and blocking decoder
process restarts. They are not evidence of continuous USB loss or an incorrect
key. DSD still reported voice/signaling FEC errors outside those transitions.

Three bridge defects have been corrected without changing the RF filters,
symbol timing, key handling or the internal/OpenDMR/Gopher vocoder paths:

- The 20 ms peak-driven input gain is removed. DSD receives the existing PCM16
  discriminator samples unchanged, so a noise peak cannot rescale the four
  symbol levels in that frame. Input peak/RMS scans now run only for sampled
  verbose diagnostics, not every frame.
- Decoded 8 kHz audio interpolation retains the previous sample across UDP
  packets. Processing one PCM sequence as a whole or in arbitrary packets now
  produces identical 48 kHz samples and duration. State resets at stream/process
  changes; stale audio during a managed restart is discarded.
- Process termination/restart uses timers instead of blocking the bridge's event
  loop for up to two seconds. A 250 ms graceful-stop window precedes a forced
  stop if needed, followed by a 100 ms settings-settle window. The latest pending
  configuration is used once; disabling the bridge cancels pending launches.
  PCM is deliberately not delivered while the managed decoder is unavailable.
  Restarting still breaks decoder continuity; this is not gapless key switching.

The Wireless/DMR self-test checks unchanged discriminator amplitudes with
alternating large peaks, both logging states, packet-independent interpolation,
stereo downmix, exact sample counts, responsive event-loop operation during a
non-cooperating child-process stop, latest-configuration restart, and disabled
shutdown. These checks establish transport correctness, not a speech quality or
over-the-air BER improvement. A new live comparison is still required.

## Subsequent DSD Crash/Transport Review

The later aborted start logged a DSD child crash and a cross-thread Qt timer
destruction warning, but no native exit code, dump, or orderly application close.
The main audio path never reported reaching SDR processing after deferred audio
startup. System memory was about 94% occupied. A subsequent normal close spent
about seven seconds closing the audio output device, while audio workers joined
in milliseconds. These observations do not establish the cause of the DSD
child's internal failure; neither an input filter nor a key offset is justified
as its remedy.

During the later working interval, no IQ sequence gaps or bridge PCM queue drops
were logged, but DSD still reported FEC and signaling errors. Managed decoding
now uses bounded raw PCM stdin, eliminating initial UDP warmup loss (three 20 ms
frames) once the child is ready. A queue discontinuity/stalled writer triggers
a decoder reset rather than continuing with silently skipped samples. Exit
status/native code and single, escalating crash backoff improve recovery and
diagnosis. They do not establish repaired speech quality or solve the separate
audio-driver startup/shutdown stall. Synthetic transport/crash tests verify
ordering, startup frames, wrong-rate rejection, restart, and owner-thread cleanup.
A regression test also exposed an early coarse-timer expiry that abandoned a
restart until new PCM arrived; remaining backoff is now rescheduled explicitly.

## Controlled Recording: Legacy DSD Mode

A subsequent known-ARC4 channel-IQ recording reproduced signaling errors offline,
without the live bridge. At the same 24 kHz discriminator PCM and with C4FM/input
filtering unchanged, removing the forced `-nm` argument reduced FEC-bearing log
lines from 19 to 8, wrong-color-code lines from 6 to 0, and explicit CRC-error
lines from 1 to 0. These are diagnostic line counts, not BER or a measured voice
quality percentage. The recording starts before a call header and contains idle
RF after the call; its AGC-normalized channel IQ is not a bit-identical capture
of the live discriminator PCM.

Managed DSD now retains the default slot-aware decoder and its stereo output.
The bridge still downmixes to mono. RF filters, 24/48 kHz choices, privacy-key
forwarding and other backends are unchanged. Shared argument construction has
a regression test against forced legacy mono. The private offline comparison
tool supports `--legacy-mono` for reproducing the previous behavior. A live voice
comparison is required; residual FEC/MI errors remain on this fixture.

## Gopher Adapter Continuity And Privacy Review

### Follow-up: Late-Entry Timeline And Hytera Basic

The next RF test showed near-zero AMBE FEC errors, verified late-entry MI and
decoded PCM reaching the application, but still no intelligible ARC4 speech.
Review found the adapter committed A-F late-entry MI before decoding the same
superframe. That MI belongs to the following superframe. It is now committed
after voice processing; PI-started and late-entry regression tests compare the
actual vocoder input against independently encrypted known plaintext.

The live run also ended under severe memory pressure (99%) with IQ sequence gaps
and dropped blocks. This is separate from the cipher timeline fault and can
still cause interruptions. Real on-air voice validation remains required.

Hytera Basic beta is exposed as a separate mode/key table, first through DSD-neo
and now through the separate native pre-vocoder module described below. The
Gopher adapter does not implement it. Key lengths are 10/32/64 HEX characters,
preserved as 40/128/256 bits. DSD receives grouped `-H` plus Basic enforcement
`-4`; neither Full nor AES is inferred from Basic key size. RF testing on HP785
is pending. Working ARC4/AES DSD command arguments remain unchanged.

The later live Gopher test exposed two transport faults: B-F embedded fields
could falsely establish polarity, and a privacy-selected stream then resisted
correction at the real A sync. Data-header polarity also incorrectly inherited
voice polarity. The bridge now derives voice polarity only from A, allows a
valid A to update it, and independently normalizes data/PI bursts.

The virtual adapter now assembles exactly ordered A-F packets using their burst
index and RF cadence. Data headers and missing/reordered bursts cannot silently
splice into voice or advance an artificial continuous buffer. Corrected AMBE
hints and raw superframes no longer cause duplicate cipher-frame accounting.

The adapter's prior generic Golay parity matrix/bit order did not match DMR.
The local DMR decoder now generates the cyclic code using polynomial 0xC75 and
even parity; fixed wire vectors, code distance and three-bit correction are
tested. Late-entry extraction uses the specified C3 positions and verified A-F
order instead of searching nibble/offset permutations. MI advances by 32 LFSR
rounds and skipped AMBE frames consume their cipher position.

On a private channel-IQ fixture, the production bridge and revised adapter
produced 18 complete superframes. Seventeen passed late-entry Golay/CRC checks;
the remaining damaged late-entry field did not replace verified state. All 288
error-free raw AMBE payloads compared equal to the native 49-bit hints. A dummy
test key verified state transitions only, not the actual recording's decrypted
speech. No keys, recordings or decoded voice are committed as test fixtures.
An on-air ARC4 voice comparison remains required. AES decryption is still not
implemented by this adapter. Internal OpenDMR/mbelib currently expose only a
voice-vocoder ABI, so the UI explicitly warns that ARC4/AES keys are not applied.
The user-verified DSD-neo path is unchanged by this review. The user has now
confirmed Gopher ARC4 speech is approximately 80-90% clear after fixing the
late-entry MI commit order; artifacts remain at call starts/ends.

## Native Hytera Basic Beta

Known-key Basic decryption now runs in a separate module between AMBE FEC
and the native mbelib/OpenDMR vocoder. Explicit 40/128/256-bit masks are built
only when the configuration changes. RF frame ordinals and independent slot
contexts prevent dropped frames from shifting the key. Only decrypted 49-bit
payloads reach native voice candidates; raw encrypted fallback is disabled
for this mode. Diagnostics are verbose-only and contain no keys/plaintext.
Full is not implemented; HP785 RF validation is pending. The subsequent native
Motorola ARC4/AES-256 beta is documented in dmr_motorola_privacy.md.

## Metadata Encryption And Tier III

No public official confirmation of a new MOTOTRBO DMR over-the-air metadata
encryption rollout was found during this review. This is a limit of the evidence,
not proof that a vendor feature cannot exist.

Tier III denotes trunking, not a stronger encryption tier. The reviewed Motorola
Capacity Max description mentions protection inside its IP network; this does
not demonstrate encrypted RF link-control headers. The official IWCE 2026 P25
panel discusses Link Layer Encryption protecting control-channel data/User IDs,
and described it as in development at that time. Confusing P25 LLE with DMR
Tier III is therefore a plausible explanation of the rumor, not a confirmed one.

Obrii's internal DMR path parses CACH/embedded link control and recognizes CSBK
types; it is not yet a complete Tier III grant/follow engine. Known-key voice
ARC4/AES support in an external backend is not equivalent to encrypted signaling
support. RF/spectrum/synchronization analysis can remain useful when IDs become
unavailable; ID-driven filtering and trunk following would need the actual
protocol, backend support and authorized keys.

References: [DMR Association standards and tiers](https://dmrassociation.org/dmr-standards.html),
[Motorola Capacity Max](https://www.motorolasolutions.com/en_us/products/mototrbo-systems/mototrbo-private-systems/mototrbo-capacity-max-bridge.html),
[IWCE 2026 P25 security panel](https://agenda.iwceexpo.com/session/security-triad-for-p25-lmr-encryptionaccesscyber-ids/916402).
