# Obrii SDR 6.4.1

Receiver calibration, Y-factor measurement and reproducible scientific export.
Windows and Raspberry/Linux only. No Wireless add-on or Android upload.

## Українською

- Додано запис власного шуму приймача з узгодженим навантаженням 50 Ом.
  Профіль прив'язується до приймача, входу, центральної частоти, sample rate,
  FFT/вікна, RF-смуги, підсилення та амплітудної калібрації.
- Додано вимірювання Y-factor: накопичення Cold/Hot, калібрований ENR або відомі
  температури джерел, таблиця й графік noise figure та еквівалентної шумової
  температури по частоті, експорт CSV.
- Візуальні режими вирівнювання шуму, надлишкової потужності та приведення до
  входу є окремими вимикачами. Вони не змінюють сирі IQ, аудіо, записи або
  наукові вимірювання й не виконують фонових обчислень, коли вимкнені.
- Пакет експерименту може додати відповідні таблиці власного шуму та Y-factor
  як окремі CSV із SHA-256 і повною конфігурацією. Контекст початку запису не
  підміняється поточними налаштуваннями під час експорту.
- Калібрування приймача перенесено із загальних налаштувань до розділу
  «Вимір спектра». Уточнено назви калібрування спурів і довідку.

## Verification And Limits

Windows Release build passed. Receiver calibration passed 9 deterministic
checks, the science suite passed 110 checks, and experiment-bundle tests passed
round-trip, privacy, integrity, path-traversal, cancellation and active-writer
coverage. The Linux/Raspberry source is updated but was not compiled on ARM in
this release session.

Absolute dBm, noise figure and noise-temperature accuracy still depend on the
individual receiver, termination, calibrated noise source, cabling, temperature
and unchanged gain/filter settings. The software preserves the calibration
identity and provenance; it cannot replace traceable hardware standards.

## Downloads

- `ObriiSDR-v6.4.1-windows-x64.zip`: portable Windows core and science runtime.
- `ObriiSDR-v6.4.1-raspberry-source.tar.gz`: Linux/Raspberry source. Extract it
  and run `./tools/build_linux.sh`.
- `ObriiSDR-v6.4.1-SHA256SUMS.txt`: checksums for both packages.

The existing optional 6.1.1 Wireless add-on remains compatible and is not
uploaded again. Personal settings, API keys, logs, recordings, coordinates,
tokens and local import folders are excluded.

Feedback: [mojamajsternya@gmail.com](mailto:mojamajsternya@gmail.com).
Include the version and reproduction steps, and remove private data first.
