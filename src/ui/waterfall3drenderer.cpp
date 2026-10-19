#include "waterfall3drenderer.h"

#include <QtGui/qopengl.h>
#include <QDebug>
#include <QMatrix4x4>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QVector4D>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>

namespace {
float normalizedHeight(float level, float levelMin, float levelMax) {
    if (!std::isfinite(level) || !std::isfinite(levelMin) ||
        !std::isfinite(levelMax) || levelMax <= levelMin) {
        return 0.0f;
    }
    return std::clamp((level - levelMin) / (levelMax - levelMin), 0.0f, 1.0f);
}

std::array<unsigned char, 3> monochromeAmplitudeColor(float value,
                                                       const QColor &baseColor,
                                                       int depthPercent) {
    const float t = std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f);
    const float depth = std::clamp(static_cast<float>(depthPercent) / 100.0f,
                                   0.0f,
                                   1.0f);
    const float lowFactor = 1.0f - 0.86f * depth;
    const float highFactor = 1.0f + 0.38f * depth;
    const float factor = lowFactor + (highFactor - lowFactor) * std::sqrt(t);
    const auto channel = [factor](int component) {
        return static_cast<unsigned char>(std::lround(std::clamp(
            static_cast<float>(component) * factor, 0.0f, 255.0f)));
    };
    return {
        channel(baseColor.red()), channel(baseColor.green()), channel(baseColor.blue())
    };
}
QVector3D densityProfileColor(float value) {
    const float t = std::clamp(value, 0.0f, 1.0f);
    if (t < 0.16f) {
        const float u = t / 0.16f;
        return QVector3D(0.03f * (1.0f - u),
                         0.07f + 0.06f * u,
                         0.19f + 0.28f * u);
    }
    if (t < 0.38f) {
        const float u = (t - 0.16f) / 0.22f;
        return QVector3D(0.0f, 0.13f + 0.74f * u, 0.47f + 0.47f * u);
    }
    if (t < 0.64f) {
        const float u = (t - 0.38f) / 0.26f;
        return QVector3D(0.96f * u, 0.87f + 0.10f * u, 0.94f * (1.0f - u));
    }
    if (t < 0.84f) {
        const float u = (t - 0.64f) / 0.20f;
        return QVector3D(0.96f, 0.97f * (1.0f - u) + 0.20f * u, 0.0f);
    }
    const float u = (t - 0.84f) / 0.16f;
    return QVector3D(0.96f + 0.04f * u, 0.20f + 0.80f * u, u);
}

struct FrontFacePoint {
    float frequency = 0.0f;
    float height = 0.0f;
};

using FrontFacePolygon = std::array<FrontFacePoint, 8>;

int clipFrontFacePolygon(const FrontFacePolygon &input,
                         int inputCount,
                         FrontFacePolygon &output,
                         float boundary,
                         bool keepAbove) {
    if (inputCount <= 0) {
        return 0;
    }

    int outputCount = 0;
    FrontFacePoint previous = input[static_cast<std::size_t>(inputCount - 1)];
    bool previousInside = keepAbove ? previous.height >= boundary
                                    : previous.height <= boundary;
    for (int index = 0; index < inputCount; ++index) {
        const FrontFacePoint current = input[static_cast<std::size_t>(index)];
        const bool currentInside = keepAbove ? current.height >= boundary
                                             : current.height <= boundary;
        if (currentInside != previousInside && outputCount < int(output.size())) {
            const float denominator = current.height - previous.height;
            const float ratio = std::abs(denominator) > 1.0e-7f
                                    ? std::clamp((boundary - previous.height) / denominator,
                                                 0.0f,
                                                 1.0f)
                                    : 0.0f;
            output[static_cast<std::size_t>(outputCount++)] = {
                previous.frequency + (current.frequency - previous.frequency) * ratio,
                boundary};
        }
        if (currentInside && outputCount < int(output.size())) {
            output[static_cast<std::size_t>(outputCount++)] = current;
        }
        previous = current;
        previousInside = currentInside;
    }
    return outputCount;
}

}

void Waterfall3DRenderer::appendRow(const std::vector<float> &levels,
                                    const std::vector<unsigned char> &rgb,
                                    float levelMin,
                                    float levelMax,
                                    int repeatRows) {
    if (levels.empty()) {
        return;
    }

    const int sourceColumns = static_cast<int>(levels.size());
    const int outputColumns = (std::max)(2, sourceColumns / resolutionDivisor);
    if (!historyRows.empty() &&
        static_cast<int>(historyRows.back().size()) != outputColumns) {
        resampleHistoryColumns(outputColumns);
    }
    HistoryRow row(static_cast<std::size_t>(outputColumns));

    for (int column = 0; column < outputColumns; ++column) {
        const int begin = column * sourceColumns / outputColumns;
        const int end = (std::max)(begin + 1, (column + 1) * sourceColumns / outputColumns);
        double levelSum = 0.0;
        int validLevels = 0;
        for (int source = begin; source < end && source < sourceColumns; ++source) {
            const float level = levels[static_cast<std::size_t>(source)];
            if (std::isfinite(level)) {
                levelSum += level;
                ++validLevels;
            }
        }

        const float averageLevel = validLevels > 0
                                       ? static_cast<float>(levelSum / static_cast<double>(validLevels))
                                       : levelMin;
        int representativeIndex = begin;
        float closestDistance = std::numeric_limits<float>::infinity();
        for (int source = begin; source < end && source < sourceColumns; ++source) {
            const float level = levels[static_cast<std::size_t>(source)];
            if (!std::isfinite(level)) {
                continue;
            }
            const float distance = std::abs(level - averageLevel);
            if (distance < closestDistance) {
                closestDistance = distance;
                representativeIndex = source;
            }
        }

        VertexSample &sample = row[static_cast<std::size_t>(column)];
        sample.height = normalizedHeight(averageLevel, levelMin, levelMax);
        sample.levelDb = averageLevel;
        const std::size_t colorOffset = static_cast<std::size_t>(representativeIndex) * 3U;
        if (colorOffset + 2U < rgb.size()) {
            sample.color = {{rgb[colorOffset], rgb[colorOffset + 1U], rgb[colorOffset + 2U]}};
        }
    }

    const int previousRowCount = static_cast<int>(historyRows.size());
    const int copies = (std::clamp)(repeatRows, 1, 8);
    for (int copy = 0; copy < copies; ++copy) {
        historyRows.push_back(row);
        if (!gpuTexturesResetRequired) {
            pendingGpuRows.push_back(row);
            if (static_cast<int>(pendingGpuRows.size()) > maxHistoryRows) {
                pendingGpuRows.clear();
                gpuTexturesResetRequired = true;
            }
        }
        while (static_cast<int>(historyRows.size()) > maxHistoryRows) {
            historyRows.pop_front();
            if (highlightedHistoryRow >= 0) {
                --highlightedHistoryRow;
            }
            if (spectrumSliceActive && spectrumSliceCapture && spectrumSliceCaptureFixed) {
                --capturedSpectrumRowsRemaining;
                if (capturedSpectrumRowsRemaining <= 0) {
                    capturedSpectrumRows.clear();
                    capturedSpectrumFirstRow = -1;
                    spectrumSliceActive = false;
                    selectedSpectrumRow = -1;
                }
            } else if (spectrumSliceActive && spectrumSliceCapture) {
                --selectedSpectrumRow;
            }
        }
    }
    if (previousRowCount != static_cast<int>(historyRows.size())) {
        gpuMeshDirty = true;
    }
}

