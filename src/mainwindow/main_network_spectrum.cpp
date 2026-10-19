#include "main.h"
#include "appconstants.h"
#include "appruntimeutils.h"
#include "diagnosticlogging.h"
#include "iqbuffer.h"
#include "modulationutils.h"
#include "samplefileutils.h"
#include "scanvisualutils.h"
#include "spectrumfftworker.h"
#include "tuningutils.h"
#include "wirelesslabdialog.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

extern bool colorf;
extern bool secondGraph;
void YourClassName::sendNetworkSpectrumFrame(const std::vector<float> &frequencies,
                                             const std::vector<float> &magnitudes,
                                             const std::vector<float> &referenceMagnitudes,
                                             double frameCenterFrequency,
                                             double frameMinFrequency,
                                             double frameMaxFrequency,
                                             const QVector<ScanVisualSegment> &scanSegments,
                                             bool frameFresh,
                                             bool showScanSegmentMarkers) {
    if (networkMode != NetworkMode::Server ||
        isFullIqProcessingMode() ||
        !networkController ||
        !networkController->isControlReady() ||
        frequencies.empty() ||
        magnitudes.empty()) {
        return;
    }

    const int dataCount = std::min(static_cast<int>(frequencies.size()), static_cast<int>(magnitudes.size()));
    if (dataCount <= 0) {
        return;
    }
    if ((isChannelIqProcessingMode() || networkFullResolutionSpectrumFrames) &&
        networkController->pendingBytes() > NETWORK_SPECTRUM_MAX_PENDING_BYTES) {
        return;
    }

    const int minIntervalMs = networkFullResolutionSpectrumFrames
                                  ? NETWORK_FULL_RESOLUTION_SPECTRUM_INTERVAL_MS
                                  : (isChannelIqProcessingMode()
                                         ? NETWORK_CHANNEL_SPECTRUM_INTERVAL_MS
                                         : NETWORK_SPECTRUM_INTERVAL_MS);
    if (networkSpectrumFrameTimer.isValid() &&
        networkSpectrumFrameTimer.elapsed() < minIntervalMs) {
        return;
    }
    networkSpectrumFrameTimer.restart();

    if (!std::isfinite(frameCenterFrequency)) {
        double scanCenterFrequency = currentAgileScanCenterFrequencyHz();
        if (!std::isfinite(scanCenterFrequency)) {
            scanCenterFrequency = currentStandardScanCenterFrequencyHz();
        }
        frameCenterFrequency = std::isfinite(scanCenterFrequency)
                                   ? scanCenterFrequency
                                   : pendingSettings.centerFrequency;
    }
    if (!std::isfinite(frameMinFrequency) || !std::isfinite(frameMaxFrequency)) {
        frameMinFrequency = minFrequency;
        frameMaxFrequency = maxFrequency;
    }
    if (!std::isfinite(frameMinFrequency) ||
        !std::isfinite(frameMaxFrequency) ||
        frameMaxFrequency <= frameMinFrequency) {
        frameMinFrequency = frequencies.front();
        frameMaxFrequency = frequencies.back();
    }
    double frameListeningFrequency = pendingSettings.listeningFrequency;
    if (!std::isfinite(frameListeningFrequency)) {
        frameListeningFrequency = frameCenterFrequency;
    }

    auto lower = std::lower_bound(frequencies.begin(), frequencies.begin() + dataCount, static_cast<float>(frameMinFrequency));
    auto upper = std::upper_bound(frequencies.begin(), frequencies.begin() + dataCount, static_cast<float>(frameMaxFrequency));
    int sourceStart = static_cast<int>(std::distance(frequencies.begin(), lower));
    int sourceEnd = static_cast<int>(std::distance(frequencies.begin(), upper));
    sourceStart = (std::max)(0, sourceStart - 1);
    sourceEnd = (std::min)(dataCount, sourceEnd + 1);
    if (sourceEnd <= sourceStart) {
        sourceStart = 0;
        sourceEnd = dataCount;
        frameMinFrequency = frequencies.front();
        frameMaxFrequency = frequencies.back();
    }

    const int sourceCount = sourceEnd - sourceStart;
    const int maxBins = isChannelIqProcessingMode()
                            ? NETWORK_CHANNEL_SPECTRUM_MAX_BINS
                            : NETWORK_SPECTRUM_MAX_BINS;
    const int targetCount = networkFullResolutionSpectrumFrames
                                ? sourceCount
                                : (std::min)(maxBins, sourceCount);
    if (targetCount <= 0) {
        return;
    }
    const double step = static_cast<double>(sourceCount) / static_cast<double>(targetCount);
    QJsonArray frequencyArray;
    QJsonArray magnitudeArray;
    QJsonArray referenceMagnitudeArray;
    std::vector<float> resampledMagnitudes(static_cast<std::size_t>(targetCount),
                                           -160.0f);
    std::vector<float> resampledReferenceMagnitudes(static_cast<std::size_t>(targetCount),
                                                    -160.0f);
    const bool haveReferenceMagnitudes =
        static_cast<int>(referenceMagnitudes.size()) >= dataCount &&
        pendingSettings.inputMode == INPUT_HF_NOISE_CANCEL;
    const bool useBinarySpectrumFrame = networkFullResolutionSpectrumFrames;

    for (int i = 0; i < targetCount; ++i) {
        const int index = (std::min)(sourceEnd - 1,
                                     sourceStart + static_cast<int>(std::floor(i * step)));
        const int magnitudeIndex = (index + dataCount / 2) % dataCount;
        if (!std::isfinite(frequencies[index]) || !std::isfinite(magnitudes[magnitudeIndex])) {
            continue;
        }
        if (!useBinarySpectrumFrame) {
            frequencyArray.append(frequencies[index]);
        }
        resampledMagnitudes[static_cast<std::size_t>((i + targetCount / 2) % targetCount)] =
            magnitudes[magnitudeIndex];
        if (haveReferenceMagnitudes && std::isfinite(referenceMagnitudes[magnitudeIndex])) {
            resampledReferenceMagnitudes[static_cast<std::size_t>((i + targetCount / 2) % targetCount)] =
                referenceMagnitudes[magnitudeIndex];
        }
    }

    if (!useBinarySpectrumFrame) {
        for (const float value : resampledMagnitudes) {
            magnitudeArray.append(std::isfinite(value) ? value : -160.0f);
        }
        if (haveReferenceMagnitudes) {
            for (const float value : resampledReferenceMagnitudes) {
                referenceMagnitudeArray.append(std::isfinite(value) ? value : -160.0f);
            }
        }
    }

    QJsonObject frame;
    frame["type"] = "spectrum";
    frame["sequence"] = QString::number(++networkSpectrumFrameSequence);
    frame["centerFrequency"] = frameCenterFrequency;
    frame["listeningFrequency"] = frameListeningFrequency;
    frame["sampleRate"] = pendingSettings.sampleRate;
    frame["bandwidth"] = pendingSettings.bandwidth;
    frame["modulationType"] = pendingSettings.modulationType;
    frame["sstvDemodulationMode"] =
        normalizedSstvDemodulationMode(pendingSettings.sstvDemodulationMode);
    frame["inputMode"] = pendingSettings.inputMode;
    frame["fftLength"] = targetCount;
    frame["sourceFftLength"] = dataCount;
    frame["fullResolution"] = networkFullResolutionSpectrumFrames;
    frame["fresh"] = frameFresh;
    frame["showScanSegmentMarkers"] = showScanSegmentMarkers;
    frame["minFrequency"] = frameMinFrequency;
    frame["maxFrequency"] = frameMaxFrequency;
    if (!scanSegments.isEmpty()) {
        QJsonArray segmentArray;
        for (const ScanVisualSegment &segment : scanSegments) {
            if (!std::isfinite(segment.startHz) ||
                !std::isfinite(segment.endHz) ||
                segment.endHz <= segment.startHz) {
                continue;
            }
            QJsonObject item;
            item["startHz"] = segment.startHz;
            item["endHz"] = segment.endHz;
            item["centerHz"] = segment.centerHz;
            item["actualStartHz"] = segment.actualStartHz;
            item["actualEndHz"] = segment.actualEndHz;
            item["actualCenterHz"] = segment.actualCenterHz;
            item["label"] = segment.label;
            segmentArray.append(item);
        }
        if (!segmentArray.isEmpty()) {
            frame["scanSegments"] = segmentArray;
        }
    }
    if (useBinarySpectrumFrame) {
        frame["type"] = "spectrumbin";
        frame["binFormat"] = "f32le";
        frame["hasReferenceMagnitudes"] = haveReferenceMagnitudes;
        frame["frequencyLayout"] = "linear-min-max";

        QByteArray payload;
        payload.reserve(static_cast<int>(targetCount * static_cast<int>(sizeof(float)) *
                                         (haveReferenceMagnitudes ? 2 : 1)));
        payload.append(reinterpret_cast<const char*>(resampledMagnitudes.data()),
                       targetCount * static_cast<int>(sizeof(float)));
        if (haveReferenceMagnitudes) {
            payload.append(reinterpret_cast<const char*>(resampledReferenceMagnitudes.data()),
                           targetCount * static_cast<int>(sizeof(float)));
        }
        networkController->sendBinaryCommand(frame, payload);
        return;
    }
    frame["frequencies"] = frequencyArray;
    frame["magnitudes"] = magnitudeArray;
    if (haveReferenceMagnitudes) {
        frame["referenceMagnitudes"] = referenceMagnitudeArray;
    }

    networkController->sendControlCommand(frame);
}

