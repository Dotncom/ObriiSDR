#include "displayspectrumcodec.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>

namespace {
void appendDb10(QByteArray *payload, float value) {
    const qint16 quantized = static_cast<qint16>(std::lround((std::clamp)(
        std::isfinite(value) ? value : -160.0f, -3276.0f, 3276.0f) * 10.0f));
    const quint16 bits = qToLittleEndian(static_cast<quint16>(quantized));
    payload->append(reinterpret_cast<const char *>(&bits), sizeof(bits));
}
}

QByteArray DisplaySpectrumCodec::encodeDb10(const std::vector<float> &magnitudes,
                                             const std::vector<float> &reference) {
    if (magnitudes.empty() || magnitudes.size() > 32 * 1024 * 1024 ||
        (!reference.empty() && reference.size() != magnitudes.size())) return {};
    QByteArray payload;
    payload.reserve(static_cast<int>((magnitudes.size() + reference.size()) * 2));
    for (const float value : magnitudes) appendDb10(&payload, value);
    for (const float value : reference) appendDb10(&payload, value);
    return payload;
}

bool DisplaySpectrumCodec::decodeDb10(const QByteArray &payload, int count,
                                       bool hasReference, std::vector<float> *magnitudes,
                                       std::vector<float> *reference) {
    if (!magnitudes || count <= 0 || count > 32 * 1024 * 1024 ||
        (hasReference && !reference) ||
        payload.size() != static_cast<qint64>(count) * 2 * (hasReference ? 2 : 1)) return false;
    const auto decodeInto = [&](int offset, std::vector<float> *output) {
        output->resize(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            const auto *bytes = reinterpret_cast<const uchar *>(
                payload.constData() + offset + index * 2);
            (*output)[static_cast<std::size_t>(index)] =
                static_cast<qint16>(qFromLittleEndian<quint16>(bytes)) / 10.0f;
        }
    };
    decodeInto(0, magnitudes);
    if (hasReference) decodeInto(count * 2, reference);
    else if (reference) reference->clear();
    return true;
}
