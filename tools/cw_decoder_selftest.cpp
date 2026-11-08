#include "cwaudiodecoder.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>

#include <cmath>
#include <cstring>

namespace {
constexpr double Pi = 3.14159265358979323846;

void appendTone(QByteArray &pcm, int samples, int sampleRate, double toneHz, bool keyed, double &phase) {
    const double step = 2.0 * Pi * toneHz / sampleRate;
    for (int index = 0; index < samples; ++index) {
        const qint16 value = keyed
                                 ? static_cast<qint16>(std::sin(phase) * 22000.0)
                                 : 0;
        pcm.append(reinterpret_cast<const char *>(&value), sizeof(value));
        phase += step;
        if (phase >= 2.0 * Pi) phase -= 2.0 * Pi;
    }
}

QByteArray makeMorse(const QStringList &words, int sampleRate, double toneHz, int wpm) {
    const int unit = qRound(1.2 * sampleRate / wpm);
    QByteArray pcm;
    double phase = 0.0;
    appendTone(pcm, unit * 8, sampleRate, toneHz, false, phase);
    for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
        const QStringList characters = words[wordIndex].split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (int charIndex = 0; charIndex < characters.size(); ++charIndex) {
            const QString pattern = characters[charIndex];
            for (int symbol = 0; symbol < pattern.size(); ++symbol) {
                appendTone(pcm,
                           unit * (pattern[symbol] == QLatin1Char('-') ? 3 : 1),
                           sampleRate,
                           toneHz,
                           true,
                           phase);
                if (symbol + 1 < pattern.size()) {
                    appendTone(pcm, unit, sampleRate, toneHz, false, phase);
                }
            }
            if (charIndex + 1 < characters.size()) {
                appendTone(pcm, unit * 3, sampleRate, toneHz, false, phase);
            }
        }
        if (wordIndex + 1 < words.size()) {
            appendTone(pcm, unit * 7, sampleRate, toneHz, false, phase);
        }
    }
    appendTone(pcm, unit * 8, sampleRate, toneHz, false, phase);
    return pcm;
}

QString decode(const QByteArray &pcm, int alphabet, int selectivity = 5) {
    CwAudioDecoder decoder;
    CwAudioDecoder::Settings settings;
    settings.sampleRate = 48000;
    settings.toneHz = 700.0;
    settings.initialWpm = 18;
    settings.adaptiveSpeed = true;
    settings.alphabet = alphabet;
    settings.selectivity = selectivity;
    decoder.configure(settings);
    QString text;
    constexpr int chunkBytes = 48000 / 50 * 2;
    for (int offset = 0; offset < pcm.size(); offset += chunkBytes) {
        text += decoder.processPcm16(pcm.mid(offset, chunkBytes)).text;
    }
    return text.trimmed();
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QByteArray english = makeMorse({QStringLiteral("... --- ..."),
                                          QStringLiteral("- . ... -")},
                                         48000,
                                         700.0,
                                         18);
    const QString englishDecoded = decode(english, 0);
    qInfo().noquote() << "English:" << englishDecoded;
    if (englishDecoded != QStringLiteral("SOS TEST")) return 1;
    for (const int selectivity : {1, 3, 5, 10}) {
        const QString decoded = decode(english, 0, selectivity);
        if (decoded != QStringLiteral("SOS TEST")) {
            qWarning() << "CW selectivity failure" << selectivity << decoded;
            return 4 + selectivity;
        }
    }

    const QByteArray ukrainian = makeMorse({QStringLiteral(".... --.")},
                                           48000,
                                           700.0,
                                           18);
    const QString ukrainianDecoded = decode(ukrainian, 1);
    qInfo().noquote() << "Ukrainian:" << ukrainianDecoded;
    if (ukrainianDecoded != QString::fromUtf8(u8"ГҐ")) return 2;

    const QString bilingualDecoded = decode(ukrainian, 2);
    qInfo().noquote() << "Bilingual:" << bilingualDecoded;
    if (bilingualDecoded != QString::fromUtf8(u8"EN: HG | UK: ГҐ")) return 3;
    return 0;
}
