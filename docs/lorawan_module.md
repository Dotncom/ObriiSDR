# Окремий модуль LoRaWAN

Версія 0.2.0: **C++17 ядро LoRaWAN 1.0.4 / 1.1**, EU868, unicast Class A/B/C
та адаптер LoRa IQ. Кінцевий вузол і мережева сторона працюють у конструкторі.
Додатково є незалежний Python-кодек Semtech UDP v2 для майбутнього gateway-host.
Модуль можна збирати окремо. Інтеграція в розподілений конструктор Obrii 8.1
описана в `mesh_lorawan_adapter.md`; Android її не містить. Встановлення,
підключення SDR або запуск мережевих
служб не відбувається. Це робоче протокольне ядро, **не сертифікований повний MAC-стек**.

## Реалізовані частини

| Частина | API / файл | Поведінка |
|---|---|---|
| Кадри й криптографія | `lorawan.h`, `codec.cpp`, `crypto.cpp` | Join Request/Accept, MHDR/FHDR/FOpts/FPort/FRMPayload, AES128, CMAC/MIC, сесійні ключі |
| Активація вузла | `OtaaDevice` | OTAA, постійний DevNonce, перевірка JoinNonce, атомарне створення сесії |
| Мережева активація | `JoinServer` | Один запис provisioned-пристрою: перевірка EUI/MIC/DevNonce, Join Accept і ключі |
| Сесія | `Session` | ABP або OTAA, Device/Network, FCnt32, ACK, NbTrans, дедуплікація, захист від повторів |
| Регіон і MAC | `regional.h`, `region.cpp` | EU868, CFList, MAC-команди, ADR і відкат, RX1/RX2, airtime-паузи |
| Збереження | `state.cpp` | Обмежені версійовані blobs для сесії, join, EU868 і duty state, CRC32 від пошкодження |
| IQ | `lorawanlora.h` | Прямий PHYPayload ↔ наш LoRa-модем, потік довільних блоків IQ |
| Gateway codec | `obrii_lorawan_gateway` | Semtech UDP v2 datagrams, uplink metadata, downlink requests, ACK, uint32 timestamp wrap |
| Offline tool | `ObriiLoRaWANTool` | Кодування/декодування й ключові тестові операції через stdin без RF/сокетів |

Підтримуються LoRaWAN 1.0.4 і явно вибраний 1.1, EU868, unicast Class A/B/C.
Наявні профілі й збережені blobs 1.0.4 читаються без скидання ключів/лічильників.
У конструкторі модуль підключений через `src/mesh/meshlorawanadapter.cpp`;
див. [налаштування адаптера](mesh_lorawan_adapter.md).
EUI у C++ — 8 байтів у звичайному порядку відображення; на дроті порядок
перевертається. DevAddr/лічильники/NetID/JoinNonce у структурах — числові значення,
на дроті — little-endian. Порт 0 шифрується NwkSKey, порти 1..255 — AppSKey.
Порти застосунку — 1..223; протокольний кодек також читає зарезервовані порти,
але не запускає через них тестові або прикладні команди.

## Збірка

```powershell
.\tools\build_lorawan_module.ps1 -Test
```

Потрібні CMake/MSVC 2022 та Python 3.10+ для тестів gateway-кодека. Python-пакети
встановлювати не потрібно. Результат:

- `build/lorawan/Release/ObriiLoRaWAN.lib`;
- `build/lorawan/Release/ObriiLoRaWANTool.exe`.

`-CoreOnly` збирає протокол, стан і регіональні функції без LoRa PHY.
Для іншого CMake-host: `add_subdirectory(src/lorawan)` та
`target_link_libraries(host PRIVATE ObriiLoRaWAN)`. Qt та Python runtime
не потрібні C++ бібліотеці; Python gateway codec є необов'язковою окремою частиною.
`ObriiLoRa` повторно використовується, якщо ціль уже створена іншими модулями.

## OTAA, ABP і постійний стан

`Credentials` містить JoinEUI, DevEUI та AppKey. Для ABP потрібні DevAddr,
NwkSKey, AppSKey та актуальні лічильники в `SessionState`.
Для кожного вузла слід provision окремі DevEUI та AppKey.
Модуль не генерує тестові ключі для реальних вузлів і не підміняє пошкоджений стан
новим. Ключі передає host зі свого provisioning-сховища.

