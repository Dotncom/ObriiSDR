#include "spectrumframerecorder.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr char kSpectrumMagic[] = "FBSPEC1\n";
constexpr quint32 kFrameMagic = 0x314d5246U; // FRM1, little-endian through QDataStream.

} // namespace

bool SpectrumFrameRecorder::start(const RadioSettings &settings,
                                  double displayScalePercent,
                                  int targetBins,
                                  qint64 firstFrameUtcMs,
                                  QString *errorMessage) {
    stop();

    recordingSettings = settings;
    scalePercent = std::isfinite(displayScalePercent) && displayScalePercent > 0.0
                       ? displayScalePercent
                       : 100.0;
    binLimit = std::clamp(targetBins, 256, 2097152);
    startedAtUtc = QDateTime::currentDateTimeUtc();
    firstUtcMs = firstFrameUtcMs > 0 ? firstFrameUtcMs : startedAtUtc.toMSecsSinceEpoch();
    framesWritten = 0;
    filePath = makeRecordingPath();

    QDir dir = QFileInfo(filePath).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot create spectrum recordings folder");
        }
        return false;
    }

    outputFile.setFileName(filePath);
    if (!outputFile.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = outputFile.errorString();
        }
        return false;
    }

    if (!writeHeader(errorMessage)) {
        outputFile.close();
        return false;
    }

    active = true;
    return true;
}

void SpectrumFrameRecorder::stop() {
    if (outputFile.isOpen()) {
        outputFile.flush();
        outputFile.close();
    }
    active = false;
}

bool SpectrumFrameRecorder::isRecording() const {
    return active;
}

QString SpectrumFrameRecorder::currentFilePath() const {
    return filePath;
}

quint64 SpectrumFrameRecorder::frameCount() const {
    return framesWritten;
}

int SpectrumFrameRecorder::targetBins() const {
    return binLimit;
}

qint64 SpectrumFrameRecorder::firstFrameUtcMs() const {
    return firstUtcMs;
}

void SpectrumFrameRecorder::setExtendedMetadata(const QJsonObject &metadata) {
    extendedMetadata = metadata;
}

bool SpectrumFrameRecorder::appendFrame(const std::vector<float> &frequencies,
                                        const std::vector<float> &magnitudes,
                                        double centerFrequency,
                                        double minFrequency,
                                        double maxFrequency,
                                        int fftLength) {
    if (!active || !outputFile.isOpen() || frequencies.size() != magnitudes.size() || magnitudes.empty()) {
        return false;
    }

    return appendFrameRecord(makeFrame(frequencies,
                                       magnitudes,
                                       centerFrequency,
                                       minFrequency,
                                       maxFrequency,
                                       fftLength,
                                       binLimit,
                                       QDateTime::currentMSecsSinceEpoch()));
}

bool SpectrumFrameRecorder::appendFrameRecord(const SpectrumFrameRecord &frame) {
    if (!active || !outputFile.isOpen() || frame.magnitudes.empty()) {
        return false;
    }

    QDataStream stream(&outputFile);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_5_15);
    stream << kFrameMagic;
    stream << static_cast<qint64>(frame.utcMs - firstUtcMs);
    stream << static_cast<qint64>(frame.utcMs);
    stream << frame.centerFrequency;
    stream << frame.minFrequency;
    stream << frame.maxFrequency;
    stream << static_cast<qint32>(frame.fftLength);
    stream << static_cast<qint32>(frame.magnitudes.size());
    const qint64 bytes = static_cast<qint64>(frame.magnitudes.size() * sizeof(float));
    if (outputFile.write(reinterpret_cast<const char *>(frame.magnitudes.data()), bytes) != bytes) {
        active = false;
        return false;
    }
    ++framesWritten;
    return true;
}

