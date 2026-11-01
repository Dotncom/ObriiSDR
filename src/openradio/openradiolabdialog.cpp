#include "openradiolabdialog.h"

#include "ax25codec.h"
#include "ax25afsk1200.h"
#include "ax25livedecoder.h"
#include "kisscodec.h"
#include "m17adapter.h"
#include "openradiobackend.h"
#include "openradioprotocol.h"
#include "virtualsignalframe.h"

#include <QComboBox>
#include <QAbstractItemView>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QSaveFile>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QVBoxLayout>

namespace {
QString compactHex(const QByteArray &bytes) {
    return QString::fromLatin1(bytes.toHex(' '));
}

QByteArray parseHexOrUtf8(const QString &text) {
    const QString trimmed = text.trimmed();
    const QString compact = QString(trimmed).remove(' ').remove('\n').remove('\r').remove('\t');
    if (!compact.isEmpty() && !(compact.size() % 2) &&
        compact.contains(QRegularExpression(QStringLiteral("^[0-9A-Fa-f]+$")))) {
        return QByteArray::fromHex(compact.toLatin1());
    }
    return trimmed.toUtf8();
}
}

OpenRadioLabDialog::OpenRadioLabDialog(bool useUkrainian, QWidget *parent)
    : QDialog(parent), ukrainian(useUkrainian) {
    setAttribute(Qt::WA_DeleteOnClose, true);
    setWindowTitle(text("Open amateur radio laboratory", "Лабораторія відкритого аматорського радіо"));
    resize(920, 620);
    auto *root = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    tabs->addTab(createOverviewPage(), text("Protocols", "Протоколи"));
    tabs->addTab(createAx25Page(), QStringLiteral("AX.25 / KISS"));
    tabs->addTab(createM17Page(), QStringLiteral("M17"));
    tabs->addTab(createTransportPage(), text("Virtual transport", "Віртуальний транспорт"));
    root->addWidget(tabs);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    root->addWidget(buttons);
}

QString OpenRadioLabDialog::text(const char *english, const char *ukrainianText) const {
    return ukrainian ? QString::fromUtf8(ukrainianText) : QString::fromUtf8(english);
}

QWidget *OpenRadioLabDialog::createOverviewPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *table = new QTableWidget(page);
    table->setColumnCount(7);
    table->setHorizontalHeaderLabels({text("Protocol", "Протокол"), text("State", "Стан"),
                                      text("Frame RX", "Кадр RX"), text("Frame TX", "Кадр TX"),
                                      text("Services", "Сервіси"), text("Engine", "Рушій"),
                                      text("Boundary", "Межа готовності")});
    const QList<OpenRadioProtocolDescriptor> protocols = OpenRadioProtocolCatalog::protocols();
    table->setRowCount(protocols.size());
    for (int row = 0; row < protocols.size(); ++row) {
        const OpenRadioProtocolDescriptor &protocol = protocols[row];
        QStringList services;
        if (protocol.packet) services << text("packet", "пакети");
        if (protocol.voice) services << text("voice", "голос");
        if (protocol.ipBearer) services << QStringLiteral("IP/TAP");
        const QString limitation = ukrainian
            ? QString::fromUtf8(u8"Див. документацію: твердження про RF з'явиться лише після апаратної перевірки.")
            : protocol.limitation;
        const QStringList cells{protocol.name,
            OpenRadioProtocolCatalog::readinessName(protocol.readiness, ukrainian),
            protocol.receive ? text("yes", "так") : QStringLiteral("-"),
            protocol.transmit ? text("yes", "так") : QStringLiteral("-"),
            services.join(QStringLiteral(", ")), protocol.engine, limitation};
        for (int column = 0; column < cells.size(); ++column)
            table->setItem(row, column, new QTableWidgetItem(cells[column]));
    }
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table);
    auto *note = new QLabel(text(
        "M17 framing/symbol generation, AX.25/KISS byte framing and offline AFSK1200 WAV are active here. "
        "External rows are extension points, not simulated decoders.",
        "Тут активні M17 framing/генерація символів, байтовий AX.25/KISS та офлайн AFSK1200 WAV. "
        "Зовнішні рядки є точками розширення, а не імітацією декодерів."), page);
    note->setWordWrap(true);
    layout->addWidget(note);
    return page;
}

