#include "main.h"

#include "diagnosticlogging.h"
#include "gnssqthhelpers.h"
#include "gnssserialutils.h"
#include "qthlocator.h"
#include "qthmapwidget.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QSerialPortInfo>
#include <QSignalBlocker>
#include <QTextCursor>

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
        return true;
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
void YourClassName::toggleGnssSerial() {
    if (gnssSerialPort && gnssSerialPort->isOpen()) {
        stopGnssSerial();
    } else {
        startGnssSerial();
    }
}

void YourClassName::startGnssSerial() {
    if (!gnssSerialPort) {
        updateGnssSerialStatus(uiText(QStringLiteral("gnss_serial_unavailable"),
                                      QStringLiteral("NMEA serial: Qt serial port is unavailable.")));
        return;
    }
    if (gnssSerialPort->isOpen()) {
        stopGnssSerial();
    }

    if (gnssSerialPortEdit) {
        gnssSerialPortName = gnssSerialPortEdit->currentText().trimmed();
    }
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    gnssSerialPortName = preferredGnssSerialPortName(gnssSerialPortName, ports);
    refreshGnssSerialCombo(gnssSerialPortEdit, ports, gnssSerialPortName);
    if (gnssSerialPortName.isEmpty() || !serialPortNameExists(ports, gnssSerialPortName)) {
        const QStringList available = currentSerialPortNames(ports);
        const QString message = available.isEmpty()
                                    ? uiText(QStringLiteral("gnss_serial_no_ports"),
                                             QStringLiteral("NMEA serial: no serial ports found."))
                                    : uiText(QStringLiteral("gnss_serial_port_missing"),
                                             QStringLiteral("NMEA serial: selected port is unavailable. Available ports: %1."))
                                          .arg(available.join(QStringLiteral(", ")));
        updateGnssSerialStatus(message);
        qDebug() << "[GNSS serial] selected port unavailable"
                 << "requested" << (gnssSerialPortEdit ? gnssSerialPortEdit->currentText().trimmed() : QString())
                 << "resolved" << gnssSerialPortName
                 << "available" << available;
        return;
    }
    if (gnssSerialBaudSpin) {
        gnssSerialBaud = gnssSerialBaudSpin->value();
    }
    gnssSerialBaud = (std::clamp)(gnssSerialBaud, 1200, 921600);
    gnssSerialBuffer.clear();
    gnssSerialSentenceCount = 0;
    gnssSerialUbxFrameCount = 0;
    gnssSerialFixCount = 0;
    gnssUbxOutputEnabled = false;
    gnssPendingCfgGnssApply = false;
    gnssLastCfgGnssPayload.clear();
    gnssLastUbxFixMs = 0;
    gnssSerialFixQuality = -1;
    gnssSerialFixMode = -1;
    gnssSerialSatellitesUsed = -1;
    gnssSerialGpsSatellites = -1;
    gnssSerialGlonassSatellites = -1;
    gnssSerialGalileoSatellites = -1;
    gnssSerialBeidouSatellites = -1;
    gnssSerialOtherSatellites = -1;
    gnssSerialUtc.clear();
    gnssSerialHdop = std::numeric_limits<double>::quiet_NaN();
    gnssSerialVdop = std::numeric_limits<double>::quiet_NaN();
    gnssSerialPdop = std::numeric_limits<double>::quiet_NaN();
    gnssSerialAltitudeM = std::numeric_limits<double>::quiet_NaN();
    gnssSerialGeoidSeparationM = std::numeric_limits<double>::quiet_NaN();
    gnssSerialSpeedKmh = std::numeric_limits<double>::quiet_NaN();
    gnssSerialCourseDeg = std::numeric_limits<double>::quiet_NaN();
    resetGnssNmeaSatellites();

    if (gnssUbxAutoEnable &&
        normalizedGnssPositionPolicy(gnssPositionPolicy) == QStringLiteral("nmea_only")) {
        gnssPositionPolicy = QStringLiteral("ubx_preferred");
        if (gnssPositionPolicyCombo) {
            QSignalBlocker blocker(gnssPositionPolicyCombo);
            const int policyIndex = gnssPositionPolicyCombo->findData(gnssPositionPolicy);
            if (policyIndex >= 0) {
                gnssPositionPolicyCombo->setCurrentIndex(policyIndex);
            }
        }
        qDebug() << "[GNSS policy] auto UBX promoted position policy to"
                 << gnssPositionPolicy;
    }

    gnssSerialPort->setPortName(gnssSerialPortName);
    gnssSerialPort->setBaudRate(gnssSerialBaud);
    gnssSerialPort->setDataBits(QSerialPort::Data8);
    gnssSerialPort->setParity(QSerialPort::NoParity);
    gnssSerialPort->setStopBits(QSerialPort::OneStop);
    gnssSerialPort->setFlowControl(QSerialPort::NoFlowControl);

    if (!gnssSerialPort->open(QIODevice::ReadWrite)) {
        const QString message = uiText(QStringLiteral("gnss_serial_open_failed"),
                                       QStringLiteral("NMEA serial: cannot open %1 at %2 baud (%3)."))
                                    .arg(gnssSerialPortName)
                                    .arg(gnssSerialBaud)
                                    .arg(gnssSerialPort->errorString());
        updateGnssSerialStatus(message);
        qDebug() << "[GNSS serial] open failed"
                 << "port" << gnssSerialPortName
                 << "baud" << gnssSerialBaud
                 << "error" << gnssSerialPort->errorString();
        return;
    }

    qthSource = QStringLiteral("nmea");
    qthPositionVisible = false;
    updateQthControls();
    savePersistentSettings();
    updateGnssSerialStatus(uiText(QStringLiteral("gnss_serial_waiting"),
                                  QStringLiteral("NMEA serial: %1 open at %2 baud, waiting for fix..."))
                               .arg(gnssSerialPortName)
                               .arg(gnssSerialBaud));
    qDebug() << "[GNSS serial] open"
             << "port" << gnssSerialPortName
             << "baud" << gnssSerialBaud;
    if (gnssUbxAutoEnable) {
        QTimer::singleShot(50, this, [this]() {
            if (gnssSerialPort && gnssSerialPort->isOpen()) {
                sendGnssUbxConfiguration(true);
            }
        });
    }
}

