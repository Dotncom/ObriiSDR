#include "cliaudiooutput.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    const QString path = temporary.filePath(QStringLiteral("audio.wav"));
    CliAudioOutput output;
    QString error;
    constexpr double rate = 480000.0;
    if (!output.open(path, false, INPUT_RF, MOD_AM, 10000.0,
                     rate, 100e6, 100e6, true, &error)) {
        std::cerr << error.toStdString() << '\n'; return 1;
    }
    std::vector<float> iq(48000 * 2);
    for (std::size_t n = 0; n < iq.size() / 2; ++n) {
        iq[2 * n] = 0.3f * (1.0f + 0.4f * std::sin(6.283185307179586 * 1000.0 * n / rate));
        iq[2 * n + 1] = 0.0f;
    }
    for (std::size_t offset = 0; offset < iq.size() / 2; offset += 4096) {
        const std::size_t count = (std::min)(std::size_t(4096), iq.size() / 2 - offset);
        if (!output.process(iq.data() + offset * 2, count, &error)) {
            std::cerr << error.toStdString() << '\n'; return 1;
        }
    }
    if (!output.close(&error)) { std::cerr << error.toStdString() << '\n'; return 1; }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 1;
    const QByteArray wave = file.readAll();
    if (wave.size() < 9000 || wave.left(4) != "RIFF" || wave.mid(8, 4) != "WAVE" ||
        wave.mid(36, 4) != "data" || wave.size() % 2) return 1;
    const auto *bytes = reinterpret_cast<const unsigned char *>(wave.constData());
    const quint32 declared = quint32(bytes[40]) | (quint32(bytes[41]) << 8) |
                             (quint32(bytes[42]) << 16) | (quint32(bytes[43]) << 24);
    if (declared != static_cast<quint32>(wave.size() - 44)) return 1;
    double energy = 0.0;
    for (int i = 44; i + 1 < wave.size(); i += 2) {
        const qint16 sample = static_cast<qint16>(quint16(bytes[i]) | (quint16(bytes[i + 1]) << 8));
        energy += double(sample) * sample;
    }
    if (std::sqrt(energy / (declared / 2)) < 100.0) return 1;
    std::cout << "CLI audio self-test passed\n";
    return 0;
}
