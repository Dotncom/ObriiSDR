#include "spectrumiqeventrecorder.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

namespace {

void appendFourCc(QByteArray &data, const char *text) {
    data.append(text, 4);
}

void appendLe16(QByteArray &data, quint16 value) {
    data.append(static_cast<char>(value & 0xff));
    data.append(static_cast<char>((value >> 8) & 0xff));
}

void appendLe32(QByteArray &data, quint32 value) {
    data.append(static_cast<char>(value & 0xff));
    data.append(static_cast<char>((value >> 8) & 0xff));
    data.append(static_cast<char>((value >> 16) & 0xff));
    data.append(static_cast<char>((value >> 24) & 0xff));
}

QByteArray le32Bytes(quint32 value) {
    QByteArray data;
    appendLe32(data, value);
    return data;
}

quint32 size32(quint64 value) {
    return value > 0xffffffffULL ? 0xffffffffU : static_cast<quint32>(value);
}

} // namespace

bool SpectrumIqEventRecorder::start(const QString &eventBasePath,
                                    Mode mode,
                                    const RadioSettings &settings,
                                    double sampleRate,
                                    qint64 firstFrameUtcMsValue,
                                    qint64 spectrumFirstFrameUtcMsValue,
                                    QString *errorMessage) {
    stop();
    if (eventBasePath.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("empty event base path");
        }
        return false;
    }
    if (sampleRate <= 0.0 || !std::isfinite(sampleRate)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("invalid IQ sample rate");
        }
        return false;
    }

    activeMode = mode;
    recordingSettings = settings;
    recordingSampleRate = sampleRate;
    firstFrameUtcMs = firstFrameUtcMsValue;
    lastFrameUtcMs = firstFrameUtcMsValue;
    spectrumFirstFrameUtcMs = spectrumFirstFrameUtcMsValue;
    writtenBytes = 0;
    framesWritten = 0;
    writeErrors = 0;
    writeNsSum = 0;
    writeNsMax = 0;
    flushNs = 0;
    dataSizeFieldOffset = -1;
    waveHeaderBytes = 0;
    filePath = eventBasePath + (mode == Mode::ChannelIqS16
                                    ? QStringLiteral("_channel_iq.wav")
                                    : QStringLiteral("_full_iq_s8.iq8"));
    sidecarPath = eventBasePath + (mode == Mode::ChannelIqS16
                                       ? QStringLiteral("_channel_iq.json")
                                       : QStringLiteral("_full_iq_s8.json"));

    return mode == Mode::ChannelIqS16 ? openChannelWav(errorMessage) : openFullIqRaw(errorMessage);
}

void SpectrumIqEventRecorder::stop() {
    if (!active && !outputFile.isOpen()) {
        return;
    }
    patchWavHeader();
    if (outputFile.isOpen()) {
        QElapsedTimer timer;
        timer.start();
        if (!outputFile.flush()) ++writeErrors;
        flushNs = timer.nsecsElapsed();
        outputFile.close();
    }
    writeSidecar();
    writeSigMfMetadata();
    active = false;
}

bool SpectrumIqEventRecorder::isRecording() const {
    return active;
}

QString SpectrumIqEventRecorder::currentFilePath() const {
    return filePath;
}

quint64 SpectrumIqEventRecorder::bytesWritten() const {
    return writtenBytes;
}

SpectrumIqEventRecorder::Mode SpectrumIqEventRecorder::mode() const {
    return activeMode;
}

void SpectrumIqEventRecorder::setExtendedMetadata(const QJsonObject &metadata) {
    extendedMetadata = metadata;
}

