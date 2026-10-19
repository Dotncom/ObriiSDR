# Obrii SDR 6.4.0

Protected DMR transmission, FPV auto-scan and transmit-path reliability.
Windows and Raspberry/Linux only. No Wireless add-on or Android upload.

## Українською

- DMR Tier II direct/simplex тепер передає відкритий голос, Motorola ARC4
  40-bit та AES-256. Реалізовано PI, MI, late-entry і вирівнювання ключового
  потоку по 18 AMBE-кадрах суперкадру. Потрібні дозволений користувачеві ключ,
  той самий KID та опційний OpenDMR voice backend.
- Стабілізовано HackRF TX: буфер, перевірка живого USB-потоку, underrun,
  зупинка та перемикання джерел. Оновлено Windows-захват мікрофона через
  WASAPI shared із запасними WinMM/Qt шляхами.
- Додано автоматичний FPV-пошук: сканування пресета, фіксація широкого
  кандидата, перевірка ATV HSync/VSync та повернення до сканування після
  втрати сигналу. Підтримано Standard і Agile Fobos.
- Поліпшено SSTV через NFM, короткі втрати синхронізації та ATV-каналізатор.
  Signal Lab отримала плаваючий маячок і точніші самотести модуляцій.
- Додано згортувану HAM-панель діапазонів/демодуляцій та нові режими
  оформлення 3D-водоспаду: градієнт за висотою, власний однотонний колір і
  глибина його градієнту.

## Verification And Limits

Windows Release build passed. DMR privacy, complete DMR DSP pipeline, protected
DMR TX waveform, OpenDMR encode/decode probe, SSTV, Signal Lab and FPV auto-scan
self-tests passed. The Linux/Raspberry source is updated but was not compiled on
ARM in this release session.

DMR TX remains direct/simplex only: repeater wake-up, trunk grants and Tier III
operation are not implemented. The OpenDMR module is GPL-2.0-compatible and is
packaged separately under `dmr_voice_backends` with its license notices. Keys
remain local, are stored in user settings for reuse and are never printed in
diagnostics. Use RF transmission only when authorized and with a suitable load
or antenna.

## Downloads

- `ObriiSDR-v6.4.0-windows-x64.zip`: portable Windows core with the updated
  optional DMR voice modules. Wi-Fi/Wireshark runtimes remain a separate add-on.
- `ObriiSDR-v6.4.0-raspberry-source.tar.gz`: Linux/Raspberry source. Extract it
  and run `./tools/build_linux.sh`; optional DMR backends can be built with the
  documented helper steps.
- `ObriiSDR-v6.4.0-SHA256SUMS.txt`: checksums for both packages.

The existing optional 6.1.1 Wireless add-on remains compatible and is not
uploaded again. Personal settings, API keys, logs, recordings, coordinates,
tokens and local import folders are excluded.

Feedback: [mojamajsternya@gmail.com](mailto:mojamajsternya@gmail.com).
Include the version and reproduction steps, and remove private data first.