void YourClassName::stopGnssSerial() {
    if (gnssSerialPort && gnssSerialPort->isOpen()) {
        qDebug() << "[GNSS serial] close" << "port" << gnssSerialPort->portName();
        gnssSerialPort->close();
    }
    if (gnssNmeaLogFile && gnssNmeaLogFile->isOpen()) {
        gnssNmeaLogFile->flush();
        gnssNmeaLogFile->close();
        gnssNmeaLogFile.reset();
        gnssNmeaLogPath.clear();
    }
    if (gnssRawSerialLogFile && gnssRawSerialLogFile->isOpen()) {
        gnssRawSerialLogFile->flush();
        gnssRawSerialLogFile->close();
        gnssRawSerialLogFile.reset();
        gnssRawSerialLogPath.clear();
    }
    gnssSerialBuffer.clear();
    gnssSerialSentenceCount = 0;
    gnssSerialUbxFrameCount = 0;
    gnssSerialFixCount = 0;
    gnssSerialFixQuality = -1;
    gnssSerialFixMode = -1;
    gnssSerialSatellitesUsed = -1;
    gnssSerialGpsSatellites = -1;
    gnssSerialGlonassSatellites = -1;
    gnssSerialGalileoSatellites = -1;
    gnssSerialBeidouSatellites = -1;
    gnssSerialOtherSatellites = -1;
    gnssSerialUtc.clear();
    gnssSerialHdop = std::numeric_limits<double>::quiet_NaN();
    gnssSerialVdop = std::numeric_limits<double>::quiet_NaN();
    gnssSerialPdop = std::numeric_limits<double>::quiet_NaN();
    gnssSerialAltitudeM = std::numeric_limits<double>::quiet_NaN();
    gnssSerialGeoidSeparationM = std::numeric_limits<double>::quiet_NaN();
    gnssSerialSpeedKmh = std::numeric_limits<double>::quiet_NaN();
    gnssSerialCourseDeg = std::numeric_limits<double>::quiet_NaN();
    resetGnssNmeaSatellites();
    if (gnssSerialButton) {
        gnssSerialButton->setText(uiText(QStringLiteral("connect"), QStringLiteral("Connect")));
    }
    gnssSerialUbxFrameCount = 0;
    gnssUbxOutputEnabled = false;
    gnssPendingCfgGnssApply = false;
    gnssLastCfgGnssPayload.clear();
    gnssLastUbxFixMs = 0;
    updateGnssSerialStatus(uiText(QStringLiteral("gnss_serial_idle"),
                                  QStringLiteral("NMEA serial: disconnected")));
}

void YourClassName::handleGnssSerialReadyRead() {
    if (!gnssSerialPort) {
        return;
    }
    const QByteArray bytes = gnssSerialPort->readAll();
    if (bytes.isEmpty()) {
        return;
    }
    if (gnssRawSerialLogFile && gnssRawSerialLogFile->isOpen()) {
        gnssRawSerialLogFile->write(bytes);
        if (((gnssSerialSentenceCount + gnssSerialUbxFrameCount) % 32) == 0) {
            gnssRawSerialLogFile->flush();
        }
    }
    gnssSerialBuffer.append(bytes);
    if (gnssSerialBuffer.size() > 65536) {
        gnssSerialBuffer = gnssSerialBuffer.right(32768);
    }

    while (true) {
        const int nmeaIndex = gnssSerialBuffer.indexOf('$');
        const int ubxIndex = gnssSerialBuffer.indexOf(QByteArray::fromHex("B562"));
        int nextIndex = -1;
        if (nmeaIndex >= 0 && ubxIndex >= 0) {
            nextIndex = (std::min)(nmeaIndex, ubxIndex);
        } else {
            nextIndex = (std::max)(nmeaIndex, ubxIndex);
        }
        if (nextIndex < 0) {
            if (gnssSerialBuffer.size() > 4096) {
                gnssSerialBuffer.clear();
            }
            break;
        }
        if (nextIndex > 0) {
            gnssSerialBuffer.remove(0, nextIndex);
        }

        if (gnssSerialBuffer.size() >= 2 &&
            static_cast<quint8>(gnssSerialBuffer.at(0)) == 0xB5 &&
            static_cast<quint8>(gnssSerialBuffer.at(1)) == 0x62) {
            if (gnssSerialBuffer.size() < 6) {
                break;
            }
            const int payloadLength = static_cast<int>(ubxU2(gnssSerialBuffer, 4));
            if (payloadLength < 0 || payloadLength > 4096) {
                gnssSerialBuffer.remove(0, 1);
                continue;
            }
            const int frameLength = payloadLength + 8;
            if (gnssSerialBuffer.size() < frameLength) {
                break;
            }
            const QByteArray frame = gnssSerialBuffer.left(frameLength);
            gnssSerialBuffer.remove(0, frameLength);
            if (!hasValidUbxChecksum(frame)) {
                if (fobosVerboseLoggingEnabled()) {
                    qDebug() << "[GNSS UBX] bad checksum" << "bytes" << frameLength;
                }
                continue;
            }
            processGnssUbxFrame(static_cast<quint8>(frame.at(2)),
                                static_cast<quint8>(frame.at(3)),
                                frame.mid(6, payloadLength));
            continue;
        }

        int lineEnd = gnssSerialBuffer.indexOf('\n');
        const int crEnd = gnssSerialBuffer.indexOf('\r');
        if (lineEnd < 0 || (crEnd >= 0 && crEnd < lineEnd)) {
            lineEnd = crEnd;
        }
        if (lineEnd < 0) {
            break;
        }

        const QByteArray rawLine = gnssSerialBuffer.left(lineEnd);
        gnssSerialBuffer.remove(0, lineEnd + 1);
        const QString line = QString::fromLatin1(rawLine).trimmed();
        if (!line.isEmpty()) {
            processGnssNmeaLine(line, false);
        }
    }
}

