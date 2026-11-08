#include "transmitmediagenerator.h"

#include <QColor>

#include "transmitwaveformgenerator.h"

#include <QFile>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr double Pi = 3.14159265358979323846;

double normalizeAudioForTransmit(QVector<float> *samples) {
    if (!samples || samples->isEmpty()) return 0.0;

    double mean = 0.0;
    for (float sample : *samples) mean += sample;
    mean /= samples->size();

    double sumSquares = 0.0;
    double peak = 0.0;
    for (float sample : *samples) {
        const double centered = sample - mean;
        sumSquares += centered * centered;
        peak = (std::max)(peak, std::abs(centered));
    }
    const double rms = std::sqrt(sumSquares / samples->size());
    if (peak < 1.0e-7 || rms < 1.0e-8) return 0.0;

    constexpr double TargetPeak = 0.92;
    constexpr double TargetRms = 0.20;
    constexpr double MaximumGain = 7.943282347; // +18 dB
    const double gain = (std::clamp)((std::min)(TargetPeak / peak, TargetRms / rms),
                                     0.10, MaximumGain);
    for (float &sample : *samples) {
        sample = static_cast<float>((std::clamp)((sample - mean) * gain,
                                                 -TargetPeak, TargetPeak));
    }
    return 20.0 * std::log10(gain);
}

struct SstvMode {
    const char *id;
    int vis;
    int height;
    double sync;
    double channel;
    double separator;
    bool robot;
};

const SstvMode *modeForId(const QString &id) {
    static const SstvMode modes[] = {
        {"robot36", 8, 240, 0.009, 0.044, 0.0045, true},
        {"robot72", 12, 240, 0.009, 0.088, 0.0045, true},
        {"martin-m1", 44, 256, 0.004862, 0.146432, 0.000572, false},
        {"martin-m2", 40, 256, 0.004862, 0.073216, 0.000572, false}
    };
    for (const SstvMode &mode : modes) {
        if (id == QLatin1String(mode.id)) return &mode;
    }
    return nullptr;
}

void appendTone(QVector<float> &audio,
                int sampleRate,
                double frequency,
                double seconds,
                double &phase,
                float amplitude = 0.82f) {
    const int count = (std::max)(1, qRound(seconds * sampleRate));
    const int oldSize = audio.size();
    audio.resize(oldSize + count);
    const double step = 2.0 * Pi * frequency / sampleRate;
    for (int index = 0; index < count; ++index) {
        audio[oldSize + index] = amplitude * static_cast<float>(std::sin(phase));
        phase = std::remainder(phase + step, 2.0 * Pi);
    }
}

void appendPixels(QVector<float> &audio,
                  int sampleRate,
                  const QVector<uchar> &pixels,
                  double seconds,
                  double &phase) {
    const int count = (std::max)(1, qRound(seconds * sampleRate));
    const int oldSize = audio.size();
    audio.resize(oldSize + count);
    for (int index = 0; index < count; ++index) {
        const int pixel = (std::min)(pixels.size() - 1,
                                     static_cast<int>((static_cast<qint64>(index) * pixels.size()) / count));
        const double frequency = 1500.0 + (800.0 * pixels[pixel] / 255.0);
        audio[oldSize + index] = 0.82f * static_cast<float>(std::sin(phase));
        phase = std::remainder(phase + 2.0 * Pi * frequency / sampleRate, 2.0 * Pi);
    }
}

void appendVis(QVector<float> &audio, int sampleRate, int vis, double &phase) {
    appendTone(audio, sampleRate, 1900.0, 0.300, phase);
    appendTone(audio, sampleRate, 1200.0, 0.010, phase);
    appendTone(audio, sampleRate, 1900.0, 0.300, phase);
    appendTone(audio, sampleRate, 1200.0, 0.030, phase);
    int ones = 0;
    for (int bit = 0; bit < 7; ++bit) {
        const bool one = (vis & (1 << bit)) != 0;
        ones += one ? 1 : 0;
        appendTone(audio, sampleRate, one ? 1100.0 : 1300.0, 0.030, phase);
    }
    appendTone(audio, sampleRate, (ones & 1) ? 1100.0 : 1300.0, 0.030, phase);
    appendTone(audio, sampleRate, 1200.0, 0.030, phase);
}

