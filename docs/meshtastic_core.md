# Meshtastic: ядро PSK/PKI-радіомережі для інтеграції

`src/meshtastic` — автономна бібліотека C++17 без Qt, мережевого I/O, потоків
і доступу до SDR. Вона реалізує радіоформат Meshtastic, PSK-канали,
PKI direct messages, remote admin client/server, пакетну логіку вузла та
адаптер до `ObriiLoRa`. Докладно про нові API — [PKI та адміністрування](meshtastic_pki_admin.md).
Конструктор, основна програма, їхні CMake й робочі EXE не змінювалися.

Ціль поточного етапу — спільне TX/RX-ядро для віртуальних вузлів і майбутнього
SDR-каналу. Це підтримувана PSK/PKI-підмножина протоколу, а не повна заміна
прошивки Meshtastic чи її SDK керування пристроєм.

## Що реалізовано

- PKI X25519/SHA256/AES256-CCM, явні peer-key pins, CSPRNG, автентифіковані
  ACK proofs; remote admin із allowlist, passkey-сесіями, replay cache та
  атомарними транзакціями. Windows використовує CNG (bcrypt), інші платформи — OpenSSL Crypto.

- 16-байтовий радіозаголовок: little-endian to/from/id, hop_limit, hop_start,
  want_ack, via_mqtt, channel hash, next_hop, relay_node. Повний PHY payload
  не перевищує 255 байтів, тіло — 239 байтів.
- Канали з перевіркою локального індексу, short-PSK 0/1..255, AES128/AES256,
  padding часткових ключів та hash від ефективного імені й ключа.
- AES-CTR із nonce `LE64(packetId) | LE32(sender) | BE32(counter=0)`.
  Використано наявний tiny-AES-c у двох ізольованих namespace; DMR crypto
  не змінюється й не має конфліктів імен із новими AES-символами.
- Прямий bounded protobuf codec для `Data`, `User`, базового `Position`
  та `Routing`. У `Data` збережено request/reply ID, emoji, optional bitfield,
  signature bytes. Невідомі поля зберігаються. Відомі поля з неправильною
  wire type, обірвані довжини, overflow varint і некоректний UTF-8 відхиляються.
- UTF-8 текст, NodeInfo, координати та довільні application payload bytes.
  Максимальний чистий text payload без додаткових полів — 234 байти.
- Вузол із TX-чергою, одним активним TX, ACK/NAK, повторними передачами,
  timeout/cancel та окремим повідомленням про локальну помилку передачі.
- Базовий managed flooding: зменшення hop_limit, збереження ciphertext,
  затримка й детермінований jitter, пригнічення дублікатів, скасування relay
  у Client-режимі, збереження relay у Router-режимі та upgrade кращого hop_limit.
- Повтор вихідного пакета проходить через relay знову, якщо ACK загубився.
  Одержувач повторює той самий ACK, але не доставляє текст застосунку вдруге
  протягом вікна історії. ID і ciphertext повторної передачі не змінюються.
- Broadcast confirmation позначається як **Relayed**, а не end-to-end delivery.
  On-air want_ack у broadcast не встановлюється.
- Обмежені історія, node database, черга й журнал подій. Переповнення черги
  відхиляється/позначається QueueFull; старі event entries витісняються,
  їхню кількість показує `droppedEventCount()`.
- Серійний/TCP API **framer**: magic94c3, big-endian length, bounded payload,
  відновлення після шуму, розбитих блоків і oversized header. Вміст кадру
  залишається непрозорим ToRadio/FromRadio protobuf.
- `LoRaRadio`: готовий перехід Meshtastic bytes ↔ LoRa IQ через `ObriiLoRa`.
  RX приймає тільки перевірені PHY CRC пакети.

## Контракт інтеграції

Основний API — `meshtastic.h`, namespace `obrii::meshtastic`.
Адаптер IQ — `meshlora.h`.