void Waterfall3DRenderer::resampleHistoryColumns(int outputColumns) {
    outputColumns = (std::max)(2, outputColumns);
    if (historyRows.empty() || historyRows.front().empty()) {
        return;
    }

    const int previousColumns = static_cast<int>(historyRows.front().size());
    if (previousColumns == outputColumns) {
        return;
    }

    auto resampleRow = [outputColumns](const HistoryRow &source) {
        HistoryRow destination(static_cast<std::size_t>(outputColumns));
        if (source.empty()) {
            return destination;
        }
        if (source.size() == 1U) {
            std::fill(destination.begin(), destination.end(), source.front());
            return destination;
        }

        const double sourceLast = static_cast<double>(source.size() - 1U);
        const double destinationLast = static_cast<double>((std::max)(1, outputColumns - 1));
        for (int column = 0; column < outputColumns; ++column) {
            const double sourcePosition = static_cast<double>(column) * sourceLast / destinationLast;
            const int first = (std::clamp)(static_cast<int>(std::floor(sourcePosition)),
                                           0,
                                           static_cast<int>(source.size()) - 1);
            const int second = (std::min)(first + 1, static_cast<int>(source.size()) - 1);
            const float ratio = static_cast<float>(sourcePosition - static_cast<double>(first));
            const VertexSample &a = source[static_cast<std::size_t>(first)];
            const VertexSample &b = source[static_cast<std::size_t>(second)];
            VertexSample &sample = destination[static_cast<std::size_t>(column)];
            sample.height = a.height + (b.height - a.height) * ratio;
            sample.levelDb = a.levelDb + (b.levelDb - a.levelDb) * ratio;
            for (int channel = 0; channel < 3; ++channel) {
                const float color = static_cast<float>(a.color[static_cast<std::size_t>(channel)]) +
                                    (static_cast<float>(b.color[static_cast<std::size_t>(channel)]) -
                                     static_cast<float>(a.color[static_cast<std::size_t>(channel)])) * ratio;
                sample.color[static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(
                    (std::clamp)(static_cast<int>(std::lround(color)), 0, 255));
            }
        }
        return destination;
    };

    for (HistoryRow &row : historyRows) {
        row = resampleRow(row);
    }
    for (HistoryRow &row : capturedSpectrumRows) {
        row = resampleRow(row);
    }
    if (selectedSliceColumn >= 0 && previousColumns > 1) {
        const double ratio = static_cast<double>(selectedSliceColumn) /
                             static_cast<double>(previousColumns - 1);
        selectedSliceColumn = (std::clamp)(
            static_cast<int>(std::lround(ratio * static_cast<double>(outputColumns - 1))),
            0,
            outputColumns - 1);
    }
    pendingGpuRows.clear();
    resetGpuSurfaceData();
    gpuMeshDirty = true;
}

void Waterfall3DRenderer::clear() {
    historyRows.clear();
    densityFrontProfile.clear();
    selectedSliceColumn = -1;
    frequencySliceActive = false;
    highlightedHistoryRow = -1;
    selectedSpectrumRow = -1;
    spectrumSliceActive = false;
    capturedSpectrumRows.clear();
    capturedSpectrumFirstRow = -1;
    capturedSpectrumRowsRemaining = 0;
    resetGpuSurfaceData();
}

bool Waterfall3DRenderer::hasData() const {
    return historyRows.size() >= 2 && !historyRows.front().empty();
}

void Waterfall3DRenderer::setResolutionDivisor(int divisor) {
    const int normalizedDivisor = std::clamp(divisor, 1, 64);
    if (resolutionDivisor == normalizedDivisor) {
        return;
    }
    resolutionDivisor = normalizedDivisor;
    historyRows.clear();
    selectedSliceColumn = -1;
    frequencySliceActive = false;
    selectedSpectrumRow = -1;
    spectrumSliceActive = false;
    capturedSpectrumRows.clear();
    capturedSpectrumFirstRow = -1;
    capturedSpectrumRowsRemaining = 0;
    resetGpuSurfaceData();
}

void Waterfall3DRenderer::setHistoryCapacity(int rows) {
    const int normalizedRows = std::clamp(rows, 2, 2048);
    if (maxHistoryRows == normalizedRows) {
        return;
    }
    maxHistoryRows = normalizedRows;
    while (static_cast<int>(historyRows.size()) > maxHistoryRows) {
        historyRows.pop_front();
        if (spectrumSliceActive && spectrumSliceCapture && spectrumSliceCaptureFixed) {
            --capturedSpectrumRowsRemaining;
        } else if (spectrumSliceActive && spectrumSliceCapture) {
            --selectedSpectrumRow;
        }
    }
    if (spectrumSliceActive && spectrumSliceCapture && spectrumSliceCaptureFixed &&
        capturedSpectrumRowsRemaining <= 0) {
        capturedSpectrumRows.clear();
        capturedSpectrumFirstRow = -1;
        spectrumSliceActive = false;
        selectedSpectrumRow = -1;
    }
    if (spectrumSliceActive && !spectrumSliceCapture && !historyRows.empty()) {
        selectedSpectrumRow = std::clamp(selectedSpectrumRow,
                                         0,
                                         static_cast<int>(historyRows.size()) - 1);
    }
    highlightedHistoryRow = -1;
    resetGpuSurfaceData();
}

void Waterfall3DRenderer::setHighlightedHistoryRow(int row) {
    highlightedHistoryRow = row >= 0 && row < static_cast<int>(historyRows.size()) ? row : -1;
}

void Waterfall3DRenderer::setSliceScrollStep(int points) {
    sliceScrollStep = std::clamp(points, 1, 256);
}

void Waterfall3DRenderer::setSliceWidth(int points) {
    sliceWidth = std::clamp(points, 1, 4096);
}

void Waterfall3DRenderer::setSpectrumSliceScrollStep(int rows) {
    spectrumSliceScrollStep = std::clamp(rows, 1, 2048);
}

void Waterfall3DRenderer::setSpectrumSliceWidth(int rows) {
    spectrumSliceWidth = std::clamp(rows, 1, 2048);
}

void Waterfall3DRenderer::setSpectrumSliceCapture(bool enabled) {
    spectrumSliceCapture = enabled;
    if (enabled && spectrumSliceCaptureFixed && spectrumSliceActive) {
        refreshCapturedSpectrumRows();
    } else if (!enabled) {
        capturedSpectrumRows.clear();
        capturedSpectrumFirstRow = -1;
        capturedSpectrumRowsRemaining = 0;
    }
    if (!enabled && spectrumSliceActive && !historyRows.empty()) {
        selectedSpectrumRow = std::clamp(selectedSpectrumRow,
                                         0,
                                         static_cast<int>(historyRows.size()) - 1);
    }
}

void Waterfall3DRenderer::setSpectrumSliceCaptureFixed(bool enabled) {
    spectrumSliceCaptureFixed = enabled;
    capturedSpectrumRows.clear();
    capturedSpectrumFirstRow = -1;
    capturedSpectrumRowsRemaining = 0;
    if (enabled && spectrumSliceCapture && spectrumSliceActive) {
        refreshCapturedSpectrumRows();
    }
}

void Waterfall3DRenderer::refreshCapturedSpectrumRows() {
    capturedSpectrumRows.clear();
    capturedSpectrumFirstRow = -1;
    capturedSpectrumRowsRemaining = 0;
    if (!spectrumSliceCapture || !spectrumSliceCaptureFixed ||
        !spectrumSliceActive || historyRows.empty() ||
        selectedSpectrumRow < 0) {
        return;
    }

    const int rowCount = static_cast<int>(historyRows.size());
    const int halfWidth = spectrumSliceWidth / 2;
    const int firstRow = std::max(0, selectedSpectrumRow - halfWidth);
    const int lastRow = std::min(rowCount - 1, firstRow + spectrumSliceWidth - 1);
    capturedSpectrumRows.reserve(static_cast<std::size_t>(lastRow - firstRow + 1));
    for (int row = firstRow; row <= lastRow; ++row) {
        capturedSpectrumRows.push_back(historyRows[static_cast<std::size_t>(row)]);
    }
    capturedSpectrumFirstRow = firstRow;
    capturedSpectrumRowsRemaining = lastRow + 1;
}

bool Waterfall3DRenderer::beginFrequencySlice(int screenX,
                                             int screenY,
                                             int viewportWidth,
                                             int viewportHeight) {
    if (!hasData() || viewportWidth <= 0 || viewportHeight <= 0) {
        return false;
    }

    const int rowCount = static_cast<int>(historyRows.size());
    const int columnCount = static_cast<int>(historyRows.front().size());
    if (rowCount < 2 || columnCount < 1) {
        return false;
    }

    if (fixedFrontPresentation) {
        const double ratio = std::clamp(static_cast<double>(screenX) /
                                            static_cast<double>((std::max)(1, viewportWidth - 1)),
                                        0.0,
                                        1.0);
        selectedSliceColumn = std::clamp(
            static_cast<int>(std::lround(ratio * static_cast<double>(columnCount - 1))),
            0,
            columnCount - 1);
        frequencySliceActive = true;
        spectrumSliceActive = false;
        selectedSpectrumRow = -1;
        return true;
    }

    const QMatrix4x4 transform = viewParameters(viewportWidth, viewportHeight).transform;

    float bestDistanceSquared = std::numeric_limits<float>::infinity();
    int bestColumn = -1;
    const int rowStep = std::max(1, rowCount / 512);
    const int columnStep = std::max(1, columnCount / 1024);
    for (int row = 0; row < rowCount; row += rowStep) {
        const HistoryRow &historyRow = historyRows[static_cast<std::size_t>(row)];
        if (static_cast<int>(historyRow.size()) != columnCount) {
            continue;
        }
        const float time = -1.0f + 2.0f * static_cast<float>(row) /
                                       static_cast<float>(rowCount - 1);
        for (int column = 0; column < columnCount; column += columnStep) {
            const float frequency = columnCount > 1
                                        ? -1.0f + 2.0f * static_cast<float>(column) /
                                                      static_cast<float>(columnCount - 1)
                                        : 0.0f;
            const VertexSample &sample = historyRow[static_cast<std::size_t>(column)];
            const QVector4D clip = transform * QVector4D(time,
                                                        frequency,
                                                        sample.height * 0.72f,
                                                        1.0f);
            if (clip.w() <= 0.0f) {
                continue;
            }
            const float inverseW = 1.0f / clip.w();
            const float projectedX = (clip.x() * inverseW + 1.0f) * 0.5f * viewportWidth;
            const float projectedY = (1.0f - clip.y() * inverseW) * 0.5f * viewportHeight;
            const float dx = projectedX - static_cast<float>(screenX);
            const float dy = projectedY - static_cast<float>(screenY);
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared < bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestColumn = column;
            }
        }
    }

    constexpr float MaxPickDistancePixels = 36.0f;
    if (bestColumn < 0 ||
        bestDistanceSquared > MaxPickDistancePixels * MaxPickDistancePixels) {
        return false;
    }
    selectedSliceColumn = bestColumn;
    frequencySliceActive = true;
    spectrumSliceActive = false;
    selectedSpectrumRow = -1;
    return true;
}

