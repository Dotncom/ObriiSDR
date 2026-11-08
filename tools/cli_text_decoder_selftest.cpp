#include "clitextdecoder.h"

#include <cmath>
#include <cstdint>

namespace {
void appendSignal(QByteArray &pcm, int samples, bool keyed, double &phase) {
    constexpr double step = 2.0 * 3.14159265358979323846 * 700.0 / 48000.0;
    for (int i = 0; i < samples; ++i) {
        const qint16 value = keyed ? static_cast<qint16>(std::sin(phase) * 22000.0) : 0;
        pcm.append(reinterpret_cast<const char *>(&value), 2);
        phase += step;
        if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
    }
}

void feed(CliTextDecoder &decoder, const QByteArray &pcm, QString &text) {
    constexpr int bytesPerChunk = 1920;
    for (int offset = 0; offset < pcm.size(); offset += bytesPerChunk)
        decoder.process(pcm.mid(offset, bytesPerChunk), [&](const QJsonObject &line) {
            text += line.value(QStringLiteral("text")).toString();
        });
}
}

int main() {
    CliTextDecoder decoder;
    CliTextDecoder::Settings settings;
    settings.mode = CliTextDecoder::Mode::Cw;
    settings.cwSelectivity = 10;
    decoder.configure(settings);

    QByteArray noise;
    std::uint32_t random = 0x12345678;
    for (int i = 0; i < 48000 * 5; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        const qint16 value = static_cast<qint16>((random & 0xffff) - 32768);
        noise.append(reinterpret_cast<const char *>(&value), 2);
    }
    QString text;
    feed(decoder, noise, text);
    if (!text.isEmpty() || decoder.status().value(QStringLiteral("gateOpen")).toBool()) return 1;

    decoder.reset();
    QByteArray morse;
    const int dot = 3200;
    double phase = 0.0;
    appendSignal(morse, dot * 8, false, phase);
    for (const QString &pattern : {QStringLiteral("..."), QStringLiteral("---"), QStringLiteral("...")}) {
        for (int i = 0; i < pattern.size(); ++i) {
            appendSignal(morse, dot * (pattern[i] == QLatin1Char('-') ? 3 : 1), true, phase);
            if (i + 1 < pattern.size()) appendSignal(morse, dot, false, phase);
        }
        appendSignal(morse, dot * 3, false, phase);
    }
    appendSignal(morse, dot * 8, false, phase);
    text.clear();
    feed(decoder, morse, text);
    return text.trimmed() == QStringLiteral("SOS") ? 0 : 2;
}
