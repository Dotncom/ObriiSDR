#include "gnssserialutils.h"

#include <QDate>
#include <QDateTime>
#include <QTime>

#include <algorithm>
#include <cmath>

int parseNmeaIntField(const QStringList &fields, int index, int fallback) {
    if (index < 0 || index >= fields.size()) {
        return fallback;
    }
    bool ok = false;
    const int value = fields.at(index).trimmed().toInt(&ok);
    return ok ? value : fallback;
}

double parseNmeaDoubleField(const QStringList &fields, int index, double fallback) {
    if (index < 0 || index >= fields.size()) {
        return fallback;
    }
    bool ok = false;
    const double value = fields.at(index).trimmed().toDouble(&ok);
    return ok ? value : fallback;
}

QStringList nmeaBodyFields(const QString &line) {
    QString sentence = line.trimmed();
    if (sentence.isEmpty()) {
        return {};
    }
    const int start = sentence.indexOf(QLatin1Char('$'));
    if (start >= 0) {
        sentence = sentence.mid(start + 1);
    } else if (sentence.startsWith(QLatin1Char('!'))) {
        sentence.remove(0, 1);
    }
    const int checksumPos = sentence.indexOf(QLatin1Char('*'));
    const QString body = checksumPos >= 0 ? sentence.left(checksumPos) : sentence;
    return body.split(QLatin1Char(','));
}

QString gnssSystemForTalker(const QString &talker, int prn) {
    const QString upper = talker.trimmed().toUpper();
    if (upper == QStringLiteral("GP")) {
        if (prn >= 33 && prn <= 64) {
            return QStringLiteral("SBAS");
        }
        return QStringLiteral("GPS");
    }
    if (upper == QStringLiteral("GL")) {
        return QStringLiteral("GLONASS");
    }
    if (upper == QStringLiteral("GA")) {
        return QStringLiteral("Galileo");
    }
    if (upper == QStringLiteral("GB") || upper == QStringLiteral("BD")) {
        return QStringLiteral("BeiDou");
    }
    if (upper == QStringLiteral("GQ")) {
        return QStringLiteral("QZSS");
    }
    if (upper == QStringLiteral("GN")) {
        return QStringLiteral("Mixed");
    }
    return QStringLiteral("Other");
}

QString gnssSatelliteKey(const QString &talker, int prn) {
    return QStringLiteral("%1:%2").arg(gnssSystemForTalker(talker, prn)).arg(prn);
}

quint16 ubxU2(const QByteArray &bytes, int offset) {
    if (offset < 0 || offset + 1 >= bytes.size()) {
        return 0;
    }
    return static_cast<quint16>(static_cast<quint8>(bytes.at(offset))) |
           static_cast<quint16>(static_cast<quint8>(bytes.at(offset + 1)) << 8);
}

quint32 ubxU4(const QByteArray &bytes, int offset) {
    if (offset < 0 || offset + 3 >= bytes.size()) {
        return 0;
    }
    return static_cast<quint32>(static_cast<quint8>(bytes.at(offset))) |
           (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 1))) << 8) |
           (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 2))) << 16) |
           (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 3))) << 24);
}

void ubxPutU4(QByteArray &bytes, int offset, quint32 value) {
    if (offset < 0 || offset + 3 >= bytes.size()) {
        return;
    }
    bytes[offset] = char(value & 0xFFu);
    bytes[offset + 1] = char((value >> 8) & 0xFFu);
    bytes[offset + 2] = char((value >> 16) & 0xFFu);
    bytes[offset + 3] = char((value >> 24) & 0xFFu);
}

void ubxAppendU4(QByteArray &bytes, quint32 value) {
    bytes.append(char(value & 0xFFu));
    bytes.append(char((value >> 8) & 0xFFu));
    bytes.append(char((value >> 16) & 0xFFu));
    bytes.append(char((value >> 24) & 0xFFu));
}

qint16 ubxI2(const QByteArray &bytes, int offset) {
    return static_cast<qint16>(ubxU2(bytes, offset));
}