```cpp
#include "meshtastic.h"
#include "meshlora.h"
using namespace obrii::meshtastic;

NodeConfig settings;
settings.nodeId = 0x12345678; // окрема унікальна ідентичність кожного вузла
settings.channels = {Channel{"LongFast", {1}, true}};

// Приклад для ізольованої симуляції. Для постійного вузла див. нижче про ID.
PacketIdSequence ids(1);
Node node(settings, [&] { return ids.next(); });
LoRaRadio air;
node.send(textMessage("Obrii"), 0, Broadcast, false, nowMs);

if (auto tx = node.poll(nowMs, channelIsBusy)) {
    auto iq = air.modulate(*tx);
    // Подати IQ у зовнішній TX-тракт. Зберегти tx->ticket до завершення TX.
    // Після реального завершення/відмови:
    node.completeTransmission(tx->ticket, actualTxSucceeded, completionMs);
}

// На прийомі: IQ вже має бути вибрано, відфільтровано й передискретизовано.
for (const auto &packet : air.push(iqBlock)) {
    node.receive(packet.radioBytes, nowMs);
}
for (const auto &event : node.takeEvents()) {
    // Message/Observed, Sent, Acknowledged/NegativeAck, Relayed, TimedOut тощо.
}
```

`nowMs` — монотонний час одного вузла, той самий у send/receive/poll/complete.
Зворотний рух часу відхиляється. Віртуальна мережа може використовувати час
симуляції. Об'єкти не thread-safe: викликати з одного worker/event loop.

`poll()` видає максимум одну передачу. Поки не викликано
`completeTransmission(ticket, ...)`, інша передача не видається. ACK timeout
починається після підтвердженого завершення TX, а не після постановки в чергу.
Після cancel уже активний TX має зупинити зовнішній radio host; його ticket
усе одно треба завершити. Відкладені роботи без radio host самі не виконуються.

Періодично викликати `poll()` і при зайнятому каналі (`channelBusy=true`),
щоб оброблялися таймери. Timeout і затримки налаштувати відповідно до airtime,
кількості hops та MAC-планувальника. Jitter і relay policy цього ядра —
спрощена власна модель; це не відтворення всіх алгоритмів ролей firmware2.8.

`LoRaRadio` за замовчуванням використовує SF11/BW250/CR4/5, sync0x2b,
преамбулу16, explicit header та CRC. Інші підтримувані SF/BW/CR можна задати
через `obrii::lora::Config`. IQ normal, sampleRate=BW×oversampling.
Налаштування частоти, front-end filter/resampler, channel busy/CAD, airtime
планувальник, duty-cycle accounting та фізичний TX/RX надає хост.
Після discontinuity викликати `LoRaRadio::reset()`.

Якщо пакет прийшов із raw packet source, можна викликати `node.receive(bytes)`
без LoRa. Для власного message scheduler доступні `seal`, `open`,
`encodeRadio/decodeRadio` та protobuf codecs без створення `Node`.

## Ключі, ідентичності та значення ACK

`Channel.name` — уже розв'язане ефективне ім'я радіоканалу. Порожнє ім'я
firmware треба попередньо розгорнути в ім'я modem preset; secondary channel
без власного PSK — попередньо розв'язати через primary key. Ядро не має
глобального стану налаштувань firmware й не вгадує ці значення.
Тут порожній PSK або `{0}` явно означає cleartext.

PSK-CTR формат Meshtastic не автентифікує відправника. Декодування protobuf,
PHY CRC, channel hash, NodeInfo.public_key і PSK ACK не доводять справжність
віддаленого вузла. API й утиліта не позначають такі дані authenticated.
Сторонні application payload/текст потребують звичайного екранування в UI.

PacketIdSequence не використовує прихований PRNG. Хост надає строго зростаючий
32-бітовий ID через callback; повтор/wrap відхиляється. Для реального вузла
значення наступного ID треба зберегти **до передачі нового пакета** під тим
самим nodeId/key. Зручно заздалегідь атомарно резервувати діапазон ID, тоді
callback у DSP-потоці не потребує дискового I/O. Не скидати sequence на1
при перезапуску постійної ідентичності. Після UINT32_MAX джерело відхиляє
нові пакети. Retries використовують старі байти, а не новий plaintext із
тим самим nonce. Ключі й ID persistence цей модуль сам на диск не записує.

`Acknowledged` означає сумісний PSK routing response від очікуваного peer
для конкретного pending ID/channel; `Relayed` — лише overheard broadcast relay.
`Sent` означає локальне завершення TX. UI не повинен змішувати ці стани.
При кількох успішних PSK-кандидатах одного hash повертається AmbiguousChannel.
Адресати, opaque payload та невідомі канали доступні для аналізу; невідомі
канали ретранслюються лише при явному `relayUnknownChannels=true`.

## Збірка й автономна перевірка