quint32 readU32(const QByteArray &data, int offset) {
    return offset + 4 <= data.size()
               ? qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + offset))
               : 0;
}

quint16 readU16(const QByteArray &data, int offset) {
    return offset + 2 <= data.size()
               ? qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(data.constData() + offset))
               : 0;
}
}

QVector<float> TransmitMediaGenerator::resampleAudio(const QVector<float> &input,
                                                       int sourceRate,
                                                       int targetRate) {
    if (input.isEmpty() || sourceRate <= 0 || targetRate <= 0) return {};
    if (sourceRate == targetRate) return input;

    const qsizetype outputCount = static_cast<qsizetype>(
        std::llround(static_cast<double>(input.size()) * targetRate / sourceRate));
    QVector<float> output(outputCount);
    const double sourceStep = static_cast<double>(sourceRate) / targetRate;
    const double cutoff = 0.94 * (std::min)(1.0, static_cast<double>(targetRate) / sourceRate);
    constexpr int Radius = 24;
    const auto sinc = [](double value) {
        if (std::abs(value) < 1.0e-12) return 1.0;
        const double angle = Pi * value;
        return std::sin(angle) / angle;
    };

    for (qsizetype outputIndex = 0; outputIndex < outputCount; ++outputIndex) {
        const double position = outputIndex * sourceStep;
        const int center = static_cast<int>(std::floor(position));
        double weighted = 0.0;
        double weightSum = 0.0;
        for (int tap = -Radius + 1; tap <= Radius; ++tap) {
            const int sourceIndex = center + tap;
            if (sourceIndex < 0 || sourceIndex >= input.size()) continue;
            const double distance = sourceIndex - position;
            const double windowPosition = std::abs(distance) / Radius;
            if (windowPosition >= 1.0) continue;
            const double window = 0.5 + 0.5 * std::cos(Pi * windowPosition);
            const double weight = cutoff * sinc(cutoff * distance) * window;
            weighted += input[sourceIndex] * weight;
            weightSum += weight;
        }
        output[outputIndex] = std::abs(weightSum) > 1.0e-12
                                  ? static_cast<float>(weighted / weightSum)
                                  : 0.0f;
    }
    return output;
}

TxAudioMedia TransmitMediaGenerator::loadWav(const QString &path, int targetSampleRate) {
    TxAudioMedia result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    const QByteArray data = file.readAll();
    if (data.size() < 44 || data.mid(0, 4) != "RIFF" || data.mid(8, 4) != "WAVE") {
        result.error = QStringLiteral("Only RIFF/WAVE audio files are supported");
        return result;
    }
    int formatOffset = -1;
    int pcmOffset = -1;
    int pcmBytes = 0;
    for (int offset = 12; offset + 8 <= data.size();) {
        const QByteArray id = data.mid(offset, 4);
        const int size = static_cast<int>(readU32(data, offset + 4));
        if (id == "fmt ") formatOffset = offset + 8;
        if (id == "data") { pcmOffset = offset + 8; pcmBytes = (std::min)(size, data.size() - pcmOffset); }
        offset += 8 + size + (size & 1);
    }
    if (formatOffset < 0 || pcmOffset < 0) {
        result.error = QStringLiteral("WAVE file has no fmt or data chunk");
        return result;
    }
    const int format = readU16(data, formatOffset);
    const int channels = readU16(data, formatOffset + 2);
    const int sourceRate = static_cast<int>(readU32(data, formatOffset + 4));
    const int bits = readU16(data, formatOffset + 14);
    const int bytesPerSample = bits / 8;
    const int frameBytes = channels * bytesPerSample;
    if ((format != 1 && format != 3) || channels < 1 || channels > 8 ||
        sourceRate <= 0 || frameBytes <= 0) {
        result.error = QStringLiteral("Unsupported WAVE format");
        return result;
    }
    QVector<float> mono;
    mono.reserve(pcmBytes / frameBytes);
    for (int frame = 0; frame + frameBytes <= pcmBytes; frame += frameBytes) {
        double sum = 0.0;
        for (int channel = 0; channel < channels; ++channel) {
            const char *sample = data.constData() + pcmOffset + frame + channel * bytesPerSample;
            float value = 0.0f;
            if (format == 3 && bits == 32) {
                std::memcpy(&value, sample, sizeof(float));
            } else if (format == 1 && bits == 16) {
                value = static_cast<float>(qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(sample))) / 32768.0f;
            } else if (format == 1 && bits == 8) {
                value = (static_cast<unsigned char>(*sample) - 128) / 128.0f;
            } else if (format == 1 && bits == 24) {
                qint32 raw = static_cast<unsigned char>(sample[0]) |
                             (static_cast<unsigned char>(sample[1]) << 8) |
                             (static_cast<unsigned char>(sample[2]) << 16);
                if (raw & 0x800000) raw |= ~0xffffff;
                value = static_cast<float>(raw) / 8388608.0f;
            } else {
                result.error = QStringLiteral("Supported WAVE encodings: PCM 8/16/24-bit and float32");
                return result;
            }
            sum += value;
        }
        mono.append(static_cast<float>((std::clamp)(sum / channels, -1.0, 1.0)));
    }
    result.samples = resampleAudio(mono, sourceRate, targetSampleRate);
    const double normalizationDb = normalizeAudioForTransmit(&result.samples);
    result.sampleRate = targetSampleRate;
    result.summary = QStringLiteral("WAV: %1 Hz -> %2 Hz, %3 s | source gain %4 dB")
                         .arg(sourceRate).arg(targetSampleRate)
                         .arg(static_cast<double>(result.samples.size()) / targetSampleRate, 0, 'f', 2)
                         .arg(normalizationDb, 0, 'f', 1);
    return result;
}