QWidget *OpenRadioLabDialog::createAx25Page() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout();
    axSource = new QLineEdit(QStringLiteral("N0CALL-1"), page);
    axDestination = new QLineEdit(QStringLiteral("APRS"), page);
    axDigipeaters = new QLineEdit(page);
    axDigipeaters->setPlaceholderText(QStringLiteral("WIDE1-1,WIDE2-1"));
    axWireFormat = new QComboBox(page);
    axWireFormat->addItem(QStringLiteral("AX.25 UI (no FCS)"), 0);
    axWireFormat->addItem(QStringLiteral("AX.25 UI + FCS"), 1);
    axWireFormat->addItem(QStringLiteral("KISS data frame"), 2);
    form->addRow(text("Source:", "Джерело:"), axSource);
    form->addRow(text("Destination:", "Одержувач:"), axDestination);
    form->addRow(text("Digipeaters:", "Дігіпітери:"), axDigipeaters);
    form->addRow(text("Wire format:", "Формат:"), axWireFormat);
    layout->addLayout(form);
    axPayload = new QPlainTextEdit(page);
    axPayload->setPlaceholderText(text("UTF-8 information field", "Інформаційне поле UTF-8"));
    axPayload->setPlainText(QStringLiteral("Obrii SDR AX.25 test"));
    layout->addWidget(axPayload);
    auto *commands = new QHBoxLayout();
    auto *encode = new QPushButton(text("Encode", "Кодувати"), page);
    auto *decode = new QPushButton(text("Decode hex", "Декодувати HEX"), page);
    commands->addWidget(encode);
    commands->addWidget(decode);
    commands->addStretch();
    layout->addLayout(commands);
    auto *audioCommands = new QHBoxLayout();
    auto *exportWav = new QPushButton(text("Export AFSK1200 WAV", "Експорт AFSK1200 WAV"), page);
    auto *decodeWav = new QPushButton(text("Decode AFSK1200 WAV", "Декодувати AFSK1200 WAV"), page);
    audioCommands->addWidget(exportWav);
    audioCommands->addWidget(decodeWav);
    audioCommands->addStretch();
    layout->addLayout(audioCommands);
    auto *kissControls = new QHBoxLayout();
    kissPort = new QSpinBox(page);
    kissPort->setRange(1024, 65535);
    kissPort->setValue(8001);
    kissToggle = new QPushButton(text("Start live KISS RX", "Запустити живий KISS RX"), page);
    kissControls->addWidget(new QLabel(QStringLiteral("TCP KISS:"), page));
    kissControls->addWidget(kissPort);
    kissControls->addWidget(kissToggle);
    kissControls->addStretch();
    layout->addLayout(kissControls);
    kissStatus = new QLabel(text("RX only; select FM audio and start receiver", "Лише RX; оберіть FM-аудіо й запустіть приймач"), page);
    layout->addWidget(kissStatus);
    axHex = new QPlainTextEdit(page);
    axHex->setPlaceholderText(text("Encoded bytes", "Закодовані байти"));
    layout->addWidget(axHex);
    connect(encode, &QPushButton::clicked, this, &OpenRadioLabDialog::encodeAx25);
    connect(decode, &QPushButton::clicked, this, &OpenRadioLabDialog::decodeAx25);
    connect(exportWav, &QPushButton::clicked, this, &OpenRadioLabDialog::exportAx25Wav);
    connect(decodeWav, &QPushButton::clicked, this, &OpenRadioLabDialog::decodeAx25Wav);
    connect(kissToggle, &QPushButton::clicked, this, &OpenRadioLabDialog::toggleKissReceiver);
    return page;
}

