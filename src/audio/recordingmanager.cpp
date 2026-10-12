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

quint32 wavSizeField(quint64 value) {
    return value > 0xffffffffULL ? 0xffffffffU : static_cast<quint32>(value);
}

} // namespace

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
    dataSizeFieldOffset = -1;
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

    patchWaveHeader();
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
        updateStatus(QStringLiteral("Recording saved: %1%2")
                         .arg(QFileInfo(finishedFile).fileName(),
                              sidecarWritten ? QStringLiteral(" + JSON") : QString()));
    } else {
        updateStatus(QStringLiteral("Recording stopped: no data"));
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
    const qint64 written = outputFile.write(pcmData);
    if (written > 0) {
        dataBytesWritten += static_cast<quint64>(written);
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

    const qint64 written = outputFile.write(iqData);
    if (written > 0) {
        dataBytesWritten += static_cast<quint64>(written);
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
    writeWaveHeader(sampleRate, channels, bitsPerSample);
    waveOpen = true;
    return true;
}

void RecordingManager::writeWaveHeader(int sampleRate, int channels, int bitsPerSample) {
    const quint16 blockAlign = static_cast<quint16>(channels * bitsPerSample / 8);
    const quint32 byteRate = static_cast<quint32>(static_cast<quint64>(sampleRate) * blockAlign);

    QByteArray header;
    appendFourCc(header, "RIFF");
    appendLe32(header, 0);
    appendFourCc(header, "WAVE");
    appendFourCc(header, "fmt ");
    appendLe32(header, 16);
    appendLe16(header, 1);
    appendLe16(header, static_cast<quint16>(channels));
    appendLe32(header, static_cast<quint32>(sampleRate));
    appendLe32(header, byteRate);
    appendLe16(header, blockAlign);
    appendLe16(header, static_cast<quint16>(bitsPerSample));

    const QByteArray metadata = makeMetadataChunk();
    if (!metadata.isEmpty()) {
        appendFourCc(header, "fbos");
        appendLe32(header, static_cast<quint32>(metadata.size()));
        header.append(metadata);
        if ((metadata.size() & 1) != 0) {
            header.append('\0');
        }
    }

    appendFourCc(header, "data");
    dataSizeFieldOffset = header.size();
    appendLe32(header, 0);
    waveHeaderBytes = static_cast<quint64>(header.size());
    outputFile.write(header);
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

void RecordingManager::patchWaveHeader() {
    if (!waveOpen || !outputFile.isOpen()) {
        return;
    }

    const quint32 dataSize = wavSizeField(dataBytesWritten);
    const quint32 riffSize = wavSizeField(dataBytesWritten + (waveHeaderBytes > 8 ? waveHeaderBytes - 8 : 0));

    outputFile.flush();
    outputFile.seek(4);
    outputFile.write(le32Bytes(riffSize));
    if (dataSizeFieldOffset >= 0) {
        outputFile.seek(dataSizeFieldOffset);
        outputFile.write(le32Bytes(dataSize));
    }
    outputFile.flush();
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

    jsonFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
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
