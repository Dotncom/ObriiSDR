#pragma once

#include <QByteArray>
#include <vector>

namespace DisplaySpectrumCodec {
QByteArray encodeDb10(const std::vector<float> &magnitudes,
                      const std::vector<float> &reference = {});
bool decodeDb10(const QByteArray &payload, int count, bool hasReference,
                std::vector<float> *magnitudes,
                std::vector<float> *reference = nullptr);
}
