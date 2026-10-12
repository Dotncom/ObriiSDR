#include "ft8stationutils.h"

#include <QDataStream>
#include <QHash>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr quint32 WsjtMagic = 0xadbccbdaU;
constexpr quint32 WsjtSchema = 3U;

bool isGrid(const QString &value) {
    static const QRegularExpression pattern(
        QStringLiteral("^[A-R]{2}[0-9]{2}(?:[A-X]{2})?$"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(value).hasMatch();
}

bool isReport(const QString &value) {
    static const QRegularExpression pattern(QStringLiteral("^R?[+-][0-9]{2}$"));
    return pattern.match(value).hasMatch();
}

bool isCallsign(QString value) {
    value = value.trimmed().toUpper();
    if (value.startsWith(QLatin1Char('<')) && value.endsWith(QLatin1Char('>'))) {
        value = value.mid(1, value.size() - 2);
    }
    if (value.size() < 3 || value.size() > 15 || isGrid(value)) return false;
    static const QRegularExpression characters(QStringLiteral("^[A-Z0-9/]+$"));
    return characters.match(value).hasMatch() &&
           value.contains(QRegularExpression(QStringLiteral("[A-Z]"))) &&
           value.contains(QRegularExpression(QStringLiteral("[0-9]")));
}

QString cleanAdifValue(QString value) {
    value.replace(QLatin1Char('<'), QLatin1Char('('));
    value.replace(QLatin1Char('>'), QLatin1Char(')'));
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return value.trimmed();
}

QString adifField(const QString &name, const QString &rawValue) {
    const QString value = cleanAdifValue(rawValue);
    if (value.isEmpty()) return QString();
    return QStringLiteral("<%1:%2>%3").arg(name.toUpper()).arg(value.toUtf8().size()).arg(value);
}

void appendU16(QByteArray &data, quint16 value) {
    const quint16 network = qToBigEndian(value);
    data.append(reinterpret_cast<const char *>(&network), sizeof(network));
}

void appendU32(QByteArray &data, quint32 value) {
    const quint32 network = qToBigEndian(value);
    data.append(reinterpret_cast<const char *>(&network), sizeof(network));
}

void appendVariableString(QByteArray &data, const QString &value) {
    QByteArray utf8 = value.toUtf8();
    if (utf8.size() > 254) utf8.truncate(254);
    data.append(static_cast<char>(utf8.size()));
    data.append(utf8);
}

void padToFour(QByteArray &data) {
    while ((data.size() & 3) != 0) data.append('\0');
}

void patchU16(QByteArray &data, int offset, quint16 value) {
    const quint16 network = qToBigEndian(value);
    if (offset >= 0 && offset + 2 <= data.size()) {
        std::memcpy(data.data() + offset, &network, sizeof(network));
    }
}

void writeWsjtHeader(QDataStream &out, quint32 type, const QString &id) {
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_5_4);
    out << WsjtMagic << WsjtSchema << type << id.toUtf8();
}

QHash<QString, QString> parseAdifFields(const QByteArray &record) {
    QHash<QString, QString> fields;
    int position = 0;
    while (position < record.size()) {
        const int open = record.indexOf('<', position);
        if (open < 0) break;
        const int close = record.indexOf('>', open + 1);
        if (close < 0) break;
        const QByteArray descriptor = record.mid(open + 1, close - open - 1);
        const QList<QByteArray> parts = descriptor.split(':');
        const QByteArray name = parts.value(0).trimmed().toUpper();
        if (name == "EOR" || name == "EOH") {
            position = close + 1;
            continue;
        }
        bool ok = false;
        const int length = parts.value(1).toInt(&ok);
        if (!ok || length < 0 || close + 1 + length > record.size()) {
            position = close + 1;
            continue;
        }
        fields.insert(QString::fromLatin1(name),
                      QString::fromUtf8(record.constData() + close + 1, length));
        position = close + 1 + length;
    }
    return fields;
}

QDateTime adifDateTime(const QString &date, const QString &time) {
    QString normalizedTime = time.trimmed();
    if (normalizedTime.size() == 4) normalizedTime += QStringLiteral("00");
    QDateTime value = QDateTime::fromString(date.trimmed() + normalizedTime,
                                            QStringLiteral("yyyyMMddHHmmss"));
    value.setTimeSpec(Qt::UTC);
    return value;
}
} // namespace