void YourClassName::displayNetworkSpectrumFrame(const QJsonObject &frame) {
    if (networkMode == NetworkMode::Client &&
        runState == RadioRunState::Idle &&
        networkController &&
        !networkController->clientHasControl()) {
        runState = RadioRunState::Running;
        updateUiForRunState();
    }

    const QJsonArray frequencyArray = frame.value("frequencies").toArray();
    const QJsonArray magnitudeArray = frame.value("magnitudes").toArray();
    const QJsonArray referenceMagnitudeArray = frame.value("referenceMagnitudes").toArray();
    const int dataCount = (std::min)(frequencyArray.size(), magnitudeArray.size());
    if (dataCount <= 0) {
        return;
    }

    std::vector<float> frequencies;
    std::vector<float> magnitudes;
    std::vector<float> referenceMagnitudes;
    frequencies.reserve(dataCount);
    magnitudes.reserve(dataCount);
    if (referenceMagnitudeArray.size() >= dataCount) {
        referenceMagnitudes.reserve(dataCount);
    }

    for (int i = 0; i < dataCount; ++i) {
        const double frequency = frequencyArray.at(i).toDouble(std::numeric_limits<double>::quiet_NaN());
        const double magnitude = magnitudeArray.at(i).toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(frequency) || !std::isfinite(magnitude)) {
            continue;
        }
        frequencies.push_back(static_cast<float>(frequency));
        magnitudes.push_back(static_cast<float>(magnitude));
        if (referenceMagnitudeArray.size() >= dataCount) {
            const double referenceMagnitude =
                referenceMagnitudeArray.at(i).toDouble(std::numeric_limits<double>::quiet_NaN());
            referenceMagnitudes.push_back(std::isfinite(referenceMagnitude)
                                              ? static_cast<float>(referenceMagnitude)
                                              : -160.0f);
        }
    }

    if (frequencies.empty() || magnitudes.empty()) {
        return;
    }

    const double frameMinFrequency = frame.value("minFrequency").toDouble(minFrequency);
    const double frameMaxFrequency = frame.value("maxFrequency").toDouble(maxFrequency);
    const double frameCenterFrequency = frame.value("centerFrequency").toDouble(pendingSettings.centerFrequency);
    const bool frameFresh = frame.value("fresh").toBool(true);
    const QVector<ScanVisualSegment> frameScanSegments = scanSegmentsFromFrame(frame);
    const bool frameShowScanSegmentMarkers =
        frame.value("showScanSegmentMarkers").toBool(!frameScanSegments.isEmpty());
    const double displayCenterFrequency = frameCenterFrequency;
    const double displayListeningFrequency = pendingSettings.listeningFrequency;
    const double displayBandwidth = effectiveDemodulationBandwidth(
        pendingSettings.bandwidth,
        pendingSettings.modulationType,
        pendingSettings.sstvDemodulationMode);
    const int displayModulationType = effectiveDemodulationModulationType(
        pendingSettings.modulationType,
        pendingSettings.sstvDemodulationMode);

    if (scaleWidget) {
        scaleWidget->setScanSegments(frameScanSegments);
        scaleWidget->setScanSegmentMarkersVisible(frameShowScanSegmentMarkers);
        scaleWidget->setTuning(displayListeningFrequency,
                               displayCenterFrequency,
                               displayBandwidth,
                               displayModulationType);
        scaleWidget->setRange(frameMinFrequency, frameMaxFrequency);
    }
    const std::vector<float> hunterFrequencies =
        !frameScanSegments.isEmpty()
            ? actualFrequenciesFromScanSegments(frequencies, frameScanSegments)
            : std::vector<float>();
    const std::vector<float> &detectorFrequencies =
        hunterFrequencies.size() == frequencies.size() ? hunterFrequencies : frequencies;
    if (dmrHunterSettings.enabled) {
        updateDmrHunter(detectorFrequencies, magnitudes);
    }
    if (fpvHunterSettings.enabled) {
        updateFpvHunter(detectorFrequencies, magnitudes);
    }
    if (digitalVideoHunterSettings.enabled) {
        updateDigitalVideoHunter(detectorFrequencies, magnitudes);
    }
    updateScanMeasurement(detectorFrequencies, magnitudes);

    const int frameFftLength = static_cast<int>(frequencies.size());
    const bool spectrumShapeChanged =
        !networkSpectrumFrameMetadataValid ||
        std::abs(networkSpectrumFrameMinFrequency - frameMinFrequency) > 0.5 ||
        std::abs(networkSpectrumFrameMaxFrequency - frameMaxFrequency) > 0.5 ||
        networkSpectrumFrameFftLength != frameFftLength;
    if (spectrumShapeChanged) {
        qDebug() << "[NetworkSpectrum] frame range changed; preserving waterfall history"
                 << "min" << frameMinFrequency
                 << "max" << frameMaxFrequency
                 << "bins" << frameFftLength;
        networkSpectrumFrameMetadataValid = true;
        networkSpectrumFrameMinFrequency = frameMinFrequency;
        networkSpectrumFrameMaxFrequency = frameMaxFrequency;
        networkSpectrumFrameFftLength = frameFftLength;
    }

    const double frequencyStep =
        frameFftLength > 1
            ? (frameMaxFrequency - frameMinFrequency) / static_cast<double>(frameFftLength - 1)
            : 0.0;
    const bool haveReferenceMagnitudes =
        static_cast<int>(referenceMagnitudes.size()) == frameFftLength;

    if (graphWidget) {
        const int graphTargetCount =
            (std::min)(frameFftLength,
                       (std::max)(512, graphWidget->width() > 0 ? graphWidget->width() * 2 : 2048));
        std::vector<float> graphFrequencies(static_cast<std::size_t>(graphTargetCount), 0.0f);
        std::vector<float> graphMagnitudes(static_cast<std::size_t>(graphTargetCount), -160.0f);
        std::vector<float> graphReferenceMagnitudes;
        if (haveReferenceMagnitudes) {
            graphReferenceMagnitudes.assign(static_cast<std::size_t>(graphTargetCount), -160.0f);
        }

        const double pointsPerGraphBin =
            static_cast<double>(frameFftLength) / static_cast<double>((std::max)(1, graphTargetCount));
        for (int i = 0; i < graphTargetCount; ++i) {
            const int sourceBegin =
                (std::clamp)(static_cast<int>(std::floor(i * pointsPerGraphBin)), 0, frameFftLength - 1);
            const int sourceEnd =
                (std::clamp)(static_cast<int>(std::ceil((i + 1) * pointsPerGraphBin)), sourceBegin + 1, frameFftLength);
            float peakMagnitude = -160.0f;
            float peakReferenceMagnitude = -160.0f;
            for (int sourceIndex = sourceBegin; sourceIndex < sourceEnd; ++sourceIndex) {
                const int shiftedIndex = (sourceIndex + frameFftLength / 2) % frameFftLength;
                const float value = magnitudes[static_cast<std::size_t>(shiftedIndex)];
                if (std::isfinite(value)) {
                    peakMagnitude = (std::max)(peakMagnitude, value);
                }
                if (haveReferenceMagnitudes) {
                    const float referenceValue = referenceMagnitudes[static_cast<std::size_t>(shiftedIndex)];
                    if (std::isfinite(referenceValue)) {
                        peakReferenceMagnitude = (std::max)(peakReferenceMagnitude, referenceValue);
                    }
                }
            }
            const double centerSourceIndex = (sourceBegin + sourceEnd - 1) * 0.5;
            graphFrequencies[static_cast<std::size_t>(i)] =
                static_cast<float>(frameMinFrequency + frequencyStep * centerSourceIndex);
            graphMagnitudes[static_cast<std::size_t>((i + graphTargetCount / 2) % graphTargetCount)] =
                peakMagnitude;
            if (haveReferenceMagnitudes) {
                graphReferenceMagnitudes[static_cast<std::size_t>((i + graphTargetCount / 2) % graphTargetCount)] =
                    peakReferenceMagnitude;
            }
        }
        const std::vector<float> graphMeasurementFrequencies =
            !frameScanSegments.isEmpty()
                ? actualFrequenciesFromScanSegments(graphFrequencies, frameScanSegments)
                : std::vector<float>();
        const std::vector<float> &measurementFrequencies =
            graphMeasurementFrequencies.size() == graphFrequencies.size()
                ? graphMeasurementFrequencies
                : graphFrequencies;
        const std::vector<float> measurementOverlay =
            scanMeasurementOverlay(measurementFrequencies, graphTargetCount);

        const bool scienceRequired = spectrumScienceAnalysisRequired();
        const bool wirelessRequired = wirelessLabDialog && wirelessLabDialog->isVisible() &&
                                      frameScanSegments.isEmpty();
        if (scienceRequired || wirelessRequired) {
            std::vector<float> orderedScienceLevels(static_cast<std::size_t>(graphTargetCount), -160.0f);
            for (int i = 0; i < graphTargetCount; ++i) {
                orderedScienceLevels[static_cast<std::size_t>(i)] =
                    graphMagnitudes[static_cast<std::size_t>((i + graphTargetCount / 2) % graphTargetCount)];
            }
            if (scienceRequired) {
                updateSpectrumScience(measurementFrequencies, orderedScienceLevels);
                if (frameScanSegments.isEmpty()) {
                    feedZeroSpanFrame(frequencies, magnitudes, true);
                } else {
                    feedZeroSpanFrame(measurementFrequencies, orderedScienceLevels, false);
                }
            }
            if (wirelessRequired) {
                wirelessLabDialog->appendSpectrum(graphFrequencies, orderedScienceLevels,
                    frame.value("sampleRate").toDouble(pendingSettings.sampleRate));
            }
        }

        graphWidget->setLevelRange(displayLevelMin, displayLevelMax);
        graphWidget->setScanSegments(frameScanSegments);
        graphWidget->setScanSegmentMarkersVisible(frameShowScanSegmentMarkers);
        graphWidget->setData(graphFrequencies,
                             graphMagnitudes,
                             frameMinFrequency,
                             frameMaxFrequency,
                             graphTargetCount,
                             colorf);
        graphWidget->setOverlayData(!measurementOverlay.empty() ? measurementOverlay : graphReferenceMagnitudes,
                                    !measurementOverlay.empty() ||
                                        static_cast<int>(graphReferenceMagnitudes.size()) == graphTargetCount);
    }
    if (waterfallWidget && frameFresh) {
        waterfallWidget->setData(frequencies, magnitudes, frameMinFrequency, frameMaxFrequency, frameFftLength,
                                 secondGraph, colorf, contrast, sensitivity, displayLevelMin, displayLevelMax);
    }
    if (waterfallWidget) {
        waterfallWidget->setScanSegments(frameScanSegments);
        waterfallWidget->setScanSegmentMarkersVisible(frameShowScanSegmentMarkers);
    }
}

