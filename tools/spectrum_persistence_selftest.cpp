#include "spectrumpersistence.h"
#include <QCoreApplication>
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
    std::cout << "Spectrum persistence: " << failures << " failures\n";
    return failures ? 1 : 0;
}
