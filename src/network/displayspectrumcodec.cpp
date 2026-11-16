#include "displayspectrumcodec.h"
#include "../utils/displaylevelstep.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>

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

DisplaySpectrumCodec::PackedLevels DisplaySpectrumCodec::encodeU8(
    const std::vector<float> &magnitudes, const std::vector<float> &reference, int stepTenths) {
    if (stepTenths <= DisplayLevelStep::DefaultNetworkTenths ||
        stepTenths > DisplayLevelStep::MaximumTenths ||
        magnitudes.empty() || magnitudes.size() > 32 * 1024 * 1024 ||
        (!reference.empty() && reference.size() != magnitudes.size())) return {};

    const auto stepIndex = [stepTenths](float value) {
        const double level = std::isfinite(value) ? value : -160.0;
        return static_cast<int>(std::lround((std::clamp)(level, -3276.0, 3276.0) * 10.0 / stepTenths));
    };
    int minimum = std::numeric_limits<int>::max();
    int maximum = std::numeric_limits<int>::min();
    for (const auto *levels : {&magnitudes, &reference}) {
        for (const float level : *levels) {
            const int index = stepIndex(level);
            minimum = (std::min)(minimum, index);
            maximum = (std::max)(maximum, index);
            if (maximum - minimum > 255) return {};
        }
    }

    PackedLevels packed;
    packed.offsetSteps = minimum;
    packed.payload.resize(static_cast<int>(magnitudes.size() + reference.size()));
    auto *bytes = reinterpret_cast<uchar *>(packed.payload.data());
    for (const auto *levels : {&magnitudes, &reference}) {
        for (const float level : *levels) {
            *bytes++ = static_cast<uchar>(stepIndex(level) - minimum);
        }
    }
    return packed;
}

bool DisplaySpectrumCodec::decodeCompact(const QJsonObject &header, const QByteArray &payload,
                                          std::vector<float> *magnitudes, std::vector<float> *reference) {
    const int count = header.value(QStringLiteral("fftLength")).toInt();
    const bool hasReference = header.value(QStringLiteral("hasReferenceMagnitudes")).toBool();
    const auto format = header.value(QStringLiteral("binFormat")).toString();
    if (format == QStringLiteral("s16le-db10"))
        return decodeDb10(payload, count, hasReference, magnitudes, reference);

    const int step = header.value(QStringLiteral("levelStepTenths")).toInt();
    const int offset = header.value(QStringLiteral("levelOffsetSteps")).toInt(
        std::numeric_limits<int>::min());
    if (format != QStringLiteral("u8-level-step") || !magnitudes ||
        count <= 0 || count > 32 * 1024 * 1024 || (hasReference && !reference) ||
        step <= DisplayLevelStep::DefaultNetworkTenths || step > DisplayLevelStep::MaximumTenths ||
        offset < -32760 || offset > 32760 ||
        payload.size() != static_cast<qint64>(count) * (hasReference ? 2 : 1)) return false;

    const auto *bytes = reinterpret_cast<const uchar *>(payload.constData());
    const auto decode = [&](std::vector<float> &levels, int start) {
        levels.resize(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            levels[static_cast<std::size_t>(i)] = (offset + bytes[start + i]) * step / 10.0f;
        }
    };
    decode(*magnitudes, 0);
    if (hasReference) decode(*reference, count);
    else if (reference) reference->clear();
    return true;
}
