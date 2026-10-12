#include "ft8stationdialog.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <limits>

#ifndef FOBOSAPP_VERSION
#define FOBOSAPP_VERSION "unknown"
#endif

namespace {
QString defaultFt8LogPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("logs/ObriiSDR_FT8_Log.adi"));
}
}

void Ft8StationDialog::setOnlineStatus(const QString &status, bool error) {
    onlineStatusLabel->setText(status);
    onlineStatusLabel->setStyleSheet(error ? QStringLiteral("color:#d94b4b") : QString());
}

void Ft8StationDialog::sendWsjtDatagram(const QByteArray &packet) {
    if (!wsjtEnabledCheck->isChecked() || packet.isEmpty()) return;
    QHostAddress address;
    if (!address.setAddress(wsjtAddressEdit->text().trimmed())) {
        setOnlineStatus(text(QStringLiteral("WSJT-X UDP address must be numeric."),
                             QString::fromUtf8(u8"UDP-адреса WSJT-X має бути числовою.")), true);
        return;
    }
    if (wsjtSocket->writeDatagram(packet, address, static_cast<quint16>(wsjtPortSpin->value())) < 0) {
        setOnlineStatus(text(QStringLiteral("WSJT-X UDP error: "), QString::fromUtf8(u8"Помилка UDP WSJT-X: ")) + wsjtSocket->errorString(), true);
    }
}

void Ft8StationDialog::sendWsjtHeartbeatAndStatus() {
    if (!wsjtEnabledCheck->isChecked()) return;
    sendWsjtDatagram(buildWsjtHeartbeat(QStringLiteral("ObriiSDR"), QStringLiteral(FOBOSAPP_VERSION)));
    sendWsjtDatagram(buildWsjtStatus(QStringLiteral("ObriiSDR"),
        static_cast<quint64>(std::llround(dialFrequencySpin->value() * 1e6)),
        dxCallEdit->text().trimmed().toUpper(), receivedReportEdit->text().trimmed(),
        static_cast<quint32>(txAudioSpin->value()), static_cast<quint32>(txAudioSpin->value()),
        stationCallEdit->text().trimmed().toUpper(), stationGridEdit->text().trimmed().toUpper(),
        dxGridEdit->text().trimmed().toUpper(), QString()));
}

void Ft8StationDialog::queuePskReport(const Ft8MessageInfo &info, const QDateTime &utc,
                                      double rfFrequencyHz) {
    if (!pskEnabledCheck->isChecked() || !info.valid || info.senderCall.isEmpty() ||
        info.senderCall == stationCallEdit->text().trimmed().toUpper() || rfFrequencyHz <= 0.0 ||
        rfFrequencyHz > static_cast<double>((std::numeric_limits<quint32>::max)())) return;
    const qint64 now = utc.isValid() ? utc.toUTC().toMSecsSinceEpoch() : QDateTime::currentMSecsSinceEpoch();
    const QString key = info.senderCall + QLatin1Char('@') + QString::number(std::llround(rfFrequencyHz / 10.0));
    if (now - lastPskReportByStation.value(key, 0) < 15 * 60 * 1000) return;
    lastPskReportByStation.insert(key, now);
    Ft8ReceptionReport report;
    report.senderCall = info.senderCall;
    report.senderGrid = info.grid;
    report.frequencyHz = static_cast<quint32>(std::llround(rfFrequencyHz));
    report.epochSeconds = static_cast<quint32>((utc.isValid() ? utc : QDateTime::currentDateTimeUtc()).toSecsSinceEpoch());
    pendingPskReports.push_back(report);
    if (!pskFlushTimer->isActive()) {
        const int jitterMs = static_cast<int>(QRandomGenerator::global()->bounded(30001U));
        pskFlushTimer->start(300000 + jitterMs);
        setOnlineStatus(text(QStringLiteral("PSK Reporter reception queued; upload is rate-limited."),
                             QString::fromUtf8(u8"Звіт PSK Reporter поставлено в чергу з обмеженням частоти.")));
    }
}

