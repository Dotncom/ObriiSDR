# Obrii SDR IQ Path Contract

This note defines the current IQ-path contract and the rules that should be
preserved before any cleanup or optimization. It is intentionally focused on the
hot path. UI structure, settings dialogs, maps, presets, and help text should be
cleaned up separately.

## Design Goal

Obrii SDR is optimized first for the Fobos receiver, where RF input can produce
very large complex-IQ streams. At 50 MS/s with interleaved 32-bit float IQ, the
internal stream is about 400 MB/s before any extra copies. At 80 MS/s, it is
about 640 MB/s. Any additional full-stream copy, conversion, Qt signal hop, or
generic plugin layer in this path can become expensive on Raspberry Pi, Android,
and Steam Deck class hardware.

The hot IQ path should therefore stay short, explicit, and measurable:

```text
receiver callback/read
  -> DataProcessor
  -> IqBuffer publish
  -> spectrum snapshot / audio blocks / decoder snapshots
```

The app can become more modular around this path, but the sample path itself
should not be made abstract merely for architectural neatness.

## Current Hot Path

1. `DataProcessor` owns live receiver reading.
   - Fobos standard and Agile streams use the native Fobos APIs.
   - RTL-TCP, native RTL-SDR, and SoapySDR convert into the same float-IQ path.
   - Fobos async callbacks enter `DataProcessor::handleData(float *buf,
     uint32_t buf_length)`.
2. `buf_length` is the number of complex IQ samples, not the number of floats.
   - The number of interleaved float values is always `buf_length * 2`.
   - The layout is `I0, Q0, I1, Q1, ...`.
3. `IqBuffer::publish(...)` receives interleaved float IQ.
   - `updateSnapshot=true` appends the data into a circular snapshot buffer.
   - `queueBlock=true` copies the same data into queued blocks for live audio.
   - `expectedEpoch` rejects data from a stale retune/start epoch.
4. Spectrum/FFT reads the latest tail via `IqBuffer::snapshotRecent(...)`.
5. Audio reads ordered queued blocks via `IqBuffer::popBlock(...)`.
6. GNSS/video and some lab paths currently read snapshots rather than a strict
   continuous decoder queue.

This means the current system has two different consumer models:

1. Snapshot consumers:
   - spectrum/waterfall;
   - GNSS quick/deep/replay helper paths when reading live snapshots;
   - video snapshot mode.
2. Ordered block consumers:
   - live audio demodulation;
   - live DMR baseband handoff through the audio demodulator path.

Both models are useful, but they must not be treated as equivalent. Snapshots
are excellent for display and coarse analysis. Ordered blocks are required for
audio, DMR bursts, and any future decoder that needs continuity.

## Invariants

These rules should be treated as part of the program contract:

1. Fobos native reading is the primary optimized path.
   - Generic backends must adapt into the Obrii SDR IQ model, not force the Fobos
     path through a slower universal abstraction.
2. No UI work in the receiver callback.
   - The callback may publish IQ, update cheap counters, and emit optional
     frame data only when explicitly enabled.
3. No avoidable heap allocation in the hottest Fobos callback path.
   - Any future optimization should prefer preallocated buffers or a buffer pool.
4. `buf_length * 2` must remain the only conversion from complex samples to
   interleaved float count.
5. Epoch protects retunes and stream restarts.
   - Consumers must not mix IQ from different epochs when continuity matters.
6. Sequence numbers identify buffer progress, not absolute RF time.
   - They are good for stale-data detection, but not sufficient for timing
     recovery by themselves.
7. Center frequency and listening frequency are separate concepts.
   - Center frequency belongs to the receiver/backend.
   - Listening frequency belongs to the demodulator/channel selection layer.
8. Waterfall history is user working memory.
   - Retune or scan code must not clear it merely to hide transient data unless
     the user explicitly asks or the operation changes display semantics.
9. Snapshot data is not guaranteed to be continuous from a decoder point of view.
   - Any decoder requiring burst/frame continuity should eventually use an
     ordered frame/block consumer model.
10. Logging in the hot path must be bounded.
    - Diagnostic traces should be opt-in, count-limited, or rate-limited.

## Metadata That Is Worth Adding

If future diagnostics or refactors need richer IQ metadata, keep it cheap and
block-level only. A practical `IqFrame` sidecar would contain:

1. `const float *samples` or a buffer reference;
2. `floatCount`;
3. `sampleRateHz`;
4. `centerFrequencyHz`;
5. `inputMode`;
6. `epoch`;
7. `sequence`;
8. optional monotonic timestamp/callback counter;
9. optional source backend id.

This metadata should not become a per-sample object and should not introduce
virtual calls inside the sample loop.

## DataProcessor + IqBuffer Merge Evaluation

Merging `DataProcessor` and `IqBuffer` could reduce some overhead, but the
benefit is likely smaller than it first appears unless the memory model changes.

Current costs:

1. `IqBuffer::publish` copies into the snapshot ring when snapshot publishing is
   enabled.
2. `IqBuffer::publish` also copies into a queued `std::vector<float>` block when
   audio block queuing is enabled.
3. The mutex protects snapshot and queue state.
4. Consumers copy data out again when reading snapshots or queued blocks.

A simple source-file merge would not remove those copies. It would mostly remove
a function boundary and a namespace boundary, which are not the expensive part.
The expensive part is memory traffic and allocation.

Possible advantages of a merge:

1. Fewer public functions and less cross-module bookkeeping.
2. Easier access to DataProcessor state such as current backend/sample rate.
3. Potentially simpler epoch handling in one owner object.

Risks of a merge:

1. It couples receiver reading and shared IQ storage too tightly.
2. It makes offline playback, network IQ, and future test injection harder.
3. It can make `DataProcessor` responsible for every consumer policy.
4. It increases the chance that display/audio/decoder policy leaks into the
   receiver callback.
5. It makes it harder to reason about ownership if multiple non-live sources
   publish IQ.

Better near-term optimization:

1. Keep `IqBuffer` as a separate hot-path boundary.
2. Replace queued block allocations with a reusable buffer pool or fixed ring of
   blocks.
3. Keep the snapshot ring, but avoid extra temporary copies where possible.
4. Add cheap block metadata beside snapshot/queue entries.
5. Split snapshot consumers from continuous consumers more explicitly.

Recommended conclusion:

Do not merge `DataProcessor` and `IqBuffer` now. The better path is to keep the
boundary but make it lower-copy. If later profiling proves that the mutex or
function call itself is a bottleneck, then an internal `IqBuffer` owner inside
`DataProcessor` can be considered. For now, the highest-value target is buffer
reuse and clearer consumer contracts, not a structural merge.

## Safe Cleanup Order

1. Sanitary pass:
   - remove stale debug hooks and obsolete comments;
   - bound verbose logs;
   - verify no always-on heavy diagnostics remain.
2. Main-file extraction:
   - move cold UI/settings/help/preset/map glue out of `main.cpp`;
   - keep hot start/stop/tuning/spectrum behavior unchanged.
3. IQ buffer optimization:
   - introduce reusable queued blocks;
   - add block metadata;
   - preserve current behavior behind compatibility wrappers.
4. Decoder contract cleanup:
   - use ordered block/frame consumers for continuity-sensitive paths;
   - leave snapshots for display and coarse inspection.

