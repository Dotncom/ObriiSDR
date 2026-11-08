# Obrii SDR 6.2.0

DSP stream management, experimental LAN computation and connection diagnostics,
plus DSP-board and USB discovery fixes. Android is unchanged and is not included.

## Українською

- Додано менеджер DSP-потоків: локальна обробка, обчислювач на іншому ПК,
  GNU Radio-адаптер і окремий запис IQ вибраних DDC-гілок.
- Основний RF FFT можна передати обчислювачу окремими IQ-вікнами або повним
  безперервним потоком. Приймач та аудіо залишаються на локальному ПК.
- Тест з'єднання вимірює затримку, її коливання і швидкість в обидва боки,
  оцінює потрібний трафік без передавання реального IQ. Оновіть обидва ПК.
- Виправлено виліт при видаленні групи блоків DSP-дошки. Оновлення USB більше
  не робить апаратний reset і не підміняє відсутній Fobos іншим приймачем.
- Мережеві обчислення експериментальні. Використовуйте довірену локальну мережу;
  токен дозволяє доступ, але не шифрує трафік. Реальна швидкодія залежить від
  обох ПК, мережі та параметрів FFT.

## Changes

- Optional shared stream manager for independent DDC branches, native Obrii
  compute workers and persistent one-in/one-out, same-rate GNU Radio blocks.
  Per-branch CF32LE/JSON recording has bounded sessions and explicit gap segments.
- Main RF FFT offload uses the existing FFT kernel with chunked IQ/results,
  generation checks and upload/memory budgets. Choose individual windows or
  continuous full-band IQ; fallback is explicit. Scan/HF use local processing.
- Authenticated, cancellable connection preflight measures TCP/handshake time,
  RTT min/median/p95, jitter and synthetic upload/download throughput. Estimates
  show bandwidth headroom and memory limits without enabling IQ transfer.
- Continuous FFT starts from the newest retained complete window; IQ sample
  lag is reported separately from processing-cycle latency.
- Receiver/IQ -> FFT -> workspace display links bind the shared main spectrum
  path. Fixed mixed block/edge deletion, worker startup and board teardown.
- Simplified operating mode keeps its previous settings key and disables optional
  stream branches/recording without changing the main audio path.
- USB refresh preserves owned Fobos sessions, retries delayed discovery, keeps
  serial-based selection and never silently substitutes RTL-SDR or changes rate.

## Boundaries And Setup

Update both computers to 6.2.0. On the compute PC start the worker with its LAN
address; enter that address, port and current session token on the receiver PC.
Run the connection test before allowing IQ transfer. Active compute sessions
must stop before the synthetic test; it does not disconnect them automatically.
See [DSP stream manager](https://github.com/Dotncom/ObriiSDR/blob/v6.2.0/docs/dsp_stream_manager.md)
and [GNU Radio bridge](https://github.com/Dotncom/ObriiSDR/blob/v6.2.0/docs/gnu_radio_bridge_usage.md).

Transport is authenticated but not encrypted: use a trusted LAN or a separately
secured tunnel, not a public Internet listener. Real full-rate Raspberry-to-laptop
performance still needs qualification. At 50 MS/s, continuous CF32 requires
400 MB/s upstream before overhead, beyond a gigabit link. Window mode can reduce
traffic substantially. The current pipeline is serialized; a fast connection
does not guarantee faster FFT, and local fallback is not seamless failover.

## Downloads

- `ObriiSDR-v6.2.0-windows-x64.zip`: portable Windows core and science runtimes.
- `ObriiSDR-v6.2.0-raspberry-source.tar.gz`: Linux/Raspberry source, not a prebuilt
  ARM executable. Extract with `tar -xzf`, then use `tools/build_linux.sh`.
- `ObriiSDR-v6.2.0-SHA256SUMS.txt`: checksums for these two packages.
- Optional wireless runtime: reuse the unchanged [6.1.1 Wireless add-on](https://github.com/Dotncom/ObriiSDR/releases/download/v6.1.1/ObriiSDR-v6.1.1-wireless-windows-x64.zip).
  Existing installations do not need to download it again. Fresh installations
  can run the supplied `Install-Wireless-AddOn.ps1`, which verifies SHA-256.
  The GNU Radio adapter script itself is included in the core under `dsp/`.

Personal settings, session/API keys, geographic markers, captures, diagnostic
logs and USB proxy loggers are excluded. Settings stay in the per-user directory.
Software checks cover FFT parity, protocol/gap handling, connection-test failure
and cancellation, DSP-board lifecycle and USB selection. Windows was built and
tested here; the Linux source is updated but was not compiled on Raspberry here.

## Feedback

[mojamajsternya@gmail.com](mailto:mojamajsternya@gmail.com)

Include the version and reproduction steps. Remove API keys, private locations
and other sensitive data from logs or screenshots before sharing them.
