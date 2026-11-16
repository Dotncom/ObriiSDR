# Concurrent full-band IQ recording and analog video

The GUI event mode **Spectrum + Full IQ 16-bit** writes interleaved signed
little-endian I/Q to `_full_iq_s16.cs16`. Each complex sample occupies four
bytes. The accompanying JSON and SigMF files identify the sample rate,
receiver center, timing, `ci16_le` encoding, unity input gain and `1/32768`
sample scale. Spectrum replay, wireless IQ import and experiment bundles
recognize this format. Existing IQ8 recordings remain readable.

The old event path shared the compact network IQ8 producer, whose implicit
gain of 16 could saturate otherwise valid receiver samples. Archival PCM16
now uses receiver amplitudes directly: clamp only at the original normalized
ADC limits, round to PCM16, replace non-finite input components with zero.
Gain, AGC and filtering used for video do not affect the full-band recording.

## Independent consumers

The receiver callback publishes full-band PCM16 through its own bounded
32-MiB queue. RF ATV uses a separate worker with a bounded 32-MiB float input
queue, followed by the existing channel-IQ dispatch and an 8-MiB decoder
queue. The callback never performs the wideband video channelizer itself.
Enabling full-band recording no longer disables channel IQ for video.

The video channelizer retains its filter and resampling geometry. Contiguous
FIR history, SSE2 FIR accumulation and an RF ATV vector oscillator reduce
its CPU cost on x86-64. The factor-two front decimator uses the equivalent
`[1, 3, 3, 1]/8` filter in pairs of complex outputs. ARM retains the scalar implementation. Video block
gain is separate and immediately limited when input peaks rise.

Overflow of a video queue resets synchronization before subsequent samples;
it cannot stop full-band recording. Overflow of the recording queue or a
receiver retune closes the current capture at its valid prefix and reports
the discontinuity instead of joining samples across an unmarked gap. Disk
write/open errors stop the capture and finalize the companion metadata.
The IQ prebuffer is capped at 64 MiB in addition to its time limit.

At 50 MS/s the raw recording requires **200 MB/s** of sustained disk writes
(about 12 GB per minute), plus spectrum data. These bounds protect memory
and continuity; they cannot compensate for a disk or decoder that cannot
sustain the selected rate. Changing the listening frequency affects video;
changing receiver center or sample rate ends the current full-band capture.

## Regression coverage

`ObriiFullIqRecordingSelfTest` checks PCM16 amplitudes and half-LSB error,
NaN/Inf handling, exact raw readback, sample counts and SigMF encoding,
legacy IQ8, simultaneous raw recording and ATV output, queue independence,
reported input overflow and recovery, and waveform/chunk continuity for
18-, 32- and 64-tap filters against a scalar oscillator reference.
The optional `--paced` run records about 403 MB while feeding video at
50 MS/s through the production FM demodulator and rasterizer in a separate
thread. It checks every block and rendered frames with free-running raster
timing; it does not claim PAL/NTSC synchronization on an unknown RF signal.

The existing channelizer, shared recording core and experiment bundle tests
cover the shared DSP/container paths. A synthetic concurrent capture verifies
transport and format; successful decoding of an unknown RF recording still
depends on signal format, bandwidth, tuning and signal quality.
