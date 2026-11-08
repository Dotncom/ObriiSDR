#include "scanvisualutils.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <limits>

int normalizedScanVisualMode(int value) {
    return (std::clamp)(value,
                        static_cast<int>(ScanVisualMode::CompressedMosaic),
                        static_cast<int>(ScanVisualMode::PassComposite));
}

ScanVisualMode scanVisualModeFromInt(int value) {
    return static_cast<ScanVisualMode>(normalizedScanVisualMode(value));
}

int nearestScanFrequencyIndex(const QVector<double> &frequencies, double centerHz) {
    if (frequencies.isEmpty() || !std::isfinite(centerHz)) {
        return -1;
    }
    int bestIndex = -1;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (int i = 0; i < frequencies.size(); ++i) {
        const double frequency = frequencies.at(i);
        if (!std::isfinite(frequency)) {
            continue;
        }
        const double distance = std::abs(frequency - centerHz);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
        }
    }
    return bestIndex;
}

QVector<ScanVisualSegment> scanSegmentsFromFrame(const QJsonObject &frame) {
    QVector<ScanVisualSegment> segments;
    const QJsonArray segmentArray = frame.value("scanSegments").toArray();
    segments.reserve(segmentArray.size());
    for (const QJsonValue &value : segmentArray) {
        const QJsonObject item = value.toObject();
        const double startHz = item.value("startHz").toDouble(std::numeric_limits<double>::quiet_NaN());
        const double endHz = item.value("endHz").toDouble(std::numeric_limits<double>::quiet_NaN());
        const double centerHz = item.value("centerHz").toDouble((startHz + endHz) * 0.5);
        const double actualStartHz = item.value("actualStartHz").toDouble(startHz);
        const double actualEndHz = item.value("actualEndHz").toDouble(endHz);
        const double actualCenterHz = item.value("actualCenterHz").toDouble((actualStartHz + actualEndHz) * 0.5);
        if (!std::isfinite(startHz) ||
            !std::isfinite(endHz) ||
            endHz <= startHz) {
            continue;
        }
        QString label = item.value("label").toString();
        if (label.isEmpty()) {
            label = QStringLiteral("%1 MHz").arg(actualCenterHz / 1000000.0, 0, 'f', 3);
        }
        segments.push_back({startHz,
                            endHz,
                            centerHz,
                            actualStartHz,
                            actualEndHz,
                            actualCenterHz,
                            label});
    }
    return segments;
}

std::vector<float> actualFrequenciesFromScanSegments(const std::vector<float> &displayFrequencies,
                                                     const QVector<ScanVisualSegment> &segments) {
    if (displayFrequencies.empty() || segments.isEmpty()) {
        return {};
    }

    std::vector<float> actualFrequencies;
    actualFrequencies.reserve(displayFrequencies.size());
    for (const float displayFrequency : displayFrequencies) {
        double actualFrequency = displayFrequency;
        for (const ScanVisualSegment &segment : segments) {
            if (!std::isfinite(segment.startHz) ||
                !std::isfinite(segment.endHz) ||
                !std::isfinite(segment.actualStartHz) ||
                !std::isfinite(segment.actualEndHz) ||
                segment.endHz <= segment.startHz ||
                displayFrequency < segment.startHz ||
                displayFrequency > segment.endHz) {
                continue;
            }
            const double ratio =
                (static_cast<double>(displayFrequency) - segment.startHz) /
                (segment.endHz - segment.startHz);
            actualFrequency = segment.actualStartHz +
                              (std::clamp)(ratio, 0.0, 1.0) *
                                  (segment.actualEndHz - segment.actualStartHz);
            break;
        }
        actualFrequencies.push_back(static_cast<float>(actualFrequency));
    }
    return actualFrequencies;
}

