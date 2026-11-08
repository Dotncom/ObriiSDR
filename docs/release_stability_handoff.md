# Release stability handoff

This note captures the current pre-release stability decisions so the project can
be resumed later without rediscovering the same failure modes.

## Stable paths

- Fobos SDR Standard firmware on Raspberry Pi was tested with repeated
  application open/close cycles and did not show the startup frequency shift.
- Fobos SDR Agile can start with a shifted IQ/frequency view on some units. The
  current workaround is a pre-reader startup frequency kick:
  `center + 1 Hz`, then `center` again.
- Native RTL-SDR receive works and is included as a secondary/test backend.
- Standard scan works with Fobos and RTL-SDR. Agile scan remains a
  firmware-level Fobos Agile feature.
- NMEA/UBX GNSS modules such as NEO-M8N work through the serial-port path and
  update QTH/map data.

## Experimental paths

- SDR-based GNSS acquisition is still a workbench. It has synthetic/replay
  tools and IQ monitoring, but no repeatable real-world lock yet.
- DMR voice is usable only as an experimental path. `Obrii SDR + mbelib` is the
  most practical current baseline; DSD-neo and GopherTrunk bridges need more
  work on framing, timing, and privacy handling.
- ARC4/AES DMR privacy UI and key management exist, but decrypted voice is not
  confirmed.
- Native bladeRF RX and SoapySDR are integration/beta paths. They should not be
  treated as release-blocking without real hardware tests.

## Do not regress

- Do not clear the waterfall on ordinary retune; it is useful operator memory.
- Do not call Fobos Agile `cancel_async` for ordinary live retune.
- Keep `Start` as the reader-start path. Apply idle hardware settings before
  reading whenever possible.
- Keep hot IQ processing short. Avoid extra full-buffer copies in spectrum,
  waterfall, audio, and decoder handoff paths.
- Keep verbose diagnostic logging opt-in. Normal logs should stay small and
  should preserve crash/startup essentials only.

## Next release checklist

1. Build and smoke-test Windows x64.
2. Build or at least package-check Raspberry Pi source/scripts.
3. Check Android project synchronization before publishing Android artifacts.
4. Remove real IQ/audio recordings, local logs, tokens, and temporary downloads
   from release assets.
5. Keep synthetic test assets and docs if they are small and useful.
6. Mention the Fobos Agile startup-kick workaround in release notes as a known
   compatibility workaround.
