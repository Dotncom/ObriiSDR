#include "main.h"

#include "appconstants.h"
#include "diagnosticlogging.h"
#include "gnssqthhelpers.h"
#include "gnssserialutils.h"
#include "presethelpers.h"
#include "qthlocator.h"
#include "qthmapwidget.h"
#include "samplefileutils.h"
#include "tuningutils.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHostInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace {

QStringList currentSerialPortNames(const QList<QSerialPortInfo> &ports) {
    QStringList names;
    names.reserve(ports.size());
    for (const QSerialPortInfo &port : ports) {
        if (!port.portName().isEmpty()) {
            names.append(port.portName());
        }
    }
    return names;
}

bool serialPortNameExists(const QList<QSerialPortInfo> &ports, const QString &name) {
    for (const QSerialPortInfo &port : ports) {
        if (port.portName().compare(name, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool isLikelyGnssSerialPort(const QSerialPortInfo &port) {
    if (port.hasVendorIdentifier() && port.vendorIdentifier() == 0x1546) {
        return true; // u-blox GNSS receivers.
    }

    const QString haystack = QStringList {
                                 port.portName(),
                                 port.description(),
                                 port.manufacturer(),
                                 port.serialNumber(),
                                 port.systemLocation()
                             }.join(QLatin1Char(' ')).toLower();
    return haystack.contains(QStringLiteral("u-blox")) ||
           haystack.contains(QStringLiteral("ublox")) ||
           haystack.contains(QStringLiteral("gnss")) ||
           haystack.contains(QStringLiteral("gps")) ||
           haystack.contains(QStringLiteral("nmea"));
}

QString preferredGnssSerialPortName(const QString &requested,
                                    const QList<QSerialPortInfo> &ports) {
    const QString trimmed = requested.trimmed();
    if (!trimmed.isEmpty() && serialPortNameExists(ports, trimmed)) {
        return trimmed;
    }

    for (const QSerialPortInfo &port : ports) {
        if (isLikelyGnssSerialPort(port)) {
            return port.portName();
        }
    }

    if (trimmed.isEmpty() && ports.size() == 1) {
        return ports.constFirst().portName();
    }

    return trimmed;
}

void refreshGnssSerialCombo(QComboBox *combo,
                            const QList<QSerialPortInfo> &ports,
                            const QString &selectedPort) {
    if (!combo) {
        return;
    }

    QSignalBlocker blocker(combo);
    combo->clear();
    const QStringList names = currentSerialPortNames(ports);
    for (const QString &name : names) {
        combo->addItem(name);
    }
    if (!selectedPort.isEmpty() &&
        combo->findText(selectedPort, Qt::MatchFixedString) < 0) {
        combo->insertItem(0, selectedPort);
    }
    if (!selectedPort.isEmpty()) {
        combo->setCurrentText(selectedPort);
    } else if (combo->count() > 0) {
        combo->setCurrentIndex(0);
    }
}

bool normalizeM8nUbxConstellationSelection(bool &useGps,
                                           bool &useSbas,
                                           bool &useGalileo,
                                           bool &useBeidou,
                                           bool &useQzss,
                                           bool &useGlonass,
                                           QStringList *notes) {
    const int primaryCount = (useGps ? 1 : 0) +
                             (useGlonass ? 1 : 0) +
                             (useGalileo ? 1 : 0) +
                             (useBeidou ? 1 : 0);
    if (primaryCount <= 2 && useGps) {
        return false;
    }

    const bool keepGlonass = useGlonass;
    const bool keepGalileo = !keepGlonass && useGalileo;
    const bool keepBeidou = !keepGlonass && !keepGalileo && useBeidou;

    useGps = true;
    useGlonass = keepGlonass;
    useGalileo = keepGalileo;
    useBeidou = keepBeidou;
    if (!useGlonass && !useGalileo && !useBeidou) {
        useGlonass = true;
    }

    if (notes) {
        notes->append(QStringLiteral("M8N safe: GPS + %1, SBAS %2, QZSS %3")
                          .arg(useGlonass ? QStringLiteral("GLONASS")
                                           : useGalileo ? QStringLiteral("Galileo")
                                                        : QStringLiteral("BeiDou"),
                               useSbas ? QStringLiteral("on") : QStringLiteral("off"),
                               useQzss ? QStringLiteral("on") : QStringLiteral("off")));
    }
    return true;
}

QString uhdGnssRejectionLogPath() {
    const auto overridePath = qEnvironmentVariable("OBRII_UHD_GNSS_FIX_LOG");
    if (!overridePath.isEmpty()) return overridePath;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("uhd-gnss-fixes.jsonl"));
}

QStringList uhdGnssRejectionLabels(const QStringList &reasons, bool uk) {
    static const struct { const char *code; const char *english; const char *ukrainian; } labels[] = {
        {"invalid_reference", "invalid reference locator", u8"некоректний контрольний квадрат"},
        {"invalid_gga", "no valid GGA fix", u8"немає валідного фікса GGA"},
        {"invalid_rmc", "no valid RMC fix", u8"немає валідного фікса RMC"},
        {"gga_rmc_position_mismatch", "GGA and RMC positions differ", u8"позиції GGA і RMC розходяться"},
        {"outside_reference_area", "outside the reference area", u8"поза допуском від контрольного квадрата"},
        {"gga_rmc_epoch_mismatch", "GGA and RMC timestamps differ", u8"час GGA і RMC розходиться"},
        {"invalid_utc", "invalid or missing UTC", u8"UTC відсутній або некоректний"},
        {"utc_outside_tolerance", "UTC differs from the system clock", u8"UTC поза допуском від системного часу"},
        {"uhd_no_consistent_fix", "waiting for a consistent UHD fix", u8"очікування узгодженого фікса UHD"},
        {"serial_no_consistent_fix", "waiting for a consistent external fix", u8"очікування узгодженого фікса зовнішнього модуля"},
        {"receiver_position_mismatch", "receiver positions differ by more than 100 m", u8"позиції приймачів розходяться більш ніж на 100 м"},
        {"receiver_utc_mismatch", "receiver UTC differs by more than 3 s", u8"UTC приймачів розходиться більш ніж на 3 с"},
    };
    QStringList result;
    for (const auto &reason : reasons) {
        const auto label = std::find_if(std::begin(labels), std::end(labels), [&](const auto &item) {
            return reason == QLatin1String(item.code);
        });
        result << (label == std::end(labels) ? reason : QString::fromUtf8(uk ? label->ukrainian : label->english));
    }
    return result;
}

bool appendUhdGnssFixLog(const QString &path, const UhdGnssStatus &reading, const QDateTime &hostUtc,
                         const QString &event, const QString &selected, const QStringList &reasons = {},
                         const UhdGnssStatus *external = nullptr, const QStringList &externalReasons = {}) {
    QJsonObject entry{
        {QStringLiteral("hostUtc"), hostUtc.toString(Qt::ISODateWithMs)},
        {QStringLiteral("event"), event},
        {QStringLiteral("selected"), selected},
        {QStringLiteral("gga"), reading.gga},
        {QStringLiteral("rmc"), reading.rmc},
    };
    if (!reasons.isEmpty()) entry.insert(QStringLiteral("reasons"), QJsonArray::fromStringList(reasons));
    if (!reading.ubxHex.isEmpty())
        entry.insert(QStringLiteral("ubxHex"), QJsonArray::fromStringList(reading.ubxHex.split(QLatin1Char('\n'))));
    if (external) {
        entry.insert(QStringLiteral("serialGga"), external->gga);
        entry.insert(QStringLiteral("serialRmc"), external->rmc);
        entry.insert(QStringLiteral("serialReasons"), QJsonArray::fromStringList(externalReasons));
    }
    const QByteArray line = QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
    QFile log(path);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !log.open(QIODevice::WriteOnly | QIODevice::Append) ||
#ifdef Q_OS_UNIX
        !log.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
#endif
        log.write(line) != line.size() || !log.flush()) {
        qWarning() << "[UHD GPS] Could not save fix to local log" << path << log.errorString();
        return false;
    }
    return true;
}

void recordUhdGnssFix(const UhdGnssStatus &reading, const ParsedNmeaPosition &fix, const QDateTime &hostUtc,
                      bool firstFix, double previousLatitude, double previousLongitude,
                      const UhdGnssStatus *external = nullptr) {
    const QString path = qEnvironmentVariable("OBRII_UHD_GNSS_FIX_LOG");
    if (path.isEmpty()) return;

    constexpr double positionJumpMeters = 100000.0;
    if (!firstFix && vectorNorm(geodeticToEcef(fix.latitude, fix.longitude, 0.0) -
                               geodeticToEcef(previousLatitude, previousLongitude, 0.0)) < positionJumpMeters) return;
    appendUhdGnssFixLog(path, reading, hostUtc,
                        firstFix ? QStringLiteral("fix_acquired") : QStringLiteral("position_jump"), fix.sentence, {}, external);
}

} // namespace

void YourClassName::showUhdGnssValidationSettings() {
    const bool uk = uiLanguage.startsWith(QLatin1String("uk"));
    const auto text = [uk](const char *ua, const char *en) { return QString::fromUtf8(uk ? ua : en); };
    QDialog dialog(this);
    dialog.setWindowTitle(text(u8"Перевірка фікса UHD GPS", "UHD GPS fix validation"));
    QFormLayout layout(&dialog);
    QCheckBox enable(text(u8"Перевіряти фікси UHD GPS", "Validate UHD GPS fixes"), &dialog);
    enable.setChecked(uhdGnssValidationEnabled);
    QLineEdit locator(uhdGnssReferenceLocator, &dialog);
    locator.setMaxLength(8);
    locator.setPlaceholderText(QStringLiteral("JN58TD"));
    locator.setToolTip(text(u8"Квадрат Maidenhead із 4, 6 або 8 символів", "Maidenhead locator with 4, 6 or 8 characters"));
    QDoubleSpinBox distance(&dialog), time(&dialog);
    distance.setRange(0, 1000);
    distance.setSuffix(text(u8" км", " km"));
    distance.setValue(uhdGnssMaxDistanceKm);
    time.setRange(0, 3600);
    time.setSuffix(text(u8" с", " s"));
    time.setValue(uhdGnssMaxTimeErrorSeconds);
    QCheckBox log(text(u8"Логувати відхилені фікси", "Log rejected fixes"), &dialog);
    log.setChecked(uhdGnssLogRejections);
    log.setToolTip(QDir::toNativeSeparators(uhdGnssRejectionLogPath()));
    QCheckBox show(text(u8"Показувати відхилені фікси на карті", "Show rejected fixes on the map"), &dialog);
    show.setChecked(uhdGnssShowRejections);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout.addRow(&enable);
    layout.addRow(text(u8"Контрольний квадрат", "Reference Maidenhead locator"), &locator);
    layout.addRow(text(u8"Допуск від межі квадрата", "Distance beyond locator boundary"), &distance);
    layout.addRow(text(u8"Допуск від системного UTC", "Difference from system UTC"), &time);
    layout.addRow(&log);
    layout.addRow(&show);
    layout.addRow(&buttons);
    const auto update = [&] {
        const bool enabled = enable.isChecked();
        const auto grid = locator.text().trimmed();
        buttons.button(QDialogButtonBox::Ok)->setEnabled(!enabled || (grid.size() >= 4 && qth::maidenheadBounds(grid).valid));
        log.setEnabled(enabled || qthSource == QLatin1String("uhd_nmea"));
        show.setEnabled(enabled || qthSource == QLatin1String("uhd_nmea"));
    };
    connect(&enable, &QCheckBox::toggled, &dialog, update);
    connect(&locator, &QLineEdit::textChanged, &dialog, update);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    update();
    if (dialog.exec() != QDialog::Accepted) return;

    const auto grid = locator.text().trimmed().toUpper();
    const bool criteriaChanged = uhdGnssValidationEnabled != enable.isChecked() || uhdGnssReferenceLocator != grid ||
                                 uhdGnssMaxDistanceKm != distance.value() || uhdGnssMaxTimeErrorSeconds != time.value();
    uhdGnssValidationEnabled = enable.isChecked();
    uhdGnssReferenceLocator = grid;
    uhdGnssMaxDistanceKm = distance.value();
    uhdGnssMaxTimeErrorSeconds = time.value();
    uhdGnssLogRejections = log.isChecked();
    uhdGnssShowRejections = show.isChecked();
    lastUhdGnssRejectedSample.clear();
    if (criteriaChanged || !uhdGnssShowRejections) uhdGnssRejectedMarkers.clear();
    if (isUhdGnssSource(qthSource) && criteriaChanged) qthPositionVisible = false;
    updateQthControls();
    savePersistentSettings();
    refreshUhdGnssPosition();
}

UhdGnssStatus YourClassName::combinedGnssSerialReading() const {
    if (!gnssSerialPort || !gnssSerialPort->isOpen()) return {QStringLiteral("closed"), {}, {}, {}};
    constexpr qint64 maxAgeMs = 3500;
    const auto gga = combinedSerialGgaAge.isValid() && !combinedSerialGgaAge.hasExpired(maxAgeMs)
        ? combinedSerialGga : QString();
    const auto rmc = combinedSerialRmcAge.isValid() && !combinedSerialRmcAge.hasExpired(maxAgeMs)
        ? combinedSerialRmc : QString();
    return {QStringLiteral("available"), gga, rmc, {}};
}

QString YourClassName::combinedGnssSerialStatus() const {
    if (qthSource != QLatin1String("uhd_nmea")) return {};
    const auto reading = combinedGnssSerialReading();
    const bool uk = uiLanguage.startsWith(QLatin1String("uk"));
    if (reading.state == QLatin1String("closed")) {
        return uk ? QStringLiteral("\nNMEA GPS: порт закрито.") : QStringLiteral("\nNMEA GPS: serial port closed.");
    }
    if (reading.gga.isEmpty() || reading.rmc.isEmpty()) {
        return uk ? QStringLiteral("\nNMEA GPS: очікування свіжої пари GGA/RMC…")
                  : QStringLiteral("\nNMEA GPS: waiting for a fresh GGA/RMC pair…");
    }
    ParsedNmeaPosition fix;
    if (parseNmeaPositionSentence(reading.gga, &fix) && parseNmeaPositionSentence(reading.rmc, &fix)) {
        return uk ? QStringLiteral("\nNMEA GPS: модуль повідомив фікс.")
                  : QStringLiteral("\nNMEA GPS: module reports a fix.");
    }
    return uk ? QStringLiteral("\nNMEA GPS: очікування фікса…") : QStringLiteral("\nNMEA GPS: waiting for a fix…");
}

void YourClassName::showUhdGnssStatus(const QString &status, bool keepSatelliteReport) {
    if (!keepSatelliteReport &&
        (uhdGnssHasSkyReport || uhdGnssSatellitesUsed >= 0 || uhdGnssFixQuality >= 0)) {
        updateUhdGnssSatellites({}, -1, -1);
    }
    if (qthPositionVisible) {
        qthPositionVisible = false;
        updateQthControls();
    }
    if (qthStatusLabel) qthStatusLabel->setText(status + combinedGnssSerialStatus());
}

void YourClassName::applyUhdGnssReading(const UhdGnssStatus &reading) {
    const bool uk = uiLanguage.startsWith(QLatin1String("uk"));
    const auto hostUtc = QDateTime::currentDateTimeUtc();
    if (reading.state == QLatin1String("unsupported")) {
        showUhdGnssStatus(uk ? QStringLiteral("UHD GPS: датчики недоступні в поточній збірці FPGA/UHD.")
                            : QStringLiteral("UHD GPS: sensors unavailable in this FPGA/UHD build."));
        return;
    }
    if (reading.state != QLatin1String("available")) {
        showUhdGnssStatus(uk ? QStringLiteral("UHD GPS: очікування даних NMEA…")
                            : QStringLiteral("UHD GPS: waiting for NMEA data…"));
        return;
    }
    QString details;
    if (!reading.ubxHex.isEmpty()) {
        details += (uk ? QStringLiteral("\nПакетів UBX: %1.")
                       : QStringLiteral("\nUBX packets: %1.")).arg(1 + reading.ubxHex.count(QLatin1Char('\n')));
    }
    const auto gga = nmeaBodyFields(reading.gga);
    const int used = parseNmeaIntField(gga, 7);
    if (used >= 0) {
        details += (uk ? QStringLiteral("\nВикористано для фікса: %1.")
                       : QStringLiteral("\nSatellites used for fix: %1.")).arg(used);
    }
    const auto sky = summarizeUhdGnssGsv(reading.gsv);
    updateUhdGnssSatellites(sky, used, parseNmeaIntField(gga, 6));
    if (sky.satellitesInView >= 0) {
        details += (uk ? QStringLiteral("\nУ полі зору за GSV: %1, із виміряним сигналом: %2.")
                       : QStringLiteral("\nIn view (GSV): %1, with measured signal: %2."))
                       .arg(sky.satellitesInView).arg(sky.satellitesWithSignal);
    }
    if (sky.strongestCn0DbHz >= 0) {
        details += (uk ? QStringLiteral(" Найвищий C/N₀: %1 дБ·Гц.")
                       : QStringLiteral(" Strongest C/N₀: %1 dB-Hz.")).arg(sky.strongestCn0DbHz);
    }
    if (sky.incomplete) {
        details += uk ? QStringLiteral("\nЗвіт GSV неповний.")
                      : QStringLiteral("\nGSV report is incomplete.");
    }

    ParsedNmeaPosition fix;
    if (!parseNmeaPositionSentence(reading.gga, &fix) && !parseNmeaPositionSentence(reading.rmc, &fix)) {
        showUhdGnssStatus((uk ? QStringLiteral("UHD GPS: дані надходять, очікування координат…")
                             : QStringLiteral("UHD GPS: receiving data, waiting for a fix…")) + details, true);
        return;
    }

    const bool combined = qthSource == QLatin1String("uhd_nmea");
    const auto external = combined ? combinedGnssSerialReading() : UhdGnssStatus{};
    const auto checkReference = [&](const UhdGnssStatus &candidate) {
        return uhdGnssFixRejections(candidate.gga, candidate.rmc, uhdGnssReferenceLocator,
                                    uhdGnssMaxDistanceKm, uhdGnssMaxTimeErrorSeconds, hostUtc);
    };
    QStringList reasons, externalReasons;
    if (uhdGnssValidationEnabled) reasons = checkReference(reading);
    if (combined) {
        const auto agreement = combinedGnssFixRejections(reading.gga, reading.rmc, external.gga, external.rmc, hostUtc);
        reasons.append(agreement);
        if (uhdGnssValidationEnabled && !agreement.contains(QStringLiteral("serial_no_consistent_fix"))) {
            externalReasons = checkReference(external);
        }
    }
    if (!reasons.isEmpty() || !externalReasons.isEmpty()) {
        const auto title = combined
            ? (uk ? QStringLiteral("UHD GPS + NMEA: фікс не підтверджено") : QStringLiteral("UHD GPS + NMEA: fix unconfirmed"))
            : (uk ? QStringLiteral("UHD GPS: фікс відхилено") : QStringLiteral("UHD GPS: fix rejected"));
        QString description = title;
        if (!reasons.isEmpty()) description += QLatin1Char('\n') + uhdGnssRejectionLabels(reasons, uk).join(QLatin1Char('\n'));
        for (const auto &reason : uhdGnssRejectionLabels(externalReasons, uk)) {
            description += QStringLiteral("\nNMEA GPS: ") + reason;
        }
        if (uhdGnssLogRejections) {
            const auto key = QStringList{reading.gga, reading.rmc, external.gga, external.rmc,
                                         reasons.join(QLatin1Char(',')), externalReasons.join(QLatin1Char(','))}.join(QLatin1Char('\n'));
            if (key != lastUhdGnssRejectedSample &&
                appendUhdGnssFixLog(uhdGnssRejectionLogPath(), reading, hostUtc, QStringLiteral("fix_rejected"),
                                    fix.sentence, reasons, combined ? &external : nullptr, externalReasons)) {
                lastUhdGnssRejectedSample = key;
            }
        }
        if (uhdGnssShowRejections) {
            auto &markers = uhdGnssRejectedMarkers;
            const auto previous = std::find_if(markers.begin(), markers.end(), [&](const auto &marker) {
                return marker.position.latitude == fix.latitude && marker.position.longitude == fix.longitude;
            });
            if (previous != markers.end()) markers.erase(previous);
            if (markers.size() >= 32) markers.removeFirst();
            markers.append({{fix.latitude, fix.longitude}, description});
            if (qthMapWidget) qthMapWidget->setRejectedFixMarkers(markers);
        }
        showUhdGnssStatus(description + details, true);
        return;
    }
    lastUhdGnssRejectedSample.clear();
    if (!qthPositionBeforeUhd) {
        qthPositionBeforeUhd = QPointF(qthLongitude, qthLatitude);
        qthPositionVisibleBeforeUhd = qthPositionVisible;
    }
    const bool firstFix = !qthPositionVisible;
    if (!isNetworkClientMode()) {
        recordUhdGnssFix(reading, fix, hostUtc, firstFix, qthLatitude, qthLongitude, combined ? &external : nullptr);
    }
    if (firstFix || qthLatitude != fix.latitude || qthLongitude != fix.longitude) {
        qthLatitude = fix.latitude;
        qthLongitude = fix.longitude;
        qthPositionVisible = true;
        updateQthControls();
        if (firstFix && qthMapWidget) qthMapWidget->centerOn(qthLatitude, qthLongitude);
    }
    if (qthStatusLabel) {
        const auto title = combined
            ? (uk ? QStringLiteral("UHD GPS + NMEA: фікс підтверджено") : QStringLiteral("UHD GPS + NMEA: fix confirmed"))
            : (uk ? QStringLiteral("UHD GPS: координати") : QStringLiteral("UHD GPS: fix"));
        qthStatusLabel->setText(title + QStringLiteral(" %1°, %2° (%3)")
            .arg(QString::number(fix.latitude, 'f', 6), QString::number(fix.longitude, 'f', 6),
                 qth::maidenheadLocator(fix.latitude, fix.longitude, 6)) + details + combinedGnssSerialStatus());
    }
}

void YourClassName::refreshUhdGnssPosition() {
    if (!isUhdGnssSource(qthSource)) return;

    const bool uk = uiLanguage.startsWith(QLatin1String("uk"));
    const bool remote = isNetworkClientMode();
    const bool localRunning = !remote && isUhdNativeSelected() && processor &&
                              runState == RadioRunState::Running && processor->isRunning();
    if (uhdGnssProcessor && (!localRunning || uhdGnssProcessor.data() != processor)) {
        uhdGnssProcessor->setUhdGnssPollingEnabled(false);
        uhdGnssProcessor.clear();
    }

    if (!remote) {
        uhdGnssRequestId.clear();
        if (!localRunning) {
            showUhdGnssStatus(uk ? QStringLiteral("UHD GPS: виберіть UHD-приймач і запустіть RX.")
                                : QStringLiteral("UHD GPS: select a UHD receiver and start RX."));
            return;
        }
        if (!uhdGnssProcessor) {
            uhdGnssProcessor = processor;
            uhdGnssProcessor->setUhdGnssPollingEnabled(true);
        }
        applyUhdGnssReading(uhdGnssProcessor->uhdGnssStatus());
        return;
    }

    if (!networkController || !networkController->isControlReady()) {
        uhdGnssRequestId.clear();
        showUhdGnssStatus(uk ? QStringLiteral("UHD GPS: під’єднайтеся до UHD-сервера.")
                            : QStringLiteral("UHD GPS: connect to the UHD server."));
        return;
    }
    if (isRemoteVfoProcessingMode() || !networkController->clientHasControl()) {
        uhdGnssRequestId.clear();
        showUhdGnssStatus(uk ? QStringLiteral("UHD GPS: потрібен доступ до керування приймачем.")
                            : QStringLiteral("UHD GPS: receiver control access is required."));
        return;
    }
    const auto rx = networkClientConfirmedSettingsJson.value(QStringLiteral("uhdRx")).toObject();
    if (!rx.value(QStringLiteral("gnssControl")).toBool()) {
        uhdGnssRequestId.clear();
        showUhdGnssStatus(uk ? QStringLiteral("UHD GPS: GPS на сервері недоступний або вимкнений.")
                            : QStringLiteral("UHD GPS: GPS is unavailable or disabled on the server."));
        return;
    }
    if (runState != RadioRunState::Running) {
        uhdGnssRequestId.clear();
        showUhdGnssStatus(uk ? QStringLiteral("UHD GPS: запустіть RX на сервері.")
                            : QStringLiteral("UHD GPS: start RX on the server."));
        return;
    }
    if (!uhdGnssRequestId.isEmpty()) {
        if (!uhdGnssRequestAge.hasExpired(5000)) return;
        showUhdGnssStatus(uk ? QStringLiteral("UHD GPS: сервер не відповів на запит.")
                            : QStringLiteral("UHD GPS: the server did not reply."));
    }
    uhdGnssRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    uhdGnssRequestAge.start();
    if (!networkController->sendControlCommand({
            {QStringLiteral("type"), QStringLiteral("control")},
            {QStringLiteral("action"), QStringLiteral("getUhdGnss")},
            {QStringLiteral("requestId"), uhdGnssRequestId}})) {
        uhdGnssRequestId.clear();
        showUhdGnssStatus(uk ? QStringLiteral("UHD GPS: не вдалося надіслати запит.")
                            : QStringLiteral("UHD GPS: could not send the request."));
    }
}

void YourClassName::handleUhdGnssReply(const QJsonObject &command) {
    if (!isUhdGnssSource(qthSource) || uhdGnssRequestId.isEmpty() ||
        !isNetworkClientMode() || isRemoteVfoProcessingMode() || runState != RadioRunState::Running ||
        !networkController || !networkController->isControlReady() || !networkController->clientHasControl() ||
        command.value(QStringLiteral("type")) != QLatin1String("control") ||
        command.value(QStringLiteral("action")) != QLatin1String("uhdGnssStatus") ||
        command.value(QStringLiteral("requestId")).toString() != uhdGnssRequestId) return;

    uhdGnssRequestId.clear();
    const auto reading = command.value(QStringLiteral("status")).toObject();
    applyUhdGnssReading({reading.value(QStringLiteral("state")).toString(),
                        reading.value(QStringLiteral("gga")).toString(),
                        reading.value(QStringLiteral("rmc")).toString(),
                        reading.value(QStringLiteral("gsv")).toString(),
                        validatedUhdGnssUbx(reading.value(QStringLiteral("ubxHex")).toString().toLatin1())});
}

void YourClassName::updateQthControls() {
    qthLatitude = (std::clamp)(qthLatitude, -90.0, 90.0);
    qthLongitude = (std::clamp)(qthLongitude, -180.0, 180.0);
    const QthOnlineProviderPreset currentOnlineProvider = qthOnlineProviderPreset(qthOnlineProviderId);
    const int qthMaxZoom = (std::clamp)(currentOnlineProvider.maxZoom, 0, 19);
    qthMapZoom = (std::clamp)(qthMapZoom, 0, qthMaxZoom);
    const QString locator = qth::maidenheadLocator(qthLatitude, qthLongitude, 6);

    if (qthSourceCombo) {
        QSignalBlocker blocker(qthSourceCombo);
        const int index = qthSourceCombo->findData(qthSource);
        qthSourceCombo->setCurrentIndex(index >= 0 ? index : qthSourceCombo->findData(QStringLiteral("manual")));
    }
    if (uhdGnssValidationButton) {
        uhdGnssValidationButton->setVisible(isUhdGnssSource(qthSource));
        uhdGnssValidationButton->setText(uiLanguage.startsWith(QLatin1String("uk"))
            ? QStringLiteral("Перевірка фікса…") : QStringLiteral("Fix validation…"));
    }
    if (gnssSystemCombo) {
        QSignalBlocker blocker(gnssSystemCombo);
        const int index = gnssSystemCombo->findData(gnssSystemId);
        gnssSystemCombo->setCurrentIndex(index >= 0 ? index : gnssSystemCombo->findData(QStringLiteral("gps_l1_ca")));
    }
    if (gnssPositionPolicyCombo) {
        QSignalBlocker blocker(gnssPositionPolicyCombo);
        const int index = gnssPositionPolicyCombo->findData(normalizedGnssPositionPolicy(gnssPositionPolicy));
        gnssPositionPolicyCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (gnssTimeZoneCombo) {
        QSignalBlocker blocker(gnssTimeZoneCombo);
        const int index = gnssTimeZoneCombo->findData(gnssTimeZoneOffsetMinutes);
        gnssTimeZoneCombo->setCurrentIndex(index >= 0 ? index : gnssTimeZoneCombo->findData(0));
    }
    if (qthLatitudeSpin) {
        QSignalBlocker blocker(qthLatitudeSpin);
        qthLatitudeSpin->setValue(qthLatitude);
        qthLatitudeSpin->setReadOnly(isUhdGnssSource(qthSource));
    }
    if (qthLongitudeSpin) {
        QSignalBlocker blocker(qthLongitudeSpin);
        qthLongitudeSpin->setValue(qthLongitude);
        qthLongitudeSpin->setReadOnly(isUhdGnssSource(qthSource));
    }
    if (gnssSerialPortEdit) {
        const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
        const QString portName = preferredGnssSerialPortName(gnssSerialPortName, ports);
        if (!portName.isEmpty()) {
            gnssSerialPortName = portName;
        }
        refreshGnssSerialCombo(gnssSerialPortEdit, ports, gnssSerialPortName);
    }
    if (gnssSerialBaudSpin) {
        QSignalBlocker blocker(gnssSerialBaudSpin);
        gnssSerialBaudSpin->setValue((std::clamp)(gnssSerialBaud, 1200, 921600));
    }
    if (gnssSerialButton) {
        const bool connected = gnssSerialPort && gnssSerialPort->isOpen();
        gnssSerialButton->setText(connected
                                      ? uiText(QStringLiteral("disconnect"), QStringLiteral("Disconnect"))
                                      : uiText(QStringLiteral("connect"), QStringLiteral("Connect")));
    }
    if (gnssSerialStatusLabel && (!gnssSerialPort || !gnssSerialPort->isOpen()) && gnssSerialStatusLabel->text().isEmpty()) {
        gnssSerialStatusLabel->setText(uiText(QStringLiteral("gnss_serial_idle"),
                                              QStringLiteral("NMEA serial: disconnected")));
    }
    if (qthLocatorLabel) {
        qthLocatorLabel->setText(isUhdGnssSource(qthSource) && !qthPositionVisible ? QString() : locator);
    }
    if (gnssMonitorCheckbox) {
        QSignalBlocker blocker(gnssMonitorCheckbox);
        gnssMonitorCheckbox->setChecked(gnssMonitorEnabled);
    }
    updateGnssSatelliteView();
    if (gnssMonitorStatusLabel && !gnssSignalMonitor.lastReport().valid) {
        gnssMonitorStatusLabel->setText(gnssMonitorEnabled
                                            ? uiText(QStringLiteral("gnss_iq_monitor_waiting"),
                                                     QStringLiteral("GNSS IQ monitor: waiting for IQ samples"))
                                            : uiText(QStringLiteral("gnss_iq_monitor_idle"),
                                                     QStringLiteral("GNSS IQ monitor: off")));
    }
    if (qthMapWidget) {
        qthMapWidget->setUiLanguage(uiLanguage);
        qthMapWidget->setPosition(qthLatitude, qthLongitude);
        qthMapWidget->setPositionVisible(qthPositionVisible);
        qthMapWidget->setTileLayerMode(qthMapLayer == 1
                                           ? QthMapWidget::TileLayerMode::LocalXyz
                                           : (qthMapLayer == 2
                                                  ? QthMapWidget::TileLayerMode::OnlineXyz
                                                  : QthMapWidget::TileLayerMode::GridOnly));
        qthMapWidget->setTileDirectory(qthTileDirectory);
        qthMapWidget->setOnlineTileTemplate(resolvedQthOnlineTileUrlTemplate());
        qthMapWidget->setOnlineAttribution(qthOnlineAttribution);
        qthMapWidget->setOnlineDiskCacheEnabled(!qthOnlineNoDiskCache);
        qthMapWidget->setMapZoomRange(0, qthMaxZoom);
        qthMapWidget->setMapZoom(qthMapZoom);
        qthMapWidget->setGridPrecision(qthGridPrecision);
        qthMapWidget->setOverlayMode(static_cast<QthMapWidget::OverlayMode>((std::clamp)(qthMapOverlayMode, 0, 3)));
        qthMapWidget->setUserMarkers(qthUserMarkers);
        qthMapWidget->setRejectedFixMarkers(isUhdGnssSource(qthSource) && uhdGnssShowRejections
                                               ? uhdGnssRejectedMarkers : QVector<qth::RejectedFixMarker>{});
        updateQthMapSatelliteOverlay();
    }
    updateQthMapControls();
    if (isUhdGnssSource(qthSource)) {
        if (!uhdGnssPollTimer->isActive()) {
            uhdGnssPollTimer->start();
            applyUhdGnssReading({});
            refreshUhdGnssPosition();
        }
        return;
    }
    uhdGnssPollTimer->stop();
    if (uhdGnssProcessor) {
        uhdGnssProcessor->setUhdGnssPollingEnabled(false);
        uhdGnssProcessor.clear();
    }
    uhdGnssRequestId.clear();
    qthPositionBeforeUhd.reset();
    if (qthStatusLabel) {
        const bool manual = qthSource == QStringLiteral("manual") || qthSource.isEmpty();
        if (!qthPositionVisible) {
            qthStatusLabel->setText(uiText(QStringLiteral("qth_status_cleared"),
                                           QStringLiteral("Current QTH position is cleared. Manual/NMEA update will show it again.")));
        } else if (manual) {
            qthStatusLabel->setText(uiText(QStringLiteral("qth_status_manual"),
                                           QStringLiteral("Manual QTH. Use Save GNSS IQ to write the current raw IQ snapshot.")));
        } else if (qthSource == QStringLiteral("nmea")) {
            qthStatusLabel->setText(uiText(QStringLiteral("qth_status_nmea"),
                                           QStringLiteral("NMEA QTH is active. Paste GGA/RMC text to update coordinates.")));
        } else {
            qthStatusLabel->setText(uiText(QStringLiteral("qth_status_future_source"),
                                           QStringLiteral("This position source is planned; manual coordinates are used for now.")));
        }
    }
}

void YourClassName::applyQthOnlineProviderPreset(const QString &providerId, bool applyTemplate) {
    const QthOnlineProviderPreset preset = qthOnlineProviderPreset(providerId.trimmed());
    qthOnlineProviderId = preset.id;
    if (applyTemplate && preset.id != QStringLiteral("custom")) {
        qthOnlineTileUrlTemplate = preset.urlTemplate;
        qthOnlineAttribution = preset.attribution;
        qthOnlineNoDiskCache = preset.noDiskCache;
    }
}

QString YourClassName::resolvedQthOnlineTileUrlTemplate() const {
    QString urlTemplate = qthOnlineTileUrlTemplate.trimmed();
    if (urlTemplate.isEmpty()) {
        return {};
    }
    const bool needsKey = qthOnlineTemplateNeedsKey(urlTemplate);
    const QString key = qthOnlineApiKey.trimmed();
    if (needsKey && key.isEmpty()) {
        return {};
    }
    const QString encodedKey = QString::fromLatin1(QUrl::toPercentEncoding(key));
    urlTemplate.replace(QStringLiteral("{key}"), encodedKey, Qt::CaseInsensitive);
    urlTemplate.replace(QStringLiteral("{token}"), encodedKey, Qt::CaseInsensitive);
    return urlTemplate;
}

void YourClassName::applyQthPositionFromUi() {
    if (qthLatitudeSpin) {
        qthLatitude = qthLatitudeSpin->value();
    }
    if (qthLongitudeSpin) {
        qthLongitude = qthLongitudeSpin->value();
    }
    if (qthSourceCombo) {
        qthSource = qthSourceCombo->currentData().toString();
        if (qthSource.isEmpty()) {
            qthSource = QStringLiteral("manual");
        }
    }
    qthPositionVisible = true;
    updateQthControls();
    savePersistentSettings();
}

void YourClassName::clearQthPosition() {
    qthPositionVisible = false;
    updateQthControls();
    if (qthMapWidget) {
        qthMapWidget->setPositionVisible(false);
        qthMapWidget->clearSearchMarker();
    }

    const QString status = uiText(
        QStringLiteral("qth_position_cleared"),
        QStringLiteral("Current QTH position cleared. GNSS reception remains active; the next valid manual/NMEA/UBX update will show it again."));
    if (qthStatusLabel) {
        qthStatusLabel->setText(status);
    }
    if (qthMapStatusLabel) {
        qthMapStatusLabel->setText(status);
    }
    savePersistentSettings();
}

void YourClassName::pasteNmeaPositionFromClipboard() {
    QClipboard *clipboard = QApplication::clipboard();
    const QString text = clipboard ? clipboard->text() : QString();
    QString status;
    if (!applyNmeaPositionText(text, &status) && status.isEmpty()) {
        status = uiText(QStringLiteral("nmea_parse_failed"),
                        QStringLiteral("NMEA: no valid GGA/RMC position found in clipboard."));
    }
    if (qthStatusLabel) {
        qthStatusLabel->setText(status);
    }
}

bool YourClassName::applyNmeaPositionText(const QString &text, QString *statusMessage, bool persist) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        if (statusMessage) {
            *statusMessage = uiText(QStringLiteral("nmea_parse_failed"),
                                    QStringLiteral("NMEA: no valid GGA/RMC position found in clipboard."));
        }
        return false;
    }

    const QStringList lines = trimmed.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                            Qt::SkipEmptyParts);
    ParsedNmeaPosition parsed;
    bool found = false;
    for (const QString &line : lines) {
        if (parseNmeaPositionSentence(line, &parsed)) {
            found = true;
            break;
        }
    }

    if (!found) {
        if (statusMessage) {
            *statusMessage = uiText(QStringLiteral("nmea_parse_failed"),
                                    QStringLiteral("NMEA: no valid GGA/RMC position found in clipboard."));
        }
        qDebug() << "[GNSS NMEA] parse failed" << "chars" << trimmed.size() << "lines" << lines.size();
        return false;
    }

    qthLatitude = parsed.latitude;
    qthLongitude = parsed.longitude;
    qthSource = QStringLiteral("nmea");
    qthPositionVisible = true;
    const QString locator = qth::maidenheadLocator(qthLatitude, qthLongitude, 6);
    updateQthControls();
    if (qthMapWidget) {
        qthMapWidget->centerOn(qthLatitude, qthLongitude);
    }
    if (persist) {
        savePersistentSettings();
    }

    const QString status = uiText(QStringLiteral("nmea_position_applied"),
                                  QStringLiteral("NMEA QTH: %1 %2, %3 (%4)"))
                               .arg(QStringLiteral("%1%2").arg(parsed.talker, parsed.sentence))
                               .arg(qthLatitude, 0, 'f', 6)
                               .arg(qthLongitude, 0, 'f', 6)
                               .arg(locator);
    if (statusMessage) {
        *statusMessage = status;
    }
    if (qthStatusLabel) {
        qthStatusLabel->setText(status);
    }
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[GNSS NMEA] position applied"
                 << "sentence" << QStringLiteral("%1%2").arg(parsed.talker, parsed.sentence)
                 << "lat" << qthLatitude
                 << "lon" << qthLongitude
                 << "qth" << locator
                 << "utc" << parsed.utc;
    }
    return true;
}
