#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <vector>

namespace DisplaySpectrumCodec {
struct PackedLevels {
    QByteArray payload;
    int offsetSteps = 0;
};

QByteArray encodeDb10(const std::vector<float> &magnitudes,
                      const std::vector<float> &reference = {});
bool decodeDb10(const QByteArray &payload, int count, bool hasReference,
                std::vector<float> *magnitudes,
                std::vector<float> *reference = nullptr);
PackedLevels encodeU8(const std::vector<float> &magnitudes,
                      const std::vector<float> &reference, int stepTenths);
bool decodeCompact(const QJsonObject &header, const QByteArray &payload,
                   std::vector<float> *magnitudes, std::vector<float> *reference = nullptr);
}
