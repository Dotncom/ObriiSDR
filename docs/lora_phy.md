# LoRa PHY: автономний модуль для майбутнього Meshtastic

Стан: перша реалізація пакетного кодека, IQ-модулятора й потокового
приймача. Код у `src/lora` збирається окремо від основної програми та
конструктора. Змін у їхніх CMake, UI, CLI-командах чи транспорті немає.
Залежності: стандартна бібліотека C++17; Qt, GNU Radio і MATLAB не потрібні.

## Реалізовано

- SF7–SF12; BW 31.25 / 62.5 / 125 / 250 / 500 кГц; CR 4/5–4/8.
- До 255 байтів PHY payload, включно з нульовою довжиною.
- Explicit header із перевіркою checksum, implicit header із заданою довжиною.
- Payload whitening, Hamming FEC, діагональне перемежування, Gray mapping,
  специфічна для LoRa CRC16; CRC обчислюється до whitening.
- Перший блок із CR4/8 і зменшеною кількістю бітів; LDRO auto/on/off.
- Chirp IQ, преамбула, два sync-символи, SFD із 2.25 downchirp.
- Налаштовувані sync word, довжина преамбули, інверсія IQ та амплітуда.
- Частоти дискретизації BW × 1/2/4/8; dechirp + FFT.
- Пошук у потоці з довільними межами блоків, корекція цілого й дробового CFO,
  перевірка sync word, відхилення пошкоджених пакетів і обмежений буфер.
- Розрахунок кількості символів та часу в ефірі.
- Утиліта `ObriiLoRaTool` для офлайн-файлів CF32, без апаратної передачі.

CR у `Config::codingRate` — число 1–4, тобто знаменник мінус 4.
CR4/5 та CR4/6 виявляють помилкові кодові слова без спроб неоднозначного
виправлення; CR4/7 і CR4/8 виправляють одну помилку в кодовому слові.
`crcChecked == false` означає відсутність CRC, а не успішну перевірку.
За наявності CRC пакет із неправильною CRC не повертається користувачу.

## Інтерфейс інтеграції

`loraphy.h`, namespace `obrii::lora`:

```cpp
Config config; // SF11, BW250k, CR4/5, OS2, preamble16, sync0x2b, CRC
Modem tx(config);
Receiver rx(config);
Bytes radioPayload = {0x01, 0x02, 0x03};
Iq samples = tx.modulate(radioPayload);
for (const Reception &received : rx.push(samples)) {
    // received.packet.payload — сирі байти радіопакета після PHY CRC/FEC.
    // Передати їх майбутньому парсеру Meshtastic.
}
```

`PacketCodec` окремо перетворює bytes ↔ symbols. `Modem::demodulateAligned`
приймає один повний burst із відомим початком; `Receiver` шукає пакети
в неперервному потоці. `Reception` містить зміщення першого payload IQ-семпла
від останнього `reset()` та оцінку CFO в герцах. CFO наведено після нормалізації
інверсії IQ; для inverted IQ його знак протилежний фізичному зсуву входу.

Для підключення до IQ-тракту:

1. Вибрати канал, відфільтрувати та передискретизувати IQ до `config.sampleRate()`.
2. Подавати блоки `std::complex<float>` у `Receiver::push` у DSP/робочому потоці.
3. Викликати `reset()` після розриву IQ, втрати/перестановки транспортного блока,
   переналаштування або зміни частоти дискретизації. Для нового SF/BW створити
   новий `Receiver`. Один об'єкт не можна викликати одночасно з різних потоків.
4. Передати перевірений PHY payload протоколу Meshtastic. Його радіозаголовок,
   protobuf, шифрування, ACK і маршрутизація належать наступному шару.
5. TX `Iq` можна серіалізувати в CF32 для наявного `VirtualSignalFrame`
   з `ComplexFloat32`, правильними sampleRate/centerFrequencyHz і неперервною
   послідовністю блоків. Цей адаптер поки не під'єднано до мережевого hub.

У майбутньому CMake достатньо `add_subdirectory(src/lora)` і
`target_link_libraries(YourTarget PRIVATE ObriiLoRa)`. При включенні як
підкаталогу утиліта й тести за замовчуванням вимкнені; опції
`OBRII_LORA_BUILD_TOOLS` та `OBRII_LORA_BUILD_TESTS` вмикають їх явно.