Кожен `Session`, `OtaaDevice`, `JoinServer` вимагає callback `Persist`.
Callback повинен синхронно й атомарно зберегти стан або кинути виняток.
Лічильник резервується **до повернення TX-байтів**, а прийнятий counter —
до повернення RX-даних застосунку. Якщо сховище повідомило помилку, об'єкт
блокує подальші зміни до відновлення з постійного стану: запис міг уже відбутися.

Один власник і один потік змін на контекст пристрою. Stateful-об'єкти не копіюються;
host повинен забезпечити також виключне володіння сховищем між процесами.
Завантаження старого snapshot з тими самими ключами не є коректним «скиданням».
CRC32 blobs виявляє випадкове пошкодження, але не шифрує й не автентифікує сховище.
Ключі, credentials і blobs зберігати як закриті локальні дані, поза публічними пресетами.

Зв'язування активної OTAA-сесії з одним записом стану:

```cpp
// persistDevice: власний durable callback host, який зберігає
// saveDeviceJoin(state) атомарно. Після reboot читаємо loadDeviceJoin(blob).
obrii::lorawan::OtaaDevice join(credentials, restoredJoinState, persistDevice);
auto request = join.request();
// Host передає request та приймає Join Accept у відповідному RX-вікні.
auto nextRegion = region;
auto accept = join.accept(joinAcceptWire, [&](const auto &parameters) {
    nextRegion.applyJoin(parameters); // Перевірка регіону до commit активації.
});
region = nextRegion;
obrii::lorawan::Session session(
    obrii::lorawan::Role::Device, *join.state().activated,
    [&](const auto &state) { join.updateSession(state); });
```

Аналогічно мережева `Session(Role::Network, ...)` зберігає живі counters через
`JoinServer::updateSession`. Мережевий host тримає окремі provisioned-записи на
DevEUI/JoinEUI та відображення DevAddr → сесія. Усі живі зміни сесії повинні
потрапляти в цей запис, щоб нова join-спроба не перезаписала counters старою копією.
Оновлення з іншими ключами, відкатом counters або конфліктним TX-кешем відхиляються.

DevNonce починається з 0, витрачається на кожен новий Join Request і зупиняється
на вичерпанні 65536 значень. Мережевий JoinNonce теж монотонний, до 24 біт.
Вузол за замовчуванням вимагає зростання JoinNonce — строгий профіль захисту від
повторної активації в дусі TR001. Параметр `requireIncreasingJoinNonce=false`
передбачений для сумісності зі старими мережами, що видають непослідовні nonce;
це не повний кеш усіх раніше використаних JoinNonce.

`Session::transmit` резервує новий FCnt. `repeatUplink` повертає ті самі байти
з тим самим FCnt і обмежується NbTrans; після валідного downlink повтори припиняються.
`setNbTrans` дозволяє застосувати `Eu868.nbTrans` до сесії.
На мережевій стороні повтор має `duplicate=true`, порожні payload/options і не
повинен повторно запускати прикладні обробники. Підтвердження йде новим downlink
з ACK. Downlink із повторним FCnt не обробляється вдруге.

FCnt передається 16 молодшими бітами, але MIC використовує повні 32 біти.
Сесія відновлює наступну епоху при переповненні low16. Якщо втрачено цілу епоху
або перший відомий counter вищий за 65535, host може передати відомий повний
`counterHint`; MIC і монотонність все одно перевіряються. На 2^32 counters
нова передача забороняється до нової активації з іншими сесійними ключами.

## EU868 і Class A

Регіональний профіль містить 16 каналів, початкові 868.1/868.3/868.5 МГц,
RX2 869.525 МГц/DR0, RX1 offset0, receive delay1 с і NbTrans1.
Тут частоти є параметрами протоколу; бібліотека не налаштовує та не запускає RF.

| Data rate | PHY | Найбільший FRMPayload без FOpts |
|---|---|---|
| DR0..2 | SF12..10 / BW125 | 51 байт |
| DR3 | SF9 / BW125 | 115 байтів |
| DR4..5 | SF8..7 / BW125 | 242 байти |
| DR6 | SF7 / BW250 | 242 байти |

Для repeater-compatible режиму DR4..6 — 222 байти. FOpts зменшує доступну
місткість. `Eu868::checkPayload` перевіряє повний PHYPayload за фактичним DR.
DR7/FSK, LR-FHSS та інші регіони поки не реалізовані.

`receiveWindows` бере фактичний кінець TX у мікросекундах, частоту, DR і параметри
сесії. Повертає nominal/open/close для RX1 і RX2; для join це 5/6 с, для звичайних
передач — налаштовані delay/delay+1 с. RX1 враховує DlChannel та RX1DROffset.
Інтервал open/close є вікном **виявлення початку** пакета: успішно знайдений пакет
потрібно дочитати до кінця, навіть якщо його airtime довший за це вікно.
`nextUplinkTime` враховує RX2 і timeout підтвердженої передачі; host має надати
випадкове значення retry delay у межах 1..3 с та виконати hopping/backoff.

