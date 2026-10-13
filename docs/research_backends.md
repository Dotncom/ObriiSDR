# Optional portable research engines

## Windows

The development/runtime folder is `build/runtime`. Research engines run as
separate, below-normal-priority processes, not inside the real-time IQ/audio
callback. Ordinary listening does not launch them or load HDF5.

| Folder | Engine | Integration |
| --- | --- | --- |
| pocketsdr | PocketSDR v0.20, pinned 03787da53c65a8eb291eec571b6412ec470aeb17 | live / recorded IQ -> acquisition, tracking, navigation, PVT, NMEA |
| satdump | SatDump 1.2.2, offline Meteor plugin subset | recorded IQ -> OQPSK/QPSK, FEC, LRPT packets and images |
| hdf5 | HDF5 1.14.6 | streamed spectrum-frame conversion into chunked HDF5 |
| itu_hf | official P.533/P.372 model, pin 82017594a1c6cacfaa7e86954c4ae7b3a5825a3d | monthly MUF/SNR/reliability with all twelve public datasets |

`tools/prepare_science_backends_windows.ps1` downloads pinned, checksum-verified
upstream binary assets, copies their required runtime libraries and original
notices, and creates exact SHA-256 manifests. The Windows release packager only
accepts the listed bytes. Unknown configs, captures, logs, tokens, coordinates,
screenshots and caches cannot become release files through these manifests.
See THIRD_PARTY_LICENSES.txt for licenses and corresponding-source obligations.

## GNSS from SDR

GNSS / QTH -> SDR PVT (also in the DSP GNSS block) opens the PocketSDR tool.
Choose stationary live IQ or a recording and explicitly press Start. For raw
files specify CF32/CS16/CS8, little-endian I+jQ. Stereo PCM16 IQ WAV is supported;
Obrii's sample-rate/center sidecar is read where available. Mono audio is not IQ.

GPS/QZSS/SBAS L1, Galileo E1-B, BeiDou B1I and GLONASS L1OF are selectable.
Signals must fit the captured band; uncovered systems are rejected. The live
adapter accepts 2.046..32 MS/s and does not silently change receiver settings or
resample. Retune, RX stop, snapshot overrun or an IQ epoch change stops tracking.
The engine receives bounded CS16 copies through localhost TCP, leaving the
normal audio queue intact. It never opens the receiver itself.
For live GPS L1 testing, tune the receiver center to 1575.42 MHz and select a
low sample rate that still covers the signal (for example 8 MS/s). PocketSDR
must process IQ as fast as the receiver supplies it: if it falls behind the
bounded snapshot, tracking stops with the measured lag rather than silently
continuing across a gap. Weak live IQ is normalized before CS16 conversion;
recorded IQ values remain unchanged.

PVT needs enough satellites, valid ephemerides, a stable GNSS-capable antenna
and a continuous recording, commonly tens of seconds or longer. A short
acquisition snapshot is not sufficient for a complete position solution.
The checkbox for map/table use is independent of processing; serial GNSS must
be disconnected so fixes from two unrelated receivers are not mixed. The
module's UBX-only policy does not suppress explicitly selected SDR NMEA.
Existing display satellite filters do not re-solve an external PVT solution.

NMEA and the engine's navigation cache can contain exact coordinates. Working
files reside in the private user profile, never the executable's portable
directory. This integration does not upload them. Online map tiles and other
existing opt-in network features keep their own privacy rules.

## Meteor LRPT

Video -> LRPT decode (also DSP digital-video settings) processes a saved
baseband file. Select Meteor M2-x OQPSK 72k/80k or original M2 QPSK 72k, the
actual recording sample rate and IQ format. The recording should already be
centered on the downlink. DC removal and I/Q swap are explicit options.
Each run creates a new timestamped result directory, preserving prior runs.
Decoded images appear in a selector; Open output exposes all generated products.

