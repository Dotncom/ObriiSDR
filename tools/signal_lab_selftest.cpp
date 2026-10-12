#include "signallabgenerator.h"
#include "signallabpreview.h"
#include "signallabconstellation3d.h"
#include "iqconstellationpreview.h"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>
#include <limits>
#include <random>
#include <stdexcept>

// The standalone test does not link the live receiver FFT processor.
std::mutex &fftwPlannerGlobalMutex() {
    static std::mutex mutex;
    return mutex;
}

struct SignalLabConstellation3DTestAccess {
    static int points(const SignalLabConstellation3DWidget &widget) {
        int count = 0;
        for (const auto &primitive : widget.scene_) if (primitive.point) ++count;
        return count;
    }
    static int lines(const SignalLabConstellation3DWidget &widget) {
        int count = 0;
        for (const auto &primitive : widget.scene_) if (!primitive.point && !primitive.surface) ++count;
        return count;
    }
    static int triangles(const SignalLabConstellation3DWidget &widget) {
        int count = 0;
        for (const auto &primitive : widget.scene_) if (primitive.surface) ++count;
        return count;
    }
    static bool frameBehindSurface(const SignalLabConstellation3DWidget &widget) {
        bool sawData = false;
        for (const auto &primitive : widget.scene_) {
            if (primitive.frame && sawData) return false;
            if (!primitive.frame) sawData = true;
        }
        return sawData;
    }
};

