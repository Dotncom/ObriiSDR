# IQ Pipeline Audit Notes

This note tracks the current working theory for DMR/GNSS synchronization failures and the likely optimization points in the receiver path.

## Current Observation

The new `[IQ stream]` diagnostics from the 2026-06-13 GNSS test do not show a catastrophic USB/IQ continuity failure:

1. `blockSamples` stayed fixed at 65536.
2. `blockMin` and `blockMax` stayed fixed at 65536.
3. `measuredRate` stayed very close to 50 MS/s after startup.
4. `lateCallbacks` stayed at 0.
5. `clipPercent` stayed at 0.
6. `nonFinitePercent` stayed at 0.
7. The live queue depth moved but did not grow without bound.

This does not prove that every downstream consumer sees a perfect continuous stream, but it makes a basic Fobos/libusb block-drop problem less likely for this test.

The 2026-06-13 synthetic GPS self-test also found the injected PRN immediately in terms of correctness (`prnMatch true`, metric about 25.8 dB), but the full search still took about 113 seconds. That points to performance/architecture work in acquisition rather than a pure math failure.

## Remaining Suspect Areas

1. Channelizer and resampler correctness:
   - ordinary analog/audio modes and network Channel IQ now share the streaming frequency-translating channelizer;
   - coarse decimation is followed by a 64-phase, 32-tap Blackman-windowed FIR resampler, and the advertised output rate is the exact target rate rather than input rate divided by an integer;
   - DMR and dual-input HF noise cancellation intentionally retain their specialized timing paths;
   - verify frequency-shift sign with real offset test tones, not only synthetic GNSS;
   - verify I/Q order and conjugation assumptions with a known positive/negative offset source;
   - compare Obrii SDR channelized output against a trusted external tool.
2. Receiver spurs and interference:
   - the waterfall showed stable narrow lines near the GNSS band;
   - true GPS L1 C/A is spread-spectrum and often below the visible noise floor;
   - fixed narrow peaks are more likely local spurs, USB/power/display noise, or external interference than GPS satellites.
3. GNSS acquisition model:
   - current GPS C/A search uses 4.092 MS/s, PRN/code-phase/Doppler search, and 1 ms correlation power accumulation;
   - the variable name `coherentMs` is misleading because the implementation accumulates correlation power per millisecond;
   - the 160 ms deep scan is computationally expensive and took about 132 seconds on the Steam Deck in the latest test.
4. GNSS signal level:
   - the reported IQ RMS around -55.7 dBFS may be valid, but a bicone antenna without an active GNSS LNA/filter is a hard RF condition;
   - phone GNSS lock is not a fair comparison because phones use assisted GNSS, dedicated RF front ends, filtering, low-noise amplification, and multi-constellation receivers.
5. DMR-specific path:
   - if raw IQ is stable, DMR issues are more likely in channel filtering, 4FSK discriminator shaping, symbol timing, burst framing, or AMBE bit ordering;
   - the DMR path still needs a stage-by-stage offline harness before further voice tuning.

## Optimization Ideas

See also `docs/architecture_iq_path.md` for the current IQ-path contract,
hot-path invariants, and the evaluation of whether `DataProcessor` and
`IqBuffer` should be merged.

1. Reduce GNSS Doppler search cost:
   - start with narrower Doppler windows when assisted time/location/known PRNs are available;
   - use a coarse search first, then refine only the top candidates.
2. Cache repeated acquisition data:
   - precompute and cache GPS C/A FFTs per PRN;
   - precompute or recursively update Doppler wipe-off oscillators instead of calling `sin/cos` for every sample.
3. Limit expensive deep scans:
   - default live acquisition should stay short and responsive;
   - deep scans should run as offline/replay jobs with progress and cancellation.
4. Add acquisition dump files:
   - save PRN/Doppler/code-phase grids for a recording; implemented as `recordings/gnss_reports/gnss_acq_*` JSON/CSV artifacts;
   - compare these dumps with GNSS-SDR or another reference implementation.
