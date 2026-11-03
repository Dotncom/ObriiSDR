# Meshtastic PKI та віддалене адміністрування

Додано до `ObriiMeshtastic` як окремі API: `meshpki.h`, `meshadmin.h`.
Вони не відкривають SDR, не запускають shell, не підключаються до пристроїв
і не змінюють конструктор. Бібліотека готова до підключення до event loop
програми; застосування налаштувань до її власного обладнання/процесів виконує
явний host callback.

## PKI TX/RX

- X25519 ECDH → SHA256(shared secret) → AES256-CCM.
- Повний Meshtastic PKI wire format: channel hash0, unicast, ciphertext,
  tag8 та extra nonce4. CCM nonce13: `LE32(packetId) | extra4 | LE32(from) | 00`.
- Максимальний серіалізований Data plaintext — 227 байтів; чистий текст без
  додаткових полів — до222 байтів. Враховується overhead12 до PHY MTU255.
- Генерація приватних ключів і nonce використовує системний CSPRNG.
- Windows10+: нативний CNG/bcrypt для X25519, SHA256, HMAC, RNG та AES-CCM;
  додаткові crypto DLL не потрібні. Інші платформи: OpenSSL Crypto1.1.1+
  (рекомендовано підтримувану версію3.x; потрібні development headers).
- Примітиви elliptic curve і CCM не реалізовані вручну.
- `PkiContext::pinPeer` явно прив'язує node ID до public key. Заміна існуючого
  pin відхиляється; NodeInfo не може її виконати. Низькопорядкові X25519 ключі
  та all-zero shared secrets відхиляються.
- `Node::sendPki()` ніколи не переходить на PSK/plaintext при відсутності ключа.
  RX із неправильною CCM tag не доставляє plaintext і не заповнює history
  адресата перед наступним валідним пакетом з тим самим ID.
- Проміжні вузли можуть ретранслювати opaque PKI без приватних ключів;
  керується `NodeConfig::relayPki`.
- Events містять `security`, `authenticated`, `senderPublicKey`. PKI Message
  автентифікований відносно явно встановленого pin; PSK NodeInfo цим не стає.
- Pairwise ACK proof: HMAC-SHA256 із shared AES key над
  `"ack" | LE32(from) | LE32(to) | LE32(requestId) | RoutingWithoutProof`, перші8 байтів.
  Непідписаний PSK ACK/NAK не може зняти pending PKI передачу. Відповідь із
  коректним proof або повністю PKI-шифрована відповідь може її підтвердити.

Admin getter response з want_ack також отримує receipt ACK. Сама admin-команда
не підтверджується до перевірки прав і виконання сервісом.

PKI-контекст не є PKI certificate authority. Довіра до конкретного public key
встановлюється хостом. Не створювати authenticated Event із поля стороннього
JSON/неперевіреного API — використовувати результат RX цього Node.

## Ідентичність та життєвий цикл

```cpp
auto crypto = platformCrypto();
// Для симуляції можна generatePrivateKey(*crypto).
// Для постійного вузла: завантажити раніше збережений приватний ключ.
auto pki = std::make_shared<PkiContext>(crypto, localPrivateKey);
pki->pinPeer(remoteNodeId, verifiedRemotePublicKey);

NodeConfig config;
config.nodeId = localNodeId;
config.pki = pki;
Node node(config, durableNextPacketId);
node.sendPki(textMessage("private message"), remoteNodeId, true, nowMs);
```

Приватний ключ зберігає host keystore. Модуль не записує його в presets,
JSON, stdout чи журнали. Не генерувати нову ідентичність на кожне відкриття UI.
Packet ID залишається унікальним для sender/key; діє той самий контракт
durable sequence, що описаний у `meshtastic_core.md`. Retries повторюють
первісний ciphertext/ID; нові повідомлення отримують новий ID і random extra nonce.

Для зміни pin: завершити/скасувати pending роботу, очистити admin session через
`AdminClient::forgetPeer(node, peer, now)`, явно видалити старий pin, встановити
перевірений новий. Об'єкти не thread-safe, усі операції — в одному worker.

При PKI-enabled Node активний PSK-канал із hash0 відхиляється як неоднозначний.
Інші PSK-канали залишаються доступними для звичайних broadcast повідомлень.

## Admin codec та клієнт