bool Waterfall3DRenderer::beginSpectrumSlice(int screenX,
                                            int screenY,
                                            int viewportWidth,
                                            int viewportHeight) {
    if (!hasData() || viewportWidth <= 0 || viewportHeight <= 0) {
        return false;
    }

    const int rowCount = static_cast<int>(historyRows.size());
    const int columnCount = static_cast<int>(historyRows.front().size());
    if (rowCount < 2 || columnCount < 1) {
        return false;
    }

    const QMatrix4x4 transform = viewParameters(viewportWidth, viewportHeight).transform;

    float bestDistanceSquared = std::numeric_limits<float>::infinity();
    int bestRow = -1;
    const int rowStep = std::max(1, rowCount / 512);
    const int columnStep = std::max(1, columnCount / 1024);
    for (int row = 0; row < rowCount; row += rowStep) {
        const HistoryRow &historyRow = historyRows[static_cast<std::size_t>(row)];
        if (static_cast<int>(historyRow.size()) != columnCount) {
            continue;
        }
        const float time = -1.0f + 2.0f * static_cast<float>(row) /
                                       static_cast<float>(rowCount - 1);
        for (int column = 0; column < columnCount; column += columnStep) {
            const float frequency = columnCount > 1
                                        ? -1.0f + 2.0f * static_cast<float>(column) /
                                                      static_cast<float>(columnCount - 1)
                                        : 0.0f;
            const VertexSample &sample = historyRow[static_cast<std::size_t>(column)];
            const QVector4D clip = transform * QVector4D(time,
                                                        frequency,
                                                        sample.height * 0.72f,
                                                        1.0f);
            if (clip.w() <= 0.0f) {
                continue;
            }
            const float inverseW = 1.0f / clip.w();
            const float projectedX = (clip.x() * inverseW + 1.0f) * 0.5f * viewportWidth;
            const float projectedY = (1.0f - clip.y() * inverseW) * 0.5f * viewportHeight;
            const float dx = projectedX - static_cast<float>(screenX);
            const float dy = projectedY - static_cast<float>(screenY);
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared < bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestRow = row;
            }
        }
    }

    const float maxPickDistancePixels = fixedFrontPresentation
                                            ? static_cast<float>((std::max)(viewportWidth,
                                                                           viewportHeight))
                                            : 36.0f;
    if (bestRow < 0 ||
        bestDistanceSquared > maxPickDistancePixels * maxPickDistancePixels) {
        return false;
    }
    selectedSpectrumRow = bestRow;
    spectrumSliceActive = true;
    frequencySliceActive = false;
    selectedSliceColumn = -1;
    if (spectrumSliceCapture && spectrumSliceCaptureFixed) {
        refreshCapturedSpectrumRows();
    }
    return true;
}

void Waterfall3DRenderer::stepFrequencySlice(int direction) {
    if (!frequencySliceActive || selectedSliceColumn < 0 || historyRows.empty()) {
        return;
    }
    const int columnCount = static_cast<int>(historyRows.front().size());
    selectedSliceColumn = std::clamp(selectedSliceColumn + direction * sliceScrollStep,
                                     0,
                                     (std::max)(0, columnCount - 1));
}

void Waterfall3DRenderer::stepSpectrumSlice(int direction) {
    if (!spectrumSliceActive || historyRows.empty()) {
        return;
    }
    const int rowCount = static_cast<int>(historyRows.size());
    selectedSpectrumRow = std::clamp(selectedSpectrumRow + direction * spectrumSliceScrollStep,
                                     0,
                                     std::max(0, rowCount - 1));
    if (spectrumSliceCapture && spectrumSliceCaptureFixed) {
        refreshCapturedSpectrumRows();
    }
}

void Waterfall3DRenderer::endFrequencySlice() {
    frequencySliceActive = false;
    selectedSliceColumn = -1;
}

void Waterfall3DRenderer::endSpectrumSlice() {
    spectrumSliceActive = false;
    selectedSpectrumRow = -1;
    capturedSpectrumRows.clear();
    capturedSpectrumFirstRow = -1;
    capturedSpectrumRowsRemaining = 0;
}

double Waterfall3DRenderer::selectedFrequencyRatio() const {
    if (selectedSliceColumn < 0 || historyRows.empty() || historyRows.front().size() < 2) {
        return -1.0;
    }
    return static_cast<double>(selectedSliceColumn) /
           static_cast<double>(historyRows.front().size() - 1);
}

bool Waterfall3DRenderer::frequencySliceRange(double &centerRatio,
                                              double &firstRatio,
                                              double &lastRatio) const {
    if (!frequencySliceActive || selectedSliceColumn < 0 || historyRows.empty() ||
        historyRows.front().size() < 2) {
        return false;
    }
    const int columnCount = static_cast<int>(historyRows.front().size());
    const int halfWidth = sliceWidth / 2;
    int firstColumn = std::max(0, selectedSliceColumn - halfWidth);
    int lastColumn = std::min(columnCount - 1, firstColumn + sliceWidth - 1);
    firstColumn = std::max(0, lastColumn - sliceWidth + 1);
    const double denominator = static_cast<double>(columnCount - 1);
    centerRatio = static_cast<double>(selectedSliceColumn) / denominator;
    firstRatio = static_cast<double>(firstColumn) / denominator;
    lastRatio = static_cast<double>(lastColumn) / denominator;
    return true;
}

bool Waterfall3DRenderer::spectrumSliceRange(int &firstRow,
                                             int &lastRow,
                                             int &rowCount) const {
    if (!spectrumSliceActive || selectedSpectrumRow < 0 || historyRows.empty()) {
        return false;
    }
    rowCount = static_cast<int>(historyRows.size());
    const int halfWidth = spectrumSliceWidth / 2;
    firstRow = std::max(0, selectedSpectrumRow - halfWidth);
    lastRow = std::min(rowCount - 1, firstRow + spectrumSliceWidth - 1);
    firstRow = std::max(0, lastRow - spectrumSliceWidth + 1);
    return firstRow <= lastRow;
}

bool Waterfall3DRenderer::frequencySliceStatistics(SliceStatistics &statistics) const {
    statistics = SliceStatistics{};
    if (!frequencySliceActive || selectedSliceColumn < 0 || historyRows.empty() ||
        historyRows.front().empty()) {
        return false;
    }

    statistics.columnCount = static_cast<int>(historyRows.front().size());
    statistics.rowCount = static_cast<int>(historyRows.size());
    const int halfWidth = sliceWidth / 2;
    statistics.firstColumn = std::max(0, selectedSliceColumn - halfWidth);
    statistics.lastColumn = std::min(statistics.columnCount - 1,
                                     statistics.firstColumn + sliceWidth - 1);
    statistics.firstColumn = std::max(0,
                                      statistics.lastColumn - sliceWidth + 1);
    statistics.firstRow = 0;
    statistics.lastRow = statistics.rowCount - 1;

    double levelSum = 0.0;
    float minimumLevel = std::numeric_limits<float>::infinity();
    float maximumLevel = -std::numeric_limits<float>::infinity();
    for (int row = 0; row < statistics.rowCount; ++row) {
        const HistoryRow &historyRow = historyRows[static_cast<std::size_t>(row)];
        if (static_cast<int>(historyRow.size()) != statistics.columnCount) {
            continue;
        }
        for (int column = statistics.firstColumn;
             column <= statistics.lastColumn;
             ++column) {
            const float level = historyRow[static_cast<std::size_t>(column)].levelDb;
            if (!std::isfinite(level)) {
                continue;
            }
            minimumLevel = std::min(minimumLevel, level);
            levelSum += level;
            ++statistics.sampleCount;
            if (level > maximumLevel) {
                maximumLevel = level;
                statistics.peakColumn = column;
                statistics.peakRow = row;
            }
        }
    }

    if (statistics.sampleCount <= 0) {
        return false;
    }
    statistics.minimumLevelDb = minimumLevel;
    statistics.maximumLevelDb = maximumLevel;
    statistics.averageLevelDb = static_cast<float>(levelSum / statistics.sampleCount);
    statistics.valid = true;
    return true;
}

bool Waterfall3DRenderer::spectrumSliceStatistics(SliceStatistics &statistics) const {
    statistics = SliceStatistics{};
    if (!spectrumSliceActive || selectedSpectrumRow < 0 || historyRows.empty() ||
        historyRows.front().empty()) {
        return false;
    }

    statistics.columnCount = static_cast<int>(historyRows.front().size());
    statistics.rowCount = static_cast<int>(historyRows.size());
    statistics.firstColumn = 0;
    statistics.lastColumn = statistics.columnCount - 1;

    const bool useCapturedRows = spectrumSliceCapture && spectrumSliceCaptureFixed &&
                                 !capturedSpectrumRows.empty();
    if (useCapturedRows) {
        statistics.firstRow = capturedSpectrumFirstRow;
        statistics.lastRow = statistics.firstRow +
                             static_cast<int>(capturedSpectrumRows.size()) - 1;
    } else {
        const int halfWidth = spectrumSliceWidth / 2;
        statistics.firstRow = std::max(0, selectedSpectrumRow - halfWidth);
        statistics.lastRow = std::min(statistics.rowCount - 1,
                                      statistics.firstRow + spectrumSliceWidth - 1);
        statistics.firstRow = std::max(0,
                                       statistics.lastRow - spectrumSliceWidth + 1);
    }

    double levelSum = 0.0;
    float minimumLevel = std::numeric_limits<float>::infinity();
    float maximumLevel = -std::numeric_limits<float>::infinity();
    const int rowsToRead = useCapturedRows
                               ? static_cast<int>(capturedSpectrumRows.size())
                               : statistics.lastRow - statistics.firstRow + 1;
    for (int rowOffset = 0; rowOffset < rowsToRead; ++rowOffset) {
        const int displayedRow = statistics.firstRow + rowOffset;
        const HistoryRow &historyRow = useCapturedRows
                                           ? capturedSpectrumRows[static_cast<std::size_t>(rowOffset)]
                                           : historyRows[static_cast<std::size_t>(displayedRow)];
        if (static_cast<int>(historyRow.size()) != statistics.columnCount) {
            continue;
        }
        for (int column = 0; column < statistics.columnCount; ++column) {
            const float level = historyRow[static_cast<std::size_t>(column)].levelDb;
            if (!std::isfinite(level)) {
                continue;
            }
            minimumLevel = std::min(minimumLevel, level);
            levelSum += level;
            ++statistics.sampleCount;
            if (level > maximumLevel) {
                maximumLevel = level;
                statistics.peakColumn = column;
                statistics.peakRow = displayedRow;
            }
        }
    }

    if (statistics.sampleCount <= 0) {
        return false;
    }
    statistics.minimumLevelDb = minimumLevel;
    statistics.maximumLevelDb = maximumLevel;
    statistics.averageLevelDb = static_cast<float>(levelSum / statistics.sampleCount);
    statistics.valid = true;
    return true;
}