void Ft8StationDialog::flushPskReports() {
    if (!pskEnabledCheck->isChecked() || pendingPskReports.isEmpty()) return;
    const QString call = stationCallEdit->text().trimmed().toUpper();
    if (call.isEmpty()) {
        setOnlineStatus(text(QStringLiteral("PSK Reporter needs your call sign."),
                             QString::fromUtf8(u8"Для PSK Reporter потрібен ваш позивний.")), true);
        return;
    }
    if (pskSocket->state() != QAbstractSocket::ConnectedState) {
        pskSocket->connectToHost(QStringLiteral("report.pskreporter.info"), 4739);
        pskFlushTimer->start(15000);
        return;
    }
    if (pskSessionId == 0) pskSessionId = QRandomGenerator::global()->generate();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool templates = pskPacketCount == 0 || now - pskLastTemplateMs >= 30 * 60 * 1000;
    const QVector<Ft8ReceptionReport> batch = pendingPskReports.mid(0, (std::min)(50, pendingPskReports.size()));
    const QByteArray packet = buildPskReporterPacket(call, stationGridEdit->text().trimmed().toUpper(),
        QStringLiteral("ObriiSDR 5.1"), antennaEdit->text().trimmed(), batch,
        ++pskSequence, pskSessionId, templates);
    if (packet.isEmpty() || pskSocket->write(packet) != packet.size()) {
        setOnlineStatus(text(QStringLiteral("PSK Reporter upload failed: "), QString::fromUtf8(u8"Помилка PSK Reporter: ")) + pskSocket->errorString(), true);
        pskFlushTimer->start(300000);
        return;
    }
    pendingPskReports.remove(0, batch.size());
    ++pskPacketCount;
    if (templates) pskLastTemplateMs = now;
    setOnlineStatus(text(QStringLiteral("PSK Reporter batch sent."), QString::fromUtf8(u8"Пакет PSK Reporter надіслано.")));
    if (!pendingPskReports.isEmpty()) {
        pskFlushTimer->start(300000 + static_cast<int>(QRandomGenerator::global()->bounded(30001U)));
    }
}
void Ft8StationDialog::uploadRecordToQrz(const Ft8QsoRecord &record) {
    const QString key = qrzKeyEdit->text().trimmed();
    if (key.isEmpty()) {
        setOnlineStatus(text(QStringLiteral("Enter a QRZ Logbook API key first."),
                             QString::fromUtf8(u8"Спочатку введіть ключ API QRZ Logbook.")), true);
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("KEY"), key);
    form.addQueryItem(QStringLiteral("ACTION"), QStringLiteral("INSERT"));
    form.addQueryItem(QStringLiteral("ADIF"), ft8AdifRecord(record));
    QNetworkRequest request(QUrl(QStringLiteral("https://logbook.qrz.com/api")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("User-Agent", "ObriiSDR/5.1 FT8 Station");
    QNetworkReply *reply = network->post(request, form.query(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray response = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError && response.contains("RESULT=OK");
        setOnlineStatus(ok
            ? text(QStringLiteral("QSO uploaded to QRZ."), QString::fromUtf8(u8"QSO надіслано до QRZ."))
            : text(QStringLiteral("QRZ upload failed: "), QString::fromUtf8(u8"Помилка QRZ: ")) +
                  (reply->error() == QNetworkReply::NoError ? QString::fromUtf8(response) : reply->errorString()), !ok);
        reply->deleteLater();
    });
}

void Ft8StationDialog::uploadSelectedToQrz() {
    const int row = logTable->currentRow();
    if (row < 0 || !logTable->item(row, 0)) {
        setOnlineStatus(text(QStringLiteral("Select a QSO in the log first."),
                             QString::fromUtf8(u8"Спочатку виберіть QSO в журналі.")), true);
        return;
    }
    const int source = logTable->item(row, 0)->data(Qt::UserRole).toInt();
    if (source >= 0 && source < logRecords.size()) uploadRecordToQrz(logRecords[source]);
}

void Ft8StationDialog::uploadLogToLotw() {
    if (tqslProcess->state() != QProcess::NotRunning) {
        setOnlineStatus(text(QStringLiteral("TQSL is already running."), QString::fromUtf8(u8"TQSL уже працює.")), true);
        return;
    }
    const QString program = tqslPathEdit->text().trimmed();
    const QString logPath = defaultFt8LogPath();
    if (!QFileInfo::exists(program) || !QFileInfo::exists(logPath)) {
        setOnlineStatus(text(QStringLiteral("Select TQSL and create at least one local QSO first."),
                             QString::fromUtf8(u8"Виберіть TQSL і спочатку створіть хоча б одне локальне QSO.")), true);
        return;
    }
    QStringList arguments{QStringLiteral("-x"), QStringLiteral("-d"), QStringLiteral("-a"),
                          QStringLiteral("compliant"), QStringLiteral("-u")};
    if (!tqslLocationEdit->text().trimmed().isEmpty()) {
        arguments << QStringLiteral("-l") << tqslLocationEdit->text().trimmed();
    }
    arguments << logPath;
    tqslProcess->setProcessChannelMode(QProcess::MergedChannels);
    tqslProcess->start(program, arguments);
    setOnlineStatus(text(QStringLiteral("TQSL is signing and uploading the local ADIF log..."),
                         QString::fromUtf8(u8"TQSL підписує та надсилає локальний журнал ADIF...")));
}