void YourClassName::processGnssNmeaLine(const QString &line, bool fromReplay, bool fromSdr) {
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    const bool looksLikeNmea = trimmed.startsWith(QLatin1Char('$'));
    if (!looksLikeNmea) {
        return;
    }

    ++gnssSerialSentenceCount;
    if (gnssNmeaLogFile && gnssNmeaLogFile->isOpen() && !fromReplay) {
        gnssNmeaLogFile->write(trimmed.toLatin1());
        gnssNmeaLogFile->write("\n");
        if ((gnssSerialSentenceCount % 16) == 0) {
            gnssNmeaLogFile->flush();
        }
    }
    updateGnssNmeaSatellitesFromSentence(trimmed);

    const NmeaSerialStatus serialStatus = parseNmeaSerialStatusSentence(trimmed);
    if (serialStatus.recognized) {
        if (!serialStatus.utc.isEmpty()) {
            gnssSerialUtc = serialStatus.utc;
        }
        if (serialStatus.fixQuality >= 0) {
            gnssSerialFixQuality = serialStatus.fixQuality;
        }
        if (serialStatus.fixMode >= 0) {
            gnssSerialFixMode = serialStatus.fixMode;
        }
        if (serialStatus.satellitesUsed >= 0) {
            gnssSerialSatellitesUsed = serialStatus.satellitesUsed;
        }
        if (std::isfinite(serialStatus.hdop)) {
            gnssSerialHdop = serialStatus.hdop;
        }
        if (std::isfinite(serialStatus.vdop)) {
            gnssSerialVdop = serialStatus.vdop;
        }
        if (std::isfinite(serialStatus.pdop)) {
            gnssSerialPdop = serialStatus.pdop;
        }
        if (std::isfinite(serialStatus.altitudeM)) {
            gnssSerialAltitudeM = serialStatus.altitudeM;
        }
        if (std::isfinite(serialStatus.geoidSeparationM)) {
            gnssSerialGeoidSeparationM = serialStatus.geoidSeparationM;
        }
        if (std::isfinite(serialStatus.speedKmh)) {
            gnssSerialSpeedKmh = serialStatus.speedKmh;
        }
        if (std::isfinite(serialStatus.courseDeg)) {
            gnssSerialCourseDeg = serialStatus.courseDeg;
        }
        if (serialStatus.satellitesInView >= 0) {
            if (serialStatus.talker == QStringLiteral("GP")) {
                gnssSerialGpsSatellites = serialStatus.satellitesInView;
            } else if (serialStatus.talker == QStringLiteral("GL")) {
                gnssSerialGlonassSatellites = serialStatus.satellitesInView;
            } else if (serialStatus.talker == QStringLiteral("GA")) {
                gnssSerialGalileoSatellites = serialStatus.satellitesInView;
            } else if (serialStatus.talker == QStringLiteral("GB") ||
                       serialStatus.talker == QStringLiteral("BD")) {
                gnssSerialBeidouSatellites = serialStatus.satellitesInView;
            } else {
                gnssSerialOtherSatellites = serialStatus.satellitesInView;
            }
        }
    }

    ParsedNmeaPosition parsed;
    if (!parseNmeaPositionSentence(trimmed, &parsed)) {
        if (gnssSerialSentenceCount <= 4 || (gnssSerialSentenceCount % 32) == 0) {
            QStringList parts;
            if (gnssSerialFixQuality >= 0) {
                parts << QStringLiteral("fix %1").arg(gnssSerialFixQuality);
            }
            if (gnssSerialFixMode >= 0) {
                parts << QStringLiteral("mode %1").arg(gnssSerialFixMode);
            }
            if (gnssSerialSatellitesUsed >= 0) {
                parts << QStringLiteral("used %1").arg(gnssSerialSatellitesUsed);
            }
            QStringList viewParts;
            if (gnssSerialGpsSatellites >= 0) viewParts << QStringLiteral("GPS %1").arg(gnssSerialGpsSatellites);
            if (gnssSerialGlonassSatellites >= 0) viewParts << QStringLiteral("GLO %1").arg(gnssSerialGlonassSatellites);
            if (gnssSerialGalileoSatellites >= 0) viewParts << QStringLiteral("GAL %1").arg(gnssSerialGalileoSatellites);
            if (gnssSerialBeidouSatellites >= 0) viewParts << QStringLiteral("BDS %1").arg(gnssSerialBeidouSatellites);
            if (gnssSerialOtherSatellites >= 0) viewParts << QStringLiteral("other %1").arg(gnssSerialOtherSatellites);
            if (!viewParts.isEmpty()) {
                parts << QStringLiteral("view %1").arg(viewParts.join(QLatin1Char('/')));
            }
            if (std::isfinite(gnssSerialHdop)) parts << QStringLiteral("HDOP %1").arg(gnssSerialHdop, 0, 'f', 1);
            if (std::isfinite(gnssSerialPdop)) parts << QStringLiteral("PDOP %1").arg(gnssSerialPdop, 0, 'f', 1);
            if (std::isfinite(gnssSerialAltitudeM)) parts << QStringLiteral("alt %1 m").arg(gnssSerialAltitudeM, 0, 'f', 1);
            if (std::isfinite(gnssSerialSpeedKmh)) parts << QStringLiteral("speed %1 km/h").arg(gnssSerialSpeedKmh, 0, 'f', 1);
            if (!gnssSerialUtc.isEmpty()) {
                parts << QStringLiteral("time %1").arg(formattedGnssUtc());
            }
            const QString detail = parts.isEmpty()
                                       ? QStringLiteral("waiting for fix")
                                       : parts.join(QStringLiteral(", "));
            updateGnssSerialStatus(uiText(QStringLiteral("gnss_serial_receiving"),
                                          QStringLiteral("NMEA serial: receiving (%1 sentences), %2"))
                                       .arg(gnssSerialSentenceCount)
                                       .arg(detail));
        }
        if (fobosVerboseLoggingEnabled() &&
            (gnssSerialSentenceCount <= 4 || (gnssSerialSentenceCount % 64) == 0)) {
            qDebug() << "[GNSS serial] ignored NMEA" << trimmed.left(16);
        }
        return;
    }

    QString status;
    const QString policy = normalizedGnssPositionPolicy(gnssPositionPolicy);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!fromSdr && (policy == QStringLiteral("ubx_only") ||
        (policy == QStringLiteral("ubx_preferred") &&
         gnssLastUbxFixMs > 0 &&
         nowMs - gnssLastUbxFixMs <= 3000))) {
        if (fobosVerboseLoggingEnabled() &&
            (gnssSerialSentenceCount <= 4 || (gnssSerialSentenceCount % 64) == 0)) {
            qDebug() << "[GNSS policy] NMEA fix ignored"
                     << "policy" << policy
                     << "lastUbxAgeMs" << (gnssLastUbxFixMs > 0 ? nowMs - gnssLastUbxFixMs : -1);
        }
        return;
    }
    if (!hasEnabledGnssNmeaFixSatellite()) {
        updateGnssSerialStatus(uiText(QStringLiteral("gnss_serial_fix_filtered"),
                                      QStringLiteral("NMEA serial: fix ignored by satellite filters")));
        return;
    }
    if (applyNmeaPositionText(trimmed, &status, false)) {
        ++gnssSerialFixCount;
        QStringList extras;
        if (std::isfinite(gnssSerialHdop)) extras << QStringLiteral("HDOP %1").arg(gnssSerialHdop, 0, 'f', 1);
        if (std::isfinite(gnssSerialAltitudeM)) extras << QStringLiteral("alt %1 m").arg(gnssSerialAltitudeM, 0, 'f', 1);
        if (std::isfinite(gnssSerialSpeedKmh)) extras << QStringLiteral("speed %1 km/h").arg(gnssSerialSpeedKmh, 0, 'f', 1);
        const QString suffix = extras.isEmpty() ? QString() : QStringLiteral(", %1").arg(extras.join(QStringLiteral(", ")));
        updateGnssSerialStatus(uiText(QStringLiteral("gnss_serial_fix"),
                                      QStringLiteral("NMEA serial: %1 fix %2, %3 (%4), #%5"))
                                   .arg(QStringLiteral("%1%2").arg(parsed.talker, parsed.sentence))
                                   .arg(qthLatitude, 0, 'f', 6)
                                   .arg(qthLongitude, 0, 'f', 6)
                                   .arg(qth::maidenheadLocator(qthLatitude, qthLongitude, 6))
                                   .arg(QStringLiteral("%1%2").arg(gnssSerialFixCount).arg(suffix)));
    }
}