AdminPort6; protobuf AdminMessage із точними field numbers, oneof presence
(у тому числі ConfigType0), session_passkey101. Codec покриває поточні
стандартні get/set owner/channel/config/module config, metadata/connection,
UI, fixed position, canned/ringtone, begin/commit, reboot/shutdown, reset,
backup/restore, node operations та інші визначені `AdminTag`.

`adminGetChannel(index)` приймає zero-based0..7 та передає wire index+1.
`adminGetConfig(AdminConfigType::LoRa)` формує правильний config selector.
`adminSigned(AdminTag::Reboot,-1)` кодує скасування reboot як int32.
`AdminChannel` має typed index/role/name/PSK і зберігає raw nested settings.
Порожнє wire channel name зберігається; для runtime Channel його треба
розв'язати в ім'я preset на рівні конфігурації.

Config/ModuleConfig передаються як versioned protobuf envelopes, без копіювання
сотень generated firmware structs. Є `configEnvelope`, `configEnvelopeType`,
`replaceProtoVarint`, `replaceProtoBytes`; невідомі поля зберігаються.
Глибоку перевірку підтримуваних hardware settings здійснює host commit validator.

```cpp
AdminClient admin(pki);
// Спочатку getter: його відповідь надасть короткочасний session passkey.
auto request = admin.request(node, remoteNodeId,
    adminGetConfig(AdminConfigType::SessionKey), nowMs);

// Передавати ВСІ Node events, включно з Sent/TxFailed/TimedOut:
for (const auto &event : node.takeEvents()) {
    if (auto result = admin.handle(event, nowMs)) {
        // success/error/response; можна оновити UI запиту.
    }
}
// Після успішного getter:
admin.request(node, remoteNodeId, adminSigned(AdminTag::Reboot,5), nowMs);
```

Клієнт зіставляє remote ID, local destination, pinned key, request ID,
response variant і Config/Channel selector. Непрохана або невідповідна відповідь
не встановлює session token. Токен зберігається лише в RAM, максимум150с
після відповіді; server error36 очищає його. Для мутації без сесії потрібно
знову виконати getter, автоматичного downgrade/legacy admin немає.

Таймер відповіді запускається після actual Sent event (default120с), а не
при постановці в TX-чергу. `expire(node,now)` скасовує прострочену роботу;
`forgetPeer(node,peer,now)` очищає сесію та queued/pending запити. Скасування
не може відкликати команду, яка вже фізично передана й виконана адресатом.

## Admin server та host adapter

```cpp
AdminPolicy policy;
policy.localNodeId = localNodeId;
policy.adminKeys = {verifiedAdministratorPublicKey};
AdminServer service(policy, initialState, crypto,
    commitSettingsAtomically,       // bool(const AdminState&)
    executeAuthorizedAction);       // Routing.Error(const AdminMessage&)

for (const auto &event : node.takeEvents()) {
    if (auto result = service.handle(event, nowMs)) {
        if (pki->peerKey(result->to))
            node.sendPki(result->reply, result->to, false, nowMs);
    }
}
```

Allowlist містить максимум3 admin public keys, як у firmware. Потрібен
PKI-authenticated Message, адресований цьому вузлу, з дозволеним pinned key.
PSK admin відхиляється (Routing.Error33), інший PKI key —37. Legacy admin
channel не надає прав. Getters видають криптографічно випадковий passkey8;
усі мутації перевіряють його, прив'язку до peer/key та TTL до300с.
Відмова RNG не створює порожньої сесії та не дозволяє мутацію.

Вбудований AdminState обслуговує owner, config/module config, channels,
metadata/connection/UI, fixed position, canned messages та ringtone.
Setter config/owner/channel у транзакції змінює staged snapshot.
Commit викликає host callback один раз і публікує новий стан тільки після
успіху. Відмова callback зберігає попередній live state; прострочення сесії
скасовує staged transaction. Другий адміністратор не може перехопити її.

Getter SecurityConfig прибирає device private_key. Read/edit/write без цього
поля зберігає попередній ключ; явне поле лишається окремою операцією host
key-management adapter. Зміни PKI identity/allowlist/PHY хост застосовує так,
щоб queued reply старої сесії було передано до перебудови відповідних об'єктів.

