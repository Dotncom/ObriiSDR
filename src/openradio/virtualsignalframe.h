#ifndef VIRTUALSIGNALFRAME_H
#define VIRTUALSIGNALFRAME_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

enum class VirtualSignalPayloadType : quint8 {
    ProtocolFrame = 1,
    AudioPcm16 = 2,
    ComplexFloat32 = 3,
    ComplexInt16 = 4,
    SymbolsInt8 = 5
};

struct VirtualSignalFrame {
    quint32 sequence = 0;
    qint64 timestampNs = 0;
    QString streamId;
    QString protocolId;
    VirtualSignalPayloadType payloadType = VirtualSignalPayloadType::ProtocolFrame;
    quint32 sampleRate = 0;
    qint64 centerFrequencyHz = 0;
    quint32 symbolRate = 0;
    bool discontinuity = false;
    bool endOfStream = false;
    QJsonObject metadata;
    QByteArray payload;
};

class VirtualSignalFrameCodec {
public:
    static constexpr quint16 Version = 1;
    static QByteArray encode(const VirtualSignalFrame &frame, QString *error = nullptr);
    static bool decode(const QByteArray &wire, VirtualSignalFrame *frame,
                       QString *error = nullptr);
};

#endif // VIRTUALSIGNALFRAME_H