void YourClassName::processGnssUbxFrame(quint8 messageClass, quint8 messageId, const QByteArray &payload) {
    ++gnssSerialUbxFrameCount;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (messageClass == 0x05 && payload.size() >= 2) {
        const QString ackType = messageId == 0x01 ? QStringLiteral("ACK")
                                                  : messageId == 0x00 ? QStringLiteral("NAK")
                                                                      : QStringLiteral("ACK?");
        const quint8 ackClass = static_cast<quint8>(payload.at(0));
        const quint8 ackId = static_cast<quint8>(payload.at(1));
        qDebug() << "[GNSS UBX]" << ackType
                 << "class" << QStringLiteral("0x%1").arg(ackClass, 2, 16, QLatin1Char('0'))
                 << "id" << QStringLiteral("0x%1").arg(ackId, 2, 16, QLatin1Char('0'));
        if (ackClass == 0x06 && (ackId == 0x3E || ackId == 0x09)) {
            const QString key = ackId == 0x3E ? QStringLiteral("ubx_cfg_gnss_ack")
                                              : QStringLiteral("ubx_cfg_save_ack");
            const QString fallback = ackId == 0x3E
                                         ? QStringLiteral("UBX CFG-GNSS: %1")
                                         : QStringLiteral("UBX save config: %1");
            updateGnssSerialStatus(uiText(key, fallback).arg(ackType));
        }
        return;
    }

    if (messageClass == 0x06 && messageId == 0x3E) {
        gnssLastCfgGnssPayload = payload;
        const bool applyPendingConfig = gnssPendingCfgGnssApply;
        QStringList enabledSystems;
        QStringList blockDetails;
        QMap<QString, bool> moduleSystemEnabled;
        if (payload.size() >= 4) {
            const int blockCount = static_cast<quint8>(payload.at(3));
            for (int block = 0; block < blockCount; ++block) {
                const int offset = 4 + block * 8;
                if (offset + 7 >= payload.size()) {
                    break;
                }
                const quint8 gnssId = static_cast<quint8>(payload.at(offset));
                const int reservedChannels = static_cast<quint8>(payload.at(offset + 1));
                const int maxChannels = static_cast<quint8>(payload.at(offset + 2));
                const quint32 flags = ubxU4(payload, offset + 4);
                const QString systemName = ubxGnssSystemName(gnssId);
                const bool enabled = (flags & 0x01u) != 0;
                moduleSystemEnabled.insert(systemName, enabled);
                blockDetails << QStringLiteral("%1:%2/res%3/max%4/flags0x%5")
                                    .arg(systemName,
                                         enabled ? QStringLiteral("on") : QStringLiteral("off"))
                                    .arg(reservedChannels)
                                    .arg(maxChannels)
                                    .arg(flags, 8, 16, QLatin1Char('0'));
                if (enabled) {
                    enabledSystems << systemName;
                }
            }
        }
        if (applyPendingConfig) {
            gnssPendingCfgGnssApply = false;
            qDebug() << "[GNSS UBX] CFG-GNSS poll"
                     << "payload" << payload.size()
                     << "enabled" << enabledSystems
                     << "blocks" << blockDetails
                     << "pendingApply" << true;
            applyGnssUbxConstellationConfig();
            return;
        }
        auto syncSystem = [&moduleSystemEnabled](const QString &system, bool *target) {
            if (!target || !moduleSystemEnabled.contains(system)) {
                return;
            }
            *target = moduleSystemEnabled.value(system);
        };
        syncSystem(QStringLiteral("GPS"), &gnssUseGps);
        syncSystem(QStringLiteral("GLONASS"), &gnssUseGlonass);
        syncSystem(QStringLiteral("Galileo"), &gnssUseGalileo);
        syncSystem(QStringLiteral("BeiDou"), &gnssUseBeidou);
        syncSystem(QStringLiteral("QZSS"), &gnssUseQzss);
        syncSystem(QStringLiteral("SBAS"), &gnssUseSbas);
        updateGnssSatelliteView(true);
        qDebug() << "[GNSS UBX] CFG-GNSS poll"
                 << "payload" << payload.size()
                 << "enabled" << enabledSystems
                 << "blocks" << blockDetails;
        gnssPendingCfgGnssApply = false;
        updateGnssSerialStatus(uiText(QStringLiteral("ubx_cfg_gnss_polled"),
                                      QStringLiteral("UBX CFG-GNSS received: %1. Adjust system checkboxes and press UBX sys again to apply."))
                                   .arg(enabledSystems.isEmpty()
                                            ? uiText(QStringLiteral("none"), QStringLiteral("none"))
                                            : enabledSystems.join(QStringLiteral(", "))));
        return;
    }

    if (messageClass != 0x01) {
        return;
    }

    if (messageId == 0x35 && payload.size() >= 8) {
        const int satelliteCount = static_cast<quint8>(payload.at(5));
        QMap<QString, int> constellationCounts;
        int usedCount = 0;
        for (int i = 0; i < satelliteCount; ++i) {
            const int offset = 8 + i * 12;
            if (offset + 11 >= payload.size()) {
                break;
            }
            const quint8 gnssId = static_cast<quint8>(payload.at(offset));
            const int svId = static_cast<quint8>(payload.at(offset + 1));
            const int cn0 = static_cast<quint8>(payload.at(offset + 2));
            const int elevation = static_cast<qint8>(payload.at(offset + 3));
            const int azimuth = ubxI2(payload, offset + 4);
            const quint32 flags = ubxU4(payload, offset + 8);
            const QString system = ubxGnssSystemName(gnssId);
            const QString key = ubxSatelliteKey(gnssId, svId);

            GnssNmeaSatellite satellite = gnssNmeaSatellites.value(key);
            satellite.key = key;
            satellite.source = QStringLiteral("UBX");
            satellite.system = system;
            satellite.talker = ubxGnssTalker(gnssId);
            satellite.prn = svId;
            satellite.elevationDeg = elevation;
            satellite.azimuthDeg = azimuth;
            satellite.cn0DbHz = cn0;
            satellite.usedInFix = (flags & 0x00000008u) != 0;
            satellite.lastSeenMs = nowMs;
            gnssNmeaSatellites.insert(key, satellite);
            if (!gnssNmeaSatelliteEnabled.contains(key)) {
                gnssNmeaSatelliteEnabled.insert(key, !gnssDisabledSatelliteKeys.contains(key));
            }
            constellationCounts[system] = constellationCounts.value(system) + 1;
            if (satellite.usedInFix) {
                ++usedCount;
            }
        }

        gnssSerialSatellitesUsed = usedCount;
        gnssSerialGpsSatellites = constellationCounts.value(QStringLiteral("GPS"), -1);
        gnssSerialGlonassSatellites = constellationCounts.value(QStringLiteral("GLONASS"), -1);
        gnssSerialGalileoSatellites = constellationCounts.value(QStringLiteral("Galileo"), -1);
        gnssSerialBeidouSatellites = constellationCounts.value(QStringLiteral("BeiDou"), -1);
        gnssSerialOtherSatellites =
            constellationCounts.value(QStringLiteral("QZSS"), 0) +
            constellationCounts.value(QStringLiteral("SBAS"), 0) +
            constellationCounts.value(QStringLiteral("Other"), 0);

        if (fobosVerboseLoggingEnabled() &&
            (gnssSerialUbxFrameCount <= 8 || (gnssSerialUbxFrameCount % 64) == 0)) {
            qDebug() << "[GNSS UBX] NAV-SAT"
                     << "sats" << satelliteCount
                     << "used" << usedCount
                     << "gps" << gnssSerialGpsSatellites
                     << "glo" << gnssSerialGlonassSatellites
                     << "gal" << gnssSerialGalileoSatellites
                     << "bds" << gnssSerialBeidouSatellites;
        }

        const qint64 staleBeforeMs = nowMs - 30000;
        for (auto it = gnssNmeaSatellites.begin(); it != gnssNmeaSatellites.end();) {
            if (it->lastSeenMs > 0 && it->lastSeenMs < staleBeforeMs) {
                gnssNmeaSatelliteEnabled.remove(it.key());
                it = gnssNmeaSatellites.erase(it);
            } else {
                ++it;
            }
        }
        updateGnssSatelliteView();
        return;
    }

    if (messageId == 0x04 && payload.size() >= 18) {
        gnssSerialPdop = static_cast<double>(ubxU2(payload, 6)) * 0.01;
        gnssSerialVdop = static_cast<double>(ubxU2(payload, 10)) * 0.01;
        gnssSerialHdop = static_cast<double>(ubxU2(payload, 12)) * 0.01;
        if (fobosVerboseLoggingEnabled() &&
            (gnssSerialUbxFrameCount <= 8 || (gnssSerialUbxFrameCount % 64) == 0)) {
            qDebug() << "[GNSS UBX] NAV-DOP"
                     << "hdop" << gnssSerialHdop
                     << "vdop" << gnssSerialVdop
                     << "pdop" << gnssSerialPdop;
        }
        updateGnssSatelliteView();
        return;
    }

    if (messageId == 0x07 && payload.size() >= 92) {
        const int year = ubxU2(payload, 4);
        const int month = static_cast<quint8>(payload.at(6));
        const int day = static_cast<quint8>(payload.at(7));
        const int hour = static_cast<quint8>(payload.at(8));
        const int minute = static_cast<quint8>(payload.at(9));
        const int second = static_cast<quint8>(payload.at(10));
        gnssSerialUtc = QStringLiteral("%1-%2-%3T%4:%5:%6Z")
                            .arg(year, 4, 10, QLatin1Char('0'))
                            .arg(month, 2, 10, QLatin1Char('0'))
                            .arg(day, 2, 10, QLatin1Char('0'))
                            .arg(hour, 2, 10, QLatin1Char('0'))
                            .arg(minute, 2, 10, QLatin1Char('0'))
                            .arg(second, 2, 10, QLatin1Char('0'));

        const int fixType = static_cast<quint8>(payload.at(20));
        const quint8 flags = static_cast<quint8>(payload.at(21));
        const bool fixOk = (flags & 0x01u) != 0;
        gnssSerialFixQuality = fixOk ? fixType : 0;
        gnssSerialFixMode = fixType;
        gnssSerialSatellitesUsed = static_cast<quint8>(payload.at(23));
        const double lon = static_cast<double>(ubxI4(payload, 24)) * 1e-7;
        const double lat = static_cast<double>(ubxI4(payload, 28)) * 1e-7;
        gnssSerialAltitudeM = static_cast<double>(ubxI4(payload, 36)) / 1000.0;
        gnssSerialSpeedKmh = static_cast<double>(ubxI4(payload, 60)) * 0.0036;
        gnssSerialCourseDeg = static_cast<double>(ubxI4(payload, 64)) * 1e-5;
        gnssSerialPdop = static_cast<double>(ubxU2(payload, 76)) * 0.01;

        if (fixOk && fixType >= 2 && std::isfinite(lat) && std::isfinite(lon) &&
            lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
            gnssLastUbxFixMs = nowMs;
            if (fobosVerboseLoggingEnabled() &&
                (gnssSerialUbxFrameCount <= 8 || (gnssSerialUbxFrameCount % 32) == 0)) {
                qDebug() << "[GNSS UBX] NAV-PVT"
                         << "fix" << fixType
                         << "sv" << gnssSerialSatellitesUsed
                         << "qth" << qth::maidenheadLocator(lat, lon, 6)
                         << "altM" << gnssSerialAltitudeM
                         << "pdop" << gnssSerialPdop;
            }
            const QString policy = normalizedGnssPositionPolicy(gnssPositionPolicy);
            if (policy == QStringLiteral("nmea_only")) {
                if (fobosVerboseLoggingEnabled() &&
                    (gnssSerialUbxFrameCount <= 8 || (gnssSerialUbxFrameCount % 64) == 0)) {
                    qDebug() << "[GNSS policy] UBX PVT ignored"
                             << "policy" << policy;
                }
                updateGnssSatelliteView();
                return;
            }
            if (!hasEnabledGnssNmeaFixSatellite()) {
                updateGnssSerialStatus(uiText(QStringLiteral("gnss_serial_fix_filtered"),
                                              QStringLiteral("NMEA serial: fix ignored by satellite filters")));
                return;
            }
            qthLatitude = lat;
            qthLongitude = lon;
            qthSource = QStringLiteral("nmea");
            qthPositionVisible = true;
            ++gnssSerialFixCount;
            updateQthControls();
            if (qthMapWidget) {
                qthMapWidget->centerOn(qthLatitude, qthLongitude);
            }
            const QString locator = qth::maidenheadLocator(qthLatitude, qthLongitude, 6);
            QStringList extras;
            extras << QStringLiteral("UBX frames %1").arg(gnssSerialUbxFrameCount);
            extras << QStringLiteral("SV %1").arg(gnssSerialSatellitesUsed);
            if (std::isfinite(gnssSerialPdop)) extras << QStringLiteral("PDOP %1").arg(gnssSerialPdop, 0, 'f', 1);
            if (std::isfinite(gnssSerialAltitudeM)) extras << QStringLiteral("alt %1 m").arg(gnssSerialAltitudeM, 0, 'f', 1);
            if (std::isfinite(gnssSerialSpeedKmh)) extras << QStringLiteral("speed %1 km/h").arg(gnssSerialSpeedKmh, 0, 'f', 1);
            updateGnssSerialStatus(uiText(QStringLiteral("ubx_fix"),
                                          QStringLiteral("UBX PVT: fix %1, %2, %3 (%4), #%5, %6"))
                                       .arg(fixType)
                                       .arg(qthLatitude, 0, 'f', 6)
                                       .arg(qthLongitude, 0, 'f', 6)
                                       .arg(locator)
                                       .arg(gnssSerialFixCount)
                                       .arg(extras.join(QStringLiteral(", "))));
            updateQthMapSatelliteOverlay();
        } else if ((gnssSerialUbxFrameCount % 16) == 0) {
            updateGnssSerialStatus(uiText(QStringLiteral("ubx_receiving"),
                                          QStringLiteral("UBX: receiving %1 frames, fix %2, SV %3"))
                                       .arg(gnssSerialUbxFrameCount)
                                       .arg(fixType)
                                       .arg(gnssSerialSatellitesUsed));
        }
        updateGnssSatelliteView();
    }
}

