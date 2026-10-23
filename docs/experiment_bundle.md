# Portable Experiment Bundle

The measurement-session tool can export and open a portable experiment folder.
This is an offline operation, not a cloud upload, and does not start reception,
transmission, recording, or an external compute connection.

Open Research tools, then the measurement-session tab. Use **Export experiment**
or **Open experiment** next to the existing report export controls.

## Contents And Workflow

Choose a completed native spectrum recording (`.fbspec`) and a new destination
folder. The bundle contains the spectrum frames, supported linked IQ and timing
sidecars when present, a versioned JSON manifest, and SHA-256 file checksums.
If a receiver-noise baseline or Y-factor profile matches the calibration
identity recorded at acquisition start, the bundle also contains
`receiver_noise_baseline.csv` and/or `receiver_y_factor.csv`. The manifest binds
each table to its receiver, input, center frequency, sample rate, gains, RF
bandwidth, FFT/window and amplitude-calibration state. The large tables are
created only for an explicit bundle export; ordinary recordings retain a
compact calibration summary instead of duplicating them.
For a legacy recording without a recording-start experiment context, a table
selected at export remains explicitly marked by `contextOrigin: export-time`;
it is not presented as proof of the original acquisition state.
There is no ZIP dependency. Copy the entire folder to the other machine.

New recordings made with extended recording metadata retain an experiment
context captured at recording start: software version, numerical radio and
analysis settings, calibration information, display settings and DSP layout.
For an older recording, current export-time settings are not proof of its
original acquisition state. The recording's own frequency/rate metadata takes
precedence; absent information is not reconstructed or claimed as measured.

Opening verifies the manifest and files before offering restoration. Replay is
paused initially and uses an independent viewer. Restoring the optional DSP
layout requires reception to be stopped; it does not apply foreign hardware
settings or authorize TX/network operations. Main receiver settings and the
normal optimized RX path are not replaced by experiment import.
The previous layout is saved as the selectable **Before experiment import**
DSP profile. Session rows, name and notes can be restored separately, after
confirmation, without moving the live tuning cursor or starting accumulation.
The session context keeps the last 4096 measurement snapshots and reports how
many were omitted; name and notes are limited to 1024 and 65536 characters.

## Privacy And Integrity

Settings and metadata use allowlists, rather than copying an INI file. GNSS/QTH
coordinates, receiver serials, API/session tokens, decryption keys, external
commands and machine-specific paths are excluded. Explicitly exported experiment
notes and received signal content can still contain personal or sensitive
information. Review them before sharing. A checksum detects corruption; it is
not proof of who created a bundle.

Import rejects unsupported versions, missing or altered payloads and unsafe
relative paths. File copying and hashing run outside the UI thread with bounded
buffers and cancellation. They do no work during ordinary live reception.

## Reproduction Boundaries

This format reproduces saved spectrum observations and supported linked IQ
replay, with its captured display context. It does not turn a screen-resolution
spectrum recording into a lossless full-band IQ capture, restore lost samples,
certify calibration, or reproduce undocumented hardware/firmware behavior.
Calibration metadata is retained; already computed spectrum levels must not
have the same calibration offset applied a second time.

Arbitrary third-party files, executable GNU Radio graphs, backend credentials,
and automatic multi-device acquisition are not imported. Existing standalone
CSV, SigMF and HDF5 exports remain available in their respective tools.
