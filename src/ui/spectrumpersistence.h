#pragma once
#include <QImage>
#include <QColor>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <limits>

// Frequency-by-amplitude occupancy, updated once per incoming display frame.
// No timers or GUI objects: pausing a view freezes its accumulation.
class SpectrumPersistence {
public:
    static constexpr int MaxColumns = 1536;
    static constexpr int Rows = 256;
    void clear() { density_.clear(); smoothedLevels_.clear(); image_ = QImage(); }
    const QImage &image() const { return image_; }
    void setParameters(double seconds, int intensityPercent, int thresholdPercent = 0, int smoothingMs = 0) {
        decaySeconds_ = std::isfinite(seconds) ? (std::clamp)(seconds, 0.1, 30.0) : 1.6;
        intensity_ = (std::clamp)(intensityPercent, 10, 400) / 100.0f;
        threshold_ = (std::clamp)(thresholdPercent, 0, 90) / 100.0f;
        const double smoothing = (std::clamp)(smoothingMs, 0, 2000) / 1000.0;
        if (smoothingSeconds_ != smoothing) smoothedLevels_.clear();
        smoothingSeconds_ = smoothing;
        render();
    }
    void append(const std::vector<float> &levels, double elapsed, bool preserveGaps) {
        const int columns = (std::min)(MaxColumns, static_cast<int>(levels.size()));
        constexpr int rows = Rows;
        if (columns < 1) { clear(); return; }
        if (image_.size() != QSize(columns, rows)) {
            image_ = QImage(columns, rows, QImage::Format_RGB32);
            density_.assign(static_cast<std::size_t>(columns * rows), 0.0f);
            smoothedLevels_.clear();
        }
        elapsed = std::isfinite(elapsed) ? (std::max)(0.0, elapsed) : 0.0;
        const float decay = static_cast<float>(std::exp(-elapsed / decaySeconds_));
        for (float &density : density_) density *= decay;
        if (smoothedLevels_.size() != static_cast<std::size_t>(columns)) {
            smoothedLevels_.assign(static_cast<std::size_t>(columns), std::numeric_limits<float>::quiet_NaN());
        }
        const float alpha = smoothingSeconds_ > 0.0
            ? static_cast<float>(-std::expm1(-elapsed / smoothingSeconds_)) : 1.0f;
        // Join adjacent populated columns, retaining gaps in mosaic/scan input.
        int previousX = -1, previousY = 0;
        const float contribution = static_cast<float>(1.0 - std::exp(-(std::min)(elapsed, 0.1) / 0.22));
        for (int x = 0; x < columns; ++x) {
            if (!std::isfinite(levels[x])) {
                smoothedLevels_[x] = std::numeric_limits<float>::quiet_NaN();
                continue;
            }
            const float incoming = (std::clamp)(levels[x], 0.0f, 1.0f);
            float &smoothed = smoothedLevels_[x];
            smoothed = std::isfinite(smoothed) ? smoothed + alpha * (incoming - smoothed) : incoming;
            const int y = (std::clamp)(qRound((1.0f - smoothed) * (rows - 1)), 0, rows - 1);
            const bool join = previousX >= 0 && (!preserveGaps || x == previousX + 1);
            const int start = join ? previousX + 1 : x;
            for (int column = start; column <= x; ++column) {
                const int targetY = join ? qRound(previousY + (y - previousY) * double(column - previousX) / (x - previousX)) : y;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int row = (std::clamp)(targetY + dy, 0, rows - 1);
                    float &density = density_[static_cast<std::size_t>(row * columns + column)];
                    density = (std::min)(1.0f, density + contribution * (dy == 0 ? 1.0f : 0.35f));
                }
            }
            previousX = x;
            previousY = y;
        }
        render();
    }
private:
    void render() {
        if (image_.isNull()) return;
        const int columns = image_.width();
        // Rare traces are cyan; repeated occupancy advances through green/yellow/red.
        static const auto palette = [] {
            std::array<QRgb, 256> colors{};
            for (int i = 0; i < 256; ++i) {
                const float t = i / 255.0f;
                colors[i] = QColor::fromHsvF((1.0 - t) * 0.54, 1.0, (std::min)(1.0f, std::sqrt(t) * 2.4f)).rgb();
            }
            return colors;
        }();
        for (int y = 0; y < Rows; ++y) {
            auto *pixels = reinterpret_cast<QRgb *>(image_.scanLine(y));
            for (int x = 0; x < columns; ++x) {
                const float d = density_[static_cast<std::size_t>(y * columns + x)];
                // Gate raw occupancy before display gain: brightness cannot restore rejected clutter.
                const float visible = (std::max)(0.0f, d - threshold_) / (1.0f - threshold_);
                const float t = (std::clamp)(visible * intensity_, 0.0f, 1.0f);
                pixels[x] = palette[qRound(t * 255.0f)];
            }
        }
    }
    double decaySeconds_ = 1.6;
    float intensity_ = 1.0f;
    float threshold_ = 0.0f;
    double smoothingSeconds_ = 0.0;
    std::vector<float> smoothedLevels_;
    std::vector<float> density_;
    QImage image_;
};
