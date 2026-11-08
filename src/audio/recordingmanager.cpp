#include "recordingmanager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

RecordingManager::RecordingManager(QObject *parent)
    : QObject(parent) {
}

RecordingManager::~RecordingManager() {
    stop();
}

bool RecordingManager::start(Mode mode, const RadioSettings &settings, QString *errorMessage) {
    if (recording) {
        stop();
    }

    activeMode = mode;
    recordingSettings = settings;
    recordingStartedAtUtc = QDateTime::currentDateTimeUtc();
    dataBytesWritten = 0;
    waveOpen = false;
    waveSampleRate = 0;
    waveChannels = 0;
    waveBitsPerSample = 0;
    waveLayout = {};
    waveHeaderBytes = 0;
    recording = true;

    if (activeMode == Mode::AudioWav) {
        filePath = makeRecordingPath(QStringLiteral("audio.wav"));
        if (!openWaveFile(48000, 1, 16, errorMessage)) {
            recording = false;
            return false;
        }
        updateStatus(QStringLiteral("Recording audio: %1%2")
                         .arg(QFileInfo(filePath).fileName(),
                              labMetadata.isEmpty() ? QString() : QStringLiteral(" + metadata JSON")));
        return true;
    }

    filePath = makeRecordingPath(QStringLiteral("channel_iq.wav"));
    updateStatus(QStringLiteral("Recording channel IQ: waiting for IQ frames%1")
                     .arg(labMetadata.isEmpty() ? QString() : QStringLiteral(" + metadata JSON")));
    return true;
}

void RecordingManager::stop() {
    if (!recording && !waveOpen) {
        return;
    }

    QString errorMessage;
    const bool finalized = patchWaveHeader(&errorMessage);
    if (outputFile.isOpen()) {
        outputFile.close();
    }

    const QString finishedFile = filePath;
    const quint64 finishedBytes = dataBytesWritten;
    recording = false;
    waveOpen = false;
    dataBytesWritten = 0;

    if (!finishedFile.isEmpty() && finishedBytes > 0) {
        const bool sidecarWritten = writeSidecarMetadata(finishedFile, finishedBytes);
        updateStatus(!finalized ? QStringLiteral("Recording failed: %1").arg(errorMessage)
                               : QStringLiteral("Recording saved: %1%2")
                         .arg(QFileInfo(finishedFile).fileName(),
                              sidecarWritten ? QStringLiteral(" + JSON") : QString()));
    } else {
        updateStatus(finalized ? QStringLiteral("Recording stopped: no data")
                               : QStringLiteral("Recording failed: %1").arg(errorMessage));
    }
}

bool RecordingManager::isRecording() const {
    return recording;
}

RecordingManager::Mode RecordingManager::mode() const {
    return activeMode;
}

QString RecordingManager::currentFilePath() const {
    return filePath;
}

void RecordingManager::setDisplayScalePercent(double scalePercent) {
    if (std::isfinite(scalePercent) && scalePercent > 0.0) {
        displayScalePercent = scalePercent;
    }
}

void RecordingManager::setLabMetadata(const QJsonObject &metadata) {
    labMetadata = metadata;
}

void RecordingManager::appendAudioFrame(const QByteArray &pcmData) {
    if (!recording || activeMode != Mode::AudioWav || !waveOpen || pcmData.isEmpty()) {
        return;
    }
    QString errorMessage;
    if (!RecordingFormatCore::appendWave(outputFile, waveLayout, dataBytesWritten,
                                         pcmData.constData(), pcmData.size(), &errorMessage)) {
        stop();
        updateStatus(QStringLiteral("Recording failed: %1").arg(errorMessage));
    }
}

void RecordingManager::appendIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount) {
    if (!recording || activeMode != Mode::ChannelIqWav || iqData.isEmpty()) {
        return;
    }
    // Full IQ uses two int8 values per pair; channel WAV needs two int16 values.
    if (!std::isfinite(sampleRate) || sampleRate < 1.0 || sampleRate > 1000000000.0 ||
        sampleCount <= 0 || static_cast<qint64>(sampleCount) * 4 != iqData.size()) {
        return;
    }
    if (waveOpen && std::abs(std::round(sampleRate) - waveSampleRate) > 0.5) {
        stop();
        updateStatus(QStringLiteral("IQ recording stopped: sample rate changed"));
        return;
    }

    if (!waveOpen) {
        QString errorMessage;
        const int wavRate = static_cast<int>(std::max(1.0, std::round(sampleRate)));
        if (!openWaveFile(wavRate, 2, 16, &errorMessage)) {
            updateStatus(QStringLiteral("IQ recording failed: %1").arg(errorMessage));
            recording = false;
            return;
        }
        updateStatus(QStringLiteral("Recording channel IQ: %1").arg(QFileInfo(filePath).fileName()));
    }

    QString errorMessage;
    if (!RecordingFormatCore::appendWave(outputFile, waveLayout, dataBytesWritten,
                                         iqData.constData(), iqData.size(), &errorMessage)) {
        stop();
        updateStatus(QStringLiteral("IQ recording failed: %1").arg(errorMessage));
    }
}