void OpenRadioLabDialog::submitAfskAudioFrame(const QByteArray &pcmData, int sampleRate) {
    if (axLiveDecoder && axLiveDecoder->isRunning())
        axLiveDecoder->submitPcm16(pcmData, sampleRate);
}

void OpenRadioLabDialog::toggleKissReceiver() {
    if (kissServer) {
        kissServer->close();
        kissServer->deleteLater();
        kissServer = nullptr;
        axLiveDecoder->stop();
        axLiveDecoder->deleteLater();
        axLiveDecoder = nullptr;
        kissPort->setEnabled(true);
        kissToggle->setText(text("Start live KISS RX", "Запустити живий KISS RX"));
        kissStatus->setText(text("KISS RX stopped", "KISS RX зупинено"));
        return;
    }
    auto *server = new QTcpServer(this);
    if (!server->listen(QHostAddress::LocalHost, static_cast<quint16>(kissPort->value()))) {
        QMessageBox::warning(this, windowTitle(), server->errorString());
        server->deleteLater();
        return;
    }
    kissServer = server;
    axLiveDecoder = new Ax25LiveDecoder(this);
    connect(axLiveDecoder, &Ax25LiveDecoder::frameDecoded, this, [this](const QByteArray &ax25) {
        if (!kissServer) return;
        KissFrame kiss;
        kiss.payload = ax25;
        const QByteArray wire = KissCodec::encode(kiss);
        int sent = 0;
        for (QTcpSocket *client : kissServer->findChildren<QTcpSocket *>()) {
            if (client->state() != QAbstractSocket::ConnectedState) continue;
            if (client->bytesToWrite() > 256 * 1024) { client->disconnectFromHost(); continue; }
            if (client->write(wire) == wire.size()) ++sent;
        }
        Ax25UiFrame decoded;
        QString error;
        const QString calls = Ax25Codec::decodeUiFrame(ax25, &decoded, false, &error)
            ? Ax25Codec::addressText(decoded.source) + QStringLiteral(" > ") +
              Ax25Codec::addressText(decoded.destination)
            : QStringLiteral("AX.25");
        kissStatus->setText(QStringLiteral("%1 | %2 bytes | KISS clients: %3")
                                .arg(calls).arg(ax25.size()).arg(sent));
    });
    connect(server, &QTcpServer::newConnection, this, [this, server] {
        while (QTcpSocket *client = server->nextPendingConnection()) {
            QObject::connect(client, &QTcpSocket::readyRead, client, [client] {
                client->readAll(); // TX injection is not enabled until linked to an RF/virtual TX path.
            });
            QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        }
    });
    axLiveDecoder->start();
    kissPort->setEnabled(false);
    kissToggle->setText(text("Stop live KISS RX", "Зупинити живий KISS RX"));
    kissStatus->setText(QStringLiteral("127.0.0.1:%1 | RX only").arg(kissPort->value()));
}