bool SpectrumIqEventRecorder::appendFrame(const QByteArray &iqData,
                                          double sampleRate,
                                          int sampleCount,
                                          qint64 frameUtcMs) {
    Q_UNUSED(sampleCount);
    if (!active || !outputFile.isOpen() || iqData.isEmpty()) {
        return false;
    }
    if (recordingSampleRate <= 0.0 && sampleRate > 0.0) {
        recordingSampleRate = sampleRate;
    }
    if (firstFrameUtcMs <= 0 && frameUtcMs > 0) {
        firstFrameUtcMs = frameUtcMs;
    }
    QElapsedTimer timer;
    timer.start();
    const qint64 written = outputFile.write(iqData);
    const qint64 writeNs = timer.nsecsElapsed();
    writeNsSum += writeNs;
    writeNsMax = (std::max)(writeNsMax, writeNs);
    if (written != iqData.size()) {
        if (written > 0) writtenBytes += static_cast<quint64>(written);
        ++writeErrors;
        active = false;
        return false;
    }
    writtenBytes += static_cast<quint64>(written);
    ++framesWritten;
    if (frameUtcMs > 0) lastFrameUtcMs = frameUtcMs;
    return true;
}

bool SpectrumIqEventRecorder::openChannelWav(QString *errorMessage) {
    QDir dir = QFileInfo(filePath).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot create event IQ folder");
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
    writeWavHeader();
    active = true;
    return true;
}

bool SpectrumIqEventRecorder::openFullIqRaw(QString *errorMessage) {
    QDir dir = QFileInfo(filePath).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot create event IQ folder");
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
    active = true;
    return true;
}

void SpectrumIqEventRecorder::writeWavHeader() {
    const int sampleRate = static_cast<int>(std::max(1.0, std::round(recordingSampleRate)));
    constexpr quint16 channels = 2;
    constexpr quint16 bitsPerSample = 16;
    const quint16 blockAlign = channels * bitsPerSample / 8;
    const quint32 byteRate = static_cast<quint32>(sampleRate * blockAlign);

    QByteArray header;
    appendFourCc(header, "RIFF");
    appendLe32(header, 0);
    appendFourCc(header, "WAVE");
    appendFourCc(header, "fmt ");
    appendLe32(header, 16);
    appendLe16(header, 1);
    appendLe16(header, channels);
    appendLe32(header, static_cast<quint32>(sampleRate));
    appendLe32(header, byteRate);
    appendLe16(header, blockAlign);
    appendLe16(header, bitsPerSample);
    appendFourCc(header, "data");
    dataSizeFieldOffset = header.size();
    appendLe32(header, 0);
    waveHeaderBytes = static_cast<quint64>(header.size());
    outputFile.write(header);
}

void SpectrumIqEventRecorder::patchWavHeader() {
    if (activeMode != Mode::ChannelIqS16 || !outputFile.isOpen()) {
        return;
    }
    const quint32 wavSampleRate =
        static_cast<quint32>(std::max(1.0, std::round(recordingSampleRate)));
    constexpr quint32 wavBlockAlign = 4;
    outputFile.flush();
    outputFile.seek(4);
    outputFile.write(le32Bytes(size32(writtenBytes + (waveHeaderBytes > 8 ? waveHeaderBytes - 8 : 0))));
    outputFile.seek(24);
    outputFile.write(le32Bytes(wavSampleRate));
    outputFile.seek(28);
    outputFile.write(le32Bytes(wavSampleRate * wavBlockAlign));
    if (dataSizeFieldOffset >= 0) {
        outputFile.seek(dataSizeFieldOffset);
        outputFile.write(le32Bytes(size32(writtenBytes)));
    }
    outputFile.flush();
}