Початкові параметри орієнтовано на Meshtastic LongFast: SF11, BW250 кГц,
CR4/5. Sync0x2b, explicit header, CRC й преамбула16 звірені з офіційними
[RadioLibInterface.h](https://github.com/meshtastic/firmware/blob/master/src/mesh/RadioLibInterface.h),
[RadioInterface.h](https://github.com/meshtastic/firmware/blob/master/src/mesh/RadioInterface.h)
та [таблицею пресетів](https://meshtastic.org/docs/overview/radio-settings/).
Це параметри PHY; вибір регіону/каналу та формування Meshtastic-пакета тут
не реалізовані. Частота несучої задається зовнішнім SDR-трактом.

## Збірка та перевірка

Windows / MSVC 2022:

```powershell
tools/build_lora_phy.ps1 -Test
build/lora/Release/ObriiLoRaTool.exe encode --hex 4f62726969204c6f5261 --output build/lora/demo.cf32
build/lora/Release/ObriiLoRaTool.exe decode --input build/lora/demo.cf32
```

Результат прикладу: `payload_hex=4f62726969204c6f5261 crc=ok`, sampleRate500000.
Файл — little-endian float32 I,Q без заголовка, 8 байтів на комплексний семпл.
Параметри запису слід передати при декодуванні; файл не містить SF/BW metadata.
Код повернення утиліти: 0 — успіх, 1 — помилка аргументів/файла, 2 — жодного
валідного пакета при декодуванні. `--help` показує всі параметри.

На Linux/macOS для перевірки портованості:

```sh
cmake -S src/lora -B build/lora
cmake --build build/lora
ctest --test-dir build/lora --output-on-failure
```

Автоматична перевірка на Windows включає:

- 1344 комбінації SF/CR/header/CRC/LDRO/довжин від 0 до 255 байтів.
- Незалежний опублікований LoRaPHY вектор із 24 символів та CRC0x2eba.
  Декодується весь вектор; для encoder порівнюються повні блоки до padding,
  оскільки приклад містить довільні trailing pad nibbles.
- IQ roundtrip для SF7–12, oversampling1/2/4/8 та правильність FFT bin.
- Потоковий прийом із нерівними блоками, зміщенням початку, normal/inverted IQ,
  CFO −5.23 / +0.23 / +8.23 bins при SNR12дБ.
- Додаткові шумові сценарії SNR−3 та 0дБ для SF7/SF11.
- Короткі/implicit пакети без додаткових trailing семплів, два пакети поспіль,
  reset у середині пакета, відхилення чужого sync word, FEC/CRC та помилкових
  параметрів; обмеження пам'яті на довгому потоці тиші.

## Межі першої реалізації

Сумісність кодування підтверджено незалежним вектором. Взаємодію IQ із
фізичними SX126x/SX127x ще не перевірено; це наступна обов'язкова перевірка
перед заявою про ефірну сумісність.

Приймач очікує відомі SF/BW, достатньо довгу задану преамбулу та один
підготовлений канал. Автосканування SF/BW, front-end фільтр/довільний resampler,
soft-decision FEC, відстеження sample-clock drift, multipath, розділення колізій
і прийом біля паспортної межі чутливості LoRa поки відсутні. Початкові пороги
acquisition консервативні; тести SNR не є виміром повної чутливості.
Integer CFO обмежено діапазоном менше BW/8; наведені вище величини — перевірені
сценарії, а не гарантія роботи на всій межі цього діапазону.
SF5/SF6 і wide-LoRa 2.4GHz не реалізовані.

Один burst/буфер обмежено 32M комплексних семплів. Конфігурація, що перевищує
цей ліміт, відхиляється до створення великого burst. `Receiver::push` повертає
список прийнятих пакетів поточного виклику; caller має регулярно його обробляти.

## Походження та ліцензія

Кодування звірено й адаптовано за MIT-ліцензованим
[LoRaPHY](https://github.com/jkadbear/LoRaPHY/blob/master/LoRaPHY.m),
Copyright2020–2022 jkadbear. Повний notice та MIT license містяться в
`src/lora/NOTICE` і `src/lora/LICENSE.LoRaPHY`; CMake install включає їх.
Джерела GNU Radio/gr-lora_sdr у модуль не копіювалися.
Офіційні матеріали [Semtech](https://www.semtech.com/lora/what-is-lora)
використано як загальний опис CSS PHY.