QString normalizeFt8Callsign(const QString &callsign) {
    QString value = callsign.trimmed().toUpper();
    if (value.startsWith(QLatin1Char('<')) && value.endsWith(QLatin1Char('>')) &&
        value.size() > 2) {
        value = value.mid(1, value.size() - 2);
    }
    return value;
}

namespace {
QStringList callsignCandidates(const QString &callsign) {
    const QString normalized = normalizeFt8Callsign(callsign);
    QStringList candidates;
    if (normalized.isEmpty()) return candidates;
    candidates.push_back(normalized);
    const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part.size() >= 2 && !candidates.contains(part)) candidates.push_back(part);
    }
    return candidates;
}

QString normalizeCallsignPattern(QString pattern) {
    pattern = pattern.trimmed().toUpper();
    if (pattern.startsWith(QLatin1Char('<')) && pattern.endsWith(QLatin1Char('>')) &&
        pattern.size() > 2) {
        pattern = pattern.mid(1, pattern.size() - 2);
    }
    return pattern;
}
} // namespace

QStringList parseFt8CallsignPatterns(const QString &text) {
    QStringList patterns;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                         Qt::SkipEmptyParts);
    for (QString line : lines) {
        const int comment = line.indexOf(QLatin1Char('#'));
        if (comment >= 0) line.truncate(comment);
        const QStringList values = line.split(
            QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
        for (const QString &value : values) {
            const QString pattern = normalizeCallsignPattern(value);
            if (!pattern.isEmpty() && !patterns.contains(pattern)) patterns.push_back(pattern);
        }
    }
    return patterns;
}

bool ft8CallsignPatternMatches(const QString &callsign, const QString &patternValue) {
    const QString pattern = normalizeCallsignPattern(patternValue);
    if (pattern.isEmpty()) return false;
    QString expression = QRegularExpression::escape(pattern);
    expression.replace(QStringLiteral("\\*"), QStringLiteral(".*"));
    expression.replace(QStringLiteral("\\?"), QStringLiteral("."));
    const QRegularExpression matcher(QStringLiteral("^(?:%1)$").arg(expression),
                                     QRegularExpression::CaseInsensitiveOption);
    for (const QString &candidate : callsignCandidates(callsign)) {
        if (matcher.match(candidate).hasMatch()) return true;
    }
    return false;
}

Ft8CallsignFilterResult evaluateFt8Callsign(
    const QString &callsign,
    const QStringList &blockedCallsignPatterns,
    const QVector<Ft8CallsignRule> &countryRules) {
    Ft8CallsignFilterResult result;
    const QStringList candidates = callsignCandidates(callsign);
    if (candidates.isEmpty()) return result;

    for (const QString &pattern : blockedCallsignPatterns) {
        if (ft8CallsignPatternMatches(callsign, pattern)) {
            result.callsignBlocked = true;
            result.matchedPattern = pattern;
            break;
        }
    }

    int bestScore = -1;
    bool bestBlocked = false;
    const QString normalized = normalizeFt8Callsign(callsign);
    // Portable location prefixes take precedence over the home callsign.
    QString location;
    const auto parts = normalized.split('/');
    if (parts.size() > 1 && parts.front().size() >= 2 && parts.front().size() <= 4 &&
        parts.front().size() < parts.back().size()) location = parts.front();
    else if (parts.size() > 1 && parts.back().size() >= 2 && parts.back().size() <= 4 &&
             parts.back() != "MM" && parts.back() != "AM" &&
             parts.back().size() < parts.front().size()) location = parts.back();
    for (const Ft8CallsignRule &rule : countryRules) {
        for (QString prefix : rule.prefixes) {
            prefix = prefix.trimmed().toUpper();
            const bool exact = prefix.startsWith('=');
            if (exact) prefix.remove(0, 1);
            if (prefix.endsWith('*')) prefix.chop(1);
            if (prefix.isEmpty()) continue;
            for (const QString &candidate : candidates) {
                if (exact && candidate != normalized) continue;
                if (exact && candidate != prefix) continue;
                if (!candidate.startsWith(prefix, Qt::CaseInsensitive)) continue;
                const int score = prefix.size() + (candidate == location ? 1000 : 0) + (exact ? 10000 : 0);
                if (score > bestScore || (score == bestScore && rule.blocked && !bestBlocked)) {
                    bestBlocked = rule.blocked;
                    bestScore = score;
                    result.country = rule.country.trimmed();
                    result.matchedPrefix = prefix;
                }
            }
        }
    }
    result.countryBlocked = bestBlocked;
    result.blocked = result.callsignBlocked || result.countryBlocked;
    return result;
}
Ft8MessageInfo parseFt8Message(const QString &message) {
    Ft8MessageInfo result;
    result.raw = message.simplified().toUpper();
    const QStringList tokens = result.raw.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.size() < 2) return result;

    if (tokens[0] == QStringLiteral("CQ") || tokens[0] == QStringLiteral("QRZ")) {
        result.cq = true;
        int callIndex = -1;
        for (int i = 1; i < tokens.size(); ++i) {
            if (isCallsign(tokens[i])) {
                callIndex = i;
                break;
            }
        }
        if (callIndex < 0) return result;
        result.senderCall = tokens[callIndex];
        if (callIndex + 1 < tokens.size() && isGrid(tokens[callIndex + 1])) {
            result.grid = tokens[callIndex + 1];
        }
        result.recipientCall = tokens[0];
        result.valid = true;
        return result;
    }

    if (!isCallsign(tokens[0]) || !isCallsign(tokens[1])) return result;
    result.recipientCall = tokens[0];
    result.senderCall = tokens[1];
    if (tokens.size() >= 3) {
        const QString payload = tokens[2];
        if (isGrid(payload)) result.grid = payload;
        if (isReport(payload)) {
            result.report = payload;
            result.roger = payload.startsWith(QLatin1Char('R'));
        }
        if (payload == QStringLiteral("RRR") || payload == QStringLiteral("RR73")) {
            result.roger = true;
        }
        if (payload == QStringLiteral("73") || payload == QStringLiteral("RR73")) {
            result.signoff = true;
        }
    }
    result.valid = true;
    return result;
}