MAC-команди CID2..10 і CID13 парсяться в обох напрямках. `processDownlink`
застосовує LinkADR, DutyCycle, RXParamSetup, DevStatus, NewChannel, RXTiming,
DlChannel та читає LinkCheck/DeviceTime. TXParamSetup для EU868 не обробляється
і не підтверджується. Суміжний блок LinkADR має атомарну маску каналів;
при вимкненому ADR маски можна прийняти незалежно від зміни швидкості/потужності.
Невідома команда завершує розбір і повертається в `unparsed`; обрізана відома
команда спричиняє помилку без часткового оновлення регіону.

RXParam/RXTiming/DlChannel відповіді зберігаються в `stickyAnswers` до наступного
валідного Class A downlink. Викликати `processDownlink` для кожного такого кадру,
навіть якщо він не містить MAC-команд, і лише після MIC/counter/window перевірок.
Відповіді мають пріоритет над новими командами й application payload. Якщо вони
не вміщуються у FOpts, host надсилає їх через FPort0 без application payload.
Завеликий список відповідей обрізається на межі команд за DR; є `answersTruncated`.

`onNewUplink` викликається один раз на новий FCnt: формує ADR/ADRACKReq,
рахує ADRACKCnt, на 64 запитує відповідь, на 96 повертає початкову потужність,
далі поетапно знижує DR кожні 32 кадри й відновлює default channels/NbTrans.
Повтори з тим самим FCnt не збільшують ADRACKCnt. Валідний downlink скидає його.
MAC-параметри, sticky state та ADR count серіалізуються через `saveRegion/loadRegion`.

`reserveDuty` повертає нові абсолютні deadlines піддіапазонів EU868 та загального
DutyCycleReq. `reserveBackoff` додає профіль 1% / 0.1% / 0.01% за часом від
power-up для join та ситуацій, на які поширюється backoff. Потрібно зберегти
резервацію **до RF**, навіть якщо згодом передача не відбулася. `saveDuty/loadDuty`
зберігають ledger. Host відповідає за незворотність часової шкали або її
консервативне відновлення після reboot; зміну системного годинника ці helpers
не відстежують. MAC, сесійні counters і регіональні налаштування варто зберігати
однією транзакцією host-сховища.

## LoRa IQ та gateway

`LoRaBearer(DR, Direction, oversampling)` кодує **безпосередньо LoRaWAN PHYPayload**.
Профіль: sync0x34, preamble8, CR4/5, explicit header; uplink normal IQ з PHY CRC,
downlink inverted IQ без PHY CRC. Немає обгорток OBRN, RNode або Meshtastic.
RX приймає довільні блоки complex<float>, перевіряє PHY/довжину/напрямок і повертає
`RadioPacket.wire`. Для uplink потрібен перевірений PHY CRC; MIC і counters
потім перевіряє `Session::receive`. Після пропуску IQ, retune або зміни параметрів
викликати `reset`/створити новий bearer. Приймач має відомі SF/BW та успадковує
обмеження `docs/lora_phy.md`: багатоканальність, сканування і колізії потребують host DSP.

Сокет-незалежний `obrii_lorawan_gateway` формує PUSH_DATA/PULL_DATA/TX_ACK,
читає UDP v2 headers/JSON/ACK та PULL_RESP з LoRaWAN downlink. Є строгі bounds,
base64, діапазони DR/frequency/flags, uint32-wrap-aware `timestamp_delta`.
Обмеження: 16 КіБ/datagram, до 32 uplink у PUSH_DATA. GPS `tmms` планування
потребує окремого clock adapter. Кодек не відкриває UDP, не надсилає пакети
та не підтверджує TX самостійно.

PUSH_DATA `tmst` — час **кінця RX**, а PULL_RESP `tmst` — запитаний **початок TX**,
обидва uint32 мікросекунд. Поле C++ `payloadSampleOffset` описує початок
LoRa payload у вхідному IQ-потоці; host має перевести його в потрібну RF-подію.
Перед TX host перевіряє задані ipol/ncrc/preamble/power, timing, duty та стан SDR.
Якщо поля відрізняються від стандартного профілю `LoRaBearer`, потрібно відповідно
налаштувати `phyConfig`/низькорівневий LoRa modem або відхилити запит.
UDP ACK повідомляє про transport/scheduling, а не про доставку LoRaWAN застосунку;
сам Semtech UDP не автентифікує мережевий сервер.