```powershell
tools/build_meshtastic_core.ps1 -Test
build/meshtastic/Release/ObriiMeshtasticTool.exe encode --from 11223344 --id 12345678 --text hello
build/meshtastic/Release/ObriiMeshtasticTool.exe decode --radio ffffffff443322117856341263080044ab84efc3bfb5fdbc9b
```

Утиліта працює офлайн. Encode повертає radio hex, decode — JSON з payload,
port, from/to/id та `authenticated:false`. Для власного PSK є `--psk-file`
(raw0..32 байти), для каналу — `--channel`. Вона не записує ключі в JSON.
Коди завершення:0 — успіх,1 — неправильні аргументи/пакет/файл,2 — PSK decode
не вдався. `--help` описує всі параметри.

Результати: `build/meshtastic/Release/ObriiMeshtastic.lib`,
`build/meshtastic/lora/Release/ObriiLoRa.lib`, selftest і offline tool.
Варіант без LoRa: `tools/build_meshtastic_core.ps1 -BuildDir build/meshtastic-core-only -WithoutLoRa -Test`.
На інших платформах: `cmake -S src/meshtastic -B build/meshtastic`,
`cmake --build build/meshtastic`, `ctest --test-dir build/meshtastic --output-on-failure`.
Фактично перевірено Windows/MSVC; portable build на Linux/macOS ще не запускався.

Для інтеграції CMake: `add_subdirectory(src/meshtastic)` та
`target_link_libraries(YourTarget PRIVATE ObriiMeshtastic)`.
Як підкаталог, tools/tests за замовчуванням вимкнено. Адаптер можна вимкнути
через `OBRII_MESHTASTIC_WITH_LORA=OFF`. Якщо ObriiLoRa вже існує, ціль повторно
не створюється. CMake install додає заголовки, бібліотеки та license notices.

Тести перевіряють NIST AES128/AES256 CTR; незалежні .NET full-packet fixtures;
protobuf field numbers/MTU/невідомі поля; UTF-8; три вузли й зворотний ACK;
втрату multihop ACK; duplicate delivery; retry/timeout/cancel; busy channel;
Client/Router relay; hop upgrade; peer checks для ACK; bounded state й eviction;
10000 malformed inputs; API framing; повний маршрут A–B–C–B–A через LoRa IQ.
`tools/generate_meshtastic_vectors.ps1` незалежно від C++ коду генерує
`src/meshtastic/vectors.tsv` через системний .NET AES.

## Що не входить до цього етапу

XEdDSA broadcast signatures, MQTT, store-and-forward, full telemetry schemas,
traceroute service, сучасне learned next-hop routing, усі firmware roles,
ToRadio/FromRadio message SDK та USB/BLE/TCP device client не реалізовані.
Для невідомих application payload можна передавати raw protobuf bytes;
це не означає, що відповідний сервіс виконується вузлом.

Підтверджено програмну сумісність wire format і криптографічних векторів.
Ефірна перевірка з фізичним Meshtastic/SX126x/SX127x ще потрібна разом із
перевіркою LoRa PHY. Конструктор і основна програма поки не викликають модуль.

## Первинні джерела та ліцензії

Формат і semantics звірені 2026-11-02 з офіційними джерелами:

- [Mesh broadcast algorithm та таблиця wire header](https://meshtastic.org/docs/overview/mesh-algo/).
- [mesh.proto: Data, User, Position, Routing](https://github.com/meshtastic/protobufs/blob/master/meshtastic/mesh.proto).
- [RadioInterface.h](https://github.com/meshtastic/firmware/blob/master/src/mesh/RadioInterface.h),
  [MeshTypes.h](https://github.com/meshtastic/firmware/blob/master/src/mesh/MeshTypes.h),
  [NodeDB.h: relay ID](https://github.com/meshtastic/firmware/blob/master/src/mesh/NodeDB.h).
- [CryptoEngine.cpp: nonce та CTR](https://github.com/meshtastic/firmware/blob/master/src/mesh/CryptoEngine.cpp),
  [Channels.cpp: PSK normalization та hash](https://github.com/meshtastic/firmware/blob/master/src/mesh/Channels.cpp).

Firmware source та generated protobuf code не копіювалися. Реалізація
незалежна; AES primitive — наявний tiny-AES-c під Unlicense, див.
`src/meshtastic/NOTICE`. LoRa adapter зберігає MIT attribution LoRaPHY
через ліцензійні файли `ObriiLoRa`.