QString ft8BandForFrequency(double frequencyHz) {
    struct Band { double low; double high; const char *name; };
    static const Band bands[] = {
        {135700, 137800, "2200m"}, {472000, 479000, "630m"},
        {1800000, 2000000, "160m"}, {3500000, 4000000, "80m"},
        {5000000, 5500000, "60m"}, {7000000, 7300000, "40m"},
        {10100000, 10150000, "30m"}, {14000000, 14350000, "20m"},
        {18068000, 18168000, "17m"}, {21000000, 21450000, "15m"},
        {24890000, 24990000, "12m"}, {28000000, 29700000, "10m"},
        {50000000, 54000000, "6m"}, {70000000, 71000000, "4m"},
        {144000000, 148000000, "2m"}, {222000000, 225000000, "1.25m"},
        {420000000, 450000000, "70cm"}, {902000000, 928000000, "33cm"},
        {1240000000, 1300000000, "23cm"}, {2300000000, 2450000000, "13cm"},
        {3300000000, 3500000000, "9cm"}, {5650000000, 5925000000, "6cm"}
    };
    for (const Band &band : bands) {
        if (frequencyHz >= band.low && frequencyHz <= band.high) {
            return QString::fromLatin1(band.name);
        }
    }
    return QString();
}

QString ft8AdifRecord(const Ft8QsoRecord &record) {
    const QDateTime on = record.timeOnUtc.isValid() ? record.timeOnUtc.toUTC()
                                                     : QDateTime::currentDateTimeUtc();
    const QDateTime off = record.timeOffUtc.isValid() ? record.timeOffUtc.toUTC() : on;
    QString adif;
    adif += adifField(QStringLiteral("CALL"), record.call.toUpper());
    adif += adifField(QStringLiteral("QSO_DATE"), on.toString(QStringLiteral("yyyyMMdd")));
    adif += adifField(QStringLiteral("TIME_ON"), on.toString(QStringLiteral("HHmmss")));
    adif += adifField(QStringLiteral("QSO_DATE_OFF"), off.toString(QStringLiteral("yyyyMMdd")));
    adif += adifField(QStringLiteral("TIME_OFF"), off.toString(QStringLiteral("HHmmss")));
    adif += adifField(QStringLiteral("FREQ"), QString::number(record.frequencyHz / 1e6, 'f', 6));
    adif += adifField(QStringLiteral("BAND"), ft8BandForFrequency(record.frequencyHz));
    adif += adifField(QStringLiteral("MODE"), QStringLiteral("FT8"));
    adif += adifField(QStringLiteral("RST_SENT"), record.reportSent);
    adif += adifField(QStringLiteral("RST_RCVD"), record.reportReceived);
    adif += adifField(QStringLiteral("GRIDSQUARE"), record.grid.toUpper());
    adif += adifField(QStringLiteral("STATION_CALLSIGN"), record.stationCall.toUpper());
    adif += adifField(QStringLiteral("MY_GRIDSQUARE"), record.stationGrid.toUpper());
    adif += adifField(QStringLiteral("COMMENT"), record.comment);
    adif += QStringLiteral("<EOR>");
    return adif;
}