bool SpectrumFrameReader::open(const QString &path, QString *errorMessage) {
    file.close(); header = {};
    if (errorMessage) errorMessage->clear();
    file.setFileName(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }

    const QByteArray magic = file.read(8);
    if (magic != QByteArray(kSpectrumMagic, 8)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("not an Obrii SDR spectrum-frame recording");
        }
        return false;
    }

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_5_15);

    quint32 metadataSize = 0;
    stream >> metadataSize;
    if (metadataSize == 0 || metadataSize > 16 * 1024 * 1024) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("invalid spectrum recording metadata");
        }
        return false;
    }
    const QByteArray metadataBytes = file.read(static_cast<qint64>(metadataSize));
    const QJsonDocument metadataDoc = QJsonDocument::fromJson(metadataBytes);
    if (!metadataDoc.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("spectrum recording metadata is not JSON");
        }
        return false;
    }
    header = metadataDoc.object();
    return true;
}

bool SpectrumFrameReader::readNext(SpectrumFrameRecord &frame, QString *errorMessage) {
    if (errorMessage) errorMessage->clear();
    frame = {};
    if (!file.isOpen()) { if (errorMessage) *errorMessage = QStringLiteral("spectrum file is not open"); return false; }
    if (file.atEnd()) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_5_15);
        quint32 frameMagic = 0;
        stream >> frameMagic;
        if (stream.status() != QDataStream::Ok) {
            if (errorMessage) *errorMessage = QStringLiteral("truncated spectrum frame marker");
            return false;
        }
        if (frameMagic != kFrameMagic) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("spectrum recording frame marker mismatch");
            }
            return false;
        }

        qint32 fftLength = 0;
        qint32 count = 0;
        stream >> frame.elapsedMs;
        stream >> frame.utcMs;
        stream >> frame.centerFrequency;
        stream >> frame.minFrequency;
        stream >> frame.maxFrequency;
        stream >> fftLength;
        stream >> count;
        if (stream.status() != QDataStream::Ok || count <= 0 || count > 2097152) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("invalid spectrum recording frame");
            }
            return false;
        }
        frame.fftLength = fftLength;
        frame.magnitudes.resize(static_cast<std::size_t>(count));
        const qint64 bytes = static_cast<qint64>(frame.magnitudes.size() * sizeof(float));
        if (file.read(reinterpret_cast<char *>(frame.magnitudes.data()), bytes) != bytes) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("truncated spectrum recording frame");
            }
            return false;
        }
    if (!std::isfinite(frame.centerFrequency) || !std::isfinite(frame.minFrequency) ||
        !std::isfinite(frame.maxFrequency) || frame.maxFrequency < frame.minFrequency || fftLength <= 0) {
        if (errorMessage) *errorMessage = QStringLiteral("invalid spectrum frame geometry");
        return false;
    }
    return true;
}

bool SpectrumFrameRecorder::loadFile(const QString &path,
                                     SpectrumFrameRecording &recording,
                                     QString *errorMessage) {
    recording = {};
    SpectrumFrameReader reader;
    if (!reader.open(path, errorMessage)) return false;
    recording.metadata = reader.metadata();
    while (!reader.atEnd()) {
        SpectrumFrameRecord frame;
        if (!reader.readNext(frame, errorMessage)) return false;
        recording.frames.push_back(std::move(frame));
    }

    if (recording.frames.empty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("spectrum recording has no frames");
        }
        return false;
    }
    return true;
}

std::vector<float> SpectrumFrameRecorder::reduceMagnitudes(const std::vector<float> &magnitudes, int targetBins) {
    if (magnitudes.empty()) {
        return {};
    }
    const int sourceCount = static_cast<int>(magnitudes.size());
    const int outputCount = std::clamp(targetBins, 1, sourceCount);
    if (outputCount == sourceCount) {
        return magnitudes;
    }

    std::vector<float> reduced(static_cast<std::size_t>(outputCount), -std::numeric_limits<float>::infinity());
    for (int bin = 0; bin < outputCount; ++bin) {
        const int begin = static_cast<int>((static_cast<qint64>(bin) * sourceCount) / outputCount);
        const int end = static_cast<int>((static_cast<qint64>(bin + 1) * sourceCount) / outputCount);
        float peak = -std::numeric_limits<float>::infinity();
        for (int i = begin; i < std::max(begin + 1, end); ++i) {
            const float value = magnitudes[static_cast<std::size_t>(std::min(i, sourceCount - 1))];
            if (std::isfinite(value)) {
                peak = std::max(peak, value);
            }
        }
        reduced[static_cast<std::size_t>(bin)] =
            std::isfinite(peak) ? peak : -160.0f;
    }
    return reduced;
}