void YourClassName::sendGnssUbxConfiguration(bool enabled) {
    if (!gnssSerialPort || !gnssSerialPort->isOpen() || !gnssSerialPort->isWritable()) {
        gnssUbxOutputEnabled = false;
        updateGnssSerialStatus(uiText(QStringLiteral("ubx_requires_serial"),
                                      QStringLiteral("UBX: connect the GNSS serial port first.")));
        return;
    }

    const quint8 rate = enabled ? 1 : 0;
    const QVector<QPair<quint8, quint8>> messages = {
        {0x01, 0x07}, // NAV-PVT
        {0x01, 0x35}, // NAV-SAT
        {0x01, 0x04}, // NAV-DOP
        {0x01, 0x03}  // NAV-STATUS
    };

    qint64 bytesWritten = 0;
    for (const auto &message : messages) {
        QByteArray payload;
        payload.reserve(8);
        payload.append(char(message.first));
        payload.append(char(message.second));
        payload.append(char(0));    // I2C/DDC
        payload.append(char(rate)); // UART1
        payload.append(char(rate)); // UART2
        payload.append(char(rate)); // USB
        payload.append(char(0));    // SPI
        payload.append(char(0));
        const QByteArray frame = makeUbxFrame(0x06, 0x01, payload);
        bytesWritten += gnssSerialPort->write(frame);
    }
    gnssSerialPort->flush();

    gnssUbxOutputEnabled = enabled;
    updateGnssSerialStatus(uiText(enabled ? QStringLiteral("ubx_enabled") : QStringLiteral("ubx_disabled"),
                                  enabled ? QStringLiteral("UBX output enabled (%1 bytes sent).")
                                          : QStringLiteral("UBX output disabled (%1 bytes sent)."))
                               .arg(bytesWritten));
    qDebug() << "[GNSS UBX] CFG-MSG"
             << "enabled" << enabled
             << "bytes" << bytesWritten;
}