QString ft8AdifDocument(const QVector<Ft8QsoRecord> &records) {
    QString result = QStringLiteral("<ADIF_VER:5>3.1.7\n<PROGRAMID:8>ObriiSDR\n"
                                    "<PROGRAMVERSION:5>5.1.0\n<EOH>\n");
    for (const Ft8QsoRecord &record : records) result += ft8AdifRecord(record) + QLatin1Char('\n');
    return result;
}

QVector<Ft8QsoRecord> parseFt8Adif(const QByteArray &data) {
    QVector<Ft8QsoRecord> records;
    const QByteArray upper = data.toUpper();
    int start = 0;
    const int header = upper.indexOf("<EOH>");
    if (header >= 0) start = header + 5;
    while (start < data.size()) {
        const int end = upper.indexOf("<EOR>", start);
        if (end < 0) break;
        const QHash<QString, QString> fields = parseAdifFields(data.mid(start, end + 5 - start));
        if (!fields.value(QStringLiteral("CALL")).isEmpty() &&
            fields.value(QStringLiteral("MODE")).compare(QStringLiteral("FT8"), Qt::CaseInsensitive) == 0) {
            Ft8QsoRecord record;
            record.call = fields.value(QStringLiteral("CALL"));
            record.stationCall = fields.value(QStringLiteral("STATION_CALLSIGN"));
            record.stationGrid = fields.value(QStringLiteral("MY_GRIDSQUARE"));
            record.grid = fields.value(QStringLiteral("GRIDSQUARE"));
            record.reportSent = fields.value(QStringLiteral("RST_SENT"));
            record.reportReceived = fields.value(QStringLiteral("RST_RCVD"));
            record.comment = fields.value(QStringLiteral("COMMENT"));
            record.frequencyHz = fields.value(QStringLiteral("FREQ")).toDouble() * 1e6;
            record.timeOnUtc = adifDateTime(fields.value(QStringLiteral("QSO_DATE")),
                                            fields.value(QStringLiteral("TIME_ON")));
            record.timeOffUtc = adifDateTime(fields.value(QStringLiteral("QSO_DATE_OFF"),
                                                           fields.value(QStringLiteral("QSO_DATE"))),
                                             fields.value(QStringLiteral("TIME_OFF"),
                                                          fields.value(QStringLiteral("TIME_ON"))));
            records.push_back(record);
        }
        start = end + 5;
    }
    return records;
}

QByteArray buildPskReporterPacket(const QString &receiverCall,
                                  const QString &receiverGrid,
                                  const QString &software,
                                  const QString &antenna,
                                  const QVector<Ft8ReceptionReport> &reports,
                                  quint32 sequence,
                                  quint32 sessionId,
                                  bool includeTemplates) {
    if (receiverCall.trimmed().isEmpty() || reports.isEmpty()) return QByteArray();
    QByteArray packet;
    appendU16(packet, 10);
    appendU16(packet, 0);
    appendU32(packet, static_cast<quint32>(QDateTime::currentSecsSinceEpoch()));
    appendU32(packet, sequence);
    appendU32(packet, sessionId);

    if (includeTemplates) {
        packet.append(QByteArray::fromHex(
            "0003002c9992000400018002ffff0000768f8004ffff0000768f"
            "8008ffff0000768f8009ffff0000768f0000"));
        packet.append(QByteArray::fromHex(
            "00020034999300068001ffff0000768f800500040000768f"
            "800affff0000768f800b00010000768f8003ffff0000768f00960004"));
    }

    QByteArray receiver;
    appendU16(receiver, 0x9992);
    appendU16(receiver, 0);
    appendVariableString(receiver, receiverCall.trimmed().toUpper());
    appendVariableString(receiver, receiverGrid.trimmed().toUpper());
    appendVariableString(receiver, software);
    appendVariableString(receiver, antenna);
    padToFour(receiver);
    patchU16(receiver, 2, static_cast<quint16>(receiver.size()));
    packet.append(receiver);

    QByteArray senders;
    appendU16(senders, 0x9993);
    appendU16(senders, 0);
    for (const Ft8ReceptionReport &report : reports) {
        appendVariableString(senders, report.senderCall.trimmed().toUpper());
        appendU32(senders, report.frequencyHz);
        appendVariableString(senders, QStringLiteral("FT8"));
        senders.append(static_cast<char>(1));
        appendVariableString(senders, report.senderGrid.trimmed().toUpper());
        appendU32(senders, report.epochSeconds);
    }
    padToFour(senders);
    patchU16(senders, 2, static_cast<quint16>(senders.size()));
    packet.append(senders);
    patchU16(packet, 2, static_cast<quint16>(packet.size()));
    return packet;
}