void Waterfall3DRenderer::orbitCamera(float deltaX, float deltaY) {
    if (fixedFrontPresentation) {
        return;
    }
    cameraYawDegrees = std::fmod(cameraYawDegrees + deltaX * 0.4f, 360.0f);
    cameraTiltDegrees = std::clamp(cameraTiltDegrees + deltaY * 0.35f, -82.0f, -5.0f);
}

void Waterfall3DRenderer::panCamera(float deltaX,
                                    float deltaY,
                                    int viewportWidth,
                                    int viewportHeight) {
    if (fixedFrontPresentation) {
        return;
    }
    if (viewportWidth <= 0 || viewportHeight <= 0 ||
        (!std::isfinite(deltaX) || !std::isfinite(deltaY))) {
        return;
    }

    constexpr float Pi = 3.14159265358979323846f;
    const float inverseZoom = 1.0f / std::max(0.35f, cameraZoom);
    const float screenX = 2.25f * deltaX / static_cast<float>(viewportWidth) * inverseZoom;
    const float tiltRadians = cameraTiltDegrees * Pi / 180.0f;
    const float verticalProjection = std::max(0.22f, std::abs(std::cos(tiltRadians)));
    const float screenY = -2.25f * deltaY / static_cast<float>(viewportHeight) *
                          inverseZoom / verticalProjection;
    const float yawRadians = cameraYawDegrees * Pi / 180.0f;
    const float cosine = std::cos(yawRadians);
    const float sine = std::sin(yawRadians);

    cameraPanX = std::clamp(cameraPanX + cosine * screenX + sine * screenY,
                            -4.0f,
                            4.0f);
    cameraPanY = std::clamp(cameraPanY - sine * screenX + cosine * screenY,
                            -4.0f,
                            4.0f);
}

void Waterfall3DRenderer::zoomCamera(int wheelDelta) {
    if (fixedFrontPresentation) {
        return;
    }
    if (wheelDelta == 0) {
        return;
    }
    const float wheelSteps = static_cast<float>(wheelDelta) / 120.0f;
    cameraZoom *= std::pow(1.18f, wheelSteps);
    cameraZoom = std::clamp(cameraZoom, 0.35f, 8.0f);
}

void Waterfall3DRenderer::setFixedFrontPresentation(bool enabled) {
    fixedFrontPresentation = enabled;
}

void Waterfall3DRenderer::setFixedFrontExpanded(bool enabled) {
    fixedFrontExpanded = enabled;
}

void Waterfall3DRenderer::setFrontFaceGradient(bool enabled, int opacityPercent) {
    frontFaceGradient = enabled;
    frontFaceGradientOpacity = std::clamp(opacityPercent, 0, 100);
}

void Waterfall3DRenderer::setDensityAxes(bool enabled) {
    if (densityAxes == enabled) {
        return;
    }
    densityAxes = enabled;
    gpuMeshDirty = true;
}

void Waterfall3DRenderer::setDensityAxisMapping(int xDimension, int yDimension) {
    xDimension = std::clamp(xDimension, 0, 2);
    yDimension = std::clamp(yDimension, 0, 2);
    if (xDimension == yDimension) {
        return;
    }
    densityAxisXDimension = xDimension;
    densityAxisYDimension = yDimension;
    densityAxisZDimension = 3 - xDimension - yDimension;
}

void Waterfall3DRenderer::setDensityFrontProfile(
    const std::vector<float> &normalizedLevels) {
    densityFrontProfile = normalizedLevels;
    for (float &level : densityFrontProfile) {
        level = std::clamp(level, 0.0f, 1.0f);
    }
}

void Waterfall3DRenderer::setDensityFrontProfileStyle(int style) {
    densityFrontProfileStyle = std::clamp(style, 0, 1);
}

void Waterfall3DRenderer::setSurfaceStyle(int style) {
    style = std::clamp(style, 0, 1);
    if (surfaceStyle == style) {
        return;
    }
    surfaceStyle = style;
    gpuMeshDirty = true;
}

void Waterfall3DRenderer::setSurfaceSmoothing(int smoothing) {
    smoothing = std::clamp(smoothing, 0, 2);
    if (surfaceSmoothing == smoothing) {
        return;
    }
    surfaceSmoothing = smoothing;
    gpuTexturesResetRequired = true;
}

void Waterfall3DRenderer::setSurfaceLighting(int lighting) {
    surfaceLighting = std::clamp(lighting, 0, 2);
}

void Waterfall3DRenderer::setMonochrome(bool enabled) {
    if (monochrome == enabled) return;
    monochrome = enabled;
    gpuTexturesResetRequired = true;
    pendingGpuRows.clear();
}

void Waterfall3DRenderer::setHeightGradient(bool enabled) {
    heightGradient = enabled;
}

void Waterfall3DRenderer::setMonochromeColor(const QColor &color) {
    if (!color.isValid() || monochromeColor == color) return;
    monochromeColor = color;
    gpuTexturesResetRequired = true;
    pendingGpuRows.clear();
}

void Waterfall3DRenderer::setMonochromeGradientDepth(int percent) {
    percent = std::clamp(percent, 0, 100);
    if (monochromeGradientDepth == percent) return;
    monochromeGradientDepth = percent;
    gpuTexturesResetRequired = true;
    pendingGpuRows.clear();
}

void Waterfall3DRenderer::applyHeightColor(float height) const {
    if (monochrome) {
        const auto color = monochromeAmplitudeColor(
            height, monochromeColor, monochromeGradientDepth);
        glColor3ub(color[0], color[1], color[2]);
        return;
    }
    const QVector3D color = densityProfileColor(height);
    glColor3f(color.x(), color.y(), color.z());
}

void Waterfall3DRenderer::applySampleColor(const VertexSample &sample) const {
    if (heightGradient) {
        applyHeightColor(sample.height);
        return;
    }
    if (monochrome) {
        const auto color = monochromeAmplitudeColor(
            sample.height, monochromeColor, monochromeGradientDepth);
        glColor3ub(color[0], color[1], color[2]);
    } else {
        glColor3ub(sample.color[0], sample.color[1], sample.color[2]);
    }
}
QVector3D Waterfall3DRenderer::densityPosition(float densityRatio,
                                               float frequencyRatio,
                                               float levelRatio) const {
    const std::array<float, 3> values{{std::clamp(frequencyRatio, 0.0f, 1.0f),
                                       std::clamp(densityRatio, 0.0f, 1.0f),
                                       std::clamp(levelRatio, 0.0f, 1.0f)}};
    return QVector3D(-1.0f + 2.0f * values[static_cast<std::size_t>(densityAxisXDimension)],
                     -1.0f + 2.0f * values[static_cast<std::size_t>(densityAxisYDimension)],
                     0.72f * values[static_cast<std::size_t>(densityAxisZDimension)]);
}

bool Waterfall3DRenderer::densityPointToScreen(float densityRatio,
                                               float frequencyRatio,
                                               float levelRatio,
                                               int viewportWidth,
                                               int viewportHeight,
                                               QPointF &screen) const {
    if (!densityAxes || viewportWidth <= 0 || viewportHeight <= 0) {
        return false;
    }
    const QMatrix4x4 transform = viewParameters(viewportWidth, viewportHeight).transform;
    const QVector3D position = densityPosition(densityRatio, frequencyRatio, levelRatio);
    const QVector4D clip = transform * QVector4D(position, 1.0f);
    if (!std::isfinite(clip.w()) || std::abs(clip.w()) < 1.0e-6f) {
        return false;
    }
    const float x = clip.x() / clip.w();
    const float y = clip.y() / clip.w();
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    screen.setX((x + 1.0f) * 0.5f * static_cast<float>(viewportWidth));
    screen.setY((1.0f - y) * 0.5f * static_cast<float>(viewportHeight));
    return true;
}

bool Waterfall3DRenderer::densityAxisScreenPoints(int viewportWidth,
                                                  int viewportHeight,
                                                  QPointF &origin,
                                                  QPointF &frequencyEnd,
                                                  QPointF &densityEnd,
                                                  QPointF &levelEnd) const {
    return densityPointToScreen(0.0f, 0.0f, 0.0f, viewportWidth, viewportHeight, origin) &&
           densityPointToScreen(0.0f, 1.0f, 0.0f, viewportWidth, viewportHeight, frequencyEnd) &&
           densityPointToScreen(1.0f, 0.0f, 0.0f, viewportWidth, viewportHeight, densityEnd) &&
           densityPointToScreen(0.0f, 0.0f, 1.0f, viewportWidth, viewportHeight, levelEnd);
}

