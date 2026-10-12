#include "wirelesspacketdetails.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>

namespace Wireless {
bool packetDetails(const QByteArray &json, QJsonObject *layers, QByteArray *raw, QString *error) {
    if (error) error->clear();
    if (layers) *layers = {};
    if (raw) raw->clear();
    const auto fail = [error](const char *reason) { if (error) *error = QString::fromLatin1(reason); return false; };
    if (json.size() > 4 * 1024 * 1024) return fail("Packet detail size limit exceeded");
    QJsonParseError parsed;
    const auto document = QJsonDocument::fromJson(json, &parsed);
    if (parsed.error != QJsonParseError::NoError || !document.isArray() || document.array().size() != 1)
        return fail("TShark did not return exactly one packet");
    const auto result = document.array()[0].toObject().value("_source").toObject().value("layers").toObject();
    if (result.isEmpty()) return fail("No decoded packet layers");
    const auto rawField = result.value("frame_raw").toArray();
    const auto hex = rawField.isEmpty() ? QByteArray{} : rawField[0].toString().toLatin1();
    if (hex.size() > 2 * 65536) return fail("Raw packet size limit exceeded");
    for (char c : hex) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
        return fail("Invalid raw packet hex");
    if (hex.size() % 2) return fail("Odd raw packet hex length");
    if (layers) *layers = result;
    if (raw) *raw = QByteArray::fromHex(hex);
    return true;
}
}
WirelessPacketDetails::WirelessPacketDetails(bool uk, QWidget *parent)
    : QDialog(parent), process(new QProcess(this)), timeout(new QTimer(this)), ukrainian(uk) {
    setWindowTitle(uk ? QString::fromUtf8("Деталі пакета") : QStringLiteral("Packet details"));
    resize(800, 620);
    auto *layout = new QVBoxLayout(this);
    status = new QLabel(this); status->setWordWrap(true); layout->addWidget(status);
    auto *split = new QSplitter(Qt::Vertical, this);
    tree = new QTreeWidget(split); tree->setHeaderLabels(uk ? QStringList{QString::fromUtf8("Поле"), QString::fromUtf8("Значення")} : QStringList{"Field", "Value"});
    tree->setColumnWidth(0, 320); tree->setUniformRowHeights(true);
    bytes = new QPlainTextEdit(split); bytes->setReadOnly(true);
    bytes->setFont(QFont(QStringLiteral("monospace"), 9)); bytes->setLineWrapMode(QPlainTextEdit::NoWrap);
    split->setStretchFactor(0, 3); split->setStretchFactor(1, 1); layout->addWidget(split, 1);
    timeout->setSingleShot(true);
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000 | 0x00004000; });
#endif
    connect(timeout, &QTimer::timeout, this, [this] { timedOut = true; process->kill(); });
    connect(process, &QProcess::readyReadStandardOutput, this, [this] {
        output += process->readAllStandardOutput();
        if (output.size() > 4 * 1024 * 1024) { overflow = true; output.clear(); process->kill(); }
    });
    connect(process, &QProcess::readyReadStandardError, this, [this] { diagnostics += process->readAllStandardError(); diagnostics = diagnostics.right(2048); });
    connect(process, &QProcess::errorOccurred, this, [this] { timeout->stop(); status->setText(process->errorString()); });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int code, QProcess::ExitStatus exit) {
        timeout->stop();
        if (overflow || timedOut || code || exit != QProcess::NormalExit) {
            status->setText(ukrainian ? QString::fromUtf8("Не вдалося отримати деталі (помилка, ліміт або тайм-аут).") : QStringLiteral("Details unavailable (error, size limit or timeout)."));
            bytes->setPlainText(QString::fromUtf8(diagnostics)); return;
        }
        QJsonObject layers; QByteArray raw; QString error;
        if (!Wireless::packetDetails(output, &layers, &raw, &error)) { status->setText(error); return; }
        int nodes = 0;
        std::function<void(QTreeWidgetItem *, const QString &, const QJsonValue &, int)> add;
        add = [this, &add, &nodes](QTreeWidgetItem *parent, const QString &name, const QJsonValue &value, int depth) {
            if (++nodes > 4096 || depth > 24 || name.endsWith("_raw")) return;
            auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
            item->setText(0, name);
            if (value.isObject()) {
                const auto object = value.toObject();
                for (auto it = object.begin(); it != object.end() && nodes < 4096; ++it) add(item, it.key(), it.value(), depth + 1);
            } else if (value.isArray()) {
                const auto array = value.toArray();
                for (int i = 0; i < array.size() && nodes < 4096; ++i) add(item, QString::number(i), array[i], depth + 1);
            } else item->setText(1, value.isString() ? value.toString().left(32768) : QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));
        };
        for (auto it = layers.begin(); it != layers.end() && nodes < 4096; ++it) add(nullptr, it.key(), it.value(), 0);
        tree->expandToDepth(0);
        QString dump;
        for (int offset = 0; offset < raw.size(); offset += 16) {
            QString ascii;
            const auto line = raw.mid(offset, 16);
            for (uchar c : line) ascii += c >= 32 && c < 127 ? QChar(c) : QLatin1Char('.');
            dump += QStringLiteral("%1  %2  %3\n").arg(offset, 4, 16, QLatin1Char('0')).arg(QString::fromLatin1(line.toHex(' ')).leftJustified(47)).arg(ascii);
        }
        bytes->setPlainText(dump);
        status->setText(ukrainian ? QString::fromUtf8("Полів: %1; байт: %2. Декодування використовує вибраний профіль.").arg(nodes).arg(raw.size()) : QStringLiteral("Fields: %1; bytes: %2. Uses the selected profile.").arg(nodes).arg(raw.size()));
    });
}
WirelessPacketDetails::~WirelessPacketDetails() {
    if (process->state() != QProcess::NotRunning) { process->kill(); process->waitForFinished(500); }
}
void WirelessPacketDetails::load(const QString &program, const QString &capture, quint64 frame, const QString &profile, bool decrypt) {
    if (!frame || process->state() != QProcess::NotRunning) return;
    output.clear(); diagnostics.clear(); overflow = timedOut = false; tree->clear(); bytes->clear();
    QStringList args{"-n", "-r", capture, "-c", QString::number(frame), "-Y", QStringLiteral("frame.number == %1").arg(frame), "-T", "json", "-x"};
    if (!profile.isEmpty()) args << "-C" << profile;
    args << "-o" << (decrypt ? "wlan.enable_decryption:TRUE" : "wlan.enable_decryption:FALSE");
    status->setText(ukrainian ? QString::fromUtf8("Пакет %1: читання...").arg(frame) : QStringLiteral("Packet %1: reading...").arg(frame));
    process->start(program, args); timeout->start(15000);
}
