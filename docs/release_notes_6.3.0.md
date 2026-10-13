# Obrii SDR 6.3.0

DSP routing, independent audio branches and portable research experiments.
Windows and Raspberry/Linux only. No Wireless add-on or Android upload.

## Українською

- Окремі DDC-гілки отримали демодулятор, ланцюг фільтрів і аудіовихід.
  Повільний зовнішній обробник працює через обмежені черги окремо від
  локального аудіо. Спрощений режим вимикає додаткові гілки.
- Додано налаштування FIR, експериментальний PFB-каналізатор і адаптери
  GNU Radio зі зміною частоти дискретизації у визначених межах.
- Більше зв'язків DSP-дошки реально маршрутизують дані. Додано Undo/Redo,
  безпечніше видалення груп та застосування порядку основних аудіофільтрів
  за зв'язками лише окремою командою.
- Частоти та смуга демодулятора на дошці тепер використовують актуальні
  межі, кроки й пресети основних контролів, з оновленням відкритих редакторів.
- Удосконалено передавання основного FFT на обчислювач, обмежене конвеєрне
  завантаження, повторне підключення й явно дозволений локальний резерв.
- Прогноз покриття отримав опційні локальні втрати біля RX для різного
  середовища й щільності забудови, з явними межами моделі та ручними поправками.
- Експеримент можна перенести папкою: спектральний запис, пов'язаний IQ,
  метадані, калібрування й робоче місце. Файли перевіряються за контрольними
  сумами, реплей відкривається на паузі; відновлення дошки зберігає попередню
  конфігурацію та не запускає приймач, передачу або мережеву обробку.

## Changes And Boundaries

Independent DDC audio, FIR controls, optional PFB channelization, bounded
external workers and rational-rate GNU Radio adapters extend the existing
optimized main path. New typed links execute supported routes; the DSP board
is not an unrestricted GNU Radio scheduler. Main-chain wire-order changes
require an explicit action. Audio outputs default off.

Remote FFT now supports bounded upload pipelining with legacy negotiation,
retry/backoff and explicitly enabled local fallback. Update both computers.
Use a trusted LAN: session authentication does not encrypt IQ traffic.
Loopback measurements do not establish full-rate Raspberry-to-laptop speed.

Portable experiment folders verify native spectrum/IQ payloads and preserve
allowlisted context. Imported replay is isolated and paused; optional layout
restoration requires stopped reception and saves the previous layout first.
Private settings, keys, geographic markers and external commands are excluded.
Received IQ and user-written notes can still contain sensitive information:
review an experiment before sharing it. No automatic cloud upload occurs.

Windows build and automated frequency-control, DSP routing/history/teardown,
audio-isolation, bundle-integrity and replay-isolation tests passed. Linux source
is updated; this release was not compiled or RF-qualified on Raspberry here.
DDC/PFB, multi-channel audio and LAN performance still need hardware testing.

## Downloads

- `ObriiSDR-v6.3.0-windows-x64.zip`: portable Windows application, core and
  existing science/receiver runtimes. No bundled Wi-Fi/Wireshark runtimes.
- `ObriiSDR-v6.3.0-raspberry-source.tar.gz`: Linux/Raspberry source, not a prebuilt
  ARM executable. Extract with `tar -xzf`, then run `./tools/build_linux.sh`.
- `ObriiSDR-v6.3.0-SHA256SUMS.txt`: checksums for these two packages.

The existing optional 6.1.1 Wireless add-on remains separate and unchanged;
existing installations can keep it. No new Wireless package is uploaded.
Personal INI files, logs, recordings, secrets and proxy loggers are not included.

See [DSP board](https://github.com/Dotncom/ObriiSDR/blob/v6.3.0/docs/dsp_board_coverage.md),
[stream manager](https://github.com/Dotncom/ObriiSDR/blob/v6.3.0/docs/dsp_stream_manager.md)
and [experiment bundles](https://github.com/Dotncom/ObriiSDR/blob/v6.3.0/docs/experiment_bundle.md).

Feedback: [mojamajsternya@gmail.com](mailto:mojamajsternya@gmail.com).
Include version and reproduction steps; remove private data from attachments.