QByteArray buildWsjtHeartbeat(const QString &id, const QString &version) {
    QByteArray packet;
    QDataStream out(&packet, QIODevice::WriteOnly);
    writeWsjtHeader(out, 0, id);
    out << WsjtSchema << version.toUtf8() << QByteArray("ObriiSDR");
    return packet;
}

QByteArray buildWsjtStatus(const QString &id,
                           quint64 dialFrequencyHz,
                           const QString &dxCall,
                           const QString &report,
                           quint32 rxAudioHz,
                           quint32 txAudioHz,
                           const QString &stationCall,
                           const QString &stationGrid,
                           const QString &dxGrid,
                           const QString &txMessage) {
    QByteArray packet;
    QDataStream out(&packet, QIODevice::WriteOnly);
    writeWsjtHeader(out, 1, id);
    out << dialFrequencyHz << QByteArray("FT8") << dxCall.toUtf8() << report.toUtf8()
        << QByteArray("FT8") << false << false << false << rxAudioHz << txAudioHz
        << stationCall.toUtf8() << stationGrid.toUtf8() << dxGrid.toUtf8() << false
        << QByteArray() << false << quint8(0) << quint32(0xffffffffU) << quint32(15)
        << QByteArray("ObriiSDR") << txMessage.toUtf8();
    return packet;
}

QByteArray buildWsjtDecode(const QString &id,
                           const QDateTime &utc,
                           int snrDb,
                           double dtSeconds,
                           quint32 audioHz,
                           const QString &message,
                           bool lowConfidence) {
    QByteArray packet;
    QDataStream out(&packet, QIODevice::WriteOnly);
    writeWsjtHeader(out, 2, id);
    out << true << utc.toUTC().time() << qint32(snrDb) << dtSeconds << audioHz
        << QByteArray("FT8") << message.toUtf8() << lowConfidence << false;
    return packet;
}

QByteArray buildWsjtQsoLogged(const QString &id, const Ft8QsoRecord &record) {
    QByteArray packet;
    QDataStream out(&packet, QIODevice::WriteOnly);
    writeWsjtHeader(out, 5, id);
    out << record.timeOffUtc.toUTC() << record.call.toUtf8() << record.grid.toUtf8()
        << quint64(std::llround(record.frequencyHz)) << QByteArray("FT8")
        << record.reportSent.toUtf8() << record.reportReceived.toUtf8()
        << QByteArray() << record.comment.toUtf8() << QByteArray()
        << record.timeOnUtc.toUTC() << record.stationCall.toUtf8()
        << record.stationCall.toUtf8() << record.stationGrid.toUtf8()
        << QByteArray() << QByteArray() << QByteArray();
    return packet;
}

QByteArray buildWsjtLoggedAdif(const QString &id, const QString &adifRecord) {
    QByteArray packet;
    QDataStream out(&packet, QIODevice::WriteOnly);
    writeWsjtHeader(out, 12, id);
    const QString document = QStringLiteral("<ADIF_VER:5>3.1.7\n<PROGRAMID:8>ObriiSDR\n"
                                            "<PROGRAMVERSION:5>5.1.0\n<EOH>\n") +
                             adifRecord;
    out << document.toUtf8();
    return packet;
}
