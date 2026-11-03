#include "spectrumpersistence.h"
#include <QCoreApplication>
#include <QPainter>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        if (!ok) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
    };
    const float gap = std::numeric_limits<float>::quiet_NaN();
    SpectrumPersistence density;
    density.append({0.5f, gap, 0.5f}, 0.04, true);
    check(density.image().pixelColor(1, 128) == QColor(Qt::black), "frequency gaps remain empty");
    const QColor rare = density.image().pixelColor(0, 128);
    for (int i = 0; i < 120; ++i) density.append({0.5f, gap, 0.5f}, 0.04, true);
    const QColor common = density.image().pixelColor(0, 128);
    check(common.red() > rare.red() && common.red() > common.blue(), "frequent occupancy becomes warm");
    density.append({gap, gap, gap}, 60.0, true);
    check(density.image().pixelColor(0, 128) == QColor(Qt::black), "old traces decay after long interruption");
    density.clear();
    check(density.image().isNull(), "clear discards old frequency domain");
    density.append(std::vector<float>(100000, 0.5f), 0.04, false);
    check(density.image().width() == SpectrumPersistence::MaxColumns, "large inputs have bounded storage");
    SpectrumPersistence sparse, fast;
    sparse.append({0.5f}, 0.04, false);
    for (int i = 0; i < 4; ++i) fast.append({0.5f}, 0.01, false);
    check(std::abs(sparse.image().pixelColor(0, 128).green() - fast.image().pixelColor(0, 128).green()) < 12,
          "similar occupancy across frame rates");
    SpectrumPersistence shortTrail, longTrail;
    shortTrail.setParameters(0.1, 100);
    longTrail.setParameters(30.0, 100);
    shortTrail.append({0.5f}, 0.04, false);
    longTrail.append({0.5f}, 0.04, false);
    shortTrail.append({gap}, 2.0, false);
    longTrail.append({gap}, 2.0, false);
    check(shortTrail.image().pixelColor(0, 128) == QColor(Qt::black) &&
          longTrail.image().pixelColor(0, 128) != QColor(Qt::black), "adjustable decay changes trace lifetime");
    SpectrumPersistence intensity;
    intensity.append({0.5f}, 0.04, false);
    const QRgb normal = intensity.image().pixel(0, 128);
    intensity.setParameters(1.6, 10);
    check(intensity.image().pixelColor(0, 128).value() < QColor(normal).value(), "intensity updates existing raster immediately");
    intensity.setParameters(1.6, 100);
    check(intensity.image().pixel(0, 128) == normal, "intensity preserves accumulated history");
    intensity.setParameters(std::numeric_limits<double>::quiet_NaN(), -999);
    intensity.append({0.5f}, 0.04, false);
    check(!intensity.image().isNull(), "invalid parameters remain safe");
    SpectrumPersistence clutter;
    clutter.append({0.25f}, 0.04, false);
    const QImage unfiltered = clutter.image().copy();
    clutter.setParameters(1.6, 400, 25);
    check(clutter.image().pixelColor(0, 191) == QColor(Qt::black), "rare point rejected before brightness gain");
    clutter.setParameters(1.6, 100, 0);
    check(clutter.image() == unfiltered, "lowering threshold restores history without new frames");
    clutter.setParameters(1.6, 100, 25);
    for (int i = 0; i < 30; ++i) clutter.append({0.75f}, 0.04, false);
    check(clutter.image().pixelColor(0, 64) != QColor(Qt::black) &&
          clutter.image().pixelColor(0, 191) == QColor(Qt::black), "persistent signal survives while isolated amplitude spike disappears");
    clutter.setParameters(1.6, 100, 999);
    check(!clutter.image().isNull(), "threshold safely clamps extreme settings");
    SpectrumPersistence smoothing;
    smoothing.setParameters(1.6, 100, 0, 200);
    smoothing.append({0.2f}, 0.04, false);
    smoothing.append({0.8f}, 0.04, false);
    check(smoothing.image().pixelColor(0, 176) != QColor(Qt::black) &&
          smoothing.image().pixelColor(0, 51) == QColor(Qt::black), "smoothing reduces amplitude jump before accumulation");
    smoothing.clear();
    smoothing.append({0.8f}, 0.04, false);
    check(smoothing.image().pixelColor(0, 51) != QColor(Qt::black), "clearing domain resets smoothing without startup ramp");
    smoothing.append({gap}, 0.04, false);
    smoothing.append({0.2f}, 0.04, false);
    check(smoothing.image().pixelColor(0, 204) != QColor(Qt::black), "missing frequency resets its filter");
    SpectrumPersistence slowFps, highFps;
    slowFps.setParameters(1.6, 100, 0, 200);
    highFps.setParameters(1.6, 100, 0, 200);
    slowFps.append({0.2f}, 0.04, false);
    highFps.append({0.2f}, 0.04, false);
    slowFps.append({0.8f}, 0.04, false);
    for (int i = 0; i < 4; ++i) highFps.append({0.8f}, 0.01, false);
    check(slowFps.image().pixelColor(0, 176) != QColor(Qt::black) &&
          highFps.image().pixelColor(0, 176) != QColor(Qt::black), "smoothing response agrees across frame rates");
    smoothing.setParameters(1.6, 100, 0, 0);
    smoothing.append({0.9f}, 0.04, false);
    check(smoothing.image().pixelColor(0, 26) != QColor(Qt::black), "zero smoothing restores immediate response");

    using Style = SpectrumPersistence::Style;
    check(density.style() == Style::Classic, "classic phosphor remains the default");
    const auto columnEmpty = [](const QImage &image, int x) {
        for (int y = 0; y < image.height(); ++y)
            if (image.pixel(x, y) != qRgb(0, 0, 0)) return false;
        return true;
    };
    const auto colorsClose = [](const QColor &left, const QColor &right, int tolerance) {
        return std::abs(left.red() - right.red()) <= tolerance &&
               std::abs(left.green() - right.green()) <= tolerance &&
               std::abs(left.blue() - right.blue()) <= tolerance;
    };
    // Independent one-column reference for fractional K, Gaussian subtraction and log tone.
    const auto contourReference = [](const std::array<float, SpectrumPersistence::Rows> &histogram, int row) {
        double background = 0.0, weightSum = 0.0;
        for (int offset = -6; offset <= 6; ++offset) {
            const double weight = std::exp(-offset * offset / 8.0);
            background += weight * histogram[(std::clamp)(row + offset, 0, SpectrumPersistence::Rows - 1)];
            weightSum += weight;
        }
        const double h = histogram[row];
        const double ridge = (std::max)(0.0, h + h - background / weightSum);
        const int index = (std::clamp)(qRound(ridge * 1024.0), 0, 4095);
        const float tone = (std::clamp)(static_cast<float>(std::log1p(40.0 * index / 1024.0) /
                                                          std::log1p(40.0)), 0.0f, 1.0f);
        return QColor::fromHsvF((1.0 - tone) * 0.54, 1.0,
                               (std::min)(1.0f, std::sqrt(tone) * 2.4f));
    };
    SpectrumPersistence fractional;
    fractional.setStyle(Style::Contours);
    fractional.append({0.5f, gap, 0.0f, 1.0f}, 0.08, true);
    const float mass = static_cast<float>(-std::expm1(-0.08 / 1.6));
    std::array<float, SpectrumPersistence::Rows> reference{};
    reference[127] = reference[128] = mass * 0.5f;
    check(colorsClose(fractional.image().pixelColor(0, 127), contourReference(reference, 127), 2) &&
          colorsClose(fractional.image().pixelColor(0, 128), contourReference(reference, 128), 2),
          "contours implements normalized fractional occupancy and vertical log rendering");
    bool narrow = true;
    for (int y = 0; y < SpectrumPersistence::Rows; ++y) {
        if (y != 127 && y != 128 && fractional.image().pixel(0, y) != qRgb(0, 0, 0)) narrow = false;
    }
    check(narrow && columnEmpty(fractional.image(), 1), "contours has no broad brush, Gaussian halo or frequency bleed");
    reference.fill(0.0f);
    reference[255] = mass;
    check(colorsClose(fractional.image().pixelColor(2, 255), contourReference(reference, 255), 2),
          "contours clamps Gaussian amplitude boundaries without wrapping");
    reference.fill(0.0f);
    reference[0] = mass;
    check(colorsClose(fractional.image().pixelColor(3, 0), contourReference(reference, 0), 2),
          "contours retains full deposit at top amplitude boundary");
    SpectrumPersistence isolatedColumn, adjacentColumns;
    isolatedColumn.setStyle(Style::Contours);
    adjacentColumns.setStyle(Style::Contours);
    isolatedColumn.append({0.5f}, 0.04, false);
    adjacentColumns.append({0.9f, 0.5f, 0.1f}, 0.04, false);
    bool independent = true;
    for (int y = 0; y < SpectrumPersistence::Rows; ++y)
        if (isolatedColumn.image().pixel(0, y) != adjacentColumns.image().pixel(1, y)) independent = false;
    check(independent, "neighboring populated frequencies never change a contour column");
    SpectrumPersistence contourLifetime;
    contourLifetime.setStyle(Style::Contours);
    contourLifetime.setParameters(0.1, 100);
    contourLifetime.append({0.5f}, 0.08, false);
    reference.fill(0.0f);
    reference[127] = reference[128] = static_cast<float>(-std::expm1(-0.08 / 0.1) * 0.5);
    check(colorsClose(contourLifetime.image().pixelColor(0, 128), contourReference(reference, 128), 2),
          "contour injection uses the selected afterglow, not a fixed contribution time");

    SpectrumPersistence modes, unsmoothed;
    modes.setStyle(Style::Contours);
    modes.setParameters(1.6, 100, 0, 2000);
    unsmoothed.setStyle(Style::Contours);
    for (int i = 0; i < 600; ++i) {
        const std::vector<float> levels{i % 2 ? 0.7f : 0.3f};
        modes.append(levels, 0.04, true);
        unsmoothed.append(levels, 0.04, true);
    }
    check(modes.image() == unsmoothed.image(), "contours bypasses classic amplitude smoothing");
    check(modes.image().pixelColor(0, 76) != QColor(Qt::black) &&
          modes.image().pixelColor(0, 179) != QColor(Qt::black) &&
          modes.image().pixelColor(0, 128) == QColor(Qt::black),
          "alternating raw amplitude states remain separate with no invented midpoint");

    SpectrumPersistence contourSlow, contourFast;
    contourSlow.setStyle(Style::Contours);
    contourFast.setStyle(Style::Contours);
    contourSlow.append({0.5f}, 0.4, false);
    for (int i = 0; i < 40; ++i) contourFast.append({0.5f}, 0.01, false);
    check(contourSlow.image() == contourFast.image(), "contour occupancy is invariant to frame partitioning");
    contourSlow.append({gap}, 0.4, false);
    for (int i = 0; i < 40; ++i) contourFast.append({gap}, 0.01, false);
    check(contourSlow.image() == contourFast.image(), "contour decay is invariant to frame partitioning");
    reference.fill(0.0f);
    reference[127] = reference[128] = static_cast<float>((1.0 - std::exp(-0.4 / 1.6)) * std::exp(-0.4 / 1.6) * 0.5);
    check(colorsClose(contourSlow.image().pixelColor(0, 128), contourReference(reference, 128), 2),
          "contour history follows exponential decay with the selected T");

    const QImage contourHistory = modes.image().copy();
    modes.setParameters(1.6, 10, 0, 2000);
    check(modes.image() != contourHistory, "contour intensity redraws existing history");
    modes.setParameters(1.6, 100, 90, 2000);
    check(columnEmpty(modes.image(), 0), "contour threshold gates raw occupancy before contrast and gain");
    modes.setParameters(1.6, 100, 0, 2000);
    check(modes.image() == contourHistory, "contour threshold and intensity do not destroy history");
    modes.setParameters(1.6, 100, 0, 0);
    check(modes.image() == contourHistory, "changing unused classic smoothing does not alter contour history");
    modes.setStyle(Style::Contours);
    check(modes.image() == contourHistory, "selecting the current style preserves history");
    for (const double elapsed : {0.0, -0.04, std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity()}) {
        modes.append({0.5f}, elapsed, false);
        check(modes.image() == contourHistory, "invalid or zero contour dt performs no work");
    }
    modes.append({gap}, 60.0, false);
    check(columnEmpty(modes.image(), 0), "contour traces decay after a long interruption");
    modes.append({0.5f}, 0.04, true);
    modes.append({gap}, 0.04, true);
    check(columnEmpty(modes.image(), 0), "an explicit mosaic gap clears previous contour occupancy");
    modes.append({0.5f}, 0.04, true);
    modes.setStyle(Style::Classic);
    check(modes.image().isNull(), "changing phosphor style clears incompatible histogram units");
    modes.setParameters(1.6, 100, 0, 200);
    modes.append({0.2f}, 0.04, false);
    modes.append({0.8f}, 0.04, false);
    check(modes.image().pixelColor(0, 176) != QColor(Qt::black) &&
          modes.image().pixelColor(0, 51) == QColor(Qt::black), "returning to classic retains its amplitude smoothing behavior");
    modes.setStyle(Style::Contours);
    modes.append(std::vector<float>(100000, 0.5f), 0.04, true);
    check(modes.image().width() == SpectrumPersistence::MaxColumns, "contour raster storage is bounded");
    modes.append({0.5f}, 0.04, true);
    check(modes.image().width() == 1, "changing contour raster width resets the domain");
    modes.clear();
    check(modes.image().isNull() && modes.style() == Style::Contours, "clear releases contour storage without changing style");
    modes.append({}, 0.04, true);
    check(modes.image().isNull(), "empty contour input remains empty");
    if (argc > 1) {
        SpectrumPersistence preview;
        std::mt19937 rng(1000);
        std::uniform_real_distribution<float> random(0.0f, 1.0f);
        std::vector<float> levels(1000);
        for (int frame = 0; frame < 240; ++frame) {
            for (int x = 0; x < 1000; ++x) {
                float level = 0.12f + 0.09f * random(rng);
                for (const int peak : {140, 310, 425, 610, 780, 870}) {
                    const float delta = (x - peak) / 6.0f;
                    level += (0.3f + 0.45f * random(rng)) * std::exp(-delta * delta);
                }
                levels[x] = level;
            }
            preview.append(levels, 0.04, false);
        }
        check(preview.image().save(QString::fromLocal8Bit(argv[1])), "save synthetic preview");
    }
    if (argc > 2) {
        // Optional raster smoke fixture: same raw frames, Classic (top), Contours (bottom).
        // No fonts or GUI widgets, so the selftest keeps its QCoreApplication contract.
        SpectrumPersistence classicPreview, contoursPreview;
        classicPreview.setParameters(1.6, 100, 0, 200);
        contoursPreview.setStyle(Style::Contours);
        contoursPreview.setParameters(1.6, 100, 0, 200);
        constexpr int columns = 900;
        std::vector<float> levels(columns);
        std::mt19937 rng(100);
        std::normal_distribution<float> jitter(0.0f, 0.48f);
        for (int frame = 0; frame < 400; ++frame) {
            for (int x = 0; x < columns; ++x) {
                const double f = -100.0 + 200.0 * x / (columns - 1);
                const auto gaussian = [f](double center, double width) {
                    const double distance = (f - center) / width;
                    return std::exp(-0.5 * distance * distance);
                };
                const double db = -107.0 + (frame % 2 ? 51.0 * gaussian(-12.0, 16.0)
                                                                      : 40.0 * gaussian(1.0, 24.0)) +
                                  24.0 * gaussian(65.0, 5.0) + jitter(rng);
                levels[x] = (std::clamp)(static_cast<float>((db + 120.0) / 90.0), 0.0f, 1.0f);
            }
            for (int x = 700; x < 712; ++x) levels[x] = gap;
            classicPreview.append(levels, 0.04, true);
            contoursPreview.append(levels, 0.04, true);
        }
        QImage comparison(columns, SpectrumPersistence::Rows * 2 + 8, QImage::Format_RGB32);
        comparison.fill(QColor(32, 32, 32));
        QPainter painter(&comparison);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(0, 0, classicPreview.image());
        painter.drawImage(0, SpectrumPersistence::Rows + 8, contoursPreview.image());
        painter.end();
        check(comparison.save(QString::fromLocal8Bit(argv[2])), "save optional Classic / Contours comparison fixture");
        if (argc > 3)
            check(contoursPreview.image().save(QString::fromLocal8Bit(argv[3])), "save optional Contours raster fixture");
    }
    std::cout << "Spectrum persistence: " << failures << " failures\n";
    return failures ? 1 : 0;
}