SatDump 1.2.2 uses `[pipeline] baseband [input] [output]`, not the newer version's
`pipeline` subcommand. Exit code zero alone is not an image-decode success: the
UI reports when no image was produced. TLE auto-fetch is disabled for this
offline adapter. Resources/config/cache are prepared in the private profile.
The existing live LRPT view remains an exploratory QPSK monitor; this adapter
does not claim streaming live image decoding. Full RF image verification is
still required with a known Meteor capture.

## Spectrum HDF5 and measurement qualifications

Research -> Session -> HDF5 export converts `.fbspec` recordings in a background
worker, one frame at a time. `metadata_json_utf8` contains original metadata;
`segment_* / spectrum_db` contains frequency-ordered bins and `frame_metadata`
contains UTC/elapsed ms, center/min/max Hz and FFT length. Bin-count changes
create new segments. Cancellation leaves no completed destination and existing
files are never overwritten. Frequency order matches the replay viewer.

Calibration points can bind to receiver type, input, Fs, LNA/VGA/RTL gain and
AGC/RF-amp state. Only the most specific matching profile is interpolated;
wildcard and specific profiles are not mixed. For absolute levels disable AGC:
an AGC flag does not tell us the actual gain. Different serial devices of the
same type still need their own manually managed calibration profiles.

Up to 64 markers are retained in settings and experiment metadata. A/B retain
their legacy delta/range role. The optional single-tone analyzer estimates
SNR, SINAD=S/(N+D), THD and THD+N from a contiguous spectral span, excluding DC.
The integration width must cover the selected FFT window's main lobe; broaden
the span to include the requested harmonics. Missing harmonics explicitly mark
the result partial. These are spectral estimates, not certified metrology.

## Raspberry / Linux

Windows EXE/DLL cannot be used on ARM. `tools/install_deps_debian.sh` now adds
HDF5 development headers; rebuild Obrii to enable optional HDF5 export.
Run `./tools/prepare_pocketsdr_linux.sh` once to build the pinned native ARM
PocketSDR into `build/linux-release/pocketsdr`, without SoapySDR or AVX2.
The script limits build parallelism to two jobs by default.

For SatDump use an ARM build of 1.2.2 or its official ARM64 package:
https://github.com/SatDump/SatDump/releases/tag/1.2.2
The adapter also finds `satdump` on PATH. Installing a compatible native Linux
runtime is an architecture-specific preparation step, not an extra Windows
end-user requirement. Do not copy the Windows runtime into the Raspberry folder.

## Validation

General settings -> Online data sources edits NOAA F10.7/Kp endpoints,
NASA/JPL comet-orbit updates, eMetN Meteor Journal RSS, JPL RSS comet headlines and
JPL fireballs. The HF/Sun panel includes the event feed. Refresh is manual by
default; visible-only periodic refresh is independently opted in (>=15 min).
Sources can be disabled. HTTP(S) replacements must preserve the RSS/Atom or
JSON schema. No station coordinates are sent; services see the public IP.
Comet solution dates are PT, not discovery dates; fireballs are past records.
RSS DTD is rejected, links are HTTP(S) only and display text is HTML-escaped.
No feed images/scripts are loaded; requests are sequential, capped at 2 MiB.

HF prediction: Propagation -> HF/Sun -> HF prediction. Imports only TX/RX,
power and constant gains; set HF frequencies/month/R12 SSN/noise environment
separately. No terrain, antenna height, feeder-loss or storm correction is
silently applied. Bundled data add about 131 MiB. build_linux.sh builds the
native helper and uses the public data in the Raspberry source package.
Native Linux execution is unverified here. Details: hf_prediction.md.

Deterministic tests cover BPSK31, calibration selection, tonal metrics, marker
limits, spectrum parsing/HDF5, GNSS argument validation, PCM16 conversion,
loopback NMEA/IQ and preservation of the normal audio queue. These tests do not
replace RF GNSS/PVT or Meteor image tests. Backend installation and a successful
version probe alone are not evidence of a valid fix or decoded image.