TxAudioMedia TransmitMediaGenerator::generateSstv(const QImage &input,
                                                   const QString &modeId,
                                                   int sampleRate) {
    TxAudioMedia result;
    const SstvMode *mode = modeForId(modeId);
    if (!mode || input.isNull() || sampleRate < 8000) {
        result.error = QStringLiteral("Invalid SSTV image, mode or sample rate");
        return result;
    }
    const QImage image = input.convertToFormat(QImage::Format_RGB32)
                             .scaled(320, mode->height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QVector<float> audio;
    double phase = 0.0;
    // Let the USB transmitter, PA and receiving discriminator settle before
    // the first 1900 Hz leader instead of clipping the VIS header at startup.
    audio.fill(0.0f, qRound(0.350 * sampleRate));
    appendVis(audio, sampleRate, mode->vis, phase);
    QVector<uchar> r(320), g(320), b(320), yv(320), chroma(160);
    for (int y = 0; y < mode->height; ++y) {
        for (int x = 0; x < 320; ++x) {
            const QColor color(image.pixel(x, y));
            r[x] = static_cast<uchar>(color.red());
            g[x] = static_cast<uchar>(color.green());
            b[x] = static_cast<uchar>(color.blue());
            yv[x] = static_cast<uchar>((std::clamp)(qRound(0.299 * color.red() + 0.587 * color.green() + 0.114 * color.blue()), 0, 255));
        }
        appendTone(audio, sampleRate, 1200.0, mode->sync, phase);
        if (mode->robot) {
            appendTone(audio, sampleRate, 1500.0, 0.003, phase);
            const double lumaSeconds = modeId == QStringLiteral("robot72") ? 0.138 : 0.088;
            appendPixels(audio, sampleRate, yv, lumaSeconds, phase);
            const bool redDifferenceLine = (y & 1) != 0;
            appendTone(audio,
                       sampleRate,
                       modeId == QStringLiteral("robot36")
                           ? (redDifferenceLine ? 1500.0 : 2300.0)
                           : 1500.0,
                       mode->separator,
                       phase);
            for (int x = 0; x < 160; ++x) {
                const int source = x * 2;
                const double yy = yv[source];
                const double component = redDifferenceLine
                    ? 128.0 + 0.713 * (r[source] - yy)
                    : 128.0 + 0.564 * (b[source] - yy);
                chroma[x] = static_cast<uchar>((std::clamp)(qRound(component), 0, 255));
            }
            appendTone(audio, sampleRate, 1900.0, modeId == QStringLiteral("robot72") ? 0.0 : 0.0015, phase);
            appendPixels(audio, sampleRate, chroma,
                         modeId == QStringLiteral("robot72") ? 0.069 : 0.044, phase);
            if (modeId == QStringLiteral("robot72")) {
                appendTone(audio, sampleRate, 1500.0, mode->separator, phase);
                for (int x = 0; x < 160; ++x) {
                    const int source = x * 2;
                    chroma[x] = static_cast<uchar>((std::clamp)(qRound(128.0 + 0.713 * (r[source] - yv[source])), 0, 255));
                }
                appendPixels(audio, sampleRate, chroma, 0.069, phase);
            }
        } else {
            appendTone(audio, sampleRate, 1500.0, mode->separator, phase);
            appendPixels(audio, sampleRate, g, mode->channel, phase);
            appendTone(audio, sampleRate, 1500.0, mode->separator, phase);
            appendPixels(audio, sampleRate, b, mode->channel, phase);
            appendTone(audio, sampleRate, 1500.0, mode->separator, phase);
            appendPixels(audio, sampleRate, r, mode->channel, phase);
            appendTone(audio, sampleRate, 1500.0, mode->separator, phase);
        }
    }
    result.samples = std::move(audio);
    result.sampleRate = sampleRate;
    result.summary = QStringLiteral("SSTV %1: %2 s")
                         .arg(modeId)
                         .arg(static_cast<double>(result.samples.size()) / sampleRate, 0, 'f', 2);
    return result;
}

QVector<std::complex<float>> TransmitMediaGenerator::generateAtvFrame(
    const QImage &input,
    const TxConfiguration &configuration,
    bool fmVideo,
    int width,
    int height,
    double lineRate) {
    if (configuration.sampleRate < 200000 || lineRate <= 0.0) return {};
    QImage image = input;
    if (image.isNull()) {
        image = QImage(width, height, QImage::Format_RGB32);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int bar = (x * 8) / width;
                static const QColor colors[] = {Qt::white, Qt::yellow, Qt::cyan, Qt::green,
                                                Qt::magenta, Qt::red, Qt::blue, Qt::black};
                image.setPixelColor(x, y, colors[(std::clamp)(bar, 0, 7)]);
            }
        }
    }
    image = image.convertToFormat(QImage::Format_RGB32)
                .scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int samplesPerLine = (std::max)(16, qRound(configuration.sampleRate / lineRate));
    constexpr int verticalSyncLines = 3;
    const int totalLines = height + verticalSyncLines;
    QVector<float> video(samplesPerLine * totalLines);
    const int syncSamples = (std::max)(2, qRound(samplesPerLine * 0.08));
    const int porchSamples = (std::max)(1, qRound(samplesPerLine * 0.08));
    const int activeStart = syncSamples + porchSamples;
    const int activeSamples = (std::max)(1, samplesPerLine - activeStart - porchSamples);
    for (int lineIndex = 0; lineIndex < totalLines; ++lineIndex) {
        const int base = lineIndex * samplesPerLine;
        if (lineIndex < verticalSyncLines) {
            const int broadSyncSamples = qRound(samplesPerLine * 0.55);
            for (int sample = 0; sample < samplesPerLine; ++sample) {
                video[base + sample] = sample < broadSyncSamples ? -0.9f : -0.35f;
            }
            continue;
        }
        const int y = lineIndex - verticalSyncLines;
        for (int sample = 0; sample < samplesPerLine; ++sample) {
            float level = -0.9f;
            if (sample >= syncSamples) level = -0.35f;
            if (sample >= activeStart && sample < activeStart + activeSamples) {
                const int x = (std::min)(width - 1,
                    static_cast<int>((static_cast<qint64>(sample - activeStart) * width) / activeSamples));
                level = -0.2f + 1.15f * (qGray(image.pixel(x, y)) / 255.0f);
            }
            video[base + sample] = level;
        }
    }
    TxConfiguration modulation = configuration;
    modulation.modulation = fmVideo ? TxModulation::Nfm : TxModulation::Am;
    if (fmVideo) modulation.deviationHz = (std::max)(25000.0, configuration.deviationHz);
    return TransmitWaveformGenerator::modulateAudio(modulation, video);
}