void YourClassName::displayNetworkSpectrumFrameBinary(const QJsonObject &frame, const QByteArray &payload) {
    if (networkMode == NetworkMode::Client &&
        runState == RadioRunState::Idle &&
        networkController &&
        !networkController->clientHasControl()) {
        runState = RadioRunState::Running;
        updateUiForRunState();
    }

    const int frameFftLength = frame.value("fftLength").toInt();
    const double frameMinFrequency = frame.value("minFrequency").toDouble(minFrequency);
    const double frameMaxFrequency = frame.value("maxFrequency").toDouble(maxFrequency);
    if (frameFftLength <= 0 ||
        payload.size() < frameFftLength * static_cast<int>(sizeof(float)) ||
        !std::isfinite(frameMinFrequency) ||
        !std::isfinite(frameMaxFrequency) ||
        frameMaxFrequency <= frameMinFrequency) {
        qDebug() << "[NetworkSpectrum] invalid binary spectrum frame"
                 << "bins" << frameFftLength
                 << "payload" << payload.size()
                 << "min" << frameMinFrequency
                 << "max" << frameMaxFrequency;
        return;
    }

    std::vector<float> frequencies(static_cast<std::size_t>(frameFftLength), 0.0f);
    std::vector<float> magnitudes(static_cast<std::size_t>(frameFftLength), -160.0f);
    std::memcpy(magnitudes.data(), payload.constData(), frameFftLength * sizeof(float));

    const bool haveReferenceMagnitudes =
        frame.value("hasReferenceMagnitudes").toBool(false) &&
        payload.size() >= frameFftLength * static_cast<int>(sizeof(float)) * 2;
    std::vector<float> referenceMagnitudes;
    if (haveReferenceMagnitudes) {
        referenceMagnitudes.resize(static_cast<std::size_t>(frameFftLength), -160.0f);
        std::memcpy(referenceMagnitudes.data(),
                    payload.constData() + frameFftLength * static_cast<int>(sizeof(float)),
                    frameFftLength * sizeof(float));
    }

    const double frequencyStep =
        frameFftLength > 1
            ? (frameMaxFrequency - frameMinFrequency) / static_cast<double>(frameFftLength - 1)
            : 0.0;
    for (int i = 0; i < frameFftLength; ++i) {
        frequencies[static_cast<std::size_t>(i)] =
            static_cast<float>(frameMinFrequency + frequencyStep * static_cast<double>(i));
        if (!std::isfinite(magnitudes[static_cast<std::size_t>(i)])) {
            magnitudes[static_cast<std::size_t>(i)] = -160.0f;
        }
        if (haveReferenceMagnitudes &&
            !std::isfinite(referenceMagnitudes[static_cast<std::size_t>(i)])) {
            referenceMagnitudes[static_cast<std::size_t>(i)] = -160.0f;
        }
    }

    const double frameCenterFrequency = frame.value("centerFrequency").toDouble(pendingSettings.centerFrequency);
    const bool frameFresh = frame.value("fresh").toBool(true);
    const QVector<ScanVisualSegment> frameScanSegments = scanSegmentsFromFrame(frame);
    const bool frameShowScanSegmentMarkers =
        frame.value("showScanSegmentMarkers").toBool(!frameScanSegments.isEmpty());
    const double displayCenterFrequency = frameCenterFrequency;
    const double displayListeningFrequency = pendingSettings.listeningFrequency;
    const double displayBandwidth = effectiveDemodulationBandwidth(
        pendingSettings.bandwidth,
        pendingSettings.modulationType,
        pendingSettings.sstvDemodulationMode);
    const int displayModulationType = effectiveDemodulationModulationType(
        pendingSettings.modulationType,
        pendingSettings.sstvDemodulationMode);

    if (scaleWidget) {
        scaleWidget->setScanSegments(frameScanSegments);
        scaleWidget->setScanSegmentMarkersVisible(frameShowScanSegmentMarkers);
        scaleWidget->setTuning(displayListeningFrequency,
                               displayCenterFrequency,
                               displayBandwidth,
                               displayModulationType);
        scaleWidget->setRange(frameMinFrequency, frameMaxFrequency);
    }
    const std::vector<float> hunterFrequencies =
        !frameScanSegments.isEmpty()
            ? actualFrequenciesFromScanSegments(frequencies, frameScanSegments)
            : std::vector<float>();
    const std::vector<float> &detectorFrequencies =
        hunterFrequencies.size() == frequencies.size() ? hunterFrequencies : frequencies;
    if (dmrHunterSettings.enabled) {
        updateDmrHunter(detectorFrequencies, magnitudes);
    }
    if (fpvHunterSettings.enabled) {
        updateFpvHunter(detectorFrequencies, magnitudes);
    }
    if (digitalVideoHunterSettings.enabled) {
        updateDigitalVideoHunter(detectorFrequencies, magnitudes);
    }
    updateScanMeasurement(detectorFrequencies, magnitudes);

    const bool spectrumShapeChanged =
        !networkSpectrumFrameMetadataValid ||
        std::abs(networkSpectrumFrameMinFrequency - frameMinFrequency) > 0.5 ||
        std::abs(networkSpectrumFrameMaxFrequency - frameMaxFrequency) > 0.5 ||
        networkSpectrumFrameFftLength != frameFftLength;
    if (spectrumShapeChanged) {
        qDebug() << "[NetworkSpectrum] binary frame range changed; preserving waterfall history"
                 << "min" << frameMinFrequency
                 << "max" << frameMaxFrequency
                 << "bins" << frameFftLength;
        networkSpectrumFrameMetadataValid = true;
        networkSpectrumFrameMinFrequency = frameMinFrequency;
        networkSpectrumFrameMaxFrequency = frameMaxFrequency;
        networkSpectrumFrameFftLength = frameFftLength;
    }

    if (graphWidget) {
        const int graphTargetCount =
            (std::min)(frameFftLength,
                       (std::max)(512, graphWidget->width() > 0 ? graphWidget->width() * 2 : 2048));
        std::vector<float> graphFrequencies(static_cast<std::size_t>(graphTargetCount), 0.0f);
        std::vector<float> graphMagnitudes(static_cast<std::size_t>(graphTargetCount), -160.0f);
        std::vector<float> graphReferenceMagnitudes;
        if (haveReferenceMagnitudes) {
            graphReferenceMagnitudes.assign(static_cast<std::size_t>(graphTargetCount), -160.0f);
        }

        const double pointsPerGraphBin =
            static_cast<double>(frameFftLength) / static_cast<double>((std::max)(1, graphTargetCount));
        for (int i = 0; i < graphTargetCount; ++i) {
            const int sourceBegin =
                (std::clamp)(static_cast<int>(std::floor(i * pointsPerGraphBin)), 0, frameFftLength - 1);
            const int sourceEnd =
                (std::clamp)(static_cast<int>(std::ceil((i + 1) * pointsPerGraphBin)), sourceBegin + 1, frameFftLength);
            float peakMagnitude = -160.0f;
            float peakReferenceMagnitude = -160.0f;
            for (int sourceIndex = sourceBegin; sourceIndex < sourceEnd; ++sourceIndex) {
                const int shiftedIndex = (sourceIndex + frameFftLength / 2) % frameFftLength;
                const float value = magnitudes[static_cast<std::size_t>(shiftedIndex)];
                if (std::isfinite(value)) {
                    peakMagnitude = (std::max)(peakMagnitude, value);
                }
                if (haveReferenceMagnitudes) {
                    const float referenceValue = referenceMagnitudes[static_cast<std::size_t>(shiftedIndex)];
                    if (std::isfinite(referenceValue)) {
                        peakReferenceMagnitude = (std::max)(peakReferenceMagnitude, referenceValue);
                    }
                }
            }
            const double centerSourceIndex = (sourceBegin + sourceEnd - 1) * 0.5;
            graphFrequencies[static_cast<std::size_t>(i)] =
                static_cast<float>(frameMinFrequency + frequencyStep * centerSourceIndex);
            graphMagnitudes[static_cast<std::size_t>((i + graphTargetCount / 2) % graphTargetCount)] =
                peakMagnitude;
            if (haveReferenceMagnitudes) {
                graphReferenceMagnitudes[static_cast<std::size_t>((i + graphTargetCount / 2) % graphTargetCount)] =
                    peakReferenceMagnitude;
            }
        }
        const std::vector<float> graphMeasurementFrequencies =
            !frameScanSegments.isEmpty()
                ? actualFrequenciesFromScanSegments(graphFrequencies, frameScanSegments)
                : std::vector<float>();
        const std::vector<float> &measurementFrequencies =
            graphMeasurementFrequencies.size() == graphFrequencies.size()
                ? graphMeasurementFrequencies
                : graphFrequencies;
        const std::vector<float> measurementOverlay =
            scanMeasurementOverlay(measurementFrequencies, graphTargetCount);

        const bool scienceRequired = spectrumScienceAnalysisRequired();
        const bool wirelessRequired = wirelessLabDialog && wirelessLabDialog->isVisible() &&
                                      frameScanSegments.isEmpty();
        if (scienceRequired || wirelessRequired) {
            std::vector<float> orderedScienceLevels(static_cast<std::size_t>(graphTargetCount), -160.0f);
            for (int i = 0; i < graphTargetCount; ++i) {
                orderedScienceLevels[static_cast<std::size_t>(i)] =
                    graphMagnitudes[static_cast<std::size_t>((i + graphTargetCount / 2) % graphTargetCount)];
            }
            if (scienceRequired) {
                updateSpectrumScience(measurementFrequencies, orderedScienceLevels);
                if (frameScanSegments.isEmpty()) {
                    feedZeroSpanFrame(frequencies, magnitudes, true);
                } else {
                    feedZeroSpanFrame(measurementFrequencies, orderedScienceLevels, false);
                }
            }
            if (wirelessRequired) {
                wirelessLabDialog->appendSpectrum(graphFrequencies, orderedScienceLevels,
                    frame.value("sampleRate").toDouble(pendingSettings.sampleRate));
            }
        }

        graphWidget->setLevelRange(displayLevelMin, displayLevelMax);
        graphWidget->setScanSegments(frameScanSegments);
        graphWidget->setScanSegmentMarkersVisible(frameShowScanSegmentMarkers);
        graphWidget->setData(graphFrequencies,
                             graphMagnitudes,
                             frameMinFrequency,
                             frameMaxFrequency,
                             graphTargetCount,
                             colorf);
        graphWidget->setOverlayData(!measurementOverlay.empty() ? measurementOverlay : graphReferenceMagnitudes,
                                    !measurementOverlay.empty() ||
                                        static_cast<int>(graphReferenceMagnitudes.size()) == graphTargetCount);
    }
    if (waterfallWidget && frameFresh) {
        waterfallWidget->setData(frequencies, magnitudes, frameMinFrequency, frameMaxFrequency, frameFftLength,
                                 secondGraph, colorf, contrast, sensitivity, displayLevelMin, displayLevelMax);
    }
    if (waterfallWidget) {
        waterfallWidget->setScanSegments(frameScanSegments);
        waterfallWidget->setScanSegmentMarkersVisible(frameShowScanSegmentMarkers);
    }
}
