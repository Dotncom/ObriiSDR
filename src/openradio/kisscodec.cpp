#include "kisscodec.h"

namespace {
constexpr quint8 Fend = 0xC0;
constexpr quint8 Fesc = 0xDB;
constexpr quint8 Tfend = 0xDC;
constexpr quint8 Tfesc = 0xDD;

void setError(QString *error, const QString &value) {
    if (error) *error = value;
}
}

QByteArray KissCodec::encode(const KissFrame &frame, QString *error) {
    if (frame.port > 15 || frame.command > 15) {
        setError(error, QStringLiteral("KISS port and command must be in the range 0..15"));
        return {};
    }
    QByteArray raw;
    raw.reserve(frame.payload.size() + 1);
    raw.append(static_cast<char>((frame.port << 4U) | frame.command));
    raw += frame.payload;
    QByteArray result;
    result.reserve(raw.size() + 2);
    result.append(static_cast<char>(Fend));
    for (char byte : raw) {
        const quint8 value = static_cast<quint8>(byte);
        if (value == Fend) {
            result.append(static_cast<char>(Fesc));
            result.append(static_cast<char>(Tfend));
        } else if (value == Fesc) {
            result.append(static_cast<char>(Fesc));
            result.append(static_cast<char>(Tfesc));
        } else {
            result.append(byte);
        }
    }
    result.append(static_cast<char>(Fend));
    if (error) error->clear();
    return result;
}

bool KissCodec::decode(const QByteArray &wireFrame, KissFrame *frame, QString *error) {
    if (!frame) {
        setError(error, QStringLiteral("KISS output frame is null"));
        return false;
    }
    int begin = 0;
    int end = wireFrame.size();
    if (end > 0 && static_cast<quint8>(wireFrame.front()) == Fend) ++begin;
    if (end > begin && static_cast<quint8>(wireFrame[end - 1]) == Fend) --end;
    QByteArray raw;
    bool escaped = false;
    for (int i = begin; i < end; ++i) {
        const quint8 value = static_cast<quint8>(wireFrame[i]);
        if (escaped) {
            if (value == Tfend) raw.append(static_cast<char>(Fend));
            else if (value == Tfesc) raw.append(static_cast<char>(Fesc));
            else {
                setError(error, QStringLiteral("Invalid KISS escape sequence"));
                return false;
            }
            escaped = false;
        } else if (value == Fesc) {
            escaped = true;
        } else if (value == Fend) {
            setError(error, QStringLiteral("Unexpected KISS frame delimiter"));
            return false;
        } else {
            raw.append(static_cast<char>(value));
        }
    }
    if (escaped || raw.isEmpty()) {
        setError(error, escaped ? QStringLiteral("Truncated KISS escape sequence")
                                : QStringLiteral("Empty KISS frame"));
        return false;
    }
    const quint8 command = static_cast<quint8>(raw.front());
    frame->port = command >> 4U;
    frame->command = command & 0x0FU;
    frame->payload = raw.mid(1);
    if (error) error->clear();
    return true;
}

QList<KissFrame> KissStreamDecoder::push(const QByteArray &bytes, QStringList *errors) {
    buffer += bytes;
    QList<KissFrame> frames;
    for (;;) {
        int begin = buffer.indexOf(static_cast<char>(Fend));
        if (begin < 0) {
            if (buffer.size() > 1024 * 1024) buffer.clear();
            break;
        }
        if (begin > 0) buffer.remove(0, begin);
        const int end = buffer.indexOf(static_cast<char>(Fend), 1);
        if (end < 0) break;
        const QByteArray candidate = buffer.left(end + 1);
        buffer.remove(0, end + 1);
        if (candidate.size() <= 2) continue;
        KissFrame frame;
        QString error;
        if (KissCodec::decode(candidate, &frame, &error)) frames.push_back(frame);
        else if (errors) errors->push_back(error);
    }
    return frames;
}

void KissStreamDecoder::reset() {
    buffer.clear();
}