namespace {
constexpr double pi = 3.14159265358979323846;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
std::complex<double> referencePoint(SignalLabWaveform waveform, std::mt19937 &random) {
    if (waveform == SignalLabWaveform::Bpsk) return {(random() & 1U) ? 1.0 : -1.0, 0.0};
    if (waveform == SignalLabWaveform::Qpsk) {
        const int index = random() & 3U;
        return std::polar(1.0, pi/4 + index*pi/2);
    }
    if (waveform == SignalLabWaveform::Psk8)
        return std::polar(1.0, pi/8 + (random() & 7U)*pi/4);
    const int side = waveform == SignalLabWaveform::Qam16 ? 4 :
                     waveform == SignalLabWaveform::Qam64 ? 8 : 16;
    const int i = int(random() % unsigned(side));
    const int q = int(random() % unsigned(side));
    return std::complex<double>(2*i-side+1, 2*q-side+1) / std::sqrt(2.0*(side*side-1)/3.0);
}
void testSymbols() {
    for (auto waveform : {SignalLabWaveform::Bpsk, SignalLabWaveform::Qpsk,
                          SignalLabWaveform::Psk8, SignalLabWaveform::Pi4Dqpsk, SignalLabWaveform::Qam16,
                          SignalLabWaveform::Qam64, SignalLabWaveform::Qam256}) {
        for (int rate : {48000, 192000}) {
            SignalLabConfiguration config;
            config.waveform = waveform;
            config.sampleRate = rate;
            config.durationSeconds = 0.25;
            config.symbolRate = 1200;
            config.amplitude = 0.8;
            config.centerOffsetHz = 6000;
            auto result = SignalLabGenerator::generate(config);
            require(result.ok(), "QAM/PSK generation failed");
            require(std::abs(result.peak - config.amplitude) < 1e-5, "Generated peak normalization changed");
            require(result.samplesPerSymbol == rate/1200 &&
                    std::abs(result.occupiedBandwidthHz - 1620) < 1e-6, "Actual symbol rate/BW is inconsistent");
            const auto decoded = SignalLabGenerator::matchedSymbols(result);
            require(decoded.size() == 292, "Matched symbol count/timing changed");
            std::mt19937 random(config.randomSeed);
            std::vector<std::complex<double>> reference(312);
            std::complex<double> phase(1, 0);
            for (auto &symbol : reference) {
                if (waveform == SignalLabWaveform::Pi4Dqpsk) {
                    const double steps[] = {pi/4, 3*pi/4, -3*pi/4, -pi/4};
                    phase *= std::polar(1.0, steps[random() & 3U]);
                    symbol = phase;
                } else symbol = referencePoint(waveform, random);
            }
            double error = 0, power = 0;
            for (int index = 0; index < decoded.size(); ++index) {
                const auto expected = reference[std::size_t(index+8)];
                const auto actual = std::complex<double>(decoded[index].real(), decoded[index].imag()) /
                                    result.normalizationGain;
                error += std::norm(actual - expected);
                power += std::norm(expected);
            }
            const double evm = std::sqrt(error/power);
            std::cout << SignalLabGenerator::waveformName(waveform).toStdString()
                      << " Fs=" << rate << " matched EVM=" << evm*100 << "%\n";
            require(evm < 0.025, "RRC output is not a linear, decodable symbol waveform");
        }
    }
    SignalLabConfiguration shortConfig;
    shortConfig.waveform = SignalLabWaveform::Qam16;
    shortConfig.durationSeconds = 0.02;
    auto shortResult = SignalLabGenerator::generate(shortConfig);
    require(shortResult.ok() && SignalLabGenerator::matchedSymbols(shortResult).size() == 16,
            "20 ms QAM preview has incorrect symbol timing");
    shortConfig.durationSeconds = 0.001;
    require(SignalLabGenerator::matchedSymbols(SignalLabGenerator::generate(shortConfig)).isEmpty(),
            "Too-short QAM preview invented complete decision symbols");
    shortConfig.symbolRate = 0.000001;
    require(!SignalLabGenerator::generate(shortConfig).ok(), "Sub-Hz symbol rate bypassed the sample bound");
    SignalLabConfiguration giga;
    giga.waveform = SignalLabWaveform::Qam16;
    giga.sampleRate = 1000000000;
    giga.symbolRate = 100000000;
    giga.durationSeconds = 0.00005;
    const auto highRate = SignalLabGenerator::generate(giga);
    require(highRate.ok() && SignalLabGenerator::matchedSymbols(highRate, 7).size() <= 7,
            "High-rate symbol preview exceeded its point budget");
    giga.symbolRate = 1;
    require(SignalLabGenerator::matchedSymbols(SignalLabGenerator::generate(giga)).isEmpty(),
            "Huge samples-per-symbol request bypassed the preview memory/work bounds");
}
void testSpectralPreview() {
    for (double offset : {0.0, -3000.0, 3000.0}) {
        SignalLabConfiguration config;
        config.waveform = SignalLabWaveform::Carrier;
        config.amplitude = 0.8;
        config.durationSeconds = 0.02;
        config.centerOffsetHz = offset;
        const auto result = SignalLabGenerator::generate(config);
        const auto preview = buildSignalLabSpectralPreview(result, 16384, 160);
        require(preview.ok() && preview.windowSamples == 960 && preview.rowCount == 1,
                "Short spectral preview FFT padding/window is wrong");
        const auto &levels = preview.levels.front();
        const auto peak = std::max_element(levels.begin(), levels.end());
        const auto bin = std::distance(levels.begin(), peak);
        const double frequency = (double(bin) - 8192)*48000/16384;
        require(std::abs(frequency - offset) < 3.0, "FFT shift or frequency sign is incorrect");
        require(std::abs(*peak - 20*std::log10(0.8)) < 0.01,
                "Zero-padded short signal is spuriously attenuated");
        require(std::abs(preview.rbwHz - 1.5*48000/959) < 0.01,
                "Zero padding falsely improved the actual RBW");
    }
}
void renderPreview(const QString &path) {
    SignalLabConfiguration config;
    config.waveform = SignalLabWaveform::Qam16;
    config.durationSeconds = 0.2;
    config.amplitude = 0.8;
    SignalLabPreviewWidget widget;
    widget.setLanguage(QStringLiteral("uk"));
    widget.resize(700, 520);
    widget.setResult(SignalLabGenerator::generate(config));
    QImage image(widget.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    widget.render(&image);
    require(image.save(path), "Unable to save rendered Signal Lab preview");
}

QImage render3D(SignalLabConstellation3DWidget &widget) {
    QImage image(widget.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    widget.render(&image);
    int colored = 0;
    for (int y = 32; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto color = image.pixelColor(x, y);
            if (color.green() > 70 && std::abs(color.red()-color.blue()) > 25) ++colored;
        }
    }
    require(colored > 200, "3D constellation rendered blank or without its visual data");
    return image;
}

void testConstellation3D(const QString &path) {
    SignalLabConfiguration config;
    config.waveform = SignalLabWaveform::Qam16;
    config.amplitude = 0.8;
    config.durationSeconds = 0.02;
    const auto result = SignalLabGenerator::generate(config);
    const auto data = buildSignalLabIqPreview(result);
    require(data.samples.size() == 960 && data.samples.size() == data.sampleTimes.size() &&
            data.symbols.size() == 16 && data.symbols.size() == data.symbolTimes.size() &&
            data.continuousTrajectory, "Shared IQ preview data/counts are inconsistent");
    for (int index = 0; index < data.samples.size(); ++index)
        require(data.samples[index] == result.iq[index] &&
                data.sampleTimes[index] == double(index)/config.sampleRate,
                "3D sample values or timestamps changed");
    for (int index = 0; index < data.symbolTimes.size(); ++index)
        require(data.symbolTimes[index] == double((index+4)*result.samplesPerSymbol)/config.sampleRate,
                "3D matched symbol has an incorrect time coordinate");
    SignalLabConstellation3DWidget widget;
    widget.setLanguage(QStringLiteral("uk"));
    widget.resize(800, 560);
    widget.setData(data);
    const QImage original = render3D(widget);
    if (!path.isEmpty()) require(original.save(path), "Unable to save 3D preview screenshot");
    require(SignalLabConstellation3DTestAccess::points(widget) == 976 &&
            SignalLabConstellation3DTestAccess::lines(widget) == 23+959,
            "3D preview lost sample/symbol points or joined incorrect samples");
    const auto defaultCamera = widget.cameraState();
    const auto drag = [&](Qt::MouseButton button, QPoint from, QPoint to) {
        QMouseEvent press(QEvent::MouseButtonPress, from, button, button, Qt::ControlModifier);
        QApplication::sendEvent(&widget, &press);
        QMouseEvent move(QEvent::MouseMove, to, Qt::NoButton, button, Qt::ControlModifier);
        QApplication::sendEvent(&widget, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, to, button, Qt::NoButton, Qt::ControlModifier);
        QApplication::sendEvent(&widget, &release);
    };
    drag(Qt::LeftButton, {200,180}, {310,230});
    const auto rotatedImage = render3D(widget);
    require(widget.cameraState() != defaultCamera && rotatedImage != original,
            "Ctrl + left drag failed to rotate the 3D constellation");
    if (!path.isEmpty()) require(rotatedImage.save(path+QStringLiteral(".rotated.png")), "Unable to save rotated 3D preview");
    const auto rotated = widget.cameraState();
    require(std::abs(rotated.value("yaw").toDouble()-defaultCamera.value("yaw").toDouble()+49.5) < 1e-5 &&
            std::abs(rotated.value("pitch").toDouble()-defaultCamera.value("pitch").toDouble()-22.5) < 1e-5,
            "3D orbit horizontal or vertical drag direction is incorrect");
    SignalLabConstellation3DWidget restored;
    restored.setCameraState(rotated);
    require(restored.cameraState() == rotated, "3D camera does not round-trip through settings");
    drag(Qt::RightButton, {200,180}, {240,160});
    require(widget.cameraState().value("panX") != rotated.value("panX"), "Ctrl + right drag failed to pan");
    QWheelEvent wheel({200,180}, {200,180}, {}, {0,120}, Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    const double distance = widget.cameraState().value("distance").toDouble();
    QApplication::sendEvent(&widget, &wheel);
    require(widget.cameraState().value("distance").toDouble() < distance, "Ctrl + wheel failed to zoom in");
    QWheelEvent reverse({200,180}, {200,180}, {}, {0,-120}, Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &reverse);
    require(std::abs(widget.cameraState().value("distance").toDouble()-distance) < 1e-5, "Wheel zoom directions disagree");
    const auto beforePlainWheel = widget.cameraState();
    QWheelEvent plain({200,180}, {200,180}, {}, {0,120}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &plain);
    require(widget.cameraState() == beforePlainWheel, "Plain wheel unexpectedly changed the camera");
    QMouseEvent reset(QEvent::MouseButtonDblClick, {200,180}, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(&widget, &reset);
    require(widget.cameraState() == defaultCamera && render3D(widget) == original, "Camera reset failed");
    widget.setCameraState({{"pitch", -10000}, {"distance", 10000}, {"panX", -10000}});
    require(widget.cameraState().value("pitch").toDouble() == -85 &&
            widget.cameraState().value("distance").toDouble() == 18 &&
            widget.cameraState().value("panX").toDouble() == -10, "Camera bounds failed");
    widget.resetCamera();
    widget.resize(430,330);
    const auto smallImage = render3D(widget);
    if (!path.isEmpty()) require(smallImage.save(path+QStringLiteral(".small.png")), "Unable to save small 3D preview");
    config.durationSeconds = 1;
    const auto longData = buildSignalLabIqPreview(SignalLabGenerator::generate(config));
    require(longData.samples.size() == 2500 && longData.symbols.size() <= 2500 &&
            !longData.continuousTrajectory, "Long 3D preview exceeded its budget or invented an adjacent trajectory");
    widget.setData(longData);
    const auto longImage = render3D(widget);
    if (!path.isEmpty()) require(longImage.save(path+QStringLiteral(".long.png")), "Unable to save long-record 3D preview");
    require(SignalLabConstellation3DTestAccess::lines(widget) == 23,
            "3D preview connected decimated long-record points as a false trajectory");
    widget.clear();
    render3D(widget);
    require(SignalLabConstellation3DTestAccess::points(widget) == 0, "Clear retained old 3D points");
}

void testResearchConstellation(const QString &path) {
    constexpr double rate = 48000;
    std::vector<float> first(48000), second(48000);
    for (std::size_t index = 0; index < first.size(); ++index) {
        first[index] = float(1e-5*std::cos(2*pi*1000*index/rate));
        second[index] = float(2e-5*std::sin(2*pi*1000*index/rate));
    }
    const auto data = buildResearchIqConstellation(first, second, rate, 4800, 0.25);
    require(data.samples.size() == 2500 && data.symbols.size() == 2500 &&
            !data.continuousTrajectory && data.durationSeconds == 1,
            "Research IQ preview exceeded its budget or changed time span");
    require(std::abs(data.amplitudeScale-2e-5) < 1e-12, "Weak IQ amplitude scale is incorrect");
    require(data.sampleTimes.front() == 0 && data.sampleTimes.back() == (first.size()-1)/rate &&
            data.symbolTimes.front() == 2.5/rate && data.symbolTimes.back() > 0.99,
            "Research time coordinates or symbol sampling do not cover the full snapshot");
    int jittered = 0;
    for (int index = 0; index < data.samples.size(); ++index) {
        const auto source = std::size_t(std::llround(data.sampleTimes[index]*rate));
        require(source < first.size() && data.samples[index] == std::complex<float>(first[source], second[source]),
                "Research IQ preview altered the actual sample amplitudes");
        if (index > 0) require(data.sampleTimes[index] > data.sampleTimes[index-1], "Time samples are not ordered");
        if (source != std::size_t(index)*first.size()/2500) ++jittered;
    }
    require(jittered > 1000, "Research preview uses an alias-prone uniform sampling stride");
    const auto phaseShifted = buildResearchIqConstellation(first, second, rate, 4800, 0.75);
    require(phaseShifted.symbolTimes.front() == 7.5/rate && phaseShifted.symbols.front() != data.symbols.front(),
            "Symbol phase control did not change the research constellation");
    require(buildResearchIqConstellation(first, second, 0, 4800, 0.5).samples.isEmpty(), "Invalid sample rate was accepted");
    require(buildResearchIqConstellation(first, second, rate, rate, 0.5).symbols.isEmpty(),
            "Symbol sampling accepted fewer than two samples per symbol");
    first.resize(100); second.resize(100);
    first[10] = std::numeric_limits<float>::quiet_NaN();
    const auto shortData = buildResearchIqConstellation(first, second, rate, 4800, 0.5);
    require(shortData.continuousTrajectory && shortData.samples[10].real() == 0 && std::isfinite(shortData.amplitudeScale),
            "Invalid input broke the small research preview");
    SignalLabConstellation3DWidget widget;
    widget.resize(800,560);
    widget.setLanguage(QStringLiteral("uk"));
    widget.setData(data);
    const auto image = render3D(widget);
    if (!path.isEmpty()) require(image.save(path), "Unable to save research constellation render");
    const auto camera = widget.cameraState();
    widget.setSurfaceEnabled(true);
    const auto surface = render3D(widget);
    require(SignalLabConstellation3DTestAccess::triangles(widget) == 4998 && surface != image &&
            SignalLabConstellation3DTestAccess::points(widget) == 5000 && widget.cameraState() == camera,
            "Radial surface has missing faces, loses the IQ points or resets the camera");
    require(SignalLabConstellation3DTestAccess::frameBehindSurface(widget), "Reference grid cuts through the radial surface");
    if (!path.isEmpty()) require(surface.save(path+QStringLiteral(".surface.png")), "Unable to save radial surface render");
    widget.setCameraState({{"yaw", 130}, {"pitch", 15}});
    const auto backSurface = render3D(widget);
    require(SignalLabConstellation3DTestAccess::triangles(widget) == 4998, "Radial surface disappears from the opposite side");
    if (!path.isEmpty()) require(backSurface.save(path+QStringLiteral(".surface-back.png")), "Unable to save opposite-side surface render");
    widget.setSurfaceEnabled(false);
    widget.setCameraState(camera);
    require(render3D(widget) == image && SignalLabConstellation3DTestAccess::triangles(widget) == 0,
            "Disabling the radial surface did not restore the point view");
    widget.setSurfaceEnabled(true);
    widget.clear();
    render3D(widget);
    require(SignalLabConstellation3DTestAccess::triangles(widget) == 0, "Clearing IQ left stale surface faces");
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        testSymbols();
        testSpectralPreview();
        testConstellation3D(argc == 2 ? QString::fromLocal8Bit(argv[1]) + QStringLiteral(".3d.png") : QString());
        testResearchConstellation(argc == 2 ? QString::fromLocal8Bit(argv[1]) + QStringLiteral(".research-3d.png") : QString());
        if (argc == 2) renderPreview(QString::fromLocal8Bit(argv[1]));
        std::cout << "Signal Lab IQ, matched symbols, FFT sign, short-window amplitude and RBW: PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