QWidget *OpenRadioLabDialog::createM17Page() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout();
    m17Source = new QLineEdit(QStringLiteral("N0CALL"), page);
    m17Destination = new QLineEdit(QStringLiteral("@ALL"), page);
    m17Can = new QSpinBox(page);
    m17Can->setRange(0, 15);
    m17PayloadMode = new QComboBox(page);
    m17PayloadMode->addItem(QStringLiteral("SMS text (0x05 + UTF-8 + NUL)"), 0);
    m17PayloadMode->addItem(QStringLiteral("Raw bytes"), 1);
    form->addRow(text("Source:", "Джерело:"), m17Source);
    form->addRow(text("Destination:", "Одержувач:"), m17Destination);
    form->addRow(QStringLiteral("CAN:"), m17Can);
    form->addRow(text("Packet application:", "Застосування пакета:"), m17PayloadMode);
    layout->addLayout(form);
    m17Payload = new QPlainTextEdit(page);
    m17Payload->setPlainText(QStringLiteral("Obrii SDR M17 packet test"));
    layout->addWidget(m17Payload);
    auto *generate = new QPushButton(text("Generate packet symbols", "Згенерувати символи пакета"), page);
    auto *saveSymbols = new QPushButton(text("Save symbols", "Зберегти символи"), page);
    auto *openSymbols = new QPushButton(text("Open symbols", "Відкрити символи"), page);
    auto *symbolCommands = new QHBoxLayout();
    symbolCommands->addWidget(generate);
    symbolCommands->addWidget(saveSymbols);
    symbolCommands->addWidget(openSymbols);
    layout->addLayout(symbolCommands);
    m17Result = new QPlainTextEdit(page);
    m17Result->setReadOnly(true);
    layout->addWidget(m17Result);
    auto *note = new QLabel(text(
        "The output is a deterministic libm17 4FSK symbol stream. It is not sent to RF and is not labelled as a validated audio/Codec 2 stream.",
        "Результат є детермінованим потоком 4FSK-символів libm17. Він не передається в RF і не позначається як перевірений аудіо/Codec 2 потік."), page);
    note->setWordWrap(true);
    layout->addWidget(note);
    connect(generate, &QPushButton::clicked, this, &OpenRadioLabDialog::generateM17);
    connect(saveSymbols, &QPushButton::clicked, this, &OpenRadioLabDialog::saveM17Symbols);
    connect(openSymbols, &QPushButton::clicked, this, &OpenRadioLabDialog::openM17Symbols);
    return page;
}

QWidget *OpenRadioLabDialog::createTransportPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout();
    transportStream = new QLineEdit(QStringLiteral("lab/open-radio/1"), page);
    transportProtocol = new QLineEdit(QStringLiteral("ax25-kiss"), page);
    transportPayloadType = new QComboBox(page);
    transportPayloadType->addItem(QStringLiteral("Protocol frame"), static_cast<int>(VirtualSignalPayloadType::ProtocolFrame));
    transportPayloadType->addItem(QStringLiteral("Int8 symbols"), static_cast<int>(VirtualSignalPayloadType::SymbolsInt8));
    transportPayloadType->addItem(QStringLiteral("Complex float32 IQ"), static_cast<int>(VirtualSignalPayloadType::ComplexFloat32));
    form->addRow(text("Stream ID:", "ID потоку:"), transportStream);
    form->addRow(text("Protocol ID:", "ID протоколу:"), transportProtocol);
    form->addRow(text("Payload type:", "Тип даних:"), transportPayloadType);
    layout->addLayout(form);
    transportPayload = new QPlainTextEdit(page);
    transportPayload->setPlaceholderText(text("HEX bytes or UTF-8 text", "HEX-байти або текст UTF-8"));
    layout->addWidget(transportPayload);
    auto *build = new QPushButton(text("Build versioned envelope", "Створити версіонований envelope"), page);
    layout->addWidget(build);
    transportResult = new QPlainTextEdit(page);
    transportResult->setReadOnly(true);
    layout->addWidget(transportResult);
    auto *note = new QLabel(text(
        "This envelope is the local/network boundary reserved for the future virtual signal laboratory. Building it performs no network or RF operation.",
        "Цей envelope є локальною/мережевою межею для майбутньої віртуальної лабораторії сигналу. Його створення не запускає мережу чи RF."), page);
    note->setWordWrap(true);
    layout->addWidget(note);
    connect(build, &QPushButton::clicked, this, &OpenRadioLabDialog::buildTransportEnvelope);
    return page;
}

bool OpenRadioLabDialog::buildAx25Frame(Ax25UiFrame *frame, QString *error) const {
    if (!frame) return false;
    *frame = Ax25UiFrame{};
    if (!Ax25Codec::parseAddress(axSource->text(), &frame->source, error) ||
        !Ax25Codec::parseAddress(axDestination->text(), &frame->destination, error)) return false;
    const QStringList digipeaters = axDigipeaters->text().split(',', Qt::SkipEmptyParts);
    for (const QString &textValue : digipeaters) {
        Ax25Address address;
        if (!Ax25Codec::parseAddress(textValue, &address, error)) return false;
        frame->digipeaters.push_back(address);
    }
    frame->information = axPayload->toPlainText().toUtf8();
    return true;
}