bool RecordingManager::openWaveFile(int sampleRate, int channels, int bitsPerSample, QString *errorMessage) {
    if (filePath.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("recording path is empty");
        }
        return false;
    }

    QDir dir = QFileInfo(filePath).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot create recordings folder");
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

    waveSampleRate = sampleRate;
    waveChannels = channels;
    waveBitsPerSample = bitsPerSample;
    if (!RecordingFormatCore::beginWave(outputFile, static_cast<quint32>(sampleRate),
                                        static_cast<quint16>(channels), static_cast<quint16>(bitsPerSample),
                                        makeMetadataChunk(), waveLayout, errorMessage)) {
        outputFile.close();
        return false;
    }
    waveHeaderBytes = waveLayout.headerBytes;
    waveOpen = true;
    return true;
}

QJsonObject RecordingManager::makeMetadataObject() const {
    QJsonObject root;
    root["app"] = QStringLiteral("ObriiSDR");
    root["version"] = 2;
    root["mode"] = activeMode == Mode::ChannelIqWav ? QStringLiteral("channel_iq") : QStringLiteral("audio");
    root["recordedAtUtc"] = recordingStartedAtUtc.toString(Qt::ISODateWithMs);
    root["recordedAtLocal"] = recordingStartedAtUtc.toLocalTime().toString(Qt::ISODateWithMs);
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
    root["audioLowPassHz"] = recordingSettings.audioLowPassHz;
    root["audioHighPassHz"] = recordingSettings.audioHighPassHz;
    root["scalePercent"] = displayScalePercent;
    root["gpoValue"] = static_cast<int>(recordingSettings.gpoValue);
    root["waveSampleRate"] = waveSampleRate;
    root["waveChannels"] = waveChannels;
    root["waveBitsPerSample"] = waveBitsPerSample;
    if (!labMetadata.isEmpty()) {
        root["lab"] = labMetadata;
    }
    return root;
}

QByteArray RecordingManager::makeMetadataChunk() const {
    const QJsonObject root = makeMetadataObject();
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool RecordingManager::patchWaveHeader(QString *errorMessage) {
    if (!waveOpen || !outputFile.isOpen()) {
        return true;
    }
    return RecordingFormatCore::finalizeWave(outputFile, waveLayout, dataBytesWritten, errorMessage);
}

bool RecordingManager::writeSidecarMetadata(const QString &finishedFile, quint64 finishedBytes) {
    if (finishedFile.isEmpty()) {
        return false;
    }

    QFileInfo info(finishedFile);
    QJsonObject root = makeMetadataObject();
    root["sidecarVersion"] = 1;
    root["fileName"] = info.fileName();
    root["filePath"] = info.absoluteFilePath();
    root["dataBytes"] = static_cast<double>(finishedBytes);
    root["waveHeaderBytes"] = static_cast<double>(waveHeaderBytes);
    const int bytesPerFrame = waveChannels * waveBitsPerSample / 8;
    if (waveSampleRate > 0 && bytesPerFrame > 0) {
        root["durationSeconds"] =
            static_cast<double>(finishedBytes) /
            static_cast<double>(waveSampleRate * bytesPerFrame);
    }

    const QString jsonPath = info.absoluteDir().filePath(info.completeBaseName() + QStringLiteral(".json"));
    QFile jsonFile(jsonPath);
    if (!jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "[Recording] failed to write sidecar metadata"
                 << jsonPath
                 << jsonFile.errorString();
        return false;
    }

    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!RecordingFormatCore::checkedAppend(jsonFile, bytes.constData(), bytes.size()) || !jsonFile.flush()) {
        qDebug() << "[Recording] failed to finish sidecar metadata" << jsonPath << jsonFile.errorString();
        return false;
    }
    jsonFile.close();
    qDebug() << "[Recording] sidecar metadata saved" << jsonPath;
    return true;
}

QString RecordingManager::makeRecordingPath(const QString &suffix) const {
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString safeSuffix = suffix.startsWith('_') ? suffix.mid(1) : suffix;
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("recordings/ObriiSDR_%1_%2").arg(timestamp, safeSuffix));
}

void RecordingManager::updateStatus(const QString &status) {
    emit statusChanged(status);
}