qint32 ubxI4(const QByteArray &bytes, int offset) {
    return static_cast<qint32>(ubxU4(bytes, offset));
}

QByteArray makeUbxFrame(quint8 messageClass, quint8 messageId, const QByteArray &payload) {
    QByteArray frame;
    frame.reserve(payload.size() + 8);
    frame.append(char(0xB5));
    frame.append(char(0x62));
    frame.append(char(messageClass));
    frame.append(char(messageId));
    frame.append(char(payload.size() & 0xFF));
    frame.append(char((payload.size() >> 8) & 0xFF));
    frame.append(payload);
    quint8 ckA = 0;
    quint8 ckB = 0;
    for (int i = 2; i < frame.size(); ++i) {
        ckA = static_cast<quint8>(ckA + static_cast<quint8>(frame.at(i)));
        ckB = static_cast<quint8>(ckB + ckA);
    }
    frame.append(char(ckA));
    frame.append(char(ckB));
    return frame;
}

bool hasValidUbxChecksum(const QByteArray &frame) {
    if (frame.size() < 8 ||
        static_cast<quint8>(frame.at(0)) != 0xB5 ||
        static_cast<quint8>(frame.at(1)) != 0x62) {
        return false;
    }
    quint8 ckA = 0;
    quint8 ckB = 0;
    for (int i = 2; i < frame.size() - 2; ++i) {
        ckA = static_cast<quint8>(ckA + static_cast<quint8>(frame.at(i)));
        ckB = static_cast<quint8>(ckB + ckA);
    }
    return ckA == static_cast<quint8>(frame.at(frame.size() - 2)) &&
           ckB == static_cast<quint8>(frame.at(frame.size() - 1));
}

QString ubxGnssSystemName(quint8 gnssId) {
    switch (gnssId) {
    case 0:
        return QStringLiteral("GPS");
    case 1:
        return QStringLiteral("SBAS");
    case 2:
        return QStringLiteral("Galileo");
    case 3:
        return QStringLiteral("BeiDou");
    case 5:
        return QStringLiteral("QZSS");
    case 6:
        return QStringLiteral("GLONASS");
    default:
        return QStringLiteral("Other");
    }
}

QString ubxGnssTalker(quint8 gnssId) {
    switch (gnssId) {
    case 0:
        return QStringLiteral("GP");
    case 1:
        return QStringLiteral("SB");
    case 2:
        return QStringLiteral("GA");
    case 3:
        return QStringLiteral("GB");
    case 5:
        return QStringLiteral("GQ");
    case 6:
        return QStringLiteral("GL");
    default:
        return QStringLiteral("UX");
    }
}

QString ubxSatelliteKey(quint8 gnssId, int svId) {
    return QStringLiteral("UBX:%1:%2").arg(ubxGnssSystemName(gnssId)).arg(svId);
}

QString normalizedGnssPositionPolicy(QString policy) {
    policy = policy.trimmed().toLower();
    if (policy == QStringLiteral("ubx_preferred") ||
        policy == QStringLiteral("nmea_only") ||
        policy == QStringLiteral("ubx_only")) {
        return policy;
    }
    return QStringLiteral("auto");
}