bool SpectrumIqEventRecorder::writeSidecar() const {
    if (sidecarPath.isEmpty()) {
        return false;
    }
    QFile jsonFile(sidecarPath);
    if (!jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    jsonFile.write(QJsonDocument(makeMetadataObject()).toJson(QJsonDocument::Indented));
    return true;
}

bool SpectrumIqEventRecorder::writeSigMfMetadata() const {
    if (activeMode != Mode::FullIqS8 || filePath.isEmpty()) return false;
    const QFileInfo dataInfo(filePath);
    const QString metadataPath = dataInfo.absoluteDir().filePath(
        dataInfo.completeBaseName() + QStringLiteral(".sigmf-meta"));
    QFile metadataFile(metadataPath);
    if (!metadataFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) return false;

    QJsonObject global;
    global["core:datatype"] = QStringLiteral("ci8");
    global["core:sample_rate"] = recordingSampleRate;
    global["core:version"] = QStringLiteral("1.0.0");
    global["core:recorder"] = QStringLiteral("Obrii SDR");
    global["core:dataset"] = dataInfo.fileName();
    global["core:description"] = QStringLiteral("Obrii SDR full-band IQ event recording");

    QJsonObject capture;
    capture["core:sample_start"] = 0;
    capture["core:frequency"] = recordingSettings.centerFrequency;
    if (firstFrameUtcMs > 0) {
        capture["core:datetime"] = QDateTime::fromMSecsSinceEpoch(firstFrameUtcMs, Qt::UTC)
                                       .toString(Qt::ISODateWithMs);
    }

    QJsonObject root;
    root["global"] = global;
    root["captures"] = QJsonArray{capture};
    root["annotations"] = QJsonArray();
    metadataFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

QJsonObject SpectrumIqEventRecorder::makeMetadataObject() const {
    QJsonObject root;
    root["app"] = QStringLiteral("ObriiSDR");
    root["format"] = activeMode == Mode::ChannelIqS16
                         ? QStringLiteral("channel_iq_wav_s16le")
                         : QStringLiteral("full_iq_raw_s8_interleaved");
    root["recordedAtLocal"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    root["filePath"] = filePath;
    root["sampleRate"] = recordingSampleRate;
    root["bytesWritten"] = static_cast<double>(writtenBytes);
    root["framesWritten"] = static_cast<double>(framesWritten);
    root["writeErrors"] = static_cast<double>(writeErrors);
    root["writeCallAvgMs"] = framesWritten ? double(writeNsSum) / double(framesWritten) / 1.0e6 : 0.0;
    root["writeCallMaxMs"] = double(writeNsMax) / 1.0e6;
    root["flushMs"] = double(flushNs) / 1.0e6;
    root["firstFrameUtcMs"] = static_cast<double>(firstFrameUtcMs);
    root["lastFrameUtcMs"] = static_cast<double>(lastFrameUtcMs);
    root["spectrumFirstFrameUtcMs"] = static_cast<double>(spectrumFirstFrameUtcMs);
    if (firstFrameUtcMs > 0 && spectrumFirstFrameUtcMs > 0) {
        root["spectrumOffsetMs"] = static_cast<double>(firstFrameUtcMs - spectrumFirstFrameUtcMs);
    }
    if (firstFrameUtcMs > 0 && lastFrameUtcMs > firstFrameUtcMs) {
        const double durationSeconds =
            static_cast<double>(lastFrameUtcMs - firstFrameUtcMs) / 1000.0;
        const double bytesPerIqSample =
            activeMode == Mode::ChannelIqS16 ? 4.0 : 2.0;
        const double sampleCount = static_cast<double>(writtenBytes) / bytesPerIqSample;
        root["durationUtcSeconds"] = durationSeconds;
        if (sampleCount > 0.0 && durationSeconds > 0.0) {
            root["effectiveSampleRate"] = sampleCount / durationSeconds;
        }
    }
    root["centerFrequency"] = recordingSettings.centerFrequency;
    root["actualFrequency"] = recordingSettings.actualFrequency;
    root["listeningFrequency"] = recordingSettings.listeningFrequency;
    root["sourceSampleRate"] = recordingSettings.sampleRate;
    root["inputMode"] = recordingSettings.inputMode;
    root["modulationType"] = recordingSettings.modulationType;
    if (!extendedMetadata.isEmpty()) {
        root["scientificMetadata"] = extendedMetadata;
    }
    return root;
}
