#include "spectrumiqeventrecorder.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

bool SpectrumIqEventRecorder::start(const QString &eventBasePath,
                                    Mode mode,
                                    const RadioSettings &settings,
                                    double sampleRate,
                                    qint64 firstFrameUtcMsValue,
                                    qint64 spectrumFirstFrameUtcMsValue,
                                    QString *errorMessage) {
    stop();
    if (eventBasePath.isEmpty() ||
        (mode != Mode::ChannelIqS16 && mode != Mode::FullIqS8 && mode != Mode::FullIqS16)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("empty event base path");
        }
        return false;
    }
    if (sampleRate < 1.0 || !std::isfinite(sampleRate) || sampleRate > 1.0e9) {
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
    waveLayout = {};
    filePath = eventBasePath + (mode == Mode::ChannelIqS16
                                    ? QStringLiteral("_channel_iq.wav")
                                    : mode == Mode::FullIqS16 ? QStringLiteral("_full_iq_s16.cs16")
                                                             : QStringLiteral("_full_iq_s8.iq8"));
    sidecarPath = eventBasePath + (mode == Mode::ChannelIqS16
                                       ? QStringLiteral("_channel_iq.json")
                                       : mode == Mode::FullIqS16 ? QStringLiteral("_full_iq_s16.json")
                                                                : QStringLiteral("_full_iq_s8.json"));

    return mode == Mode::ChannelIqS16 ? openChannelWav(errorMessage) : openFullIqRaw(errorMessage);
}

void SpectrumIqEventRecorder::stop() {
    if (!active && !outputFile.isOpen()) {
        return;
    }
    patchWavHeader();
    if (activeMode != Mode::ChannelIqS16 && outputFile.isOpen() &&
        outputFile.size() != qint64(writtenBytes) && !outputFile.resize(qint64(writtenBytes)))
        ++writeErrors; // Discard a short-write tail; retain complete IQ frames only.
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
    if (!active || !outputFile.isOpen() || iqData.isEmpty()) {
        return false;
    }
    const int bytesPerSample = activeMode == Mode::FullIqS8 ? 2 : 4;
    if (!std::isfinite(sampleRate) || sampleRate != recordingSampleRate || sampleCount <= 0 ||
        qint64(sampleCount) * bytesPerSample != iqData.size()) {
        ++writeErrors;
        active = false;
        return false;
    }
    if (firstFrameUtcMs <= 0 && frameUtcMs > 0) {
        firstFrameUtcMs = frameUtcMs;
    }
    QElapsedTimer timer;
    timer.start();
    const bool written = activeMode == Mode::ChannelIqS16
        ? RecordingFormatCore::appendWave(outputFile, waveLayout, writtenBytes,
                                          iqData.constData(), iqData.size())
        : RecordingFormatCore::checkedAppend(outputFile, iqData.constData(), iqData.size());
    const qint64 writeNs = timer.nsecsElapsed();
    writeNsSum += writeNs;
    writeNsMax = (std::max)(writeNsMax, writeNs);
    if (!written) {
        ++writeErrors;
        active = false;
        return false;
    }
    if (activeMode != Mode::ChannelIqS16) writtenBytes += static_cast<quint64>(iqData.size());
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
    if (!RecordingFormatCore::beginWave(outputFile, quint32(std::round(recordingSampleRate)),
                                        2, 16, {}, waveLayout, errorMessage)) {
        outputFile.close();
        return false;
    }
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

void SpectrumIqEventRecorder::patchWavHeader() {
    if (activeMode != Mode::ChannelIqS16 || !outputFile.isOpen()) {
        return;
    }
    if (!RecordingFormatCore::finalizeWave(outputFile, waveLayout, writtenBytes)) ++writeErrors;
}

bool SpectrumIqEventRecorder::writeSidecar() const {
    if (sidecarPath.isEmpty()) {
        return false;
    }
    QFile jsonFile(sidecarPath);
    if (!jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(makeMetadataObject()).toJson(QJsonDocument::Indented);
    return RecordingFormatCore::checkedAppend(jsonFile, bytes.constData(), bytes.size()) && jsonFile.flush();
}

bool SpectrumIqEventRecorder::writeSigMfMetadata() const {
    if (activeMode == Mode::ChannelIqS16 || filePath.isEmpty()) return false;
    const QFileInfo dataInfo(filePath);
    const QString metadataPath = dataInfo.absoluteDir().filePath(
        dataInfo.completeBaseName() + QStringLiteral(".sigmf-meta"));
    QFile metadataFile(metadataPath);
    if (!metadataFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) return false;

    const QJsonObject root = RecordingFormatCore::sigMfMetadata(
        activeMode == Mode::FullIqS16 ? QStringLiteral("ci16_le") : QStringLiteral("ci8"), recordingSampleRate,
        dataInfo.fileName(), recordingSettings.centerFrequency, firstFrameUtcMs,
        {{"core:description", QStringLiteral("Obrii SDR full-band IQ event recording")}});
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return RecordingFormatCore::checkedAppend(metadataFile, bytes.constData(), bytes.size()) && metadataFile.flush();
}

QJsonObject SpectrumIqEventRecorder::makeMetadataObject() const {
    QJsonObject root;
    root["app"] = QStringLiteral("ObriiSDR");
    root["format"] = activeMode == Mode::ChannelIqS16
                         ? QStringLiteral("channel_iq_wav_s16le")
                         : activeMode == Mode::FullIqS16 ? QStringLiteral("full_iq_raw_s16le_interleaved")
                                                       : QStringLiteral("full_iq_raw_s8_interleaved");
    if (activeMode == Mode::FullIqS16) {
        root["sampleEncoding"] = QStringLiteral("ci16_le");
        root["inputGain"] = 1.0;
        root["sampleScale"] = 1.0 / 32768.0;
    }
    const double bytesPerIqSample = activeMode == Mode::FullIqS8 ? 2.0 : 4.0;
    root["sampleCount"] = double(writtenBytes) / bytesPerIqSample;
    root["durationSamplesSeconds"] = double(writtenBytes) / bytesPerIqSample / recordingSampleRate;
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