QString formatGnssUtcForDisplay(const QString &rawUtc, int offsetMinutes) {
    const QString raw = rawUtc.trimmed();
    if (raw.isEmpty()) {
        return QStringLiteral("-");
    }

    QDateTime utcDateTime;
    QString fraction;
    if (raw.contains(QLatin1Char('T'))) {
        utcDateTime = QDateTime::fromString(raw, Qt::ISODateWithMs);
        if (!utcDateTime.isValid()) {
            utcDateTime = QDateTime::fromString(raw, Qt::ISODate);
        }
        utcDateTime.setTimeSpec(Qt::UTC);
    } else {
        const int dot = raw.indexOf(QLatin1Char('.'));
        const QString compact = dot >= 0 ? raw.left(dot) : raw;
        fraction = dot >= 0 ? raw.mid(dot) : QString();
        if (compact.size() >= 6) {
            const int hour = compact.mid(0, 2).toInt();
            const int minute = compact.mid(2, 2).toInt();
            const int second = compact.mid(4, 2).toInt();
            if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 60) {
                utcDateTime = QDateTime(QDate::currentDate(), QTime(hour, minute, (std::min)(second, 59)), Qt::UTC);
            }
        }
    }

    if (!utcDateTime.isValid()) {
        return raw;
    }

    QDateTime displayTime = utcDateTime;
    QString zone;
    if (offsetMinutes == 100000) {
        displayTime = utcDateTime.toLocalTime();
        zone = QStringLiteral("Local");
    } else {
        displayTime = utcDateTime.addSecs(offsetMinutes * 60);
        const int absMinutes = std::abs(offsetMinutes);
        zone = QStringLiteral("UTC%1%2:%3")
                   .arg(offsetMinutes >= 0 ? QStringLiteral("+") : QStringLiteral("-"))
                   .arg(absMinutes / 60, 2, 10, QLatin1Char('0'))
                   .arg(absMinutes % 60, 2, 10, QLatin1Char('0'));
    }

    if (fraction.size() > 4) {
        fraction = fraction.left(4);
    }
    return QStringLiteral("%1%2 %3")
        .arg(displayTime.time().toString(QStringLiteral("HH:mm:ss")))
        .arg(fraction)
        .arg(zone);
}

NmeaSerialStatus parseNmeaSerialStatusSentence(const QString &line) {
    NmeaSerialStatus status;
    const QStringList fields = nmeaBodyFields(line);
    if (fields.isEmpty()) {
        return status;
    }

    const QString type = fields.at(0).trimmed().toUpper();
    if (type.size() < 5) {
        return status;
    }
    status.talker = type.left(type.size() - 3);
    status.sentence = type.right(3);

    if (status.sentence == QStringLiteral("GGA")) {
        status.recognized = true;
        status.utc = fields.size() > 1 ? fields.at(1).trimmed() : QString();
        status.fixQuality = parseNmeaIntField(fields, 6);
        status.satellitesUsed = parseNmeaIntField(fields, 7);
        status.hdop = parseNmeaDoubleField(fields, 8);
        status.altitudeM = parseNmeaDoubleField(fields, 9);
        status.geoidSeparationM = parseNmeaDoubleField(fields, 11);
    } else if (status.sentence == QStringLiteral("RMC")) {
        status.recognized = true;
        status.utc = fields.size() > 1 ? fields.at(1).trimmed() : QString();
        status.fixQuality = (fields.size() > 2 && fields.at(2).trimmed().toUpper() == QStringLiteral("A")) ? 1 : 0;
        const double speedKnots = parseNmeaDoubleField(fields, 7);
        if (std::isfinite(speedKnots)) {
            status.speedKmh = speedKnots * 1.852;
        }
        status.courseDeg = parseNmeaDoubleField(fields, 8);
    } else if (status.sentence == QStringLiteral("GSA")) {
        status.recognized = true;
        status.fixMode = parseNmeaIntField(fields, 2);
        status.pdop = parseNmeaDoubleField(fields, 15);
        status.hdop = parseNmeaDoubleField(fields, 16);
        status.vdop = parseNmeaDoubleField(fields, 17);
    } else if (status.sentence == QStringLiteral("GSV")) {
        status.recognized = true;
        status.satellitesInView = parseNmeaIntField(fields, 3);
    } else if (status.sentence == QStringLiteral("GLL")) {
        status.recognized = true;
        status.utc = fields.size() > 5 ? fields.at(5).trimmed() : QString();
        status.fixQuality = (fields.size() > 6 && fields.at(6).trimmed().toUpper() == QStringLiteral("A")) ? 1 : 0;
    } else if (status.sentence == QStringLiteral("VTG")) {
        status.recognized = true;
        status.courseDeg = parseNmeaDoubleField(fields, 1, parseNmeaDoubleField(fields, 3));
        status.speedKmh = parseNmeaDoubleField(fields, 7);
    }

    return status;
}
