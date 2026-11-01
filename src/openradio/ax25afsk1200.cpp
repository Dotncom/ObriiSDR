#include "ax25afsk1200.h"

#include "ax25codec.h"

#include <QBuffer>
#include <QDataStream>

#include <algorithm>
#include <cmath>

namespace {
constexpr int Baud = 1200;
constexpr double MarkHz = 1200.0;
constexpr double SpaceHz = 2200.0;
constexpr double Pi = 3.14159265358979323846;

void setError(QString *error, const QString &message) {
    if (error) *error = message;
}

bool validSampleRate(int sampleRate) {
    return sampleRate >= 12000 && sampleRate <= 192000 && sampleRate % Baud == 0;
}

void appendByteBits(QVector<quint8> *bits, quint8 byte) {
    for (int bit = 0; bit < 8; ++bit) bits->append((byte >> bit) & 1U);
}

QList<QByteArray> decodeBits(const QVector<quint8> &tones) {
    QVector<quint8> bits(tones.size(), 0);
    for (int i = 1; i < tones.size(); ++i) {
        bits[i] = tones[i] == tones[i - 1] ? 1U : 0U;
    }
    QVector<int> flags;
    for (int i = 0; i + 8 <= bits.size(); ++i) {
        if (bits[i] != 0 || bits[i + 7] != 0) continue;
        bool flag = true;
        for (int j = 1; j <= 6; ++j) flag = flag && bits[i + j] == 1;
        if (flag) {
            flags.append(i);
            i += 7;
        }
    }
    QList<QByteArray> frames;
    for (int flag = 1; flag < flags.size(); ++flag) {
        const int first = flags[flag - 1] + 8;
        const int last = flags[flag];
        if (last - first < 18 * 8 || last - first > 4096 * 8) continue;
        QVector<quint8> unstuffed;
        unstuffed.reserve(last - first);
        int ones = 0;
        bool valid = true;
        for (int i = first; i < last; ++i) {
            const quint8 bit = bits[i];
            unstuffed.append(bit);
            if (bit == 0) {
                ones = 0;
            } else if (++ones == 5) {
                if (i + 1 >= last || bits[i + 1] != 0) {
                    valid = false;
                    break;
                }
                ++i;
                ones = 0;
            }
        }
        if (!valid || unstuffed.size() % 8 != 0) continue;
        QByteArray bytes(unstuffed.size() / 8, 0);
        for (int i = 0; i < unstuffed.size(); ++i) {
            bytes[i / 8] = static_cast<char>(
                static_cast<quint8>(bytes[i / 8]) | (unstuffed[i] << (i % 8)));
        }
        const quint16 fcs = static_cast<quint8>(bytes[bytes.size() - 2]) |
            (static_cast<quint16>(static_cast<quint8>(bytes[bytes.size() - 1])) << 8U);
        if (Ax25Codec::fcs(bytes.left(bytes.size() - 2)) == fcs) frames.append(bytes);
    }
    return frames;
}
}

QVector<float> Ax25Afsk1200::modulate(const QByteArray &ax25WithoutFcs,
                                       int sampleRate,
                                       int preambleFlags,
                                       QString *error) {
    if (!validSampleRate(sampleRate) || preambleFlags < 1 || preambleFlags > 256 ||
        ax25WithoutFcs.size() < 16 || ax25WithoutFcs.size() > 4094) {
        setError(error, QStringLiteral("AFSK1200 requires 12..192 ksps divisible by 1200, 1..256 flags and an AX.25 frame of 16..4094 bytes"));
        return {};
    }
    QByteArray frame = ax25WithoutFcs;
    const quint16 fcs = Ax25Codec::fcs(frame);
    frame.append(static_cast<char>(fcs & 0xffU));
    frame.append(static_cast<char>(fcs >> 8U));

    QVector<quint8> bits;
    bits.reserve((preambleFlags + 3) * 8 + frame.size() * 10);
    for (int i = 0; i < preambleFlags; ++i) appendByteBits(&bits, 0x7eU);
    int ones = 0;
    for (char byte : frame) {
        for (int bit = 0; bit < 8; ++bit) {
            const quint8 value = (static_cast<quint8>(byte) >> bit) & 1U;
            bits.append(value);
            if (value == 0) {
                ones = 0;
            } else if (++ones == 5) {
                bits.append(0);
                ones = 0;
            }
        }
    }
    for (int i = 0; i < 3; ++i) appendByteBits(&bits, 0x7eU);

    const int samplesPerBit = sampleRate / Baud;
    QVector<float> audio;
    audio.reserve(bits.size() * samplesPerBit);
    double phase = 0.0;
    bool mark = true;
    for (quint8 bit : bits) {
        if (bit == 0) mark = !mark;
        const double phaseStep = 2.0 * Pi * (mark ? MarkHz : SpaceHz) / sampleRate;
        for (int sample = 0; sample < samplesPerBit; ++sample) {
            audio.append(static_cast<float>(0.8 * std::sin(phase)));
            phase += phaseStep;
            if (phase >= 2.0 * Pi) phase -= 2.0 * Pi;
        }
    }
    if (error) error->clear();
    return audio;
}

