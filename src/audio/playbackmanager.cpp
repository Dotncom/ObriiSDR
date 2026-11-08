#include "playbackmanager.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int AUDIO_FRAME_SAMPLES = 4800;
constexpr int IQ_FRAME_SAMPLES = 4096;

quint16 readLe16(const QByteArray &data, int offset) {
    const auto *ptr = reinterpret_cast<const uchar *>(data.constData() + offset);
    return static_cast<quint16>(ptr[0] | (ptr[1] << 8));
}

quint32 readLe32(const QByteArray &data, int offset) {
    const auto *ptr = reinterpret_cast<const uchar *>(data.constData() + offset);
    return static_cast<quint32>(ptr[0] |
                                (ptr[1] << 8) |
                                (ptr[2] << 16) |
                                (ptr[3] << 24));
}

bool fourCcEquals(const QByteArray &data, int offset, const char *value) {
    return data.size() >= offset + 4 &&
           std::memcmp(data.constData() + offset, value, 4) == 0;
}

void applyRadioMetadata(const QByteArray &metadata, PlaybackManager::WavInfo &info) {
    const QJsonDocument document = QJsonDocument::fromJson(metadata);
    if (!document.isObject()) {
        return;
    }

    const QJsonObject root = document.object();
    const QString appName = root.value(QStringLiteral("app")).toString();
    if (appName != QStringLiteral("ObriiSDR") &&
        appName != QStringLiteral("FobosAPP")) {
        return;
    }

    RadioSettings settings;
    settings.deviceIndex = root.value(QStringLiteral("deviceIndex")).toInt(settings.deviceIndex);
    settings.clockSource = root.value(QStringLiteral("clockSource")).toInt(settings.clockSource);
    settings.inputMode = root.value(QStringLiteral("inputMode")).toInt(settings.inputMode);
    settings.centerFrequency = root.value(QStringLiteral("centerFrequency")).toDouble(settings.centerFrequency);
    settings.actualFrequency = root.value(QStringLiteral("actualFrequency")).toDouble(settings.actualFrequency);
    settings.listeningFrequency = root.value(QStringLiteral("listeningFrequency")).toDouble(settings.listeningFrequency);
    settings.sampleRate = root.value(QStringLiteral("sourceSampleRate")).toDouble(settings.sampleRate);
    settings.bandwidth = root.value(QStringLiteral("bandwidth")).toDouble(settings.bandwidth);
    settings.modulationType = root.value(QStringLiteral("modulationType")).toInt(settings.modulationType);
    settings.fftLength = root.value(QStringLiteral("fftLength")).toInt(settings.fftLength);
    settings.fftWindowType = normalizedFftWindowType(
        root.value(QStringLiteral("fftWindowType")).toInt(settings.fftWindowType));
    settings.lnaGain = root.value(QStringLiteral("lnaGain")).toInt(settings.lnaGain);
    settings.vgaGain = root.value(QStringLiteral("vgaGain")).toInt(settings.vgaGain);
    settings.audioLowPassHz = root.value(QStringLiteral("audioLowPassHz")).toDouble(settings.audioLowPassHz);
    settings.audioHighPassHz = root.value(QStringLiteral("audioHighPassHz")).toDouble(settings.audioHighPassHz);
    settings.gpoValue = static_cast<std::uint8_t>(
        std::clamp(root.value(QStringLiteral("gpoValue")).toInt(settings.gpoValue), 0, 255));

    info.radioSettings = settings;
    info.hasRadioSettings = true;
    if (root.contains(QStringLiteral("scalePercent"))) {
        const double scale = root.value(QStringLiteral("scalePercent")).toDouble(info.scalePercent);
        if (std::isfinite(scale) && scale > 0.0) {
            info.scalePercent = scale;
            info.hasScalePercent = true;
        }
    }
}

} // namespace

PlaybackManager::PlaybackManager(QObject *parent)
    : QObject(parent) {
    pumpTimer.setSingleShot(true);
    connect(&pumpTimer, &QTimer::timeout, this, &PlaybackManager::pump);
}

PlaybackManager::~PlaybackManager() {
    stop();
}

bool PlaybackManager::start(const QString &filePath, QString *errorMessage) {
    if (playing) {
        stop();
    }

    WavInfo info;
    if (!readWavInfo(filePath, info, errorMessage)) {
        return false;
    }

    inputFile.setFileName(filePath);
    if (!inputFile.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = inputFile.errorString();
        }
        return false;
    }
    if (!inputFile.seek(static_cast<qint64>(info.dataOffset))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot seek to WAV data chunk");
        }
        inputFile.close();
        return false;
    }

    activeFilePath = filePath;
    activeInfo = info;
    bytesRead = 0;
    playing = true;

    emit started(activeFilePath, activeInfo);
    updateStatus(QStringLiteral("Playback: %1").arg(QFileInfo(activeFilePath).fileName()));
    pumpTimer.start(0);
    return true;
}

