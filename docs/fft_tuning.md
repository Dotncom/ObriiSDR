# Offline FFT Plan Tuning

Open **Settings > FFT tuning** with the receiver stopped. Choose an FFT size,
maximum thread count and per-candidate planning budget, then Tune. The GUI blocks
this operation while its primary/Multi-SDR receiver, replay or pending main FFT
is active. Receiver/replay starts in that instance are blocked during tuning.
Stop receivers in other instances and other heavy jobs yourself: the tool does
not seize devices or terminate other programs. Cancel/closing kills only the
owned device-free helper and leaves any existing completed profile intact.
GUI results are staged in a private pending file; the GUI publishes them only
after a successful helper exit and only if no cancellation was requested.

The helper uses deterministic synthetic CF32 data, out-of-place forward FFT,
FFTW_ESTIMATE and time-bounded FFTW_MEASURE, and 1..N threads. Before timing, it
materializes reusable wisdom with the quick estimator, exports it, clears the
planner cache and restores it with FFTW_WISDOM_ONLY. This handles incomplete
root wisdom after a planning timeout in FFTW 3.3.5. Only successfully restored
plans are timed and eligible for saving (`restoreVerified` in candidate JSON).
It checks 257
distributed output bins against a single-thread baseline (relative tolerance
1e-4 plus an absolute 1e-4 allowance), warms each candidate and compares medians
of three transforms. It stores the fastest numerically accepted candidate.
These sampled checks are not a scientific calibration or proof of every bin.
The planning time is an FFTW soft budget, not a hard process deadline; large
transforms can take longer. GUI cancellation remains available.

The profile is atomically saved under `ObriiSDR/fft-tuning` in the user's local
application-data directory, not beside the EXE. Its filename contains a hash of
machine identity, CPU/architecture, thread topology and FFTW identity plus the
FFT length. On Windows the loaded FFTW binary is hashed; elsewhere its version
and CPU details are included. Raw machine identity is not saved. The profile
contains the selected thread count, timing, trials and checksummed FFTW wisdom.
Each length is tuned separately; FFT size/window/RBW/normalization are unchanged.

When the shared `FFTResult` next creates its native CPU plan, it restores matching
wisdom without a live benchmark. A missing/corrupt/incompatible cache, unsupported
thread count or current compute-budget limit falls back to the original quick
plan. Cache changes made through this GUI invalidate its next plan. Other running
instances pick them up on restart or FFT-size change. **Clear profile** restores
default planning for the selected size and discards resident planner wisdom.
The ordinary receive path launches no
tuning process and performs no per-frame cache-file reads.

The selected thread count must fit the current total background-worker budget;
the main FFT's former per-class recommendation may be exceeded only by an
explicitly tuned profile. Recheck audio under live load after applying a profile:
offline FFT timing does not include audio, rendering, thermal changes or other
DSP branches. No speedup is guaranteed. Non-power-of-two FFT sizes are supported.

## CLI

```sh
ObriiCli fft-tune --fft 8388608 --max-threads 4 --planning-ms 500
ObriiCli fft-tune --help
```

This standalone command never starts RX or RF TX. Stop other receiving instances
first. Candidate/completion/error records are JSON lines on stdout. `--cache-dir`
selects an isolated cache for testing; omit it for the GUI's shared default cache.
The current profile layout is `cf32-forward-outofplace`. The CLI's lean in-place
display meter has a different storage contract and does not use this profile;
shared out-of-place FFTResult/compute workers do. GPU benchmarking is independent.
With diagnostic logging enabled, `benchmark cached cacheStatus applied` confirms
actual use. `cacheStatus restore-failed` means the cache loaded but FFTW rejected
its wisdom; `missing-or-incompatible` means the normal quick plan was selected.
Older profiles remain untouched; rerun tuning if their wisdom cannot be restored.

`ObriiFftTuningSelfTest` exercises a real small native tune, cross-process identity,
wisdom-only restore, invalid/foreign/corrupt cache and cancellation. The UI smoke
test checks the stopped-RX gate without opening a receiver or launching a tune.
The compute-worker regression holds the planner mutex to verify that cancellation
of a request does not report an executing FFT as idle. Publication tests cover
same-size replacement, cancellation after staging and explicit acceptance.
