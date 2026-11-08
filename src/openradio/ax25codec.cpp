#include "ax25codec.h"

#include <QRegularExpression>

namespace {
void setError(QString *error, const QString &value) {
    if (error) *error = value;
}
}

bool Ax25Codec::parseAddress(const QString &text, Ax25Address *address, QString *error) {
    if (!address) {
        setError(error, QStringLiteral("AX.25 output address is null"));
        return false;
    }
    const QRegularExpression expression(QStringLiteral("^([A-Za-z0-9]{1,6})(?:-([0-9]{1,2}))?$"));
    const QRegularExpressionMatch match = expression.match(text.trimmed());
    if (!match.hasMatch()) {
        setError(error, QStringLiteral("AX.25 address must be CALL or CALL-SSID (1..6 characters, SSID 0..15)"));
        return false;
    }
    bool ok = true;
    const int ssid = match.captured(2).isEmpty() ? 0 : match.captured(2).toInt(&ok);
    if (!ok || ssid < 0 || ssid > 15) {
        setError(error, QStringLiteral("AX.25 SSID must be in the range 0..15"));
        return false;
    }
    address->callsign = match.captured(1).toUpper();
    address->ssid = static_cast<quint8>(ssid);
    address->repeated = false;
    return true;
}

QString Ax25Codec::addressText(const Ax25Address &address) {
    return address.ssid ? QStringLiteral("%1-%2").arg(address.callsign).arg(address.ssid)
                        : address.callsign;
}

QByteArray Ax25Codec::encodeAddress(const Ax25Address &address, bool last, QString *error) {
    Ax25Address normalized;
    if (!parseAddress(addressText(address), &normalized, error)) return {};
    QByteArray result(7, 0);
    const QByteArray call = normalized.callsign.toLatin1().leftJustified(6, ' ');
    for (int i = 0; i < 6; ++i) result[i] = static_cast<char>(static_cast<quint8>(call[i]) << 1U);
    quint8 ssid = static_cast<quint8>(0x60U | ((normalized.ssid & 0x0FU) << 1U));
    if (address.repeated) ssid |= 0x80U;
    if (last) ssid |= 0x01U;
    result[6] = static_cast<char>(ssid);
    return result;
}

bool Ax25Codec::decodeAddress(const QByteArray &bytes, int offset, Ax25Address *address,
                              bool *last, QString *error) {
    if (!address || offset < 0 || offset + 7 > bytes.size()) {
        setError(error, QStringLiteral("Truncated AX.25 address field"));
        return false;
    }
    QByteArray call;
    call.reserve(6);
    for (int i = 0; i < 6; ++i) {
        const quint8 value = static_cast<quint8>(bytes[offset + i]);
        if (value & 0x01U) {
            setError(error, QStringLiteral("Invalid AX.25 shifted callsign byte"));
            return false;
        }
        call.append(static_cast<char>(value >> 1U));
    }
    address->callsign = QString::fromLatin1(call).trimmed();
    const quint8 ssid = static_cast<quint8>(bytes[offset + 6]);
    address->ssid = static_cast<quint8>((ssid >> 1U) & 0x0FU);
    address->repeated = (ssid & 0x80U) != 0;
    if (last) *last = (ssid & 0x01U) != 0;
    return !address->callsign.isEmpty();
}

quint16 Ax25Codec::fcs(const QByteArray &bytes) {
    quint16 crc = 0xFFFFU;
    for (char byte : bytes) {
        crc ^= static_cast<quint8>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? static_cast<quint16>((crc >> 1U) ^ 0x8408U)
                             : static_cast<quint16>(crc >> 1U);
    }
    return static_cast<quint16>(crc ^ 0xFFFFU);
}

QByteArray Ax25Codec::encodeUiFrame(const Ax25UiFrame &frame, bool appendFcs, QString *error) {
    if (frame.digipeaters.size() > 8) {
        setError(error, QStringLiteral("AX.25 supports at most eight digipeaters"));
        return {};
    }
    QByteArray result;
    const bool noDigipeaters = frame.digipeaters.isEmpty();
    const QByteArray destination = encodeAddress(frame.destination, false, error);
    const QByteArray source = encodeAddress(frame.source, noDigipeaters, error);
    if (destination.isEmpty() || source.isEmpty()) return {};
    result += destination;
    result += source;
    for (int i = 0; i < frame.digipeaters.size(); ++i) {
        const QByteArray encoded = encodeAddress(frame.digipeaters[i], i + 1 == frame.digipeaters.size(), error);
        if (encoded.isEmpty()) return {};
        result += encoded;
    }
    result.append(static_cast<char>(frame.control));
    result.append(static_cast<char>(frame.pid));
    result += frame.information;
    if (appendFcs) {
        const quint16 checksum = fcs(result);
        result.append(static_cast<char>(checksum & 0xFFU));
        result.append(static_cast<char>((checksum >> 8U) & 0xFFU));
    }
    if (error) error->clear();
    return result;
}

bool Ax25Codec::decodeUiFrame(const QByteArray &bytes, Ax25UiFrame *frame,
                              bool containsFcs, QString *error) {
    if (!frame) {
        setError(error, QStringLiteral("AX.25 output frame is null"));
        return false;
    }
    QByteArray data = bytes;
    if (containsFcs) {
        if (data.size() < 18) {
            setError(error, QStringLiteral("AX.25 frame is too short for FCS"));
            return false;
        }
        const quint16 expected = static_cast<quint8>(data[data.size() - 2]) |
            (static_cast<quint16>(static_cast<quint8>(data[data.size() - 1])) << 8U);
        data.chop(2);
        if (fcs(data) != expected) {
            setError(error, QStringLiteral("AX.25 FCS mismatch"));
            return false;
        }
    }
    if (data.size() < 16) {
        setError(error, QStringLiteral("AX.25 UI frame is too short"));
        return false;
    }
    Ax25UiFrame decoded;
    int offset = 0;
    bool last = false;
    if (!decodeAddress(data, offset, &decoded.destination, &last, error) || last) {
        setError(error, QStringLiteral("AX.25 destination cannot terminate the address list"));
        return false;
    }
    offset += 7;
    if (!decodeAddress(data, offset, &decoded.source, &last, error)) return false;
    offset += 7;
    while (!last) {
        if (decoded.digipeaters.size() >= 8) {
            setError(error, QStringLiteral("AX.25 address list exceeds eight digipeaters"));
            return false;
        }
        Ax25Address digipeater;
        if (!decodeAddress(data, offset, &digipeater, &last, error)) return false;
        decoded.digipeaters.push_back(digipeater);
        offset += 7;
    }
    if (offset + 2 > data.size()) {
        setError(error, QStringLiteral("AX.25 UI frame has no control/PID fields"));
        return false;
    }
    decoded.control = static_cast<quint8>(data[offset++]);
    decoded.pid = static_cast<quint8>(data[offset++]);
    decoded.information = data.mid(offset);
    *frame = decoded;
    if (error) error->clear();
    return true;
}