void YourClassName::applyGnssUbxConstellationConfig() {
    if (!gnssSerialPort || !gnssSerialPort->isOpen() || !gnssSerialPort->isWritable()) {
        updateGnssSerialStatus(uiText(QStringLiteral("ubx_requires_serial"),
                                      QStringLiteral("UBX: connect the GNSS serial port first.")));
        return;
    }
    if (!gnssUbxOutputEnabled) {
        sendGnssUbxConfiguration(true);
    }

    bool applyGps = gnssUseGps;
    bool applySbas = gnssUseSbas;
    bool applyGalileo = gnssUseGalileo;
    bool applyBeidou = gnssUseBeidou;
    bool applyQzss = gnssUseQzss;
    bool applyGlonass = gnssUseGlonass;
    QStringList safetyNotes;
    const bool m8nSafeReduced = normalizeM8nUbxConstellationSelection(applyGps,
                                                                      applySbas,
                                                                      applyGalileo,
                                                                      applyBeidou,
                                                                      applyQzss,
                                                                      applyGlonass,
                                                                      &safetyNotes);
    if (m8nSafeReduced) {
        gnssUseGps = applyGps;
        gnssUseSbas = applySbas;
        gnssUseGalileo = applyGalileo;
        gnssUseBeidou = applyBeidou;
        gnssUseQzss = applyQzss;
        gnssUseGlonass = applyGlonass;
        updateGnssSatelliteView(true);
        savePersistentSettings();
    }

    auto uiEnabledForGnssId = [applyGps,
                               applySbas,
                               applyGalileo,
                               applyBeidou,
                               applyQzss,
                               applyGlonass](quint8 gnssId) {
        switch (gnssId) {
        case 0:
            return applyGps;
        case 1:
            return applySbas;
        case 2:
            return applyGalileo;
        case 3:
            return applyBeidou;
        case 5:
            return applyQzss;
        case 6:
            return applyGlonass;
        default:
            return false;
        }
    };

    if (gnssLastCfgGnssPayload.size() < 4) {
        gnssPendingCfgGnssApply = true;
        const QByteArray poll = makeUbxFrame(0x06, 0x3E, QByteArray());
        const qint64 bytes = gnssSerialPort->write(poll);
        gnssSerialPort->flush();
        updateGnssSerialStatus(uiText(QStringLiteral("ubx_cfg_gnss_polling"),
                                      QStringLiteral("UBX CFG-GNSS: polling module first; selected systems will be applied when the response arrives (%1 bytes)."))
                                   .arg(bytes));
        qDebug() << "[GNSS UBX] CFG-GNSS poll requested" << "bytes" << bytes << "pendingApply" << true;
        return;
    }

    QByteArray payload = gnssLastCfgGnssPayload;
    const int blockCount = static_cast<quint8>(payload.at(3));
    QStringList enabledSystems;
    QStringList disabledSystems;
    int changedBlocks = 0;
    for (int block = 0; block < blockCount; ++block) {
        const int offset = 4 + block * 8;
        if (offset + 7 >= payload.size()) {
            break;
        }
        const quint8 gnssId = static_cast<quint8>(payload.at(offset));
        if (ubxGnssSystemName(gnssId) == QStringLiteral("Other")) {
            continue;
        }
        quint32 flags = ubxU4(payload, offset + 4);
        const quint32 oldFlags = flags;
        const bool enabled = uiEnabledForGnssId(gnssId);
        if (enabled) {
            flags |= 0x01u;
            enabledSystems << ubxGnssSystemName(gnssId);
        } else {
            flags &= ~0x01u;
            disabledSystems << ubxGnssSystemName(gnssId);
        }
        if (flags != oldFlags) {
            ++changedBlocks;
            ubxPutU4(payload, offset + 4, flags);
        }
    }

    const QByteArray frame = makeUbxFrame(0x06, 0x3E, payload);
    const qint64 bytes = gnssSerialPort->write(frame);
    gnssSerialPort->flush();
    QString status = uiText(QStringLiteral("ubx_cfg_gnss_sent"),
                            QStringLiteral("UBX CFG-GNSS sent: enabled %1, disabled %2 (%3 blocks changed)."))
                         .arg(enabledSystems.isEmpty()
                                  ? uiText(QStringLiteral("none"), QStringLiteral("none"))
                                  : enabledSystems.join(QStringLiteral(", ")))
                         .arg(disabledSystems.isEmpty()
                                  ? uiText(QStringLiteral("none"), QStringLiteral("none"))
                                  : disabledSystems.join(QStringLiteral(", ")))
                         .arg(changedBlocks);
    if (m8nSafeReduced) {
        status += QLatin1Char(' ');
        status += uiText(QStringLiteral("ubx_cfg_gnss_m8n_safe"),
                         QStringLiteral("M8N-safe profile applied."));
    }
    updateGnssSerialStatus(status);
    qDebug() << "[GNSS UBX] CFG-GNSS sent"
             << "bytes" << bytes
             << "blocks" << blockCount
             << "changed" << changedBlocks
             << "enabled" << enabledSystems
             << "disabled" << disabledSystems
             << "m8nSafeReduced" << m8nSafeReduced
             << "notes" << safetyNotes;
}

