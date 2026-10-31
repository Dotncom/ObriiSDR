#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <vector>

enum class DspIqFormat {
    Auto = 0,
    Cf32Le = 1,
    Cs16Le = 2,
    BlockFloat16Le = 3
};

struct DspEncodedIq {
    DspIqFormat format = DspIqFormat::Cf32Le;
    QByteArray payload;
    double scale = 1.0;
    int complexSamples = 0;
    int clippedComponents = 0;
};

namespace DspIqCodec {
QString id(DspIqFormat format);
QString displayName(DspIqFormat format);
DspIqFormat fromId(const QString &id, DspIqFormat fallback = DspIqFormat::Cf32Le);
QJsonArray advertisedFormats();
bool isAdvertised(const QJsonArray &formats, DspIqFormat format);
DspIqFormat resolve(DspIqFormat requested, const QJsonArray &peerFormats);
int bytesPerComplexSample(DspIqFormat format);

DspEncodedIq encode(const float *interleavedIq, int complexSamples, DspIqFormat format);
inline DspEncodedIq encode(const std::vector<float> &interleavedIq, DspIqFormat format) {
    return encode(interleavedIq.data(), int(interleavedIq.size() / 2U), format);
}
void addMetadata(QJsonObject &header, const DspEncodedIq &encoded);
bool decode(const QJsonObject &header, const QByteArray &payload, int complexSamples,
            std::vector<float> &interleavedIq, QString &error);
}
