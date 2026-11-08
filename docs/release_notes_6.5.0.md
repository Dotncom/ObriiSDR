# Obrii SDR 6.5.0

Standalone spectrum analyzer and Multi-SDR workspace. Windows and
Raspberry/Linux only. No Wireless add-on or Android upload.

## Українською

- Додано окрему програму Obrii Analyzer: пряме відкриття підтримуваного SDR,
  підключення до живого спектра Obrii SDR, записи кадрів, широке
  sweep-сканування, режими редукції, до десяти маркерів і таблицю парних
  вимірювань.
- Додано Multi-SDR для RX1-RX8. Незалежні приймачі формують спільні спектр,
  водоспад та абсолютну шкалу; перекриття змішується в лінійній потужності, а
  реальні проміжки між смугами не замальовуються вигаданими даними.
- Є загальний і фокусний масштаби, швидке перемикання між RX та симетричне
  панорамування. У загальному огляді всі додані RX завжди рухаються однаковою
  дельтою; галочка «Завжди рухати RX разом» поширює це правило на фокусний
  режим.
- Маячок прослуховування автоматично вибирає RF-приймач, смуга якого містить
  частоту. IQ RX2-RX8 надходить у чинний аудіодемодулятор зі своїми sample rate
  та центром, тому RX1 більше не перелаштовується за сигналом іншого RX.
- Налаштування всіх додаткових приймачів зберігаються одразу й відновлюються
  після перезапуску. Перемикання аудіоджерела очищає лише аудіочергу й не
  стирає FFT/IQ-історію наукових інструментів.
- Розширено маркери та їх парні вимірювання, підготовлено SoapySDR/USRP B210,
  виправлено редукцію великих FFT і відображення розрідженого спектра.

## Verification And Limits

Windows Release builds of Obrii SDR and Obrii Analyzer passed. DMR/audio,
spectrum mosaic, spectrum pipeline, analyzer frame channel, instrument sweep
and all 112 science checks passed. Linux/Raspberry source is updated but was
not compiled on ARM in this release session.

The Multi-SDR workspace is a frequency-aligned power-spectrum mosaic, not a
claim of coherent complex-IQ joining. Coherent phase/delay alignment still
requires shared clocks and timestamp-capable hardware. Auxiliary Fobos direct
HF inputs remain spectrum-only in automatic audio routing; ordinary RF inputs
are supported. Hardware audio-source switching needs final field validation
with the user's receiver pair.

## Downloads

- `ObriiSDR-v6.5.0-windows-x64.zip`: portable Windows core, Obrii SDR and
  Obrii Analyzer.
- `ObriiSDR-v6.5.0-raspberry-source.tar.gz`: Linux/Raspberry source. Extract it
  and run `./tools/build_linux.sh`.
- `ObriiSDR-v6.5.0-SHA256SUMS.txt`: checksums for both packages.

The existing optional 6.1.1 Wireless add-on remains compatible and is not
uploaded again. Personal settings, API keys, logs, recordings, coordinates,
tokens and local import folders are excluded.

Feedback: [mojamajsternya@gmail.com](mailto:mojamajsternya@gmail.com).
Include the version and reproduction steps, and remove private data first.
