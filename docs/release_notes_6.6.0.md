# Obrii SDR 6.6.0

Multi-user networking, source-aware DSP and automatic known-key DMR selection.
Windows and Raspberry/Linux only. No Wireless add-on or Android upload.

## Українською

- Серверний мережевий режим отримав незалежні VFO/DDC/аудіосесії для кількох
  клієнтів, явне передавання керування та вбудований браузерний спектр,
  водоспад і аудіо. Спостерігачі не можуть непомітно перехопити приймач.
- DSP-гілки тепер адресуються через `receiverId + channelId`: RX1-RX8 мають
  окремі DDC, PFB-банки, аудіо, записи та зовнішню обробку. Допоміжний IQ не
  копіюється і воркер не запускається, доки на джерело немає підписника.
- Мережевий IQ підтримує CF32, CS16 та block-float CS16 із явними метаданими
  квантування, лічильниками розривів і сумісністю зі старими обчислювачами.
  Повільна зовнішня гілка має обмежену чергу й не зупиняє локальне аудіо.
- Додано спільний менеджер обчислювального бюджету та рекомендації для
  NUMA/processor groups. Основний Fobos/Raspberry тракт не отримує додаткової
  роботи, коли розширені гілки вимкнені.
- Підготовлено когерентні multi-SDR сесії: формати IQ різної бітності,
  метадані clock/PPS/timestamp, оцінка затримки, фази та підсилення. Це каркас
  для апаратної перевірки, а не заява про когерентність незалежних приймачів.
- Obrii Analyzer отримав апаратно заблокований план лабораторних приладів для
  tracking generator, SNA, return loss/КСХ, VNA, SCPI та відтворюваних сесій.
  RF-виконання лишається вимкненим до перевірки USRP/UHD і калібрування.
- У DMR додано явні режими `Фіксований ключ` та `Автовибір PI ALG/KID`.
  Власний mbelib/OpenDMR-тракт обирає точний відомий ARC4/AES-256 ключ окремо
  для кожного таймслота; невідомий KID приглушується без перебору ключів.
## Verification And Limits

Windows Release builds of Obrii SDR and Obrii Analyzer complete successfully.
The DMR fixed/automatic known-key fixture test passed during implementation.
Later attempts to launch console self-tests in this Windows session stalled
before `main`, including an unchanged CW self-test; therefore this release does
not claim a fresh complete self-test sweep. Linux/Raspberry source is updated
but was not compiled on ARM during this release session.

Browser remote access has no built-in TLS or user-account authentication. Use
it only on a trusted LAN or behind a VPN/authenticated reverse proxy. Network
IQ is likewise unencrypted. Automatic DMR PI ALG/KID selection currently
applies to the native mbelib/OpenDMR path; DSD-neo and GopherTrunk retain their
selected fixed-key behavior.

## Downloads

- `ObriiSDR-v6.6.0-windows-x64.zip`: portable Windows core, Obrii SDR and
  Obrii Analyzer.
- `ObriiSDR-v6.6.0-raspberry-source.tar.gz`: Linux/Raspberry source. Extract it
  and run `./tools/build_linux.sh`.
- `ObriiSDR-v6.6.0-SHA256SUMS.txt`: checksums for both packages.

The existing optional 6.1.1 Wireless add-on remains compatible and is not
uploaded again. Personal settings, API keys, logs, recordings, coordinates,
tokens, DMR keys and local import folders are excluded.

Feedback: [mojamajsternya@gmail.com](mailto:mojamajsternya@gmail.com).
Include the version and reproduction steps, and remove private data first.