bool Waterfall3DRenderer::ensureSurfaceProgram() {
    if (surfaceProgramReady) {
        return true;
    }
    if (surfaceProgramTried) {
        return false;
    }
    surfaceProgramTried = true;

    static const char *vertexSource =
        "attribute vec3 gridPosition;\n"
        "uniform mat4 transform;\n"
        "uniform sampler2D heightMap;\n"
        "uniform sampler2D colorMap;\n"
        "uniform float rowCount;\n"
        "uniform float columnCount;\n"
        "uniform float textureRows;\n"
        "uniform float oldestRow;\n"
        "uniform float densityAxes;\n"
        "uniform float densityAxisX;\n"
        "uniform float densityAxisY;\n"
        "uniform float densityAxisZ;\n"
        "uniform float smoothingMode;\n"
        "varying vec3 vertexColor;\n"
        "varying float pointDensity;\n"
        "varying float gradientHeight;\n"
        "varying vec3 surfaceNormal;\n"
        "float axisValue(float dimension, vec3 values) {\n"
        "    if (dimension < 0.5) return values.x;\n"
        "    if (dimension < 1.5) return values.y;\n"
        "    return values.z;\n"
        "}\n"
        "float rawHeight(float row, float column) {\n"
        "    float clampedRow = clamp(row, 0.0, max(0.0, rowCount - 1.0));\n"
        "    float clampedColumn = clamp(column, 0.0, max(0.0, columnCount - 1.0));\n"
        "    float physicalRow = mod(oldestRow + clampedRow, textureRows);\n"
        "    vec2 coord = vec2((clampedColumn + 0.5) / columnCount,\n"
        "                      (physicalRow + 0.5) / textureRows);\n"
        "    return texture2D(heightMap, coord).r;\n"
        "}\n"
        "float filteredHeight(float row, float column) {\n"
        "    float center = rawHeight(row, column);\n"
        "    if (smoothingMode < 0.5) return center;\n"
        "    float crossValue = rawHeight(row - 1.0, column) +\n"
        "                       rawHeight(row + 1.0, column) +\n"
        "                       rawHeight(row, column - 1.0) +\n"
        "                       rawHeight(row, column + 1.0);\n"
        "    if (smoothingMode < 1.5) return (center * 4.0 + crossValue) / 8.0;\n"
        "    float corners = rawHeight(row - 1.0, column - 1.0) +\n"
        "                    rawHeight(row - 1.0, column + 1.0) +\n"
        "                    rawHeight(row + 1.0, column - 1.0) +\n"
        "                    rawHeight(row + 1.0, column + 1.0);\n"
        "    return (center * 4.0 + crossValue * 2.0 + corners) / 16.0;\n"
        "}\n"
        "vec3 densityPosition(float frequencyRatio, float densityRatio, float levelRatio) {\n"
        "    vec3 values = vec3(frequencyRatio, densityRatio, levelRatio);\n"
        "    return vec3(-1.0 + 2.0 * axisValue(densityAxisX, values),\n"
        "                -1.0 + 2.0 * axisValue(densityAxisY, values),\n"
        "                0.72 * axisValue(densityAxisZ, values));\n"
        "}\n"
        "void main() {\n"
        "    float physicalRow = mod(oldestRow + gridPosition.x, textureRows);\n"
        "    vec2 texCoord = vec2((gridPosition.y + 0.5) / columnCount,\n"
        "                         (physicalRow + 0.5) / textureRows);\n"
        "    float time = -1.0 + 2.0 * gridPosition.x / max(1.0, rowCount - 1.0);\n"
        "    float frequency = -1.0 + 2.0 * gridPosition.y / max(1.0, columnCount - 1.0);\n"
        "    float value = filteredHeight(gridPosition.x, gridPosition.y);\n"
        "    vec3 logicalValues = vec3((frequency + 1.0) * 0.5,\n"
        "                              value * gridPosition.z,\n"
        "                              (time + 1.0) * 0.5);\n"
        "    vec3 position = densityAxes > 0.5\n"
        "        ? densityPosition(logicalValues.x, logicalValues.y, logicalValues.z)\n"
        "        : vec3(time, frequency, value * 0.72);\n"
        "    float rowBefore = max(0.0, gridPosition.x - 1.0);\n"
        "    float rowAfter = min(rowCount - 1.0, gridPosition.x + 1.0);\n"
        "    float columnBefore = max(0.0, gridPosition.y - 1.0);\n"
        "    float columnAfter = min(columnCount - 1.0, gridPosition.y + 1.0);\n"
        "    float rowSpan = max(1.0, rowCount - 1.0);\n"
        "    float columnSpan = max(1.0, columnCount - 1.0);\n"
        "    vec3 tangentRow;\n"
        "    vec3 tangentColumn;\n"
        "    if (densityAxes > 0.5) {\n"
        "        tangentRow = densityPosition(logicalValues.x,\n"
        "                    filteredHeight(rowAfter, gridPosition.y),\n"
        "                    rowAfter / rowSpan) -\n"
        "                     densityPosition(logicalValues.x,\n"
        "                    filteredHeight(rowBefore, gridPosition.y),\n"
        "                    rowBefore / rowSpan);\n"
        "        tangentColumn = densityPosition(columnAfter / columnSpan,\n"
        "                       filteredHeight(gridPosition.x, columnAfter),\n"
        "                       logicalValues.z) -\n"
        "                        densityPosition(columnBefore / columnSpan,\n"
        "                       filteredHeight(gridPosition.x, columnBefore),\n"
        "                       logicalValues.z);\n"
        "    } else {\n"
        "        tangentRow = vec3(2.0 * (rowAfter - rowBefore) / rowSpan, 0.0,\n"
        "                          0.72 * (filteredHeight(rowAfter, gridPosition.y) -\n"
        "                                  filteredHeight(rowBefore, gridPosition.y)));\n"
        "        tangentColumn = vec3(0.0, 2.0 * (columnAfter - columnBefore) / columnSpan,\n"
        "                             0.72 * (filteredHeight(gridPosition.x, columnAfter) -\n"
        "                                     filteredHeight(gridPosition.x, columnBefore)));\n"
        "    }\n"
        "    surfaceNormal = normalize(cross(tangentRow, tangentColumn));\n"
        "    gl_Position = transform * vec4(position, 1.0);\n"
        "    vertexColor = texture2D(colorMap, texCoord).rgb;\n"
        "    pointDensity = value;\n"
        "    gradientHeight = logicalValues.y;\n"
        "}\n";
    static const char *fragmentSource =
        "uniform float densityAxes;\n"
        "uniform float lightingMode;\n"
        "uniform float heightGradientMode;\n"
        "uniform float monochromeMode;\n"
        "uniform vec3 monochromeColor;\n"
        "uniform float monochromeGradientDepth;\n"
        "varying vec3 vertexColor;\n"
        "varying float pointDensity;\n"
        "varying float gradientHeight;\n"
        "varying vec3 surfaceNormal;\n"
        "vec3 amplitudeColor(float value) {\n"
        "    float t = clamp(value, 0.0, 1.0);\n"
        "    if (monochromeMode > 0.5) {\n"
        "        float lowFactor = 1.0 - 0.86 * monochromeGradientDepth;\n"
        "        float highFactor = 1.0 + 0.38 * monochromeGradientDepth;\n"
        "        float factor = mix(lowFactor, highFactor, sqrt(t));\n"
        "        return clamp(monochromeColor * factor, 0.0, 1.0);\n"
        "    }\n"
        "    if (t < 0.16) return mix(vec3(0.03, 0.07, 0.19), vec3(0.0, 0.13, 0.47), t / 0.16);\n"
        "    if (t < 0.38) return mix(vec3(0.0, 0.13, 0.47), vec3(0.0, 0.87, 0.94), (t - 0.16) / 0.22);\n"
        "    if (t < 0.64) return mix(vec3(0.0, 0.87, 0.94), vec3(0.96, 0.97, 0.0), (t - 0.38) / 0.26);\n"
        "    if (t < 0.84) return mix(vec3(0.96, 0.97, 0.0), vec3(0.96, 0.20, 0.0), (t - 0.64) / 0.20);\n"
        "    return mix(vec3(0.96, 0.20, 0.0), vec3(1.0, 1.0, 1.0), (t - 0.84) / 0.16);\n"
        "}\n"
        "void main() {\n"
        "    if (densityAxes > 0.5 && pointDensity <= 0.001) discard;\n"
        "    vec3 color = heightGradientMode > 0.5\n"
        "        ? amplitudeColor(gradientHeight) : vertexColor;\n"
        "    vec3 baseColor = color;\n"
        "    if (lightingMode > 0.5) {\n"
        "        vec3 normal = normalize(surfaceNormal);\n"
        "        vec3 lightDirection = normalize(vec3(-0.42, -0.32, 0.86));\n"
        "        float diffuse = abs(dot(normal, lightDirection));\n"
        "        float strength = lightingMode > 1.5 ? 0.72 : 0.42;\n"
        "        float slopeShadow = 1.0 - strength * 0.22 *\n"
        "                            clamp(1.0 - abs(normal.z), 0.0, 1.0);\n"
        "        color *= ((1.0 - strength) + strength * (0.30 + 0.70 * diffuse)) * slopeShadow;\n"
        "        color += baseColor * strength * 0.10 * pow(diffuse, 8.0);\n"
        "    }\n"
        "    gl_FragColor = vec4(color, 1.0);\n"
        "}\n";

    if (!surfaceProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexSource) ||
        !surfaceProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentSource) ||
        !surfaceProgram.link()) {
        qWarning() << "[Waterfall3D] streamed texture renderer unavailable; using legacy fallback"
                   << surfaceProgram.log();
        surfaceProgram.removeAllShaders();
        return false;
    }
    if (!surfaceVbo.isCreated()) {
        surfaceVbo.create();
        surfaceVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    }
    surfaceProgramReady = surfaceVbo.isCreated();
    if (surfaceProgramReady) {
        qDebug() << "[Waterfall3D] streamed texture renderer ready";
    }
    return surfaceProgramReady;
}

void Waterfall3DRenderer::resetGpuSurfaceData() {
    pendingGpuRows.clear();
    gpuGridVertices.clear();
    gpuTextureColumns = 0;
    gpuTextureRows = 0;
    gpuTextureRowCount = 0;
    gpuTextureWriteRow = 0;
    gpuTextureOldestRow = 0;
    gpuTexturesResetRequired = true;
    gpuMeshDirty = true;
}