void YourClassName::saveGnssUbxConfigurationToModule() {
    if (!gnssSerialPort || !gnssSerialPort->isOpen() || !gnssSerialPort->isWritable()) {
        updateGnssSerialStatus(uiText(QStringLiteral("ubx_requires_serial"),
                                      QStringLiteral("UBX: connect the GNSS serial port first.")));
        return;
    }

    QByteArray payload;
    payload.reserve(13);
    ubxAppendU4(payload, 0x00000000u); // clearMask
    ubxAppendU4(payload, 0x0000FFFFu); // saveMask
    ubxAppendU4(payload, 0x00000000u); // loadMask
    payload.append(char(0x17));        // BBR, Flash, EEPROM, SPI flash when present

    const QByteArray frame = makeUbxFrame(0x06, 0x09, payload);
    const qint64 bytes = gnssSerialPort->write(frame);
    gnssSerialPort->flush();
    updateGnssSerialStatus(uiText(QStringLiteral("ubx_cfg_save_sent"),
                                  QStringLiteral("UBX save config sent (%1 bytes). Watch for ACK/NAK.")).
                               arg(bytes));
    qDebug() << "[GNSS UBX] CFG-CFG save sent" << "bytes" << bytes;
}

void YourClassName::handleGnssSerialError(QSerialPort::SerialPortError error) {
    if (error == QSerialPort::NoError || error == QSerialPort::TimeoutError) {
        return;
    }
    const QString message = uiText(QStringLiteral("gnss_serial_error"),
                                   QStringLiteral("NMEA serial error: %1"))
                                .arg(gnssSerialPort ? gnssSerialPort->errorString() : QString::number(error));
    updateGnssSerialStatus(message);
    qDebug() << "[GNSS serial] error"
             << "code" << error
             << "message" << (gnssSerialPort ? gnssSerialPort->errorString() : QString());
}

void YourClassName::updateGnssSerialStatus(const QString &message) {
    if (gnssSerialStatusLabel) {
        gnssSerialStatusLabel->setText(message);
    }
    if (gnssSerialButton) {
        const bool connected = gnssSerialPort && gnssSerialPort->isOpen();
        gnssSerialButton->setText(connected
                                      ? uiText(QStringLiteral("disconnect"), QStringLiteral("Disconnect"))
                                      : uiText(QStringLiteral("connect"), QStringLiteral("Connect")));
    }
    if (gnssNmeaLogButton) {
        gnssNmeaLogButton->setText(gnssNmeaLogFile && gnssNmeaLogFile->isOpen()
                                       ? uiText(QStringLiteral("nmea_stop_log"), QStringLiteral("Stop log"))
                                       : uiText(QStringLiteral("nmea_log"), QStringLiteral("Log")));
    }
    if (gnssSerialRawLogButton) {
        gnssSerialRawLogButton->setText(gnssRawSerialLogFile && gnssRawSerialLogFile->isOpen()
                                            ? uiText(QStringLiteral("gnss_raw_serial_stop"), QStringLiteral("Stop raw"))
                                            : uiText(QStringLiteral("gnss_raw_serial_log"), QStringLiteral("Raw")));
    }
}

