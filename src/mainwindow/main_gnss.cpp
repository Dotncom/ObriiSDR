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
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>
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

} // namespace

void YourClassName::showUhdGnssStatus(const QString &status) {
    if (qthPositionVisible) {
        qthPositionVisible = false;
        updateQthControls();
    }
    if (qthStatusLabel) qthStatusLabel->setText(status);
}

void YourClassName::applyUhdGnssReading(const UhdGnssStatus &reading) {
    const bool uk = uiLanguage.startsWith(QLatin1String("uk"));
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
    const int used = parseNmeaIntField(nmeaBodyFields(reading.gga), 7);
    if (used >= 0) {
        details += (uk ? QStringLiteral("\nВикористано для фікса: %1.")
                       : QStringLiteral("\nSatellites used for fix: %1.")).arg(used);
    }
    const auto sky = summarizeUhdGnssGsv(reading.gsv);
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
        showUhdGnssStatus((uk ? QStringLiteral("UHD GPS: NMEA надходить, очікування координат…")
                             : QStringLiteral("UHD GPS: receiving NMEA, waiting for a fix…")) + details);
        return;
    }

    if (!qthPositionBeforeUhd) {
        qthPositionBeforeUhd = QPointF(qthLongitude, qthLatitude);
        qthPositionVisibleBeforeUhd = qthPositionVisible;
    }
    const bool firstFix = !qthPositionVisible;
    if (firstFix || qthLatitude != fix.latitude || qthLongitude != fix.longitude) {
        qthLatitude = fix.latitude;
        qthLongitude = fix.longitude;
        qthPositionVisible = true;
        updateQthControls();
        if (firstFix && qthMapWidget) qthMapWidget->centerOn(qthLatitude, qthLongitude);
    }
    if (qthStatusLabel) {
        qthStatusLabel->setText(
            (uk ? QStringLiteral("UHD GPS: координати %1°, %2° (%3)")
                : QStringLiteral("UHD GPS: fix %1°, %2° (%3)"))
                .arg(QString::number(fix.latitude, 'f', 6), QString::number(fix.longitude, 'f', 6),
                     qth::maidenheadLocator(fix.latitude, fix.longitude, 6)) + details);
    }
}

void YourClassName::refreshUhdGnssPosition() {
    if (qthSource != QLatin1String("uhd")) return;

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
    if (qthSource != QLatin1String("uhd") || uhdGnssRequestId.isEmpty() ||
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
                        reading.value(QStringLiteral("gsv")).toString()});
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
        qthLatitudeSpin->setReadOnly(qthSource == QLatin1String("uhd"));
    }
    if (qthLongitudeSpin) {
        QSignalBlocker blocker(qthLongitudeSpin);
        qthLongitudeSpin->setValue(qthLongitude);
        qthLongitudeSpin->setReadOnly(qthSource == QLatin1String("uhd"));
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
        qthLocatorLabel->setText(qthSource == QLatin1String("uhd") && !qthPositionVisible ? QString() : locator);
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
        updateQthMapSatelliteOverlay();
    }
    updateQthMapControls();
    if (qthSource == QLatin1String("uhd")) {
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