void Waterfall3DRenderer::rebuildGpuSurface() {
    gpuGridVertices.clear();
    if (!hasData()) {
        gpuMeshDirty = false;
        return;
    }

    const int rowCount = static_cast<int>(historyRows.size());
    const int columnCount = static_cast<int>(historyRows.front().size());
    if (densityAxes && surfaceStyle == 0) {
        gpuGridVertices.reserve(static_cast<std::size_t>(rowCount) *
                                static_cast<std::size_t>(columnCount) * 2U);
        for (int row = 0; row < rowCount; ++row) {
            for (int column = 0; column < columnCount; ++column) {
                GpuGridVertex vertex;
                vertex.row = static_cast<float>(row);
                vertex.column = static_cast<float>(column);
                vertex.densityScale = 0.0f;
                gpuGridVertices.push_back(vertex);
                vertex.densityScale = 1.0f;
                gpuGridVertices.push_back(vertex);
            }
        }
        gpuMeshDirty = false;
        return;
    }
    const std::size_t stripVertices = static_cast<std::size_t>(rowCount - 1) *
                                      static_cast<std::size_t>(columnCount) * 2U;
    const std::size_t degenerateVertices = rowCount > 2
                                               ? static_cast<std::size_t>(rowCount - 2) * 2U
                                               : 0U;
    gpuGridVertices.reserve(stripVertices + degenerateVertices);

    const auto vertexAt = [](int row, int column) {
        GpuGridVertex vertex;
        vertex.row = static_cast<float>(row);
        vertex.column = static_cast<float>(column);
        return vertex;
    };

    for (int row = 0; row < rowCount - 1; ++row) {
        if (row > 0 && !gpuGridVertices.empty()) {
            gpuGridVertices.push_back(gpuGridVertices.back());
            gpuGridVertices.push_back(vertexAt(row, 0));
        }
        for (int column = 0; column < columnCount; ++column) {
            gpuGridVertices.push_back(vertexAt(row, column));
            gpuGridVertices.push_back(vertexAt(row + 1, column));
        }
    }
    gpuMeshDirty = false;
}

bool Waterfall3DRenderer::uploadGpuSurfaceRows() {
    if (!hasData()) {
        return false;
    }
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!context) {
        return false;
    }
    QOpenGLFunctions *functions = context->functions();
    const int columnCount = static_cast<int>(historyRows.front().size());
    const int textureRows = maxHistoryRows;
    int maximumTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
    if (columnCount <= 0 || textureRows <= 0 ||
        columnCount > maximumTextureSize || textureRows > maximumTextureSize) {
        return false;
    }

    if (heightTexture == 0U) {
        glGenTextures(1, &heightTexture);
    }
    if (colorTexture == 0U) {
        glGenTextures(1, &colorTexture);
    }
    if (heightTexture == 0U || colorTexture == 0U) {
        return false;
    }

    const bool resetTextures = gpuTexturesResetRequired ||
                               gpuTextureColumns != columnCount ||
                               gpuTextureRows != textureRows;
    if (resetTextures) {
        functions->glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, heightTexture);
        const GLint textureFilter = surfaceSmoothing > 0 ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, textureFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, textureFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_LUMINANCE,
                     columnCount,
                     textureRows,
                     0,
                     GL_LUMINANCE,
                     GL_UNSIGNED_BYTE,
                     nullptr);

        functions->glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, colorTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, textureFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, textureFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGB,
                     columnCount,
                     textureRows,
                     0,
                     GL_RGB,
                     GL_UNSIGNED_BYTE,
                     nullptr);

        gpuTextureColumns = columnCount;
        gpuTextureRows = textureRows;
        gpuTextureRowCount = 0;
        gpuTextureWriteRow = 0;
        gpuTextureOldestRow = 0;
        pendingGpuRows.assign(historyRows.begin(), historyRows.end());
        gpuTexturesResetRequired = false;
    }

    gpuHeightScratch.resize(static_cast<std::size_t>(columnCount));
    gpuColorScratch.resize(static_cast<std::size_t>(columnCount) * 3U);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (const HistoryRow &row : pendingGpuRows) {
        if (static_cast<int>(row.size()) != columnCount) {
            continue;
        }
        for (int column = 0; column < columnCount; ++column) {
            const VertexSample &sample = row[static_cast<std::size_t>(column)];
            gpuHeightScratch[static_cast<std::size_t>(column)] =
                static_cast<std::uint8_t>(std::lround(std::clamp(sample.height, 0.0f, 1.0f) * 255.0f));
            const std::size_t colorOffset = static_cast<std::size_t>(column) * 3U;
            if (monochrome) {
                const auto color = monochromeAmplitudeColor(
                    sample.height, monochromeColor, monochromeGradientDepth);
                gpuColorScratch[colorOffset] = color[0];
                gpuColorScratch[colorOffset + 1U] = color[1];
                gpuColorScratch[colorOffset + 2U] = color[2];
            } else {
                gpuColorScratch[colorOffset] = sample.color[0];
                gpuColorScratch[colorOffset + 1U] = sample.color[1];
                gpuColorScratch[colorOffset + 2U] = sample.color[2];
            }
        }

        const int destinationRow = gpuTextureWriteRow;
        functions->glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, heightTexture);
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        destinationRow,
                        columnCount,
                        1,
                        GL_LUMINANCE,
                        GL_UNSIGNED_BYTE,
                        gpuHeightScratch.data());
        functions->glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, colorTexture);
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        destinationRow,
                        columnCount,
                        1,
                        GL_RGB,
                        GL_UNSIGNED_BYTE,
                        gpuColorScratch.data());

        gpuTextureWriteRow = (gpuTextureWriteRow + 1) % gpuTextureRows;
        if (gpuTextureRowCount < gpuTextureRows) {
            ++gpuTextureRowCount;
            gpuTextureOldestRow = 0;
        } else {
            gpuTextureOldestRow = gpuTextureWriteRow;
        }
    }
    pendingGpuRows.clear();
    functions->glActiveTexture(GL_TEXTURE0);
    return gpuTextureRowCount == static_cast<int>(historyRows.size());
}

bool Waterfall3DRenderer::renderGpuSurface(const QMatrix4x4 &transform) {
    if (!ensureSurfaceProgram()) {
        return false;
    }
    if (!uploadGpuSurfaceRows()) {
        return false;
    }
    if (gpuMeshDirty) {
        rebuildGpuSurface();
        if (!surfaceVbo.bind()) {
            return false;
        }
        surfaceVbo.allocate(gpuGridVertices.empty() ? nullptr : gpuGridVertices.data(),
                            static_cast<int>(gpuGridVertices.size() * sizeof(GpuGridVertex)));
        surfaceVbo.release();
    }
    if (gpuGridVertices.empty() || !surfaceVbo.bind() || !surfaceProgram.bind()) {
        surfaceVbo.release();
        return false;
    }

    surfaceProgram.setUniformValue("transform", transform);
    surfaceProgram.setUniformValue("heightMap", 0);
    surfaceProgram.setUniformValue("colorMap", 1);
    surfaceProgram.setUniformValue("rowCount", static_cast<float>(historyRows.size()));
    surfaceProgram.setUniformValue("columnCount", static_cast<float>(gpuTextureColumns));
    surfaceProgram.setUniformValue("textureRows", static_cast<float>(gpuTextureRows));
    surfaceProgram.setUniformValue("oldestRow", static_cast<float>(gpuTextureOldestRow));
    surfaceProgram.setUniformValue("densityAxes", densityAxes ? 1.0f : 0.0f);
    surfaceProgram.setUniformValue("densityAxisX", static_cast<float>(densityAxisXDimension));
    surfaceProgram.setUniformValue("densityAxisY", static_cast<float>(densityAxisYDimension));
    surfaceProgram.setUniformValue("densityAxisZ", static_cast<float>(densityAxisZDimension));
    surfaceProgram.setUniformValue("smoothingMode", static_cast<float>(surfaceSmoothing));
    surfaceProgram.setUniformValue("lightingMode", static_cast<float>(surfaceLighting));
    surfaceProgram.setUniformValue("heightGradientMode", heightGradient ? 1.0f : 0.0f);
    surfaceProgram.setUniformValue("monochromeMode", monochrome ? 1.0f : 0.0f);
    surfaceProgram.setUniformValue(
        "monochromeColor",
        QVector3D(monochromeColor.redF(), monochromeColor.greenF(), monochromeColor.blueF()));
    surfaceProgram.setUniformValue(
        "monochromeGradientDepth",
        static_cast<float>(monochromeGradientDepth) / 100.0f);
    const int gridLocation = surfaceProgram.attributeLocation("gridPosition");
    if (gridLocation < 0) {
        surfaceProgram.release();
        surfaceVbo.release();
        return false;
    }
    QOpenGLFunctions *functions = QOpenGLContext::currentContext()->functions();
    functions->glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, heightTexture);
    functions->glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, colorTexture);
    surfaceProgram.enableAttributeArray(gridLocation);
    surfaceProgram.setAttributeBuffer(gridLocation,
                                      GL_FLOAT,
                                      static_cast<int>(offsetof(GpuGridVertex, row)),
                                      3,
                                      static_cast<int>(sizeof(GpuGridVertex)));
    if (densityAxes && surfaceStyle == 0) {
        glLineWidth(1.25f);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(gpuGridVertices.size()));
        glLineWidth(1.0f);
    } else {
        glDrawArrays(GL_TRIANGLE_STRIP, 0, static_cast<GLsizei>(gpuGridVertices.size()));
    }
    surfaceProgram.disableAttributeArray(gridLocation);
    surfaceProgram.release();
    surfaceVbo.release();
    functions->glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    functions->glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void Waterfall3DRenderer::releaseGpuResources() {
    if (surfaceVbo.isCreated()) {
        surfaceVbo.destroy();
    }
    if (heightTexture != 0U) {
        glDeleteTextures(1, &heightTexture);
        heightTexture = 0U;
    }
    if (colorTexture != 0U) {
        glDeleteTextures(1, &colorTexture);
        colorTexture = 0U;
    }
    surfaceProgram.removeAllShaders();
    surfaceProgramReady = false;
    surfaceProgramTried = false;
    resetGpuSurfaceData();
}