double displayFrequencyForScanActual(double actualFrequencyHz,
                                     const QVector<ScanVisualSegment> &segments,
                                     double fallbackDisplayHz) {
    if (!std::isfinite(actualFrequencyHz) || segments.isEmpty()) {
        return fallbackDisplayHz;
    }
    for (const ScanVisualSegment &segment : segments) {
        if (!std::isfinite(segment.startHz) ||
            !std::isfinite(segment.endHz) ||
            !std::isfinite(segment.actualStartHz) ||
            !std::isfinite(segment.actualEndHz) ||
            segment.endHz <= segment.startHz ||
            segment.actualEndHz <= segment.actualStartHz ||
            actualFrequencyHz < segment.actualStartHz ||
            actualFrequencyHz > segment.actualEndHz) {
            continue;
        }
        const double ratio =
            (actualFrequencyHz - segment.actualStartHz) /
            (segment.actualEndHz - segment.actualStartHz);
        return segment.startHz +
               (std::clamp)(ratio, 0.0, 1.0) *
                   (segment.endHz - segment.startHz);
    }
    return fallbackDisplayHz;
}

double actualFrequencyForScanDisplay(const ScanVisualSegment &segment, double displayFrequencyHz) {
    if (!std::isfinite(displayFrequencyHz) ||
        !std::isfinite(segment.startHz) ||
        !std::isfinite(segment.endHz) ||
        !std::isfinite(segment.actualStartHz) ||
        !std::isfinite(segment.actualEndHz) ||
        segment.endHz <= segment.startHz) {
        return displayFrequencyHz;
    }
    const double ratio =
        (displayFrequencyHz - segment.startHz) /
        (segment.endHz - segment.startHz);
    return segment.actualStartHz +
           (std::clamp)(ratio, 0.0, 1.0) *
               (segment.actualEndHz - segment.actualStartHz);
}