void PlaybackManager::stop() {
    const bool wasPlaying = playing;
    pumpTimer.stop();
    playing = false;
    bytesRead = 0;
    if (inputFile.isOpen()) {
        inputFile.close();
    }
    if (wasPlaying) {
        emit stopped();
        updateStatus(QStringLiteral("Playback: stopped"));
    }
}

bool PlaybackManager::isPlaying() const {
    return playing;
}

QString PlaybackManager::currentFilePath() const {
    return activeFilePath;
}

PlaybackManager::WavInfo PlaybackManager::currentInfo() const {
    return activeInfo;
}

bool PlaybackManager::readWavInfo(const QString &filePath, WavInfo &info, QString *errorMessage) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }

    const QByteArray header = file.read(12);
    if (header.size() != 12 ||
        !fourCcEquals(header, 0, "RIFF") ||
        !fourCcEquals(header, 8, "WAVE")) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("not a RIFF/WAVE file");
        }
        return false;
    }

    bool foundFmt = false;
    bool foundData = false;
    quint16 audioFormat = 0;

    while (!file.atEnd()) {
        const QByteArray chunkHeader = file.read(8);
        if (chunkHeader.size() != 8) {
            break;
        }
        const QByteArray chunkId = chunkHeader.left(4);
        const quint32 chunkSize = readLe32(chunkHeader, 4);
        const qint64 chunkDataOffset = file.pos();

        if (chunkId == QByteArrayLiteral("fmt ")) {
            const QByteArray fmt = file.read(static_cast<qint64>(chunkSize));
            if (fmt.size() < 16) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("invalid WAV fmt chunk");
                }
                return false;
            }
            audioFormat = readLe16(fmt, 0);
            info.channels = readLe16(fmt, 2);
            info.sampleRate = static_cast<int>(readLe32(fmt, 4));
            info.bitsPerSample = readLe16(fmt, 14);
            foundFmt = true;
        } else if (chunkId == QByteArrayLiteral("data")) {
            info.dataOffset = static_cast<quint64>(chunkDataOffset);
            info.dataSize = chunkSize;
            foundData = true;
            if (!file.seek(chunkDataOffset + chunkSize + (chunkSize & 1))) {
                break;
            }
        } else if (chunkId == QByteArrayLiteral("fbos")) {
            const QByteArray metadata = file.read(static_cast<qint64>(chunkSize));
            applyRadioMetadata(metadata, info);
            if (!file.seek(chunkDataOffset + chunkSize + (chunkSize & 1))) {
                break;
            }
        } else {
            if (!file.seek(chunkDataOffset + chunkSize + (chunkSize & 1))) {
                break;
            }
        }

        if (foundFmt && foundData) {
            break;
        }
    }

    if (!foundFmt || !foundData || audioFormat != 1) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("unsupported WAV file");
        }
        return false;
    }

    if (info.bitsPerSample != 16 ||
        (info.channels != 1 && info.channels != 2) ||
        info.sampleRate <= 0 ||
        info.dataSize == 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("unsupported WAV format");
        }
        return false;
    }

    info.mode = info.channels == 2 ? Mode::ChannelIqWav : Mode::AudioWav;
    info.valid = true;
    return true;
}

void PlaybackManager::pump() {
    if (!playing || !inputFile.isOpen() || !activeInfo.valid) {
        stop();
        return;
    }

    const int bytesPerSample = activeInfo.channels * (activeInfo.bitsPerSample / 8);
    const int frameSamples = activeInfo.mode == Mode::AudioWav ? AUDIO_FRAME_SAMPLES : IQ_FRAME_SAMPLES;
    const quint64 remaining = activeInfo.dataSize > bytesRead ? activeInfo.dataSize - bytesRead : 0;
    if (remaining == 0) {
        stop();
        return;
    }

    const int bytesToRead = static_cast<int>((std::min<quint64>)(remaining, static_cast<quint64>(frameSamples * bytesPerSample)));
    QByteArray frame = inputFile.read(bytesToRead);
    if (frame.isEmpty()) {
        stop();
        return;
    }

    const int remainder = frame.size() % bytesPerSample;
    if (remainder != 0) {
        frame.chop(remainder);
    }
    if (frame.isEmpty()) {
        stop();
        return;
    }

    bytesRead += static_cast<quint64>(frame.size());
    const int sampleCount = frame.size() / bytesPerSample;
    if (activeInfo.mode == Mode::AudioWav) {
        emit audioFrameReady(frame);
    } else {
        emit iqFrameReady(frame, activeInfo.sampleRate, sampleCount);
    }

    scheduleNext(sampleCount);
}

void PlaybackManager::scheduleNext(int sampleCount) {
    if (!playing || activeInfo.sampleRate <= 0 || sampleCount <= 0) {
        stop();
        return;
    }
    const int intervalMs = (std::max)(1, static_cast<int>(std::lround(sampleCount * 1000.0 / activeInfo.sampleRate)));
    pumpTimer.start(intervalMs);
}

void PlaybackManager::updateStatus(const QString &status) {
    emit statusChanged(status);
}
