# Native Motorola DMR Privacy (Beta)

Select `Obrii SDR + mbelib` or `Obrii SDR + OpenDMR`, select `ARC4` or
`AES-256`, and choose an existing KID in its key table. Decryption follows
the selected mode/key/backend automatically; there is no separate checkbox.
ARC4 requires exactly 10 HEX characters (40 bits), AES-256 exactly 64
(256 bits), including leading zeroes. Only supplied known keys are used.
There is no key search, guessing or brute force.

The separate privacy processor runs after AMBE FEC and before all native
vocoder candidates. External DSD-neo/Gopher bursts remain unchanged.
DSD-neo ARC4/AES-256 and the working Gopher ARC4 path are not replaced.

## Synchronization

- CRC-validated PI headers provide the algorithm, KID and 32-bit MI.
  BPTC corrects at most two channel bits; PI polarity is checked independently.
- A nonzero selected KID must match a received PI KID. KID 0 permits any
  received ID, as in the existing Gopher path. Late-entry-only recovery
  cannot verify KID because its fragments contain MI, not a key ID.
- A-F raw C3 fragments recover the next MI with extended Golay and CRC4.
  That MI is committed after F, never to the current 18 voice frames.
  Without PI, the initial superframe is muted and the next can be decoded.
- Each slot has an independent RF anchor. Missing/bad AMBE frames still
  consume their absolute ordinal; they cannot shift later keystream bytes.
  A missing late-entry fragment leaves the LFSR prediction available.
  A stale/out-of-order frame or an unverified long cadence gap is muted.
- A is queued until its complete trailing payload has arrived, just like B-F.
  A PCM block ending after sync must not drop the cipher's superframe anchor.
- Trusted Voice LC service options identify clear traffic, which bypasses
  the cipher even when a key is selected. EMB's PI bit alone is not used
  to classify Motorola enhanced privacy as clear traffic.

ARC4 uses key||MI in network byte order, RC4-drop256 and seven keystream
bytes per 49-bit AMBE frame. AES-256 uses OFB with the 32-bit MI expanded
to a 128-bit IV, discards one 16-byte block, then likewise advances seven
bytes per frame (49 used bits plus seven padding bits). Canonical unciphered
silence is preserved without changing the following frame's position.

No raw encrypted 72-bit audio fallback is allowed while native privacy is
active. Invalid keys, missing MI and PI algorithm/KID mismatches mute audio.
The existing external ARC4 offset/layout experiments do not alter this native
standard DMRA path. Select `No privacy` to bypass it without deleting keys.
Legacy saved forwarding-checkbox values are ignored during settings import.
The UI and DSP board identify the selected backend's decryption capabilities:
native ARC4/AES-256, DSD-neo ARC4/AES-256, Gopher ARC4 only (no DMR AES).

## Verification And Limits

`ObriiDmrPrivacySelfTest` checks RFC 6229 RC4 and NIST SP800-38A AES-256-OFB
vectors, plus all voice frames against independent Go standard-library ciphers
and upstream Gopher BPTC/late-entry test helpers. Public synthetic fixtures are
in tools/fixtures/dmr_motorola_vectors.json. Regenerate with the optional
TestExportNativePrivacyVectors test and OBRII_NATIVE_VECTORS output path.

The optional `--pcm-timing <private.pcm> 24000|48000` test checks recorded RF
cadence and MI recovery using a dummy key. It does not prove intelligible
speech from that recording. The user confirmed clean native Motorola ARC4
speech and successful native AES-256 reception on 2026-10-12 after fixing lost
A bursts at PCM boundaries. This verifies the tested radio/configuration, not
every vendor, firmware or RF condition. Hytera remains an unverified beta.
Synthetic 24/48 kHz PCM tests end blocks at sync and inside the trailing half
of A, checking that exactly three frames arrive with their RF positions intact.

Verbose diagnostics include mode, key width, PI/late-entry counts and reasons
for muted frames. They never print keys or decrypted AMBE. No new UI control,
network upload, FFT/IQ processing or persistent recording is introduced.

Protocol references: the existing Gopher adapter's verified DMRA timing,
DSD's PI/late-entry/OFB conventions, and the [DSD-neo privacy documentation](https://github.com/arancormonk/dsd-neo/blob/main/docs/decryption-profiles.md).
AES core: [tiny-AES-c](https://github.com/kokke/tiny-AES-c).
