#include "dspiqcodec.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        std::fprintf(stderr, "Usage: ObriiIqFormatBench recording.cf32|recording.iq8\n"
                             "Input: raw interleaved CF32LE or signed CI8 I/Q, no header.\n");
        return 2;
    }
    QFile input(app.arguments().at(1));
    const bool ci8 = QFileInfo(input.fileName()).suffix().compare(QStringLiteral("iq8"), Qt::CaseInsensitive) == 0;
    const int sourceBytesPerSample = ci8 ? 2 : 8;
    if (!input.open(QIODevice::ReadOnly) || input.size() <= 0 ||
        input.size() % sourceBytesPerSample != 0) {
        std::fprintf(stderr, "Cannot open recording or invalid IQ length\n");
        return 2;
    }
    constexpr int ComplexPerBlock = 32768;
    constexpr qint64 MaxInputBytes = 32LL * 1024 * 1024;
    const qint64 availableSamples = input.size() / sourceBytesPerSample;
    const qint64 sampleLimit = std::min(availableSamples, MaxInputBytes / sourceBytesPerSample);
    const qint64 sampleOffset = (availableSamples - sampleLimit) / 2;
    std::fprintf(stdout, "source=%s inputBytes=%lld sampleOffset=%lld samples=%lld\n",
                 ci8 ? "ci8" : "cf32le", static_cast<long long>(sampleLimit * sourceBytesPerSample),
                 static_cast<long long>(sampleOffset), static_cast<long long>(sampleLimit));
    constexpr DspIqFormat formats[] = {DspIqFormat::Cf32Le, DspIqFormat::Cs16Le,
                                       DspIqFormat::BlockFloat16Le, DspIqFormat::BlockFloat8};
    for (DspIqFormat format : formats) {
        if (!input.seek(sampleOffset * sourceBytesPerSample)) return 2;
        qint64 samples = 0;
        qint64 encodedBytes = 0;
        qint64 encodeNs = 0;
        qint64 decodeNs = 0;
        qint64 clipped = 0;
        double errorPower = 0.0;
        double signalPower = 0.0;
        while (samples < sampleLimit) {
            const int requested = static_cast<int>(std::min<qint64>(ComplexPerBlock, sampleLimit - samples));
            const QByteArray bytes = input.read(requested * sourceBytesPerSample);
            if (bytes.size() != requested * sourceBytesPerSample) return 2;
            const int count = requested;
            std::vector<float> iq(static_cast<std::size_t>(count) * 2);
            for (int index = 0; index < count * 2; ++index) {
                if (ci8) {
                    iq[static_cast<std::size_t>(index)] =
                        static_cast<float>(static_cast<qint8>(bytes.at(index))) / 128.0f;
                } else {
                    const quint32 bits = qFromLittleEndian<quint32>(
                        reinterpret_cast<const uchar *>(bytes.constData() + index * 4));
                    std::memcpy(&iq[static_cast<std::size_t>(index)], &bits, 4);
                }
                if (!std::isfinite(iq[static_cast<std::size_t>(index)])) {
                    std::fprintf(stderr, "Recording contains non-finite IQ\n");
                    return 2;
                }
            }
            QElapsedTimer timer;
            timer.start();
            const DspEncodedIq encoded = DspIqCodec::encode(iq, format);
            encodeNs += timer.nsecsElapsed();
            QJsonObject header;
            DspIqCodec::addMetadata(header, encoded);
            std::vector<float> decoded;
            QString error;
            timer.restart();
            if (!DspIqCodec::decode(header, encoded.payload, count, decoded, error)) {
                std::fprintf(stderr, "Decode failed: %s\n", qPrintable(error));
                return 3;
            }
            decodeNs += timer.nsecsElapsed();
            for (int index = 0; index < count * 2; ++index) {
                const double original = iq[static_cast<std::size_t>(index)];
                const double difference = original - decoded[static_cast<std::size_t>(index)];
                signalPower += original * original;
                errorPower += difference * difference;
            }
            samples += count;
            encodedBytes += encoded.payload.size() +
                ((format == DspIqFormat::BlockFloat16Le || format == DspIqFormat::BlockFloat8) ? 8 : 0);
            clipped += encoded.clippedComponents;
        }
        const double secondsEncode = static_cast<double>(encodeNs) * 1e-9;
        const double secondsDecode = static_cast<double>(decodeNs) * 1e-9;
        const double snr = errorPower > 0.0 && signalPower > 0.0
            ? 10.0 * std::log10(signalPower / errorPower) : INFINITY;
        std::fprintf(stdout,
            "%s samples=%lld bytes=%lld cf32Ratio=%.3f sourceRatio=%.3f encodeMs=%.2f decodeMs=%.2f "
            "encodeMSps=%.2f decodeMSps=%.2f snrDb=%.2f clipped=%lld\n",
            qPrintable(DspIqCodec::id(format)), static_cast<long long>(samples),
            static_cast<long long>(encodedBytes), static_cast<double>(encodedBytes) /
                static_cast<double>(samples * 8), static_cast<double>(encodedBytes) /
                static_cast<double>(samples * sourceBytesPerSample), secondsEncode * 1000.0,
            secondsDecode * 1000.0,
            samples / secondsEncode / 1e6, samples / secondsDecode / 1e6,
            snr, static_cast<long long>(clipped));
    }
    return 0;
}