Waterfall3DRenderer::ViewParameters Waterfall3DRenderer::viewParameters(
    int viewportWidth,
    int viewportHeight) const {
    ViewParameters parameters;
    const float aspect = static_cast<float>(viewportWidth) /
                         static_cast<float>((std::max)(1, viewportHeight));

    if (fixedFrontPresentation) {
        constexpr float Distance = 3.25f;
        constexpr float Zoom = 1.10f;
        constexpr float TiltDegrees = 58.0f;
        const float verticalTimeScale = fixedFrontExpanded ? 2.50f : 2.0f;
        const float depthTimeScale = fixedFrontExpanded ? 1.20f : 2.0f;
        constexpr float PlotTopRatio = 0.67f;
        constexpr float PlotBottomMarginPixels = 5.0f;
        const float projectionScale = 1.0f / Zoom;
        const float tiltRadians = TiltDegrees * 3.14159265358979323846f / 180.0f;
        const float timeCos = verticalTimeScale * std::cos(tiltRadians);
        const float timeSin = depthTimeScale * std::sin(tiltRadians);
        const float frontDepth = Distance - timeSin;
        const float viewportHeightF = static_cast<float>((std::max)(1, viewportHeight));
        const float bottomY = viewportHeightF - PlotBottomMarginPixels;
        const float topY = viewportHeightF * PlotTopRatio;
        const float bottomNdc = 1.0f - 2.0f * bottomY / viewportHeightF;
        const float topNdc = 1.0f - 2.0f * topY / viewportHeightF;
        const float verticalFrustum = 0.72f * projectionScale;
        const float frequencyScale = 0.995f * frontDepth * verticalFrustum * aspect;
        const float verticalOffset = bottomNdc * frontDepth * verticalFrustum + timeCos;
        const float heightScale = (topNdc - bottomNdc) * frontDepth *
                                  verticalFrustum / 0.72f;

        parameters.projection.frustum(-0.72f * aspect * projectionScale,
                                      0.72f * aspect * projectionScale,
                                      -verticalFrustum,
                                      verticalFrustum,
                                      1.0f,
                                      12.0f);
        parameters.model.setToIdentity();
        parameters.model.setRow(0, QVector4D(0.0f, frequencyScale, 0.0f, 0.0f));
        parameters.model.setRow(1, QVector4D(-timeCos, 0.0f, heightScale, verticalOffset));
        parameters.model.setRow(2, QVector4D(timeSin, 0.0f, 0.0f, -Distance));
        parameters.model.setRow(3, QVector4D(0.0f, 0.0f, 0.0f, 1.0f));
    } else {
        const float projectionScale = 1.0f / cameraZoom;
        parameters.projection.frustum(-0.72f * aspect * projectionScale,
                                      0.72f * aspect * projectionScale,
                                      -0.72f * projectionScale,
                                      0.72f * projectionScale,
                                      1.0f,
                                      12.0f);
        parameters.model.translate(0.0f, -0.08f, -cameraDistance);
        parameters.model.rotate(cameraTiltDegrees, 1.0f, 0.0f, 0.0f);
        parameters.model.rotate(cameraYawDegrees, 0.0f, 0.0f, 1.0f);
        parameters.model.translate(cameraPanX, cameraPanY, 0.0f);
        parameters.model.scale(1.05f, 1.05f, 1.18f);
    }
    parameters.transform = parameters.projection * parameters.model;
    return parameters;
}

