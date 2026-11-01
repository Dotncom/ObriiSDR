#include "dspiqcodec.h"
#include "iq8quantizer.h"
#include "iq16quantizer.h"

#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace DspIqCodec {
QString id(DspIqFormat format) {
    switch (format) {
    case DspIqFormat::Cs16Le: return QStringLiteral("cs16le");
    case DspIqFormat::BlockFloat16Le: return QStringLiteral("bfp16le");
    case DspIqFormat::BlockFloat8: return QStringLiteral("bfp8");
    case DspIqFormat::Auto: return QStringLiteral("auto");
    default: return QStringLiteral("cf32le");
    }
}

QString displayName(DspIqFormat format) {
    switch (format) {
    case DspIqFormat::Cs16Le: return QStringLiteral("CS16");
    case DspIqFormat::BlockFloat16Le: return QStringLiteral("BFP16");
    case DspIqFormat::BlockFloat8: return QStringLiteral("BFP8 (lossy)");
    case DspIqFormat::Auto: return QStringLiteral("Auto");
    default: return QStringLiteral("CF32");
    }
}

DspIqFormat fromId(const QString &value, DspIqFormat fallback) {
    if (value == QLatin1String("cf32le")) return DspIqFormat::Cf32Le;
    if (value == QLatin1String("cs16le")) return DspIqFormat::Cs16Le;
    if (value == QLatin1String("bfp16le")) return DspIqFormat::BlockFloat16Le;
    if (value == QLatin1String("bfp8")) return DspIqFormat::BlockFloat8;
    if (value == QLatin1String("auto")) return DspIqFormat::Auto;
    return fallback;
}

QJsonArray advertisedFormats() {
    return {id(DspIqFormat::Cf32Le), id(DspIqFormat::Cs16Le),
            id(DspIqFormat::BlockFloat16Le), id(DspIqFormat::BlockFloat8)};
}

bool isAdvertised(const QJsonArray &formats, DspIqFormat format) {
    const QString wanted = id(format);
    for (const auto &entry : formats) if (entry.isString() && entry.toString() == wanted) return true;
    return false;
}

DspIqFormat resolve(DspIqFormat requested, const QJsonArray &peerFormats) {
    // A legacy peer has no capability list and only understands CF32.
    if (peerFormats.isEmpty()) return DspIqFormat::Cf32Le;
    if (requested == DspIqFormat::Auto)
        return isAdvertised(peerFormats, DspIqFormat::BlockFloat16Le)
            ? DspIqFormat::BlockFloat16Le : DspIqFormat::Cf32Le;
    return isAdvertised(peerFormats, requested) ? requested : DspIqFormat::Cf32Le;
}

int bytesPerComplexSample(DspIqFormat format) {
    switch (format) {
    case DspIqFormat::Cf32Le: return 8;
    case DspIqFormat::BlockFloat8: return 2;
    default: return 4;
    }
}

DspEncodedIq encode(const float *iq, int complexSamples, DspIqFormat format) {
    DspEncodedIq result;
    if (!iq || complexSamples <= 0 || format == DspIqFormat::Auto) return result;
    result.format = format;
    result.complexSamples = complexSamples;
    const int components = complexSamples * 2;
    for (int i = 0; i < components; ++i) if (!std::isfinite(iq[i])) return {};
    if (format == DspIqFormat::Cf32Le) {
        result.payload.resize(components * 4);
        for (int i = 0; i < components; ++i) {
            quint32 bits = 0;
            std::memcpy(&bits, iq + i, 4);
            qToLittleEndian(bits, reinterpret_cast<uchar *>(result.payload.data() + i * 4));
        }
        return result;
    }
    double maximum = 0.0;
    if (format == DspIqFormat::BlockFloat16Le || format == DspIqFormat::BlockFloat8) {
        for (int i = 0; i < components; ++i) maximum = (std::max)(maximum, std::abs(double(iq[i])));
        result.scale = maximum > 0.0
            ? maximum / (format == DspIqFormat::BlockFloat8 ? 127.0 : 32767.0) : 1.0;
    } else {
        result.scale = 1.0 / 32767.0;
    }
    if (format == DspIqFormat::BlockFloat8) {
        result.payload.resize(components);
        if (maximum == 0.0) {
            // All-zero blocks have no useful scale to transport.
            result.payload.fill(0);
        } else {
            const float gain = float(1.0 / (result.scale * 127.0));
            if (std::isfinite(gain) && gain > 0.0f) {
                ObriiIq8Quantizer::quantizeScaled(iq, result.payload.data(),
                                                  std::size_t(components), gain);
            } else {
                for (int i = 0; i < components; ++i)
                    result.payload[i] = char((std::clamp)(int(std::lrint(double(iq[i]) / result.scale)), -127, 127));
            }
        }
        return result;
    }
    result.payload.resize(components * 2);
    ObriiIq16Quantizer::quantize(iq, result.payload.data(), std::size_t(components),
                                result.scale, result.clippedComponents);
    return result;
}

void addMetadata(QJsonObject &header, const DspEncodedIq &encoded) {
    header[QStringLiteral("iqFormat")] = id(encoded.format);
    header[QStringLiteral("sampleCount")] = encoded.complexSamples;
    if (encoded.format == DspIqFormat::BlockFloat16Le ||
        encoded.format == DspIqFormat::BlockFloat8)
        header[QStringLiteral("iqScale")] = encoded.scale;
    if (encoded.clippedComponents)
        header[QStringLiteral("iqClipped")] = encoded.clippedComponents;
}

bool decode(const QJsonObject &header, const QByteArray &payload, int complexSamples,
            std::vector<float> &iq, QString &error) {
    const DspIqFormat format = fromId(header.value(QStringLiteral("iqFormat")).toString(),
                                      DspIqFormat::Cf32Le);
    if (format == DspIqFormat::Auto || complexSamples <= 0 ||
        complexSamples > (std::numeric_limits<int>::max)() / 8 ||
        payload.size() != complexSamples * bytesPerComplexSample(format)) {
        error = QStringLiteral("Invalid IQ format or payload length");
        return false;
    }
    const int components = complexSamples * 2;
    iq.resize(std::size_t(components));
    if (format == DspIqFormat::Cf32Le) {
        for (int i = 0; i < components; ++i) {
            const quint32 bits = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar *>(payload.constData() + i * 4));
            std::memcpy(iq.data() + i, &bits, 4);
            if (!std::isfinite(iq[std::size_t(i)])) {
                error = QStringLiteral("Non-finite IQ component");
                return false;
            }
        }
        return true;
    }
    double scale = 1.0 / 32767.0;
    if (format == DspIqFormat::BlockFloat16Le || format == DspIqFormat::BlockFloat8) {
        scale = header.value(QStringLiteral("iqScale")).toDouble(0.0);
        if (!std::isfinite(scale) || scale <= 0.0 || scale > 1.0e12) {
            error = QStringLiteral("Invalid block-floating IQ scale");
            return false;
        }
    }
    if (format == DspIqFormat::BlockFloat8) {
        for (int i = 0; i < components; ++i)
            iq[std::size_t(i)] = float(double(qint8(payload.at(i))) * scale);
        return true;
    }
    for (int i = 0; i < components; ++i) {
        const qint16 sample = qFromLittleEndian<qint16>(
            reinterpret_cast<const uchar *>(payload.constData() + i * 2));
        iq[std::size_t(i)] = float(double(sample) * scale);
    }
    return true;
}
}