QList<QByteArray> Ax25Afsk1200::demodulate(const QVector<float> &audio,
                                            int sampleRate,
                                            QString *error) {
    if (!validSampleRate(sampleRate) || audio.size() < sampleRate / 10 ||
        audio.size() > sampleRate * 30) {
        setError(error, QStringLiteral("AFSK1200 expects 0.1..30 s of 12..192 ksps audio divisible by 1200"));
        return {};
    }
    const int samplesPerBit = sampleRate / Baud;
    QVector<double> markCos(samplesPerBit), markSin(samplesPerBit);
    QVector<double> spaceCos(samplesPerBit), spaceSin(samplesPerBit);
    for (int i = 0; i < samplesPerBit; ++i) {
        const double time = static_cast<double>(i) / sampleRate;
        markCos[i] = std::cos(2.0 * Pi * MarkHz * time);
        markSin[i] = std::sin(2.0 * Pi * MarkHz * time);
        spaceCos[i] = std::cos(2.0 * Pi * SpaceHz * time);
        spaceSin[i] = std::sin(2.0 * Pi * SpaceHz * time);
    }
    const int phaseStep = (std::max)(1, samplesPerBit / 8);
    for (int offset = 0; offset < samplesPerBit; offset += phaseStep) {
        QVector<quint8> tones;
        tones.reserve((audio.size() - offset) / samplesPerBit);
        for (int start = offset; start + samplesPerBit <= audio.size();
             start += samplesPerBit) {
            double mr = 0.0, mi = 0.0, sr = 0.0, si = 0.0;
            for (int i = 0; i < samplesPerBit; ++i) {
                const double value = audio[start + i];
                mr += value * markCos[i];
                mi += value * markSin[i];
                sr += value * spaceCos[i];
                si += value * spaceSin[i];
            }
            tones.append((mr * mr + mi * mi) >= (sr * sr + si * si) ? 1U : 0U);
        }
        const QList<QByteArray> frames = decodeBits(tones);
        if (!frames.isEmpty()) {
            if (error) error->clear();
            return frames;
        }
    }
    setError(error, QStringLiteral("No AX.25 AFSK1200 frame passed FCS; check tone, baud, sample rate and signal quality"));
    return {};
}

QByteArray Ax25Afsk1200::encodePcm16Wav(const QVector<float> &audio,
                                         int sampleRate,
                                         QString *error) {
    if (!validSampleRate(sampleRate) || audio.isEmpty() ||
        audio.size() > sampleRate * 30) {
        setError(error, QStringLiteral("Invalid AFSK1200 audio length or sample rate"));
        return {};
    }
    QByteArray wav;
    QDataStream stream(&wav, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << static_cast<quint32>(36 + audio.size() * 2);
    stream.writeRawData("WAVEfmt ", 8);
    stream << static_cast<quint32>(16) << static_cast<quint16>(1)
           << static_cast<quint16>(1) << static_cast<quint32>(sampleRate)
           << static_cast<quint32>(sampleRate * 2) << static_cast<quint16>(2)
           << static_cast<quint16>(16);
    stream.writeRawData("data", 4);
    stream << static_cast<quint32>(audio.size() * 2);
    for (float sample : audio) {
        stream << static_cast<qint16>(std::lround(
            (std::clamp)(sample, -1.0f, 1.0f) * 32767.0f));
    }
    if (error) error->clear();
    return wav;
}

bool Ax25Afsk1200::decodePcm16Wav(const QByteArray &wav,
                                   QVector<float> *audio,
                                   int *sampleRate,
                                   QString *error) {
    if (!audio || !sampleRate || wav.size() < 44 || wav.left(4) != "RIFF" ||
        wav.mid(8, 4) != "WAVE") {
        setError(error, QStringLiteral("Not a RIFF/WAVE file"));
        return false;
    }
    QBuffer buffer;
    buffer.setData(wav);
    buffer.open(QIODevice::ReadOnly);
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    buffer.seek(12);
    int rate = 0, channels = 0, bits = 0, format = 0;
    qint64 dataOffset = -1;
    quint32 dataBytes = 0;
    while (buffer.pos() + 8 <= wav.size()) {
        char tag[4];
        if (stream.readRawData(tag, 4) != 4) break;
        quint32 chunkBytes = 0;
        stream >> chunkBytes;
        const qint64 chunkStart = buffer.pos();
        if (chunkBytes > static_cast<quint32>(wav.size() - chunkStart)) break;
        if (QByteArray(tag, 4) == "fmt " && chunkBytes >= 16) {
            quint16 pcmFormat = 0, channelCount = 0, blockAlign = 0, bitCount = 0;
            quint32 sampleHz = 0, byteRate = 0;
            stream >> pcmFormat >> channelCount >> sampleHz >> byteRate
                   >> blockAlign >> bitCount;
            format = pcmFormat;
            channels = channelCount;
            rate = static_cast<int>(sampleHz);
            bits = bitCount;
        } else if (QByteArray(tag, 4) == "data") {
            dataOffset = chunkStart;
            dataBytes = chunkBytes;
        }
        if (!buffer.seek(chunkStart + chunkBytes + (chunkBytes & 1U))) break;
    }
    if (format != 1 || (channels != 1 && channels != 2) || bits != 16 ||
        !validSampleRate(rate) || dataOffset < 0 || dataBytes < 2U * channels ||
        dataBytes % (2U * channels) != 0 ||
        dataBytes > static_cast<quint32>(rate * channels * 2 * 30)) {
        setError(error, QStringLiteral("AFSK1200 WAV requires PCM16 mono/stereo, 12..192 ksps divisible by 1200, up to 30 s"));
        return false;
    }
    buffer.seek(dataOffset);
    QVector<float> decoded;
    decoded.reserve(static_cast<int>(dataBytes / (2U * channels)));
    for (quint32 i = 0; i < dataBytes / (2U * channels); ++i) {
        float mixed = 0.0f;
        for (int channel = 0; channel < channels; ++channel) {
            qint16 sample = 0;
            stream >> sample;
            mixed += static_cast<float>(sample) / 32768.0f;
        }
        decoded.append(mixed / channels);
    }
    *audio = std::move(decoded);
    *sampleRate = rate;
    if (error) error->clear();
    return true;
}