void OpenRadioLabDialog::encodeAx25() {
    Ax25UiFrame frame;
    QString error;
    if (!buildAx25Frame(&frame, &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    const int format = axWireFormat->currentData().toInt();
    QByteArray bytes = Ax25Codec::encodeUiFrame(frame, format == 1, &error);
    if (bytes.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    if (format == 2) bytes = KissCodec::encode({0, 0, bytes}, &error);
    axHex->setPlainText(compactHex(bytes));
    transportProtocol->setText(QStringLiteral("ax25-kiss"));
    transportPayload->setPlainText(compactHex(bytes));
}

void OpenRadioLabDialog::exportAx25Wav() {
    Ax25UiFrame frame;
    QString error;
    if (!buildAx25Frame(&frame, &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    const QByteArray bytes = Ax25Codec::encodeUiFrame(frame, false, &error);
    if (bytes.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    const QVector<float> audio = Ax25Afsk1200::modulate(bytes, 48000, 64, &error);
    const QByteArray wav = Ax25Afsk1200::encodePcm16Wav(audio, 48000, &error);
    if (wav.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this,
        text("Export AX.25 AFSK1200", "Експорт AX.25 AFSK1200"),
        QStringLiteral("ax25-afsk1200.wav"), QStringLiteral("WAV (*.wav)"));
    if (path.isEmpty()) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(wav) != wav.size() || !file.commit()) {
        QMessageBox::warning(this, windowTitle(), file.errorString());
        return;
    }
}

void OpenRadioLabDialog::decodeAx25Wav() {
    const QString path = QFileDialog::getOpenFileName(this,
        text("Decode AX.25 AFSK1200", "Декодувати AX.25 AFSK1200"),
        QString(), QStringLiteral("WAV (*.wav)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 192000 * 2 * 2 * 30 + 4096) {
        QMessageBox::warning(this, windowTitle(),
            text("Cannot open WAV or file exceeds 30 seconds of PCM16 audio.",
                 "Не вдалося відкрити WAV або файл довший за 30 секунд PCM16-аудіо."));
        return;
    }
    QVector<float> audio;
    int sampleRate = 0;
    QString error;
    if (!Ax25Afsk1200::decodePcm16Wav(file.readAll(), &audio, &sampleRate, &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    const QList<QByteArray> frames = Ax25Afsk1200::demodulate(audio, sampleRate, &error);
    if (frames.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    axWireFormat->setCurrentIndex(axWireFormat->findData(1));
    axHex->setPlainText(compactHex(frames.first()));
    decodeAx25();
}

void OpenRadioLabDialog::decodeAx25() {
    QString compact = axHex->toPlainText();
    compact.remove(QRegularExpression(QStringLiteral("\\s")));
    QByteArray bytes = QByteArray::fromHex(compact.toLatin1());
    QString error;
    const int format = axWireFormat->currentData().toInt();
    if (format == 2) {
        KissFrame kiss;
        if (!KissCodec::decode(bytes, &kiss, &error)) {
            QMessageBox::warning(this, windowTitle(), error);
            return;
        }
        bytes = kiss.payload;
    }
    Ax25UiFrame frame;
    if (!Ax25Codec::decodeUiFrame(bytes, &frame, format == 1, &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    axSource->setText(Ax25Codec::addressText(frame.source));
    axDestination->setText(Ax25Codec::addressText(frame.destination));
    QStringList digipeaters;
    for (const Ax25Address &address : frame.digipeaters)
        digipeaters << Ax25Codec::addressText(address);
    axDigipeaters->setText(digipeaters.join(','));
    axPayload->setPlainText(QString::fromUtf8(frame.information));
}

void OpenRadioLabDialog::generateM17() {
    QString error;
    QByteArray payload = m17Payload->toPlainText().toUtf8();
    if (m17PayloadMode->currentData().toInt() == 0) {
        payload.prepend(static_cast<char>(0x05));
        payload.append('\0');
    } else {
        payload = parseHexOrUtf8(m17Payload->toPlainText());
    }
    const M17PacketSymbols result = M17Adapter::buildPacket(
        m17Source->text(), m17Destination->text(), payload,
        m17Can->value(), &error);
    if (result.symbols.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    QByteArray symbolBytes;
    symbolBytes.resize(result.symbols.size());
    for (int i = 0; i < result.symbols.size(); ++i) symbolBytes[i] = static_cast<char>(result.symbols[i]);
    m17GeneratedSymbols = symbolBytes;
    m17Result->setPlainText(QStringLiteral("libm17 %1\nLSF: %2\nPacket frames: %3\nSymbols: %4\nDuration: %5 ms\nFirst symbols: %6")
        .arg(M17Adapter::libraryVersion(), compactHex(result.linkSetupFrame))
        .arg(result.packetFrameCount).arg(result.symbols.size())
        .arg(result.symbols.size() * 1000.0 / result.symbolRate, 0, 'f', 1)
        .arg(compactHex(symbolBytes.left(96))));
    transportProtocol->setText(QStringLiteral("m17"));
    transportPayloadType->setCurrentIndex(1);
    transportPayload->setPlainText(compactHex(symbolBytes));
}

void OpenRadioLabDialog::saveM17Symbols() {
    if (m17GeneratedSymbols.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), text("Generate a packet first", "Спочатку згенеруйте пакет"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, windowTitle(), QStringLiteral("m17-packet.m17sym"),
                                                       QStringLiteral("M17 symbols (*.m17sym)"));
    if (path.isEmpty()) return;
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(m17GeneratedSymbols) != m17GeneratedSymbols.size() ||
        !output.commit())
        QMessageBox::warning(this, windowTitle(), output.errorString());
}

void OpenRadioLabDialog::openM17Symbols() {
    const QString path = QFileDialog::getOpenFileName(this, windowTitle(), QString(),
                                                       QStringLiteral("M17 symbols (*.m17sym)"));
    if (path.isEmpty()) return;
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly) || input.size() > 36 * 192) {
        QMessageBox::warning(this, windowTitle(), text("Cannot read M17 symbol file", "Не вдається прочитати файл символів M17"));
        return;
    }
    M17DecodedPacket packet;
    QString error;
    if (!M17Adapter::decodePacketSymbols(input.readAll(), &packet, &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    m17Result->setPlainText(QStringLiteral("M17 packet CRC OK\nSource: %1\nDestination: %2\nCAN: %3\nFrames: %4\nPayload HEX: %5\nPayload text: %6")
        .arg(packet.source, packet.destination).arg(packet.channelAccessNumber)
        .arg(packet.packetFrameCount).arg(compactHex(packet.payload), QString::fromUtf8(packet.payload)));
}

void OpenRadioLabDialog::buildTransportEnvelope() {
    VirtualSignalFrame frame;
    frame.sequence = 0;
    frame.timestampNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
    frame.streamId = transportStream->text().trimmed();
    frame.protocolId = transportProtocol->text().trimmed();
    frame.payloadType = static_cast<VirtualSignalPayloadType>(transportPayloadType->currentData().toInt());
    frame.symbolRate = frame.payloadType == VirtualSignalPayloadType::SymbolsInt8 ? 4800U : 0U;
    frame.payload = parseHexOrUtf8(transportPayload->toPlainText());
    frame.metadata.insert(QStringLiteral("origin"), QStringLiteral("open-radio-lab"));
    QString error;
    const QByteArray wire = VirtualSignalFrameCodec::encode(frame, &error);
    VirtualSignalFrame verified;
    if (wire.isEmpty() || !VirtualSignalFrameCodec::decode(wire, &verified, &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    transportResult->setPlainText(QStringLiteral("Envelope v%1\nWire bytes: %2\nPayload bytes: %3\nStream: %4\nProtocol: %5\nSHA-ready HEX:\n%6")
        .arg(VirtualSignalFrameCodec::Version).arg(wire.size()).arg(verified.payload.size())
        .arg(verified.streamId, verified.protocolId, compactHex(wire)));
}