## Перевірки

`build_lorawan_module.ps1 -Test` перевіряє:

- NIST AES128, RFC4493 CMAC і опублікований незалежний Join Accept;
- **2837** незалежних векторів brocaar/lorawan: обидва напрямки, confirmed та
  unconfirmed, порти, FOpts, межі блоків, FCnt32/rollover, OTAA та ключі;
- OTAA → uplink → downlink/ACK через IQ, усі DR0..6 і найбільші payload;
- nonce/counter replay, NbTrans, duplicate suppression, reboot, stale session,
  ambiguous persistence failure, CRC/state schema;
- MAC, ADR, Class A windows, overflow і duty/backoff;
- 5000 некоректних C++ frame/MAC/state cases, зокрема blobs із правильною CRC;
- 5000 випадкових UDP datagrams, неправильні JSON/base64/параметри, timestamp wrap;
- offline tool проти всіх vectors, decode, помилки та завеликі/незавершені рядки.

Окрема core-збірка також перевіряється під MSVC AddressSanitizer.
Тестові persist callbacks використовують пам'ять для моделювання storage і збоїв;
це не приклад production-сховища.

Для регенерації еталонів потрібен Go 1.18+ та доступ до залежностей:

```powershell
.\tools\generate_lorawan_vectors.ps1
```

Залежність закріплена на `brocaar/lorawan` commit
`4bb09b3bdacba097680894e8d35dbcbc0d260c7e`, із go.mod/go.sum.
Go cache і build розміщуються лише в `build/lorawan-reference`; глобальне Go-оточення
не змінюється. Звичайна збірка використовує збережені vectors без Go або мережі.

## LoRaWAN 1.1

`Credentials::version = Version::V1_1` вмикає OTAA з окремими `nwkKey` і
`appKey`. JoinRequest MIC використовує NwkKey; JoinAccept перевіряє OptNeg,
JSIntKey, JoinEUI і **саме pending DevNonce**. Downgrade до OptNeg=0 відхиляється:
для мережі 1.0 слід явно створити профіль 1.0.4. JoinNonce завжди має зростати.

`deriveSessionKeys11` формує FNwkSIntKey (`SessionKeys::network`), SNwkSIntKey
(`servingIntegrity`), NwkSEncKey (`networkEncryption`) та AppSKey (`application`).
`deriveJoinServerKeys` формує JSEncKey/JSIntKey. ABP задає всі чотири session keys
і `SessionKeys::version` явно. Root keys не замінюють session keys.

`encodeData`/`decodeData` приймають `DataContext` з фактичними `txDataRate`,
`txChannel` і ConfFCnt16. У Session значення ConfFCnt береться зі збереженого
confirmed-обміну; зовнішній context визначає лише DR/channel. Uplink MIC поєднує
SNwkSIntKey/FNwkSIntKey; downlink використовує SNwkSIntKey. FOpts шифрується
NwkSEncKey з окремими доменами NFCntDown/AFCntDown згідно з erratum 2018 року.

Для network role `nextTransmit` — NFCntDown, `nextApplicationTransmit` —
AFCntDown. Для device role `lastReceived` — NFCntDown, `lastApplicationReceived`
— AFCntDown; FCntUp лишається окремим у протилежному напрямку. NFCntDown
використовується без FPort або з FPort=0, AFCntDown — з FPort>0. Обидва домени
мають незалежні rollover/replay/exhaustion перевірки й зберігаються атомарно.
`Session::preview` і `inspect` перевіряють кадр без зміни durable state;
`transmit`/`receive` виконують persistence barrier перед видачею результату.

`repeatUplink(context)` зберігає FCnt і ciphertext, але **перераховує MIC 1.1**
для фактичного нового DR/channel. Network дедуплікує лише після перевірки MIC
з цими параметрами й не доставляє MAC/app payload повторно. Для 1.0.4 повтори
зберігають точні wire bytes. Збережені 1.1 caches містять потрібний MIC context.

`encodeRejoinRequest`/`decodeRejoinRequest` реалізують ефірні типи 0/1/2;
JoinAccept11 приймає requestType 255/0/1/2 і використовує JSEncKey для rejoin.
Це низькорівневі функції: **вони не резервують RJcount і не запускають rejoin**.
Автоматична rejoin/roaming policy та одночасне зберігання старого/нового
network security context не реалізовані. Адаптер не видає їх за підтримувані.