5. Add spur diagnostics:
   - average the spectrum in GNSS mode;
   - log stable narrow peaks and optionally notch them before acquisition;
   - show expected GPS L1 C/A spread-spectrum bandwidth on the plot.
6. Clarify internal IQ contracts:
   - pass sequence/timing metadata with IQ frames where practical;
   - separate waterfall snapshots from decoder/acquisition streams;
   - keep decoder input paths explicit so DMR/GNSS/video do not silently consume stale or discontinuous snapshots.

## Next Practical Tests

1. Feed a known complex test tone through the live channelizer and verify positive/negative offset sign.
2. Record GNSS IQ with the current bicone setup, then run Obrii SDR acquisition offline with the `GPS replay` button and compare with GNSS-SDR.
3. Test with a GNSS active antenna plus bias tee or an external GNSS LNA/filter if available.
4. Repeat DMR with the same IQ stream diagnostics enabled and check whether the raw stream remains stable during voice tests.

## Full-IQ 50 MS/s Throughput Check

The 2026-10-31 23:52:23 Full-IQ capture reported 41.01 MS/s in the async
callback, while the same receiver path later reported roughly 50 MS/s with no
recording (2026-11-01 00:12:59, 262144 samples per callback, no late callbacks).
This makes a permanent receiver speed limit unlikely, but does not distinguish
CPU scheduling, callback work, storage pressure, and a USB hub under load.

For a controlled test, enable verbose logging briefly and use the same
receiver, 50 MS/s, frequency, FFT and visualization settings each time:

1. Run 5 seconds with no Full-IQ recording.
2. Run 5 seconds with Full-IQ recording to the normal disk.
3. Repeat both with a direct USB 3 port instead of the hub, if available.

Compare `[DataProcessor] async sample-rate check` (about once every two seconds
with verbose logging, including `measured`, `workAvgMs`,
`workMaxMs`, `bufferAvgMs`, `iqEncodeAvgMs`, `bufferRejected`) and `[IQ GUI queue]`
(`avgWaitMs`, `maxWaitMs`, `maxHandlerMs`). A 262144-sample callback at 50 MS/s
has about 5.24 ms before the next block. The new recording sidecar reports
`writeCallAvgMs`, `writeCallMaxMs`, `flushMs`, and `writeErrors`. Large callback
work points to synchronous RX processing; growing GUI queue delay points to
GUI/storage backlog. Late callbacks with low callback work require a separate
USB/host scheduling check. Compare hub versus direct-port results before
blaming either the hub or receiver.

`effectiveSampleRate` in the recording JSON is written bytes divided by the
host timestamp span. It is useful as a capture-throughput estimate, but is
not an independent measurement of the receiver ADC clock or proof of a USB
transfer failure.

### 2026-11-01 Capture Result

The same 50 MS/s receiver ran at approximately 50 MS/s before and after the
Full-IQ capture, but at 39-40 MS/s while recording. During that interval,
`iqEncodeAvgMs` rose to 5.3-5.6 ms per 262144-complex-sample callback, which
has only 5.24 ms at the configured rate. The recorder reported 0.42 ms average
write time, 2.32 ms maximum, no write errors, and the GUI queue averaged about
1.1-1.6 ms rather than growing. The callback's synchronous float-to-IQ8
quantizer, not sustained disk bandwidth or the GUI queue, explains the rate
drop in this capture. It does not prove that every hub and port combination is
equivalent under load.

An x64 SSE2 quantizer now preserves the scalar byte values and was checked
against them in `ObriiIq8QuantizerSelfTest`, including non-finite inputs and
block tails. The standalone synthetic test ran in about 0.25 ms per 262144
complex-sample block on this PC. A repeat live recording is needed to confirm
that the entire RX path now sustains 50 MS/s. ARM currently retains the scalar
fallback until a native Raspberry build and benchmark are available.
