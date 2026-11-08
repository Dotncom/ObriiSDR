#include "cliaudiooutput.h"
#include "recordingformatcore.h"

#include <QSysInfo>

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

bool CliAudioOutput::writeWaveHeader(QString *error) {
    RecordingFormatCore::WaveLayout layout;
    return RecordingFormatCore::beginWave(outputFile, 48000, 1, 16, {}, layout, error);
}

bool CliAudioOutput::open(const QString &path, bool toStdout, int inputMode, int modulation,
                          double bandwidthHz, double sourceRateHz, double centerHz,
                          double listenHz, bool followCenter, QString *error,
                          std::function<void(const QByteArray &)> frameCallback) {
    if (error) error->clear();
    if (outputFile.isOpen()) {
        if (error) *error = QStringLiteral("Audio WAV is already open");
        return false;
    }
    if (QSysInfo::ByteOrder != QSysInfo::LittleEndian ||
        (path.isEmpty() && !toStdout && !frameCallback) ||
        !std::isfinite(sourceRateHz) || sourceRateHz < 48000.0 ||
        !std::isfinite(bandwidthHz) || bandwidthHz <= 0.0 ||
        !std::isfinite(listenHz) ||
        std::abs(listenHz - centerHz) + bandwidthHz / 2 >= sourceRateHz / 2) {
        if (error) *error = QStringLiteral("Audio channel is outside the source IQ band or has invalid settings");
        return false;
    }
    sourceSettings = RadioSettings{};
    audioBytes = 0;
    discontinuity = true;
    sourceSettings.inputMode = inputMode;
    sourceSettings.modulationType = modulation;
    sourceSettings.sampleRate = sourceRateHz;
    sourceSettings.centerFrequency = centerHz;
    sourceSettings.listeningFrequency = listenHz;
    sourceSettings.bandwidth = bandwidthHz;
    sourceSettings.simplifiedAudioChannelizer = true;
    followsCenter = followCenter;
    channelizer.reset();
    demodulator.setLocalPlaybackEnabled(false);
    standardOutput = toStdout;
    onFrame = std::move(frameCallback);
#ifdef _WIN32
    if (standardOutput && _setmode(_fileno(stdout), _O_BINARY) < 0) {
        if (error) *error = QStringLiteral("Cannot set binary stdout mode");
        return false;
    }
#endif
    if (!path.isEmpty()) {
        outputFile.setFileName(path);
        if (!outputFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            if (error) *error = outputFile.errorString();
            return false;
        }
        if (!writeWaveHeader(error)) {
            outputFile.close();
            return false;
        }
    }
    return true;
}

void CliAudioOutput::setCenter(double centerHz) {
    sourceSettings.centerFrequency = centerHz;
    if (followsCenter) sourceSettings.listeningFrequency = centerHz;
    channelizer.reset();
    discontinuity = true;
}

void CliAudioOutput::setListening(double listenHz) {
    sourceSettings.listeningFrequency = listenHz;
    followsCenter = false;
    channelizer.reset();
    discontinuity = true;
}

void CliAudioOutput::setModulation(int modulation) {
    sourceSettings.modulationType = modulation;
    discontinuity = true;
}

void CliAudioOutput::setBandwidth(double bandwidthHz) {
    sourceSettings.bandwidth = bandwidthHz;
    channelizer.reset();
    discontinuity = true;
}

void CliAudioOutput::setFilterChain(const QString &json) {
    if (sourceSettings.audioFilterChainJson == json) return;
    sourceSettings.audioFilterChainJson = json;
    discontinuity = true;
}

void CliAudioOutput::setSampleRate(double sourceRateHz) {
    sourceSettings.sampleRate = sourceRateHz;
    channelizer.reset();
    discontinuity = true;
}

void CliAudioOutput::setChannelIqCallback(
    std::function<void(const float *, int, double)> callback) {
    onChannelIq = std::move(callback);
}

bool CliAudioOutput::process(const float *iq, std::size_t complexSamples, QString *error,
                             bool decodePcm) {
    if (error) error->clear();
    if (!enabled()) return true;
    const auto result = channelizer.processFloatIq(iq, complexSamples * 2,
                                                    sourceSettings, channelIq, false);
    if (!result.valid) {
        if (error) *error = QStringLiteral("Audio channelizer rejected the IQ settings");
        return false;
    }
    if (channelIq.empty()) return true;
    if (onChannelIq)
        onChannelIq(channelIq.data(), static_cast<int>(channelIq.size() / 2), result.outputRate);
    if (!decodePcm) return true;
    RadioSettings demodSettings = sourceSettings;
    demodSettings.inputMode = INPUT_RF;
    demodSettings.sampleRate = result.outputRate;
    demodSettings.centerFrequency = sourceSettings.listeningFrequency;
    if (!demodulator.processIndependentIq(channelIq, demodSettings, discontinuity, pcm)) {
        if (error) *error = QStringLiteral("Audio demodulator rejected the channel IQ");
        return false;
    }
    discontinuity = false;
    const auto bytes = static_cast<qint64>(pcm.size() * sizeof(short));
    if (!bytes) return true;
    if (outputFile.isOpen()) {
        if (!RecordingFormatCore::appendWave(outputFile, {}, audioBytes,
                                              reinterpret_cast<const char *>(pcm.data()), bytes, error)) {
            // Preserve the complete prefix and leave this failed file closed.
            RecordingFormatCore::finalizeWave(outputFile, {}, audioBytes);
            outputFile.close();
            return false;
        }
    }
    if (standardOutput) {
        if (std::fwrite(pcm.data(), sizeof(short), pcm.size(), stdout) != pcm.size() ||
            std::fflush(stdout) != 0) {
            if (error) *error = QStringLiteral("PCM stdout closed or could not be written");
            return false;
        }
    }
    if (onFrame) onFrame(QByteArray(reinterpret_cast<const char *>(pcm.data()), static_cast<int>(bytes)));
    return true;
}

bool CliAudioOutput::close(QString *error) {
    if (error) error->clear();
    if (!outputFile.isOpen()) return true;
    const bool okay = RecordingFormatCore::finalizeWave(outputFile, {}, audioBytes, error);
    outputFile.close();
    return okay;
}
