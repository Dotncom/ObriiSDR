#include "virtualsignalframe.h"

#include <QDataStream>
#include <QJsonDocument>

namespace {
constexpr quint32 Magic = 0x4F425653U; // OBVS
constexpr quint32 MaximumHeaderBytes = 1024U * 1024U;
constexpr quint32 MaximumPayloadBytes = 256U * 1024U * 1024U;

void setError(QString *error, const QString &value) {
    if (error) *error = value;
}
}

QByteArray VirtualSignalFrameCodec::encode(const VirtualSignalFrame &frame, QString *error) {
    if (frame.streamId.isEmpty()) {
        setError(error, QStringLiteral("Virtual signal stream ID is empty"));
        return {};
    }
    QJsonObject header = frame.metadata;
    header.insert(QStringLiteral("streamId"), frame.streamId);
    header.insert(QStringLiteral("protocolId"), frame.protocolId);
    header.insert(QStringLiteral("sequence"), static_cast<qint64>(frame.sequence));
    header.insert(QStringLiteral("timestampNs"), QString::number(frame.timestampNs));
    header.insert(QStringLiteral("payloadType"), static_cast<int>(frame.payloadType));
    header.insert(QStringLiteral("sampleRate"), static_cast<qint64>(frame.sampleRate));
    header.insert(QStringLiteral("centerFrequencyHz"), QString::number(frame.centerFrequencyHz));
    header.insert(QStringLiteral("symbolRate"), static_cast<qint64>(frame.symbolRate));
    header.insert(QStringLiteral("discontinuity"), frame.discontinuity);
    header.insert(QStringLiteral("endOfStream"), frame.endOfStream);
    const QByteArray headerBytes = QJsonDocument(header).toJson(QJsonDocument::Compact);
    if (headerBytes.size() > static_cast<int>(MaximumHeaderBytes) ||
        frame.payload.size() > static_cast<int>(MaximumPayloadBytes)) {
        setError(error, QStringLiteral("Virtual signal frame exceeds the bounded transport size"));
        return {};
    }
    QByteArray wire;
    QDataStream stream(&wire, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << Magic << Version << static_cast<quint16>(0)
           << static_cast<quint32>(headerBytes.size())
           << static_cast<quint32>(frame.payload.size());
    wire += headerBytes;
    wire += frame.payload;
    if (error) error->clear();
    return wire;
}

bool VirtualSignalFrameCodec::decode(const QByteArray &wire, VirtualSignalFrame *frame,
                                     QString *error) {
    if (!frame || wire.size() < 16) {
        setError(error, QStringLiteral("Virtual signal frame is truncated"));
        return false;
    }
    QDataStream stream(wire.left(16));
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 magic = 0;
    quint16 version = 0;
    quint16 flags = 0;
    quint32 headerSize = 0;
    quint32 payloadSize = 0;
    stream >> magic >> version >> flags >> headerSize >> payloadSize;
    Q_UNUSED(flags)
    if (magic != Magic || version != Version || headerSize > MaximumHeaderBytes ||
        payloadSize > MaximumPayloadBytes ||
        static_cast<quint64>(16) + headerSize + payloadSize != static_cast<quint64>(wire.size())) {
        setError(error, QStringLiteral("Invalid virtual signal frame header or length"));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        wire.mid(16, static_cast<int>(headerSize)), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("Invalid virtual signal metadata JSON"));
        return false;
    }
    const QJsonObject header = document.object();
    VirtualSignalFrame decoded;
    decoded.streamId = header.value(QStringLiteral("streamId")).toString();
    decoded.protocolId = header.value(QStringLiteral("protocolId")).toString();
    decoded.sequence = static_cast<quint32>(header.value(QStringLiteral("sequence")).toVariant().toULongLong());
    decoded.timestampNs = header.value(QStringLiteral("timestampNs")).toString().toLongLong();
    decoded.payloadType = static_cast<VirtualSignalPayloadType>(header.value(QStringLiteral("payloadType")).toInt());
    const int payloadType = static_cast<int>(decoded.payloadType);
    if (payloadType < static_cast<int>(VirtualSignalPayloadType::ProtocolFrame) ||
        payloadType > static_cast<int>(VirtualSignalPayloadType::SymbolsInt8)) {
        setError(error, QStringLiteral("Virtual signal payload type is unsupported"));
        return false;
    }
    decoded.sampleRate = static_cast<quint32>(header.value(QStringLiteral("sampleRate")).toVariant().toULongLong());
    decoded.centerFrequencyHz = header.value(QStringLiteral("centerFrequencyHz")).toString().toLongLong();
    decoded.symbolRate = static_cast<quint32>(header.value(QStringLiteral("symbolRate")).toVariant().toULongLong());
    decoded.discontinuity = header.value(QStringLiteral("discontinuity")).toBool();
    decoded.endOfStream = header.value(QStringLiteral("endOfStream")).toBool();
    decoded.metadata = header;
    decoded.payload = wire.mid(16 + static_cast<int>(headerSize), static_cast<int>(payloadSize));
    if (decoded.streamId.isEmpty()) {
        setError(error, QStringLiteral("Virtual signal metadata has no stream ID"));
        return false;
    }
    *frame = decoded;
    if (error) error->clear();
    return true;
}