void Waterfall3DRenderer::render(int viewportWidth, int viewportHeight) {
    if (viewportWidth <= 0 || viewportHeight <= 0) {
        return;
    }

    glViewport(0, 0, viewportWidth, viewportHeight);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glShadeModel(GL_SMOOTH);

    const ViewParameters parameters = viewParameters(viewportWidth, viewportHeight);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(parameters.projection.constData());

    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(parameters.model.constData());

    glColor3f(0.16f, 0.18f, 0.21f);
    glBegin(GL_LINES);
    for (int line = 0; line <= 8; ++line) {
        const float coordinate = -1.0f + 0.25f * static_cast<float>(line);
        glVertex3f(coordinate, -1.0f, 0.0f);
        glVertex3f(coordinate, 1.0f, 0.0f);
        glVertex3f(-1.0f, coordinate, 0.0f);
        glVertex3f(1.0f, coordinate, 0.0f);
    }
    glEnd();

    if (hasData()) {
        const int rowCount = static_cast<int>(historyRows.size());
        const int columnCount = static_cast<int>(historyRows.front().size());
        int firstColumn = 0;
        int lastColumn = columnCount - 1;
        int firstRow = 0;
        int lastRow = rowCount - 1;
        const bool renderCapturedSpectrum =
            spectrumSliceActive && spectrumSliceCapture && spectrumSliceCaptureFixed &&
            !capturedSpectrumRows.empty();
        if (frequencySliceActive && selectedSliceColumn >= 0) {
            const int halfWidth = sliceWidth / 2;
            firstColumn = (std::max)(0, selectedSliceColumn - halfWidth);
            lastColumn = (std::min)(columnCount - 1,
                                    firstColumn + sliceWidth - 1);
            firstColumn = (std::max)(0, lastColumn - sliceWidth + 1);
        }
        bool spectrumSliceVisible = true;
        if (renderCapturedSpectrum) {
            firstRow = capturedSpectrumFirstRow;
            lastRow = firstRow + static_cast<int>(capturedSpectrumRows.size()) - 1;
            spectrumSliceVisible = firstRow >= 0 && lastRow < rowCount;
        } else if (spectrumSliceActive) {
            const int halfWidth = spectrumSliceWidth / 2;
            const int requestedFirst = selectedSpectrumRow - halfWidth;
            const int requestedLast = requestedFirst + spectrumSliceWidth - 1;
            firstRow = std::max(0, requestedFirst);
            lastRow = std::min(rowCount - 1, requestedLast);
            spectrumSliceVisible = firstRow <= lastRow &&
                                   requestedLast >= 0 &&
                                   requestedFirst < rowCount;
        }

        const auto &rowAt = [&](int row) -> const HistoryRow & {
            if (renderCapturedSpectrum) {
                return capturedSpectrumRows[static_cast<std::size_t>(row - firstRow)];
            }
            return historyRows[static_cast<std::size_t>(row)];
        };

        if (!spectrumSliceVisible) {
            // A captured spectrum row has left the rolling 3D history.
        } else if (!frequencySliceActive && !spectrumSliceActive &&
                   renderGpuSurface(parameters.transform)) {
            // The normal full surface is submitted as one GPU batch.
        } else if (densityAxes && surfaceStyle == 0 &&
                   !frequencySliceActive && !spectrumSliceActive) {
            glLineWidth(1.25f);
            glBegin(GL_LINES);
            for (int row = firstRow; row <= lastRow; ++row) {
                const HistoryRow &densityRow = rowAt(row);
                if (static_cast<int>(densityRow.size()) != columnCount) {
                    continue;
                }
                const float levelRatio = static_cast<float>(row) /
                                         static_cast<float>((std::max)(1, rowCount - 1));
                for (int column = firstColumn; column <= lastColumn; ++column) {
                    const VertexSample &sample = densityRow[static_cast<std::size_t>(column)];
                    if (sample.height <= 0.001f) {
                        continue;
                    }
                    const float frequencyRatio = columnCount > 1
                                                     ? static_cast<float>(column) /
                                                           static_cast<float>(columnCount - 1)
                                                     : 0.5f;
                    const QVector3D base = densityPosition(0.0f, frequencyRatio, levelRatio);
                    const QVector3D tip = densityPosition(sample.height, frequencyRatio, levelRatio);
                    if (heightGradient) applyHeightColor(0.0f);
                    else applySampleColor(sample);
                    glVertex3f(base.x(), base.y(), base.z());
                    if (heightGradient) applyHeightColor(sample.height);
                    glVertex3f(tip.x(), tip.y(), tip.z());
                }
            }
            glEnd();
            glLineWidth(1.0f);
        } else if (frequencySliceActive && firstColumn == lastColumn) {
            glLineWidth(3.0f);
            glBegin(GL_LINE_STRIP);
            for (int row = firstRow; row <= lastRow; ++row) {
                const HistoryRow &historyRow = rowAt(row);
                if (static_cast<int>(historyRow.size()) != columnCount) {
                    continue;
                }
                const float time = -1.0f + 2.0f * static_cast<float>(row) /
                                               static_cast<float>(rowCount - 1);
                const float frequency = columnCount > 1
                                            ? -1.0f + 2.0f * static_cast<float>(firstColumn) /
                                                          static_cast<float>(columnCount - 1)
                                            : 0.0f;
                const VertexSample &sample = historyRow[static_cast<std::size_t>(firstColumn)];
                applySampleColor(sample);
                if (densityAxes) {
                    const QVector3D point = densityPosition(
                        sample.height,
                        columnCount > 1
                            ? static_cast<float>(firstColumn) /
                                  static_cast<float>(columnCount - 1)
                            : 0.5f,
                        rowCount > 1
                            ? static_cast<float>(row) / static_cast<float>(rowCount - 1)
                            : 0.5f);
                    glVertex3f(point.x(), point.y(), point.z());
                } else {
                    glVertex3f(time, frequency, sample.height * 0.72f);
                }
            }
            glEnd();
            glLineWidth(1.0f);
        } else if (spectrumSliceActive && firstRow == lastRow) {
            const HistoryRow &selected = rowAt(firstRow);
            if (static_cast<int>(selected.size()) == columnCount) {
                const float time = -1.0f + 2.0f * static_cast<float>(firstRow) /
                                               static_cast<float>(rowCount - 1);
                glLineWidth(3.0f);
                glBegin(GL_LINE_STRIP);
                for (int column = firstColumn; column <= lastColumn; ++column) {
                    const float frequency = columnCount > 1
                                                ? -1.0f + 2.0f * static_cast<float>(column) /
                                                              static_cast<float>(columnCount - 1)
                                                : 0.0f;
                    const VertexSample &sample = selected[static_cast<std::size_t>(column)];
                    applySampleColor(sample);
                    if (densityAxes) {
                        const QVector3D point = densityPosition(
                            sample.height,
                            columnCount > 1
                                ? static_cast<float>(column) /
                                      static_cast<float>(columnCount - 1)
                                : 0.5f,
                            rowCount > 1
                                ? static_cast<float>(firstRow) /
                                      static_cast<float>(rowCount - 1)
                                : 0.5f);
                        glVertex3f(point.x(), point.y(), point.z());
                    } else {
                        glVertex3f(time, frequency, sample.height * 0.72f);
                    }
                }
                glEnd();
                glLineWidth(1.0f);
            }
        } else for (int row = firstRow; row < lastRow; ++row) {
            const HistoryRow &first = rowAt(row);
            const HistoryRow &second = rowAt(row + 1);
            if (static_cast<int>(first.size()) != columnCount ||
                static_cast<int>(second.size()) != columnCount) {
                continue;
            }

            const float time0 = -1.0f + 2.0f * static_cast<float>(row) /
                                            static_cast<float>(rowCount - 1);
            const float time1 = -1.0f + 2.0f * static_cast<float>(row + 1) /
                                            static_cast<float>(rowCount - 1);
            glBegin(GL_TRIANGLE_STRIP);
            for (int column = firstColumn; column <= lastColumn; ++column) {
                const float frequency = columnCount > 1
                                            ? -1.0f + 2.0f * static_cast<float>(column) /
                                                          static_cast<float>(columnCount - 1)
                                            : 0.0f;
                const VertexSample &sample0 = first[static_cast<std::size_t>(column)];
                const VertexSample &sample1 = second[static_cast<std::size_t>(column)];
                applySampleColor(sample0);
                if (densityAxes) {
                    const float frequencyRatio = columnCount > 1
                                                     ? static_cast<float>(column) /
                                                           static_cast<float>(columnCount - 1)
                                                     : 0.5f;
                    const QVector3D point = densityPosition(
                        sample0.height,
                        frequencyRatio,
                        rowCount > 1
                            ? static_cast<float>(row) / static_cast<float>(rowCount - 1)
                            : 0.5f);
                    glVertex3f(point.x(), point.y(), point.z());
                } else {
                    glVertex3f(time0, frequency, sample0.height * 0.72f);
                }
                applySampleColor(sample1);
                if (densityAxes) {
                    const float frequencyRatio = columnCount > 1
                                                     ? static_cast<float>(column) /
                                                           static_cast<float>(columnCount - 1)
                                                     : 0.5f;
                    const QVector3D point = densityPosition(
                        sample1.height,
                        frequencyRatio,
                        rowCount > 1
                            ? static_cast<float>(row + 1) /
                                  static_cast<float>(rowCount - 1)
                            : 0.5f);
                    glVertex3f(point.x(), point.y(), point.z());
                } else {
                    glVertex3f(time1, frequency, sample1.height * 0.72f);
                }
            }
            glEnd();
        }

        if (fixedFrontPresentation && frontFaceGradient &&
            frontFaceGradientOpacity > 0 && !historyRows.back().empty()) {
            const HistoryRow &frontRow = historyRows.back();
            const unsigned char faceAlpha = static_cast<unsigned char>(
                std::lround(255.0 * static_cast<double>(frontFaceGradientOpacity) / 100.0));
            constexpr float FrontTime = 1.0f;
            constexpr int GradientLayers = 32;

            const auto setGradientColor = [&](float normalizedHeight) {
                const float value = std::clamp(normalizedHeight, 0.0f, 1.0f);
                if (monochrome) {
                    const auto color = monochromeAmplitudeColor(
                        value, monochromeColor, monochromeGradientDepth);
                    glColor4ub(color[0], color[1], color[2], faceAlpha);
                    return;
                }
                const QVector3D color = densityProfileColor(value);
                glColor4ub(static_cast<unsigned char>(std::lround(255.0f * color.x())),
                           static_cast<unsigned char>(std::lround(255.0f * color.y())),
                           static_cast<unsigned char>(std::lround(255.0f * color.z())),
                           faceAlpha);
            };

            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glDisable(GL_DEPTH_TEST);
            glShadeModel(GL_SMOOTH);
            glBegin(GL_TRIANGLES);
            for (int column = 1; column < columnCount; ++column) {
                const float frequency0 = -1.0f + 2.0f * static_cast<float>(column - 1) /
                                                     static_cast<float>((std::max)(1, columnCount - 1));
                const float frequency1 = -1.0f + 2.0f * static_cast<float>(column) /
                                                     static_cast<float>((std::max)(1, columnCount - 1));
                const float height0 = std::clamp(
                    frontRow[static_cast<std::size_t>(column - 1)].height, 0.0f, 1.0f);
                const float height1 = std::clamp(
                    frontRow[static_cast<std::size_t>(column)].height, 0.0f, 1.0f);
                const float segmentMaximum = (std::max)(height0, height1);

                for (int layer = 0; layer < GradientLayers; ++layer) {
                    const float lower = static_cast<float>(layer) /
                                        static_cast<float>(GradientLayers);
                    if (segmentMaximum <= lower) {
                        break;
                    }
                    const float upper = static_cast<float>(layer + 1) /
                                        static_cast<float>(GradientLayers);
                    FrontFacePolygon polygon{{
                        {frequency0, 0.0f},
                        {frequency0, height0},
                        {frequency1, height1},
                        {frequency1, 0.0f}}};
                    FrontFacePolygon clippedLower{};
                    FrontFacePolygon clippedLayer{};
                    int pointCount = clipFrontFacePolygon(
                        polygon, 4, clippedLower, lower, true);
                    pointCount = clipFrontFacePolygon(
                        clippedLower, pointCount, clippedLayer, upper, false);
                    if (pointCount < 3) {
                        continue;
                    }

                    for (int triangle = 1; triangle + 1 < pointCount; ++triangle) {
                        const FrontFacePoint points[3] = {
                            clippedLayer[0],
                            clippedLayer[static_cast<std::size_t>(triangle)],
                            clippedLayer[static_cast<std::size_t>(triangle + 1)]};
                        for (const FrontFacePoint &point : points) {
                            setGradientColor(point.height);
                            glVertex3f(FrontTime, point.frequency, point.height * 0.72f);
                        }
                    }
                }
            }
            glEnd();
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        if (densityAxes && densityFrontProfile.size() >= 2U) {
            glDisable(GL_DEPTH_TEST);
            if (densityFrontProfileStyle == 1) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glBegin(GL_TRIANGLE_STRIP);
                for (std::size_t index = 0; index < densityFrontProfile.size(); ++index) {
                    const float frequencyRatio = static_cast<float>(index) /
                                                 static_cast<float>(densityFrontProfile.size() - 1U);
                    const QVector3D bottom = densityPosition(0.0f, frequencyRatio, 0.0f);
                    const QVector3D top = densityPosition(0.0f,
                                                          frequencyRatio,
                                                          densityFrontProfile[index]);
                    const QVector3D bottomColor = densityProfileColor(0.0f);
                    const QVector3D topColor = densityProfileColor(densityFrontProfile[index]);
                    glColor4f(bottomColor.x(), bottomColor.y(), bottomColor.z(), 0.78f);
                    glVertex3f(bottom.x(), bottom.y(), bottom.z());
                    glColor4f(topColor.x(), topColor.y(), topColor.z(), 0.92f);
                    glVertex3f(top.x(), top.y(), top.z());
                }
                glEnd();
                glDisable(GL_BLEND);
                glLineWidth(2.0f);
                glBegin(GL_LINE_STRIP);
                for (std::size_t index = 0; index < densityFrontProfile.size(); ++index) {
                    const float frequencyRatio = static_cast<float>(index) /
                                                 static_cast<float>(densityFrontProfile.size() - 1U);
                    const QVector3D point = densityPosition(0.0f,
                                                            frequencyRatio,
                                                            densityFrontProfile[index]);
                    const QVector3D color = densityProfileColor(densityFrontProfile[index]);
                    glColor3f(color.x(), color.y(), color.z());
                    glVertex3f(point.x(), point.y(), point.z());
                }
                glEnd();
                glLineWidth(1.0f);
            } else {
                glLineWidth(4.0f);
                glColor3f(0.0f, 0.0f, 0.0f);
                glBegin(GL_LINE_STRIP);
                for (std::size_t index = 0; index < densityFrontProfile.size(); ++index) {
                    const float frequencyRatio = static_cast<float>(index) /
                                                 static_cast<float>(densityFrontProfile.size() - 1U);
                    const QVector3D point = densityPosition(0.0f,
                                                            frequencyRatio,
                                                            densityFrontProfile[index]);
                    glVertex3f(point.x(), point.y(), point.z());
                }
                glEnd();
                glLineWidth(2.0f);
                glColor3f(0.48f, 1.0f, 0.62f);
                glBegin(GL_LINE_STRIP);
                for (std::size_t index = 0; index < densityFrontProfile.size(); ++index) {
                    const float frequencyRatio = static_cast<float>(index) /
                                                 static_cast<float>(densityFrontProfile.size() - 1U);
                    const QVector3D point = densityPosition(0.0f,
                                                            frequencyRatio,
                                                            densityFrontProfile[index]);
                    glVertex3f(point.x(), point.y(), point.z());
                }
                glEnd();
                glLineWidth(1.0f);
            }
            glEnable(GL_DEPTH_TEST);
        }

        if (highlightedHistoryRow >= 0 && highlightedHistoryRow < rowCount) {
            const HistoryRow &highlighted = historyRows[static_cast<std::size_t>(highlightedHistoryRow)];
            if (static_cast<int>(highlighted.size()) == columnCount) {
                const float time = -1.0f + 2.0f * static_cast<float>(highlightedHistoryRow) /
                                               static_cast<float>(rowCount - 1);
                glLineWidth(2.0f);
                glColor3f(1.0f, 0.25f, 0.18f);
                glBegin(GL_LINE_STRIP);
                for (int column = 0; column < columnCount; ++column) {
                    const float frequency = columnCount > 1
                                                ? -1.0f + 2.0f * static_cast<float>(column) /
                                                              static_cast<float>(columnCount - 1)
                                                : 0.0f;
                    const VertexSample &sample = highlighted[static_cast<std::size_t>(column)];
                    glVertex3f(time, frequency, sample.height * 0.72f + 0.012f);
                }
                glEnd();
                glLineWidth(1.0f);
            }
        }
    }

    glDisable(GL_DEPTH_TEST);
}