Reboot/shutdown/reset/backup та інші апаратні або файлові дії передаються
тільки явному Action handler. Без нього повертається NoInterface4,
успіх не імітується. Callback повертає0 лише після прийняття/застосування
дії у своїй системі; автоматичного shell/PowerShell/SSH тут немає.
Local-only display mirroring та lockdown provisioning не виконуються через mesh.
Для керування фізичним Meshtastic-пристроєм достатньо клієнтської частини:
команду виконує його firmware.

Replay cache живе всередині сесії: sender/key/request ID/hash payload та
попередній результат. Повтор повертає cached response, не застосовує зміни
знову. При переповненні повертається RateLimit38, живі ID не витісняються.
Node повторно передає duplicate AdminPort повідомлення сервісу, щоб після
втрати response його можна було відтворити; це не повтор виконання команди.
Старі tokens після перезапуску не відновлюються.

`AdminResult.applied` означає прийняту операцію (у транзакції — staging),
а не обов'язкове завершення reboot чи фізичного переналаштування.
Не змішувати Radio Sent/ACK зі state-change result у UI. Якщо TX-черга
відповіді повна, host зберігає її до відправки або дозволяє клієнту повторити
запит, щоб replay cache повторно сформував відповідь.

## Збірка та перевірка

```powershell
tools/build_meshtastic_core.ps1 -Test
build/meshtastic/Release/ObriiMeshtasticPkiTest.exe
```

Поточна перевірена платформа — Windows10/MSVC2022/CNG. Backend OpenSSL
написаний для Linux/macOS, але на цих ОС у цьому середовищі не запускався.
При ручному лінкуванні Windows static lib потрібен `bcrypt.lib`; CMake target
`ObriiMeshtastic` передає залежність автоматично. Headers та notices включені
в CMake install. PSK offline tool залишається PSK-утилітою; PKI/admin доступні
через C++ API та окремий selftest.

Тести: RFC7748 public/shared keys; SHA256; незалежні PyCryptodome CCM/full
radio/ACK-proof vectors; zero-length CCM; слабкі ключі; підміна кожного байта
cipher/tag/nonce; ACK downgrade; opaque relay; pin substitution; PKI MTU;
get/set owner/config; session expiry та RNG failure; unauthorized admin;
commit/rollback; replay/lost response; private-key redaction; response
correlation; 5000 malformed admin payloads; PKI admin handshake і зміна
налаштувань A–B–C–B–A через справжній LoRa IQ-модулятор/демодулятор.

`tools/generate_meshtastic_pki_vectors.py` відтворює незалежні fixtures через
PyCryptodome (потрібен лише для регенерації). Усі fixture private keys —
публічні RFC7748 тестові значення. Фізичні пристрої та ефір у цих тестах
не використовуються; реальну interoperabilty ще потрібно перевірити.
Release, варіант без LoRa та AddressSanitizer проходять обидва test targets.

Поведінку ACK конкретної firmware слід перевірити окремо: peer, що повертає
тільки unauthenticated PSK ACK без ack_proof, не отримає статус підтвердженого
PKI setter у цьому клієнті. Це не доводить, що команда не виконана; такий стан
потребує PKI readback/перевірки адресата. Автоматичного послаблення перевірки
автентичності для такого ACK немає.

## Первинні джерела

- [Meshtastic CryptoEngine.cpp](https://github.com/meshtastic/firmware/blob/master/src/mesh/CryptoEngine.cpp): PKI ECDH/hash/CCM/nonce.
- [admin.proto](https://github.com/meshtastic/protobufs/blob/master/meshtastic/admin.proto),
  [AdminModule.cpp](https://github.com/meshtastic/firmware/blob/master/src/modules/AdminModule.cpp): admin variants, authorization та session passkey.
- [mesh.proto](https://github.com/meshtastic/protobufs/blob/master/meshtastic/mesh.proto): Routing.ack_proof binding.
- [Microsoft CNG named curves](https://learn.microsoft.com/en-us/windows/win32/seccng/cng-named-elliptic-curves),
  [OpenSSL EVP](https://docs.openssl.org/3.0/man3/EVP_EncryptInit/).

Не додавалися XEdDSA broadcast signatures, AEAD для групових каналів,
повний SDK ToRadio/FromRadio, firmware update service чи learned next-hop
routing. Їх не слід плутати з реалізованими PKI direct messages і remote admin.