## Class B і Class C

`classbc.h` містить EU868 beacon codec, `ClassBTracker` і `acceptsClassC`.
Beacon має 17 bytes, окремі CRC-16 common/gateway частин, GPS seconds modulo
2^32, крок 128 секунд. Common CRC обов'язкова; пошкоджена gateway CRC не заважає
стаціонарному вузлу синхронізуватися. **Маяк не має криптографічної автентифікації**.

Ping offset — AES128 з нульовим ключем від GPS time/DevAddr; підтримані всі
periodicity 0..7 (128..1 ping slots за beacon period). Розклад враховує reserved
2.120 s, slot quantum 30 ms, guard 3 s, ppm clock drift та двогодинний holdover.
`allowsTransaction` перевіряє повний TX/RX1/RX2 інтервал; затримана підготовка
передачі повторно перевіряється хостом. `nextTransaction` знаходить наступний
інтервал поза beacon reserved/guard. Після restart anchor **не відновлюється**:
потрібен новий маяк. Після закінчення holdover вузол повертається до Class A.

`beaconPhyConfig` задає SF9/BW125, sync 0x34, preamble10, normal IQ, implicit
header з фіксованими 17 bytes, без PHY CRC. LoRa IQ modem/Receiver працюють з цим
профілем. Ping downlink використовує звичайний downlink LoRaWAN PHY, MIC і FCnt.

Class C використовує RX2 frequency/rate як RXC, слухає до/після RX1, але RX1
має пріоритет. Host виключає TX та накладання прийому/передачі, перевіряє MIC,
лічильники й зберігає сесію до видачі payload. У конструкторі 1.1 підтримані
ResetInd/Conf, RekeyInd/Conf, DeviceModeInd/Conf; клас змінюється явною командою.
Class B використовує PingSlotInfoReq/Ans та uplink ClassB flag; device приймає
PingSlotChannelReq і BeaconFreqReq. ADRParamSetupReq/Ans підтримані для 1.1.

Додатковий self-test `ObriiLoRaWANBC11SelfTest`: 2076 незалежних Go-векторів
(1.1 data, FOpts erratum, ACK MIC context, root/session keys, join/rejoin, ping
offset), опублікований beacon CRC, усі periodicity, holdover/guard, split counters,
NbTrans з новим каналом, reboot/failure та beacon через implicit LoRa IQ.
Парні adapter/NodeService self-tests додатково перевіряють робочий scheduler і
обмін DATA/IQ. [Специфікація 1.1](https://lora-alliance.org/resource_hub/lorawan-specification-v1-1/),
[FOpts erratum](https://resources.lora-alliance.org/technical-specifications/fopts-encryption-usage-of-fcntdwn-errata-on-the-lorawan-l2-1-1-specification).

## Межі поточної версії

Це ядро кадрів/сесій і host-driven MAC/PHY helpers. Реальний Class A/B/C host повинен
з'єднати їх зі сховищем, чергами, hopping, годинником, RX-gating і RF-передачею.
Автономний scheduler/Network Server service, багатоканальний concentrator,
роумінг, backend NS/JS/AS, MQTT/Basics Station/TLS,
мультикаст/FUOTA, FSK/LR-FHSS та інші регіони тут не реалізовані.
Немає перевірки на фізичному gateway або живій TTN/ChirpStack-мережі.
Віртуальний обмін та байтова сумісність перевірені; реальний RF і часові допуски
потребують окремої кваліфікації. Конструктор підтримує модельовані DATA/IQ;
фізичний LoRaWAN RF залишається вимкненим без перевірених часових міток SDR.

Джерела: [LoRaWAN L2 1.0.4](https://lora-alliance.org/resource_hub/ts001-1-0-4-lorawan-l2-1-0-4-specification/),
[EU868 Regional Parameters](https://resources.lora-alliance.org/technical-specifications/rp002-1-0-4-regional-parameters),
[Semtech LoRaMac-node](https://github.com/Lora-net/LoRaMac-node/tree/cf779a46f52f9b22395a207c6853f3efb47f0e5b),
[незалежний протокольний еталон](https://github.com/brocaar/lorawan/tree/4bb09b3bdacba097680894e8d35dbcbc0d260c7e),
[Semtech packet-forwarder protocol](https://github.com/Lora-net/packet_forwarder/blob/master/PROTOCOL.TXT).
Ліцензійні повідомлення — `src/lorawan/NOTICE`, tiny-AES Unlicense та
`src/lorawan/tests/LICENSE.brocaar-lorawan`.