SpectrumFrameRecord SpectrumFrameRecorder::makeFrame(const std::vector<float> &frequencies,
                                                     const std::vector<float> &magnitudes,
                                                     double centerFrequency,
                                                     double minFrequency,
                                                     double maxFrequency,
                                                     int fftLength,
                                                     int targetBins,
                                                     qint64 utcMs) {
    SpectrumFrameRecord frame;
    if (frequencies.size() != magnitudes.size() || magnitudes.empty()) {
        return frame;
    }
    frame.utcMs = utcMs > 0 ? utcMs : QDateTime::currentMSecsSinceEpoch();
    frame.centerFrequency = centerFrequency;
    frame.minFrequency = minFrequency;
    frame.maxFrequency = maxFrequency;
    frame.fftLength = fftLength;
    Q_UNUSED(frequencies);
    frame.magnitudes = reduceMagnitudes(magnitudes, targetBins);
    return frame;
}

QString SpectrumFrameRecorder::makeRecordingPath() {
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("recordings/spectrum/ObriiSDR_%1_spectrum.fbspec").arg(timestamp));
}

QJsonObject SpectrumFrameRecorder::makeMetadataObject() const {
    QJsonObject root;
    root["app"] = QStringLiteral("ObriiSDR");
    root["format"] = QStringLiteral("fobos_spectrum_frames");
    root["version"] = 1;
    root["recordedAtUtc"] = startedAtUtc.toString(Qt::ISODateWithMs);
    root["recordedAtLocal"] = startedAtUtc.toLocalTime().toString(Qt::ISODateWithMs);
    root["targetBins"] = binLimit;
    root["scalePercent"] = scalePercent;
    root["deviceIndex"] = recordingSettings.deviceIndex;
    root["clockSource"] = recordingSettings.clockSource;
    root["inputMode"] = recordingSettings.inputMode;
    root["centerFrequency"] = recordingSettings.centerFrequency;
    root["actualFrequency"] = recordingSettings.actualFrequency;
    root["listeningFrequency"] = recordingSettings.listeningFrequency;
    root["sourceSampleRate"] = recordingSettings.sampleRate;
    root["bandwidth"] = recordingSettings.bandwidth;
    root["modulationType"] = recordingSettings.modulationType;
    root["fftLength"] = recordingSettings.fftLength;
    root["fftWindowType"] = normalizedFftWindowType(recordingSettings.fftWindowType);
    root["lnaGain"] = recordingSettings.lnaGain;
    root["vgaGain"] = recordingSettings.vgaGain;
    root["gpoValue"] = static_cast<int>(recordingSettings.gpoValue);
    if (!extendedMetadata.isEmpty()) {
        root["scientificMetadata"] = extendedMetadata;
    }
    return root;
}

bool SpectrumFrameRecorder::writeHeader(QString *errorMessage) {
    outputFile.write(kSpectrumMagic, 8);
    const QByteArray metadata = QJsonDocument(makeMetadataObject()).toJson(QJsonDocument::Compact);
    if (metadata.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot create spectrum recording metadata");
        }
        return false;
    }

    QDataStream stream(&outputFile);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_5_15);
    stream << static_cast<quint32>(metadata.size());
    if (outputFile.write(metadata) != metadata.size()) {
        if (errorMessage) {
            *errorMessage = outputFile.errorString();
        }
        return false;
    }
    return true;
}