void YourClassName::toggleGnssNmeaLogging() {
    if (gnssNmeaLogFile && gnssNmeaLogFile->isOpen()) {
        gnssNmeaLogFile->flush();
        gnssNmeaLogFile->close();
        const QString savedPath = gnssNmeaLogPath;
        gnssNmeaLogFile.reset();
        gnssNmeaLogPath.clear();
        updateGnssSerialStatus(uiText(QStringLiteral("nmea_log_saved"),
                                      QStringLiteral("NMEA log saved: %1"))
                                   .arg(QDir::toNativeSeparators(savedPath)));
        return;
    }

    QDir recordingsDir(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("recordings/nmea")));
    if (!recordingsDir.exists() && !recordingsDir.mkpath(QStringLiteral("."))) {
        updateGnssSerialStatus(uiText(QStringLiteral("nmea_log_failed"),
                                      QStringLiteral("NMEA log failed: %1"))
                                   .arg(QStringLiteral("cannot create recordings/nmea")));
        return;
    }

    const QString fileName =
        QStringLiteral("nmea_%1.log").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    gnssNmeaLogPath = recordingsDir.filePath(fileName);
    gnssNmeaLogFile = std::make_unique<QFile>(gnssNmeaLogPath);
    if (!gnssNmeaLogFile->open(QIODevice::WriteOnly | QIODevice::Text)) {
        const QString error = gnssNmeaLogFile->errorString();
        gnssNmeaLogFile.reset();
        updateGnssSerialStatus(uiText(QStringLiteral("nmea_log_failed"),
                                      QStringLiteral("NMEA log failed: %1"))
                                   .arg(error));
        return;
    }
    gnssNmeaLogFile->write("# Obrii SDR NMEA log\n");
    gnssNmeaLogFile->write(QStringLiteral("# UTC %1\n")
                               .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
                               .toLatin1());
    updateGnssSerialStatus(uiText(QStringLiteral("nmea_log_started"),
                                  QStringLiteral("NMEA log started: %1"))
                               .arg(QDir::toNativeSeparators(gnssNmeaLogPath)));
}

void YourClassName::toggleGnssRawSerialLogging() {
    if (gnssRawSerialLogFile && gnssRawSerialLogFile->isOpen()) {
        gnssRawSerialLogFile->flush();
        gnssRawSerialLogFile->close();
        const QString savedPath = gnssRawSerialLogPath;
        gnssRawSerialLogFile.reset();
        gnssRawSerialLogPath.clear();
        updateGnssSerialStatus(uiText(QStringLiteral("gnss_raw_serial_saved"),
                                      QStringLiteral("Raw UBX/NMEA log saved: %1"))
                                   .arg(QDir::toNativeSeparators(savedPath)));
        return;
    }

    QDir recordingsDir(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("recordings/gnss_raw")));
    if (!recordingsDir.exists() && !recordingsDir.mkpath(QStringLiteral("."))) {
        updateGnssSerialStatus(uiText(QStringLiteral("gnss_raw_serial_failed"),
                                      QStringLiteral("Raw UBX/NMEA log failed: %1"))
                                   .arg(QStringLiteral("cannot create recordings/gnss_raw")));
        return;
    }

    const QString fileName =
        QStringLiteral("gnss_raw_%1.ubx")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    gnssRawSerialLogPath = recordingsDir.filePath(fileName);
    gnssRawSerialLogFile = std::make_unique<QFile>(gnssRawSerialLogPath);
    if (!gnssRawSerialLogFile->open(QIODevice::WriteOnly)) {
        const QString error = gnssRawSerialLogFile->errorString();
        gnssRawSerialLogFile.reset();
        updateGnssSerialStatus(uiText(QStringLiteral("gnss_raw_serial_failed"),
                                      QStringLiteral("Raw UBX/NMEA log failed: %1"))
                                   .arg(error));
        return;
    }
    updateGnssSerialStatus(uiText(QStringLiteral("gnss_raw_serial_started"),
                                  QStringLiteral("Raw UBX/NMEA log started: %1"))
                               .arg(QDir::toNativeSeparators(gnssRawSerialLogPath)));
}

void YourClassName::replayGnssNmeaLog() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        uiText(QStringLiteral("nmea_replay_select"), QStringLiteral("Select NMEA log")),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("recordings/nmea")),
        uiText(QStringLiteral("nmea_log_filter"), QStringLiteral("NMEA logs (*.log *.nmea *.txt);;All files (*.*)")));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        updateGnssSerialStatus(uiText(QStringLiteral("nmea_replay_failed"),
                                      QStringLiteral("NMEA replay failed: %1"))
                                   .arg(file.errorString()));
        return;
    }

    gnssSerialBuffer.clear();
    gnssSerialSentenceCount = 0;
    gnssSerialFixCount = 0;
    gnssSerialFixQuality = -1;
    gnssSerialFixMode = -1;
    gnssSerialSatellitesUsed = -1;
    gnssSerialGpsSatellites = -1;
    gnssSerialGlonassSatellites = -1;
    gnssSerialGalileoSatellites = -1;
    gnssSerialBeidouSatellites = -1;
    gnssSerialOtherSatellites = -1;
    gnssSerialUtc.clear();
    gnssSerialHdop = std::numeric_limits<double>::quiet_NaN();
    gnssSerialVdop = std::numeric_limits<double>::quiet_NaN();
    gnssSerialPdop = std::numeric_limits<double>::quiet_NaN();
    gnssSerialAltitudeM = std::numeric_limits<double>::quiet_NaN();
    gnssSerialGeoidSeparationM = std::numeric_limits<double>::quiet_NaN();
    gnssSerialSpeedKmh = std::numeric_limits<double>::quiet_NaN();
    gnssSerialCourseDeg = std::numeric_limits<double>::quiet_NaN();
    resetGnssNmeaSatellites();

    int lines = 0;
    while (!file.atEnd()) {
        const QString line = QString::fromLatin1(file.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('$'))) {
            processGnssNmeaLine(line, true);
            ++lines;
        }
    }
    updateGnssSatelliteView(true);
    updateQthControls();
    updateGnssSerialStatus(uiText(QStringLiteral("nmea_replay_done"),
                                  QStringLiteral("NMEA replay: %1 sentences from %2"))
                               .arg(lines)
                               .arg(QFileInfo(path).fileName()));
}