ScanVisualFrame windowedScanVisualFrame(const ScanVisualFrame &frame,
                                        double centerDisplayHz,
                                        double spanHz) {
    if (!frame.valid ||
        frame.frequencies.size() < 2 ||
        frame.magnitudes.size() != frame.frequencies.size() ||
        !std::isfinite(frame.minFrequency) ||
        !std::isfinite(frame.maxFrequency) ||
        frame.maxFrequency <= frame.minFrequency ||
        !std::isfinite(centerDisplayHz) ||
        !std::isfinite(spanHz) ||
        spanHz <= 0.0) {
        return frame;
    }

    const double fullSpanHz = frame.maxFrequency - frame.minFrequency;
    if (spanHz >= fullSpanHz * 0.999) {
        return frame;
    }

    const double visibleSpanHz = (std::clamp)(spanHz, 1.0, fullSpanHz);
    double windowMinHz = centerDisplayHz - visibleSpanHz * 0.5;
    windowMinHz = (std::clamp)(windowMinHz,
                               frame.minFrequency,
                               frame.maxFrequency - visibleSpanHz);
    const double windowMaxHz = windowMinHz + visibleSpanHz;

    const int sourceCount = static_cast<int>(frame.frequencies.size());
    int first = 0;
    while (first < sourceCount - 1 &&
           frame.frequencies[static_cast<std::size_t>(first)] < windowMinHz) {
        ++first;
    }
    int last = sourceCount - 1;
    while (last > first &&
           frame.frequencies[static_cast<std::size_t>(last)] > windowMaxHz) {
        --last;
    }
    if (last - first + 1 < 2) {
        return frame;
    }

    const int targetCount = last - first + 1;
    constexpr float windowFloorDb = -160.0f;
    ScanVisualFrame window;
    window.valid = true;
    window.mosaic = frame.mosaic;
    window.fresh = frame.fresh;
    window.showSegmentMarkers = frame.showSegmentMarkers;
    window.minFrequency = windowMinHz;
    window.maxFrequency = windowMaxHz;
    window.centerFrequency = (windowMinHz + windowMaxHz) * 0.5;
    window.fftLength = targetCount;
    window.sectorCount = frame.sectorCount;
    window.frequencies.reserve(static_cast<std::size_t>(targetCount));
    window.actualFrequencies.reserve(static_cast<std::size_t>(targetCount));
    window.magnitudes.assign(static_cast<std::size_t>(targetCount), windowFloorDb);
    const bool haveReference = frame.referenceMagnitudes.size() == frame.magnitudes.size();
    if (haveReference) {
        window.referenceMagnitudes.assign(static_cast<std::size_t>(targetCount), windowFloorDb);
    }

    for (int j = 0; j < targetCount; ++j) {
        const int sourceIndex = first + j;
        window.frequencies.push_back(frame.frequencies[static_cast<std::size_t>(sourceIndex)]);
        if (frame.actualFrequencies.size() == frame.frequencies.size()) {
            window.actualFrequencies.push_back(frame.actualFrequencies[static_cast<std::size_t>(sourceIndex)]);
        }

        const int sourceLevelIndex = (sourceIndex + sourceCount / 2) % sourceCount;
        const int targetLevelIndex = (j + targetCount / 2) % targetCount;
        window.magnitudes[static_cast<std::size_t>(targetLevelIndex)] =
            frame.magnitudes[static_cast<std::size_t>(sourceLevelIndex)];
        if (haveReference) {
            window.referenceMagnitudes[static_cast<std::size_t>(targetLevelIndex)] =
                frame.referenceMagnitudes[static_cast<std::size_t>(sourceLevelIndex)];
        }
    }
    if (window.actualFrequencies.size() != window.frequencies.size()) {
        window.actualFrequencies.clear();
    }

    for (const ScanVisualSegment &segment : frame.segments) {
        if (!std::isfinite(segment.startHz) ||
            !std::isfinite(segment.endHz) ||
            segment.endHz <= segment.startHz ||
            segment.endHz < windowMinHz ||
            segment.startHz > windowMaxHz) {
            continue;
        }
        ScanVisualSegment clipped = segment;
        clipped.startHz = (std::max)(segment.startHz, windowMinHz);
        clipped.endHz = (std::min)(segment.endHz, windowMaxHz);
        if (clipped.endHz <= clipped.startHz) {
            continue;
        }
        clipped.centerHz = (clipped.startHz + clipped.endHz) * 0.5;
        clipped.actualStartHz = actualFrequencyForScanDisplay(segment, clipped.startHz);
        clipped.actualEndHz = actualFrequencyForScanDisplay(segment, clipped.endHz);
        clipped.actualCenterHz = (clipped.actualStartHz + clipped.actualEndHz) * 0.5;
        window.segments.push_back(clipped);
    }

    return window;
}

bool actualFrequencyInsideScanSegments(double frequencyHz, const QVector<ScanVisualSegment> &segments) {
    if (!std::isfinite(frequencyHz) || segments.isEmpty()) {
        return false;
    }
    for (const ScanVisualSegment &segment : segments) {
        if (std::isfinite(segment.actualStartHz) &&
            std::isfinite(segment.actualEndHz) &&
            segment.actualEndHz > segment.actualStartHz &&
            frequencyHz >= segment.actualStartHz &&
            frequencyHz <= segment.actualEndHz) {
            return true;
        }
    }
    return false;
}

double fallbackActualFrequencyForScanSegments(const QVector<ScanVisualSegment> &segments,
                                              double fallbackHz) {
    if (segments.isEmpty()) {
        return fallbackHz;
    }
    const ScanVisualSegment &segment = segments.at(segments.size() / 2);
    if (std::isfinite(segment.actualCenterHz)) {
        return segment.actualCenterHz;
    }
    if (std::isfinite(segment.actualStartHz) &&
        std::isfinite(segment.actualEndHz) &&
        segment.actualEndHz > segment.actualStartHz) {
        return (segment.actualStartHz + segment.actualEndHz) * 0.5;
    }
    return fallbackHz;
}
