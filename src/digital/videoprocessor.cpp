#include "videoprocessor.h"

#include <QColor>
#include <QDebug>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr double MIN_SAMPLES_PER_LINE = 24.0;
constexpr int MIN_FRAME_WIDTH = 160;
constexpr int MAX_FRAME_WIDTH = 1024;
constexpr int MIN_FRAME_HEIGHT = 120;
constexpr int MAX_FRAME_HEIGHT = 625;
constexpr int FRAME_INTERVAL_MS = 40;
constexpr int STATUS_INTERVAL_MS = 1000;
constexpr int TEST_PATTERN_SAMPLES_PER_LINE = 192;
constexpr int TEST_PATTERN_LINES_PER_TICK = 40;
constexpr int TEST_PATTERN_INTERVAL_MS = 20;
constexpr double TWO_PI = 6.28318530717958647692;
constexpr float HSYNC_MIN_SPAN = 0.05f;
constexpr float HSYNC_DARK_FRACTION = 0.18f;
constexpr float ANALOG_SYNC_FRACTION = 0.073f;
constexpr float ANALOG_BLANKING_FRACTION = 0.16f;
constexpr int SSTV_WIDTH = 320;
constexpr int SSTV_HEIGHT = 240;
constexpr double ROBOT36_LINE_SECONDS = 0.150;
constexpr double ROBOT36_SYNC_SECONDS = 0.009;
constexpr double ROBOT36_PORCH_SECONDS = 0.003;
constexpr double ROBOT36_LUMA_SECONDS = 0.088;
constexpr double ROBOT36_SEPARATOR_SECONDS = 0.0045;
constexpr double ROBOT36_MID_PORCH_SECONDS = 0.0015;
constexpr double ROBOT36_CHROMA_SECONDS = 0.044;
constexpr double SSTV_MIN_TONE_HZ = 1500.0;
constexpr double SSTV_MAX_TONE_HZ = 2300.0;
constexpr double SSTV_SYNC_TONE_HZ = 1200.0;
constexpr int SSTV_MAX_BUFFER_SECONDS = 4;
constexpr int SSTV_TEST_LINES_PER_TICK = 1;
constexpr int SSTV_TEST_INTERVAL_MS = 120;
constexpr int ROBOT36_VIS_CODE = 8;
constexpr double SSTV_VIS_BIT_SECONDS = 0.030;
constexpr double SSTV_VIS_LEADER_SECONDS = 0.300;
constexpr double SSTV_VIS_BREAK_SECONDS = 0.010;
constexpr double SSTV_VIS_LEADER_TONE_HZ = 1900.0;
constexpr double SSTV_VIS_ZERO_TONE_HZ = 1300.0;
constexpr double SSTV_VIS_ONE_TONE_HZ = 1100.0;
constexpr int SSTV_MAX_PREDICTED_SYNC_LINES = 8;
constexpr int APT_WIDTH = 2080;
constexpr int APT_DISPLAY_HEIGHT = 800;
constexpr double APT_PIXEL_RATE = 4160.0;
constexpr double APT_SUBCARRIER_HZ = 2400.0;
constexpr int APT_TEST_INTERVAL_MS = 100;
constexpr int APT_TEST_LINES_PER_TICK = 1;
constexpr int WEFAX_WIDTH = 576;
constexpr int WEFAX_DISPLAY_HEIGHT = 800;
constexpr double WEFAX_LINE_RATE = 2.0;
constexpr double WEFAX_PIXEL_RATE = WEFAX_WIDTH * WEFAX_LINE_RATE;
constexpr double WEFAX_BLACK_HZ = 1500.0;
constexpr double WEFAX_WHITE_HZ = 2300.0;
constexpr int WEFAX_TEST_INTERVAL_MS = 100;
constexpr int LRPT_DISPLAY_SIZE = 512;

enum class SstvSpecFamily {
    Robot36,
    Robot72,
    Rgb,
    Pd
};

struct SstvModeSpec {
    int visCode;
    const char *name;
    SstvSpecFamily family;
    int width;
    int height;
    double syncSeconds;
    double scanLineSeconds;
    double channelSeconds;
    double separatorSeconds;
};

const SstvModeSpec *sstvModeSpecForVis(int visCode) {
    static const SstvModeSpec modes[] = {
        {8,  "Robot 36",   SstvSpecFamily::Robot36, 320, 240, 0.009,    0.1500,   0.0440,   0.0045},
        {12, "Robot 72",   SstvSpecFamily::Robot72, 320, 240, 0.009,    0.3000,   0.0880,   0.0045},
        {44, "Martin M1",  SstvSpecFamily::Rgb,     320, 256, 0.004862, 0.446446, 0.146432, 0.000572},
        {40, "Martin M2",  SstvSpecFamily::Rgb,     320, 256, 0.004862, 0.226798, 0.073216, 0.000572},
        {60, "Scottie S1", SstvSpecFamily::Rgb,     320, 256, 0.009,    0.428220, 0.138240, 0.0015},
        {56, "Scottie S2", SstvSpecFamily::Rgb,     320, 256, 0.009,    0.277692, 0.088064, 0.0015},
        {76, "Scottie DX", SstvSpecFamily::Rgb,     320, 256, 0.009,    1.050300, 0.345600, 0.0015},
        {55, "SC2-180",    SstvSpecFamily::Rgb,     320, 256, 0.0055225,0.7110225,0.235000, 0.0005},
        {93, "PD50",       SstvSpecFamily::Pd,      320, 256, 0.020,    0.38816,  0.091520, 0.00208},
        {99, "PD90",       SstvSpecFamily::Pd,      320, 256, 0.020,    0.70304,  0.170240, 0.00208},
        {95, "PD120",      SstvSpecFamily::Pd,      640, 496, 0.020,    0.50848,  0.121600, 0.00208},
        {98, "PD160",      SstvSpecFamily::Pd,      512, 400, 0.020,    0.804416, 0.195584, 0.00208},
        {96, "PD180",      SstvSpecFamily::Pd,      640, 496, 0.020,    0.75424,  0.183040, 0.00208},
        {97, "PD240",      SstvSpecFamily::Pd,      640, 496, 0.020,    0.99992,  0.244480, 0.00208},
        {94, "PD290",      SstvSpecFamily::Pd,      800, 616, 0.020,    0.93728,  0.228800, 0.00208},
    };

    for (const SstvModeSpec &mode : modes) {
        if (mode.visCode == visCode) {
            return &mode;
        }
    }
    return nullptr;
}

float clampFloat(float value, float low, float high) {
    return (std::max)(low, (std::min)(value, high));
}

qint16 readInt16Le(const char *data) {
    const auto lo = static_cast<unsigned char>(data[0]);
    const auto hi = static_cast<unsigned char>(data[1]);
    return static_cast<qint16>(lo | (hi << 8));
}

double clampDouble(double value, double low, double high) {
    return (std::max)(low, (std::min)(value, high));
}

double videoTargetRate(const RadioSettings &settings) {
    // Keep the decimated IQ Nyquist limit comfortably above the wanted video
    // passband. The old 1.2 MS/s floor could fold a 1.5 MHz ATV signal back
    // into itself before the post-decimation filter, leaving only unstable
    // stripes even though the RF signal occupied the requested bandwidth.
    return clampDouble((std::max)(2500000.0, settings.bandwidth * 2.4),
                       2500000.0,
                       8000000.0);
}

double videoCutoff(const RadioSettings &settings, double outputRate) {
    return (std::min)((std::max)(750000.0, settings.bandwidth * 0.45), outputRate * 0.45);
}

int sstvPixelFrequencyWindow(int spanSamples, int pixels, int sampleRate) {
    if (spanSamples <= 0 || pixels <= 0 || sampleRate <= 0) {
        return 40;
    }
    const int twoPixels = qRound(2.1 * static_cast<double>(spanSamples) / pixels);
    const int longestUsefulWindow = qRound(0.00125 * sampleRate);
    return (std::clamp)(twoPixels, 40, (std::max)(40, longestUsefulWindow));
}

template <std::size_t N>
void sstvMedianFilter3(std::array<uchar, N> &values) {
    if constexpr (N < 3) {
        return;
    }
    const auto source = values;
    for (std::size_t index = 1; index + 1 < N; ++index) {
        const uchar a = source[index - 1];
        const uchar b = source[index];
        const uchar c = source[index + 1];
        values[index] = (std::max)((std::min)(a, b),
                                   (std::min)((std::max)(a, b), c));
    }
}

} // namespace

VideoProcessor::VideoProcessor(QObject *parent)
    : QObject(parent) {
    frameTimer.start();
    statusTimer.start();
    sstvFrameTimer.start();
    sstvStatusTimer.start();
    aptFrameTimer.start();
    aptStatusTimer.start();
    wefaxFrameTimer.start();
    wefaxStatusTimer.start();
    lrptFrameTimer.start();
    lrptStatusTimer.start();
    resetRaster();
    resetSstvState(true);
    resetAptState(true);
    resetWefaxState(true);
}

void VideoProcessor::configure(bool newEnabled,
                               int demodMode,
                               double lineRate,
                               int frameWidth,
                               int frameHeight,
                               bool invertVideo,
                               bool hSyncEnabled,
                               bool vSyncEnabled) {
    const int clampedMode = demodMode == AmVideo ? AmVideo : FmVideo;
    const double clampedLineRate = std::isfinite(lineRate) && lineRate > 1000.0 ? lineRate : 15625.0;
    const int clampedWidth = (std::clamp)(frameWidth, MIN_FRAME_WIDTH, MAX_FRAME_WIDTH);
    const int clampedHeight = (std::clamp)(frameHeight, MIN_FRAME_HEIGHT, MAX_FRAME_HEIGHT);

    const bool shapeChanged = configuredLineRate != clampedLineRate ||
                              width != clampedWidth ||
                              height != clampedHeight;
    const bool demodChanged = mode != clampedMode ||
                              invert != invertVideo ||
                              hSync != hSyncEnabled ||
                              vSync != vSyncEnabled;
    const bool enabledChanged = enabled != newEnabled;

    enabled = newEnabled;
    mode = clampedMode;
    configuredLineRate = clampedLineRate;
    width = clampedWidth;
    height = clampedHeight;
    invert = invertVideo;
    hSync = hSyncEnabled;
    vSync = vSyncEnabled;

    if (shapeChanged || demodChanged || enabledChanged) {
        reset();
    }

    emitStatus(enabled ? QStringLiteral("Video decoder ready")
                       : QStringLiteral("Video decoder disabled"),
               true);
}

void VideoProcessor::reset() {
    activeSampleRate = 0.0;
    samplesPerLine = 0;
    lineSamplePhase = 0.0;
    currentLine.clear();
    fmPreviousValid = false;
    fmPrevI = 1.0f;
    fmPrevQ = 0.0f;
    dcEstimate = 0.0f;
    levelMin = -0.25f;
    levelMax = 0.25f;
    hSyncOffsetValid = false;
    hSyncOffsetEstimate = 0.0;
    hSyncPolarityAccumulator = 0;
    autoInvertActive = false;
    hSyncLockedLines = 0;
    hSyncObservedLines = 0;
    lastHSyncPercent = 0;
    resetChannelizer();
    resetRaster();
}

void VideoProcessor::resetChannelizer() {
    ncoPhase = 0.0;
    decimationSum = std::complex<float>(0.0f, 0.0f);
    decimationCount = 0;
    lowPassState = std::complex<float>(0.0f, 0.0f);
}

void VideoProcessor::setTestPatternEnabled(bool newEnabled) {
    testPatternEnabled = newEnabled;
    ensureTestPatternTimer();
    if (testPatternEnabled) {
        reset();
        emitStatus(QStringLiteral("Video test pattern"), true);
        testPatternTimer->start(TEST_PATTERN_INTERVAL_MS);
    } else if (testPatternTimer) {
        testPatternTimer->stop();
        emitStatus(enabled ? QStringLiteral("Video decoder ready")
                           : QStringLiteral("Video decoder disabled"),
                   true);
    }
}

void VideoProcessor::configureLrpt(bool newEnabled) {
    if (lrptEnabled == newEnabled) {
        return;
    }

    lrptEnabled = newEnabled;
    if (lrptEnabled) {
        configureLrptImage();
        emit frameReady(lrptRaster.copy());
    }
    emitStatus(lrptEnabled
                   ? QStringLiteral("Meteor LRPT beta: waiting for QPSK IQ")
                   : QStringLiteral("Meteor LRPT beta disabled"),
               true);
}

void VideoProcessor::configureSstv(bool newEnabled) {
    if (sstvEnabled == newEnabled) {
        return;
    }

    sstvEnabled = newEnabled;
    resetSstvState(!newEnabled);
    if (!sstvEnabled && sstvTestPatternTimer) {
        sstvTestPatternTimer->stop();
        sstvTestPatternEnabled = false;
    }
    emitStatus(sstvEnabled
                   ? QStringLiteral("SSTV: waiting for VIS header")
                   : QStringLiteral("SSTV decoder disabled"),
               true);
}

void VideoProcessor::resetSstvDecoder() {
    if (!sstvEnabled) {
        return;
    }
    resetSstvState(true);
    emitStatus(QStringLiteral("SSTV: demodulator changed; waiting for VIS header"), true);
}

void VideoProcessor::setSstvTestPatternEnabled(bool newEnabled) {
    if (sstvTestPatternEnabled == newEnabled) {
        return;
    }

    sstvTestPatternEnabled = newEnabled;
    ensureSstvTestPatternTimer();
    if (sstvTestPatternEnabled) {
        sstvEnabled = true;
        sstvTestPatternLine = 0;
        sstvTestTonePhase = 0.0;
        resetSstvState(true);
        emitStatus(QStringLiteral("SSTV Robot36 test stream"), true);
        sstvTestPatternTimer->start(SSTV_TEST_INTERVAL_MS);
    } else if (sstvTestPatternTimer) {
        sstvTestPatternTimer->stop();
    }
}

void VideoProcessor::configureApt(bool newEnabled) {
    if (aptEnabled == newEnabled) {
        return;
    }

    aptEnabled = newEnabled;
    resetAptState(!newEnabled);
    if (!aptEnabled && aptTestPatternTimer) {
        aptTestPatternTimer->stop();
        aptTestPatternEnabled = false;
    }
    emitStatus(aptEnabled
                   ? QStringLiteral("NOAA APT: waiting for 2400 Hz subcarrier")
                   : QStringLiteral("NOAA APT decoder disabled"),
               true);
}

void VideoProcessor::setAptTestPatternEnabled(bool newEnabled) {
    if (aptTestPatternEnabled == newEnabled) {
        return;
    }

    aptTestPatternEnabled = newEnabled;
    ensureAptTestPatternTimer();
    if (aptTestPatternEnabled) {
        aptEnabled = true;
        aptTestLine = 0;
        aptTestTonePhase = 0.0;
        resetAptState(true);
        emitStatus(QStringLiteral("NOAA APT test stream"), true);
        aptTestPatternTimer->start(APT_TEST_INTERVAL_MS);
    } else if (aptTestPatternTimer) {
        aptTestPatternTimer->stop();
    }
}

void VideoProcessor::configureWefax(bool newEnabled) {
    if (wefaxEnabled == newEnabled) {
        return;
    }

    wefaxEnabled = newEnabled;
    resetWefaxState(!newEnabled);
    if (!wefaxEnabled && wefaxTestPatternTimer) {
        wefaxTestPatternTimer->stop();
        wefaxTestPatternEnabled = false;
    }
    emitStatus(wefaxEnabled
                   ? QStringLiteral("WEFAX: waiting for 1500-2300 Hz tones")
                   : QStringLiteral("WEFAX decoder disabled"),
               true);
}

void VideoProcessor::setWefaxTestPatternEnabled(bool newEnabled) {
    if (wefaxTestPatternEnabled == newEnabled) {
        return;
    }

    wefaxTestPatternEnabled = newEnabled;
    ensureWefaxTestPatternTimer();
    if (wefaxTestPatternEnabled) {
        wefaxEnabled = true;
        wefaxTestLine = 0;
        wefaxTestTonePhase = 0.0;
        resetWefaxState(true);
        emitStatus(QStringLiteral("WEFAX test stream"), true);
        wefaxTestPatternTimer->start(WEFAX_TEST_INTERVAL_MS);
    } else if (wefaxTestPatternTimer) {
        wefaxTestPatternTimer->stop();
    }
}

void VideoProcessor::resetSstvState(bool clearImage) {
    sstvAudioBuffer.clear();
    sstvNextLine = 0;
    sstvHaveEvenLine = false;
    sstvLinesSinceFrame = 0;
    lastSstvSyncScore = 0.0;
    sstvFrameActive = false;
    sstvSyncLockCount = 0;
    sstvLostSyncCount = 0;
    sstvSamplesSinceLastSync = 0;
    sstvLineOverlapActive = false;
    sstvLineTimingScale = 1.0;
    sstvPreviousAdvanceSamples = 0;
    sstvHaveLinePeriodReference = false;
    sstvLineAdvanceOverride = -1;
    sstvDecodedRows = 0;
    sstvScottiePrimed = false;
    sstvScottieGreen.fill(0);
    sstvScottieBlue.fill(0);
    sstvLastVisCode = ROBOT36_VIS_CODE;
    sstvVisSearchOffset = 0;
    sstvToneOffsetHz = 0.0;
    sstvEvenLuma.fill(0);
    sstvEvenV.fill(128);
    if (clearImage || sstvRaster.isNull()) {
        sstvRaster = QImage(SSTV_WIDTH, SSTV_HEIGHT, QImage::Format_RGB32);
        sstvRaster.fill(QColor(8, 10, 12));
    }
    sstvFrameTimer.restart();
    sstvStatusTimer.restart();
}

void VideoProcessor::resetAptState(bool clearImage) {
    aptLine.clear();
    aptLine.reserve(APT_WIDTH);
    aptNextLine = 0;
    aptCarrierPhase = 0.0;
    aptCarrierI = 0.0;
    aptCarrierQ = 0.0;
    aptEnvelopeDc = 0.0;
    aptPixelPhase = 0.0;
    aptLinesSinceFrame = 0;
    if (clearImage || aptRaster.isNull()) {
        aptRaster = QImage(APT_WIDTH, APT_DISPLAY_HEIGHT, QImage::Format_Grayscale8);
        aptRaster.fill(0);
    }
    aptFrameTimer.restart();
    aptStatusTimer.restart();
}

void VideoProcessor::resetWefaxState(bool clearImage) {
    wefaxLine.clear();
    wefaxLine.reserve(WEFAX_WIDTH);
    wefaxPixelSamples.clear();
    wefaxPixelSamples.reserve(64);
    wefaxNextLine = 0;
    wefaxPixelPhase = 0.0;
    wefaxLinesSinceFrame = 0;
    if (clearImage || wefaxRaster.isNull()) {
        wefaxRaster = QImage(WEFAX_WIDTH, WEFAX_DISPLAY_HEIGHT, QImage::Format_Grayscale8);
        wefaxRaster.fill(0);
    }
    wefaxFrameTimer.restart();
    wefaxStatusTimer.restart();
}

void VideoProcessor::processSstvPcmFrame(const QByteArray &pcmData, int sampleRate) {
    if (!sstvEnabled || pcmData.size() < static_cast<int>(sizeof(qint16))) {
        return;
    }

    if (sampleRate <= 0) {
        sampleRate = 48000;
    }
    if (std::abs(sstvSampleRate - sampleRate) > 1) {
        sstvSampleRate = sampleRate;
        resetSstvState(true);
    }

    const int sampleCount = pcmData.size() / static_cast<int>(sizeof(qint16));
    sstvAudioBuffer.reserve(sstvAudioBuffer.size() + static_cast<std::size_t>(sampleCount));
    const char *raw = pcmData.constData();
    for (int i = 0; i < sampleCount; ++i) {
        sstvAudioBuffer.push_back(readInt16Le(raw + i * static_cast<int>(sizeof(qint16))) / 32768.0f);
    }
    if (sstvFrameActive) {
        sstvSamplesSinceLastSync += sampleCount;
    }

    const std::size_t maxBufferSamples =
        static_cast<std::size_t>((std::max)(1, sstvSampleRate) * SSTV_MAX_BUFFER_SECONDS);
    if (sstvAudioBuffer.size() > maxBufferSamples) {
        const std::size_t eraseCount = sstvAudioBuffer.size() - maxBufferSamples;
        if (eraseCount > 0) {
            sstvAudioBuffer.erase(sstvAudioBuffer.begin(),
                                  sstvAudioBuffer.begin() + static_cast<std::ptrdiff_t>(eraseCount));
            sstvVisSearchOffset = (std::max)(
                0,
                sstvVisSearchOffset - static_cast<int>(eraseCount));
        }
    }

    processSstvBuffer();
}

void VideoProcessor::processAptPcmFrame(const QByteArray &pcmData, int sampleRate) {
    if (!aptEnabled || pcmData.size() < static_cast<int>(sizeof(qint16))) {
        return;
    }

    if (sampleRate <= 0) {
        sampleRate = 48000;
    }
    if (std::abs(aptSampleRate - sampleRate) > 1) {
        aptSampleRate = sampleRate;
        resetAptState(true);
    }

    const int sampleCount = pcmData.size() / static_cast<int>(sizeof(qint16));
    const char *raw = pcmData.constData();
    const double carrierIncrement = TWO_PI * APT_SUBCARRIER_HZ / static_cast<double>(aptSampleRate);
    const double envelopeAlpha = (std::clamp)(1.0 - std::exp(-TWO_PI * 1800.0 / aptSampleRate), 0.0001, 1.0);
    const double dcAlpha = (std::clamp)(1.0 - std::exp(-TWO_PI * 0.5 / aptSampleRate), 0.000001, 0.01);
    const double samplesPerPixel = static_cast<double>(aptSampleRate) / APT_PIXEL_RATE;

    for (int i = 0; i < sampleCount; ++i) {
        const float sample = readInt16Le(raw + i * static_cast<int>(sizeof(qint16))) / 32768.0f;
        const double oscI = std::cos(aptCarrierPhase);
        const double oscQ = std::sin(aptCarrierPhase);
        const double mixedI = sample * oscI;
        const double mixedQ = sample * oscQ;
        aptCarrierI += envelopeAlpha * (mixedI - aptCarrierI);
        aptCarrierQ += envelopeAlpha * (mixedQ - aptCarrierQ);
        const double envelope = std::sqrt(aptCarrierI * aptCarrierI + aptCarrierQ * aptCarrierQ) * 2.0;
        aptEnvelopeDc += dcAlpha * (envelope - aptEnvelopeDc);

        aptPixelPhase += 1.0;
        if (aptPixelPhase >= samplesPerPixel) {
            aptPixelPhase -= samplesPerPixel;
            const double normalized = (envelope - aptEnvelopeDc) * 2.8 + 0.5;
            appendAptBrightnessSample(static_cast<float>(normalized));
        }

        aptCarrierPhase = std::remainder(aptCarrierPhase + carrierIncrement, TWO_PI);
    }
}

void VideoProcessor::processWefaxPcmFrame(const QByteArray &pcmData, int sampleRate) {
    if (!wefaxEnabled || pcmData.size() < static_cast<int>(sizeof(qint16))) {
        return;
    }

    if (sampleRate <= 0) {
        sampleRate = 48000;
    }
    if (std::abs(wefaxSampleRate - sampleRate) > 1) {
        wefaxSampleRate = sampleRate;
        resetWefaxState(true);
    }

    const double samplesPerPixel = static_cast<double>(wefaxSampleRate) / WEFAX_PIXEL_RATE;
    const int sampleCount = pcmData.size() / static_cast<int>(sizeof(qint16));
    const char *raw = pcmData.constData();
    for (int i = 0; i < sampleCount; ++i) {
        wefaxPixelSamples.push_back(readInt16Le(raw + i * static_cast<int>(sizeof(qint16))) / 32768.0f);
        wefaxPixelPhase += 1.0;
        if (wefaxPixelPhase >= samplesPerPixel) {
            wefaxPixelPhase -= samplesPerPixel;
            const double frequency = estimateWefaxFrequency(wefaxPixelSamples);
            wefaxPixelSamples.clear();
            const double normalized = (frequency - WEFAX_BLACK_HZ) / (WEFAX_WHITE_HZ - WEFAX_BLACK_HZ);
            appendWefaxPixel(static_cast<float>(normalized));
        }
    }
}

void VideoProcessor::resetRaster() {
    raster = QImage(width, height, QImage::Format_Grayscale8);
    raster.fill(0);
    nextLine = 0;
    linesSinceFrame = 0;
}

void VideoProcessor::processIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount) {
    if ((!enabled && !testPatternEnabled) || iqData.isEmpty() || sampleRate <= 0.0 || sampleCount <= 0) {
        return;
    }

    const double desiredSamplesPerLine = sampleRate / configuredLineRate;
    if (!std::isfinite(desiredSamplesPerLine) || desiredSamplesPerLine < MIN_SAMPLES_PER_LINE) {
        emitStatus(QStringLiteral("Video needs wider IQ: %1 kS/s")
                       .arg(sampleRate / 1000.0, 0, 'f', 0));
        return;
    }

    const int newSamplesPerLine = static_cast<int>(std::lround(desiredSamplesPerLine));
    if (std::abs(activeSampleRate - sampleRate) > 1.0 || samplesPerLine != newSamplesPerLine) {
        activeSampleRate = sampleRate;
        samplesPerLine = (std::max)(1, newSamplesPerLine);
        lineSamplePhase = 0.0;
        currentLine.clear();
        fmPreviousValid = false;
        resetRaster();
        emitStatus(QStringLiteral("Video IQ %1 MS/s, %2 samples/line")
                       .arg(sampleRate / 1000000.0, 0, 'f', 2)
                       .arg(samplesPerLine),
                   true);
    }

    const qsizetype expectedInt16Bytes = static_cast<qsizetype>(sampleCount) * 4;
    const qsizetype expectedInt8Bytes = static_cast<qsizetype>(sampleCount) * 2;
    const bool int16Iq = iqData.size() >= expectedInt16Bytes;
    const bool int8Iq = !int16Iq && iqData.size() >= expectedInt8Bytes;
    if (!int16Iq && !int8Iq) {
        emitStatus(QStringLiteral("Unsupported video IQ frame"), true);
        return;
    }

    const char *data = iqData.constData();
    for (int n = 0; n < sampleCount; ++n) {
        float iSample = 0.0f;
        float qSample = 0.0f;
        if (int16Iq) {
            const char *sample = data + n * 4;
            iSample = readInt16Le(sample) / 32768.0f;
            qSample = readInt16Le(sample + 2) / 32768.0f;
        } else {
            const char *sample = data + n * 2;
            iSample = static_cast<signed char>(sample[0]) / 128.0f;
            qSample = static_cast<signed char>(sample[1]) / 128.0f;
        }

        if (!std::isfinite(iSample)) {
            iSample = 0.0f;
        }
        if (!std::isfinite(qSample)) {
            qSample = 0.0f;
        }

        processChannelSample(iSample, qSample);
    }

    emitStatus(QStringLiteral("Video IQ %1 MS/s, %2 samples/line")
                   .arg(sampleRate / 1000000.0, 0, 'f', 2)
                   .arg(samplesPerLine));
}

void VideoProcessor::processFloatIqSnapshot(const std::vector<float> &iqSamples,
                                            const RadioSettings &settings) {
    if (settings.modulationType == MOD_LRPT) {
        processLrptFloatIqSnapshot(iqSamples, settings);
        return;
    }

    if (!enabled || iqSamples.size() < 4 || settings.sampleRate <= 0.0) {
        return;
    }

    const double inputRate = settings.sampleRate;
    const double targetRate = videoTargetRate(settings);
    const int decimationFactor = (std::max)(1, static_cast<int>(std::floor(inputRate / targetRate)));
    const double outputRate = inputRate / static_cast<double>(decimationFactor);
    const double cutoff = videoCutoff(settings, outputRate);
    const float lowPassAlpha = static_cast<float>((std::clamp)(
        1.0 - std::exp(-TWO_PI * cutoff / outputRate),
        0.000001,
        1.0
        ));

    const bool shapeChanged =
        std::abs(activeInputSampleRate - inputRate) > 1.0 ||
        std::abs(activeCenterFrequency - settings.centerFrequency) > 0.5 ||
        std::abs(activeListeningFrequency - settings.listeningFrequency) > 0.5 ||
        activeInputMode != settings.inputMode;
    if (shapeChanged) {
        activeInputSampleRate = inputRate;
        activeCenterFrequency = settings.centerFrequency;
        activeListeningFrequency = settings.listeningFrequency;
        activeInputMode = settings.inputMode;
        resetChannelizer();
    }

    const double desiredSamplesPerLine = outputRate / configuredLineRate;
    if (!std::isfinite(desiredSamplesPerLine) || desiredSamplesPerLine < MIN_SAMPLES_PER_LINE) {
        emitStatus(QStringLiteral("Video needs wider IQ: %1 kS/s")
                       .arg(outputRate / 1000.0, 0, 'f', 0));
        return;
    }
    const int newSamplesPerLine = static_cast<int>(std::lround(desiredSamplesPerLine));
    if (std::abs(activeSampleRate - outputRate) > 1.0 || samplesPerLine != newSamplesPerLine) {
        activeSampleRate = outputRate;
        samplesPerLine = (std::max)(1, newSamplesPerLine);
        lineSamplePhase = 0.0;
        currentLine.clear();
        fmPreviousValid = false;
        resetRaster();
        emitStatus(QStringLiteral("Video snapshot %1 MS/s, %2 samples/line, BW %3 MHz")
                       .arg(outputRate / 1000000.0, 0, 'f', 2)
                       .arg(samplesPerLine)
                       .arg(settings.bandwidth / 1000000.0, 0, 'f', 1),
                   true);
    }

    const std::size_t iqCount = iqSamples.size() / 2;
    const double fShift = settings.listeningFrequency - settings.centerFrequency;
    const double phaseIncrement = -TWO_PI * fShift / inputRate;
    float rotI = static_cast<float>(std::cos(ncoPhase));
    float rotQ = static_cast<float>(std::sin(ncoPhase));
    const float rotStepI = static_cast<float>(std::cos(phaseIncrement));
    const float rotStepQ = static_cast<float>(std::sin(phaseIncrement));
    std::complex<float> sum = decimationSum;
    int count = decimationCount;
    std::complex<float> lowPass = lowPassState;

    for (std::size_t n = 0; n < iqCount; ++n) {
        float iSample = iqSamples[2 * n];
        float qSample = iqSamples[2 * n + 1];
        if (!std::isfinite(iSample)) {
            iSample = 0.0f;
        }
        if (!std::isfinite(qSample)) {
            qSample = 0.0f;
        }
        if (settings.inputMode == INPUT_HF1) {
            qSample = 0.0f;
        } else if (settings.inputMode == INPUT_HF2) {
            iSample = qSample;
            qSample = 0.0f;
        } else if (settings.inputMode == INPUT_HF_NOISE_CANCEL) {
            const float refGain = std::abs(hfNoiseCancelReferenceCoefficient(settings, settings.listeningFrequency));
            iSample -= static_cast<float>((std::clamp)(settings.hfNoiseCancelDepth, -2.0, 2.0)) * refGain * qSample;
            qSample = 0.0f;
        }

        const float mixedI = iSample * rotI - qSample * rotQ;
        const float mixedQ = iSample * rotQ + qSample * rotI;
        sum += std::complex<float>(mixedI, mixedQ);
        ++count;

        const float nextRotI = rotI * rotStepI - rotQ * rotStepQ;
        const float nextRotQ = rotI * rotStepQ + rotQ * rotStepI;
        rotI = nextRotI;
        rotQ = nextRotQ;
        if ((n & 4095) == 4095) {
            const float norm = std::sqrt(rotI * rotI + rotQ * rotQ);
            if (norm > 0.0f) {
                rotI /= norm;
                rotQ /= norm;
            }
        }

        if (count < decimationFactor) {
            continue;
        }

        const float invCount = 1.0f / static_cast<float>(count);
        const std::complex<float> channelSample = sum * invCount;
        sum = std::complex<float>(0.0f, 0.0f);
        count = 0;
        lowPass += lowPassAlpha * (channelSample - lowPass);
        processChannelSample(std::real(lowPass), std::imag(lowPass));
    }

    decimationSum = sum;
    decimationCount = count;
    lowPassState = lowPass;
    ncoPhase = std::remainder(ncoPhase + phaseIncrement * static_cast<double>(iqCount), TWO_PI);
    emitStatus(QStringLiteral("Video snapshot %1 MS/s, %2 samples/line, BW %3 MHz")
                   .arg(outputRate / 1000000.0, 0, 'f', 2)
                   .arg(samplesPerLine)
                   .arg(settings.bandwidth / 1000000.0, 0, 'f', 1));
}

void VideoProcessor::processSstvBuffer() {
    const int nominalLineSamples = static_cast<int>(
        std::lround(activeSstvLineSeconds() * sstvSampleRate));
    if (nominalLineSamples <= 0) {
        return;
    }

    if (!sstvFrameActive) {
        double visConfidence = 0.0;
        int headerEndSample = -1;
        const int visCode = detectSstvVisCode(&visConfidence, &headerEndSample);
        if (visCode >= 0) {
            const bool supported = isSupportedSstvVisCode(visCode);
            sstvLastVisCode = visCode;
            sstvFrameActive = supported;
            sstvSyncLockCount = supported ? 3 : 0;
            sstvLostSyncCount = 0;
            sstvSamplesSinceLastSync = 0;
            sstvLineOverlapActive = false;
            sstvLineTimingScale = 1.0;
            sstvPreviousAdvanceSamples = 0;
            sstvHaveLinePeriodReference = false;
            sstvLineAdvanceOverride = -1;
            sstvDecodedRows = 0;
            sstvScottiePrimed = false;
            sstvNextLine = 0;
            sstvHaveEvenLine = false;
            if (supported) {
                sstvRaster = QImage(activeSstvWidth(), activeSstvHeight(), QImage::Format_RGB32);
                sstvRaster.fill(QColor(8, 10, 12));
                emit frameReady(sstvRaster.copy());
            }
            if (headerEndSample > 0 &&
                headerEndSample < static_cast<int>(sstvAudioBuffer.size())) {
                sstvAudioBuffer.erase(sstvAudioBuffer.begin(),
                                      sstvAudioBuffer.begin() + headerEndSample);
            }
            sstvVisSearchOffset = 0;
            emitStatus(QStringLiteral("SSTV VIS %1: %2 (%3), offset %4 Hz, confidence %5")
                           .arg(visCode)
                           .arg(sstvModeName(visCode))
                           .arg(supported ? QStringLiteral("supported") : QStringLiteral("unsupported"))
                           .arg(sstvToneOffsetHz, 0, 'f', 0)
                           .arg(visConfidence, 0, 'f', 1),
                       true);
            if (!supported) {
                sstvAudioBuffer.clear();
                return;
            }
            return;
        }

        if (!sstvStatusTimer.isValid() || sstvStatusTimer.elapsed() >= STATUS_INTERVAL_MS) {
            emitStatus(QStringLiteral("SSTV: waiting for a valid VIS header"), true);
            sstvStatusTimer.restart();
        }
        return;
    }

    int decodedLines = 0;
    while (decodedLines < 6) {
        const int lineSamples = qRound(nominalLineSamples * sstvLineTimingScale);
        const int guardSamples = (std::max)(
            8,
            qRound((std::max)(activeSstvSyncSeconds() * 2.0, 0.012) *
                   sstvSampleRate * sstvLineTimingScale));
        const int requiredSamples = lineSamples +
                                    (sstvLineOverlapActive ? guardSamples : 0);
        if (sstvAudioBuffer.size() < static_cast<std::size_t>(requiredSamples)) {
            break;
        }
        double score = 0.0;
        int lineStart = findSstvLineStart(&score);
        bool predictedLine = false;
        if (lineStart < 0) {
            if (sstvFrameActive && sstvLineOverlapActive &&
                sstvLostSyncCount < SSTV_MAX_PREDICTED_SYNC_LINES) {
                lineStart = guardSamples;
                predictedLine = true;
                ++sstvLostSyncCount;
                score = 0.0;
            }
        }
        if (lineStart < 0) {
            const double holdoverSeconds = (std::clamp)(
                activeSstvLineSeconds() * (SSTV_MAX_PREDICTED_SYNC_LINES + 0.4),
                0.70,
                4.00);
            const int holdoverSamples = qRound(holdoverSeconds * sstvSampleRate);
            if (sstvFrameActive && sstvSamplesSinceLastSync > holdoverSamples) {
                sstvFrameActive = false;
                sstvSyncLockCount = 0;
                sstvLostSyncCount = 0;
                sstvSamplesSinceLastSync = 0;
                sstvLineOverlapActive = false;
                sstvVisSearchOffset = 0;
                emitStatus(QStringLiteral("SSTV %1: sync timeout; waiting for a new VIS header")
                               .arg(sstvModeName(sstvLastVisCode)),
                           true);
            }
            const std::size_t keepSamples = static_cast<std::size_t>(
                holdoverSamples + lineSamples + guardSamples);
            if (sstvAudioBuffer.size() > keepSamples) {
                sstvAudioBuffer.erase(sstvAudioBuffer.begin(),
                                      sstvAudioBuffer.end() - static_cast<std::ptrdiff_t>(keepSamples));
            }
            if (!sstvStatusTimer.isValid() || sstvStatusTimer.elapsed() >= STATUS_INTERVAL_MS) {
                emitStatus(sstvFrameActive
                               ? QStringLiteral("SSTV %1: holding mode through sync interruption (%2 ms)")
                                     .arg(sstvModeName(sstvLastVisCode))
                                     .arg(qRound(1000.0 * sstvSamplesSinceLastSync /
                                                 (std::max)(1, sstvSampleRate)))
                               : QStringLiteral("SSTV: waiting for a valid VIS header"),
                           true);
                sstvStatusTimer.restart();
            }
            return;
        }

        if (!predictedLine && sstvLineOverlapActive) {
            const int expectedLineStart = guardSamples;
            const int delta = lineStart - expectedLineStart;
            const int nearbyLimit = qRound(0.025 * sstvSampleRate);
            if (std::abs(delta) <= nearbyLimit) {
                const int maxCorrection = (std::max)(2, qRound(0.00025 * sstvSampleRate));
                lineStart = expectedLineStart +
                            (std::clamp)(qRound(delta * 0.25),
                                        -maxCorrection,
                                        maxCorrection);
            }
        }

        if (!predictedLine && sstvHaveLinePeriodReference) {
            const int observedPeriod = sstvPreviousAdvanceSamples + lineStart;
            const double observedScale = static_cast<double>(observedPeriod) /
                                         nominalLineSamples;
            if (observedScale >= 0.97 && observedScale <= 1.03) {
                sstvLineTimingScale = 0.82 * sstvLineTimingScale +
                                      0.18 * observedScale;
            }
        }

        if (lineStart > 0) {
            sstvAudioBuffer.erase(sstvAudioBuffer.begin(),
                                  sstvAudioBuffer.begin() + lineStart);
        }
        const int decodeLineSamples = qRound(nominalLineSamples * sstvLineTimingScale);
        if (sstvAudioBuffer.size() < static_cast<std::size_t>(decodeLineSamples)) {
            return;
        }

        lastSstvSyncScore = score;
        if (!predictedLine) {
            sstvLostSyncCount = 0;
            sstvSamplesSinceLastSync = 0;
        }
        if (!sstvFrameActive) {
            ++sstvSyncLockCount;
            if (sstvSyncLockCount < 3 || score < 6.0) {
                sstvAudioBuffer.erase(sstvAudioBuffer.begin(),
                                      sstvAudioBuffer.begin() + lineSamples);
                continue;
            }
            sstvFrameActive = true;
            sstvNextLine = 0;
            sstvHaveEvenLine = false;
            sstvDecodedRows = 0;
            sstvScottiePrimed = false;
            sstvRaster = QImage(activeSstvWidth(), activeSstvHeight(), QImage::Format_RGB32);
            sstvRaster.fill(QColor(8, 10, 12));
            emit frameReady(sstvRaster.copy());
            emitStatus(QStringLiteral("SSTV %1: locked by repeated line sync")
                           .arg(sstvModeName(sstvLastVisCode)),
                       true);
        }
        sstvLineAdvanceOverride = -1;
        decodeSstvLine(0);
        const int advanceBase = sstvLineAdvanceOverride > 0
                                    ? sstvLineAdvanceOverride
                                    : decodeLineSamples;
        const int advanceSamples = (std::max)(1, advanceBase - guardSamples);
        sstvAudioBuffer.erase(sstvAudioBuffer.begin(),
                              sstvAudioBuffer.begin() + advanceSamples);
        sstvPreviousAdvanceSamples = advanceSamples;
        sstvHaveLinePeriodReference = true;
        sstvLineOverlapActive = true;
        ++decodedLines;

        if (predictedLine &&
            (!sstvStatusTimer.isValid() || sstvStatusTimer.elapsed() >= STATUS_INTERVAL_MS)) {
            emitStatus(QStringLiteral("SSTV %1: predicted line timing, missed sync %2/%3")
                           .arg(sstvModeName(sstvLastVisCode))
                           .arg(sstvLostSyncCount)
                           .arg(SSTV_MAX_PREDICTED_SYNC_LINES),
                       true);
            sstvStatusTimer.restart();
        }

        if (sstvDecodedRows >= activeSstvHeight()) {
            emit frameReady(sstvRaster.copy());
            emitStatus(QStringLiteral("SSTV %1: frame complete; waiting for the next VIS header")
                           .arg(sstvModeName(sstvLastVisCode)),
                       true);
            sstvFrameActive = false;
            sstvLineOverlapActive = false;
            sstvSamplesSinceLastSync = 0;
            sstvVisSearchOffset = 0;
            sstvAudioBuffer.clear();
            return;
        }
    }
}

int VideoProcessor::findSstvLineStart(double *bestScore) const {
    const double lineSeconds = activeSstvLineSeconds();
    const int lineSamples = static_cast<int>(
        std::lround(lineSeconds * sstvSampleRate * sstvLineTimingScale));
    const int syncSamples = static_cast<int>(
        std::lround(activeSstvSyncSeconds() * sstvSampleRate * sstvLineTimingScale));
    double porchSeconds = ROBOT36_PORCH_SECONDS;
    if (const SstvModeSpec *mode = sstvModeSpecForVis(sstvLastVisCode)) {
        if (mode->family == SstvSpecFamily::Rgb || mode->family == SstvSpecFamily::Pd) {
            porchSeconds = mode->separatorSeconds;
        }
    }
    const int porchSamples = (std::max)(
        8,
        static_cast<int>(std::lround(porchSeconds * sstvSampleRate * sstvLineTimingScale)));
    if (lineSamples <= 0 || syncSamples <= 0 ||
        sstvAudioBuffer.size() < static_cast<std::size_t>(lineSamples)) {
        return -1;
    }

    const int maxStart = (std::min)(
        static_cast<int>(sstvAudioBuffer.size()) - lineSamples,
        static_cast<int>(std::lround(lineSeconds * sstvSampleRate * 3.0)));
    const int step = (std::max)(4, sstvSampleRate / 2000);
    double best = 0.0;
    double bestSyncEnergy = 0.0;
    int bestIndex = -1;

    auto scoreAt = [&](int start, double *syncEnergyOut) {
        const double syncEnergy = toneEnergy(sstvAudioBuffer, start, syncSamples,
                                             SSTV_SYNC_TONE_HZ + sstvToneOffsetHz);
        const double lowEnergy = toneEnergy(sstvAudioBuffer, start, syncSamples,
                                            1000.0 + sstvToneOffsetHz);
        const double highEnergy = toneEnergy(sstvAudioBuffer, start, syncSamples,
                                             SSTV_MIN_TONE_HZ + sstvToneOffsetHz);
        const double porchEnergy = toneEnergy(sstvAudioBuffer,
                                              start + syncSamples,
                                              porchSamples,
                                              SSTV_MIN_TONE_HZ + sstvToneOffsetHz);
        const double competitor = (std::max)(lowEnergy, highEnergy);
        const double score = (syncEnergy / (competitor + 1.0e-8)) *
                             (1.0 + (std::min)(2.0, porchEnergy / (syncEnergy + 1.0e-8)) * 0.15);
        if (syncEnergyOut) {
            *syncEnergyOut = syncEnergy;
        }
        return score;
    };

    auto searchRange = [&](int rangeStart, int rangeEnd) {
        rangeStart = (std::clamp)(rangeStart, 0, maxStart);
        rangeEnd = (std::clamp)(rangeEnd, rangeStart, maxStart);
        for (int start = rangeStart; start <= rangeEnd; start += step) {
            double syncEnergy = 0.0;
            const double score = scoreAt(start, &syncEnergy);
            if (score > best) {
                best = score;
                bestSyncEnergy = syncEnergy;
                bestIndex = start;
            }
        }
    };

    const int expectedStart = sstvLineOverlapActive
                                  ? qRound((std::max)(activeSstvSyncSeconds() * 2.0, 0.012) *
                                           sstvSampleRate * sstvLineTimingScale)
                                  : 0;
    const int preferredBefore = qRound(0.006 * sstvSampleRate);
    const int preferredAfter = qRound(0.006 * sstvSampleRate);
    searchRange(expectedStart - preferredBefore, expectedStart + preferredAfter);
    if (!(bestIndex >= 0 && best > 4.0 && bestSyncEnergy > 5.0e-5)) {
        if (sstvLineOverlapActive) {
            if (bestScore) {
                *bestScore = best;
            }
            return -1;
        }
        best = 0.0;
        bestSyncEnergy = 0.0;
        bestIndex = -1;
        searchRange(0, maxStart);
    }

    if (bestIndex >= 0) {
        const int fineStart = (std::max)(0, bestIndex - step);
        const int fineEnd = (std::min)(maxStart, bestIndex + step);
        for (int start = fineStart; start <= fineEnd; ++start) {
            double syncEnergy = 0.0;
            const double score = scoreAt(start, &syncEnergy);
            if (score > best) {
                best = score;
                bestSyncEnergy = syncEnergy;
                bestIndex = start;
            }
        }
    }

    // scoreAt() locates the sync pulse, but its maximum may move inside the
    // 9 ms pulse as phase and noise change. Locate the rising 1200 Hz edge so
    // every decoded raster line starts at the same horizontal coordinate.
    if (bestIndex >= 0 && best > 4.0 && bestSyncEnergy > 5.0e-5) {
        const int edgeRadius = (std::max)(syncSamples, qRound(0.004 * sstvSampleRate));
        const int afterOffset = qRound(0.0007 * sstvSampleRate);
        const int afterLength = (std::max)(24, qRound(0.0040 * sstvSampleRate));
        const int beforeOffset = qRound(0.0005 * sstvSampleRate);
        const int beforeLength = (std::max)(24, qRound(0.0025 * sstvSampleRate));
        const int porchOffset = qRound(0.0003 * sstvSampleRate);
        const int porchLength = (std::max)(16, (std::min)(porchSamples,
                                                          qRound(0.0015 * sstvSampleRate)));
        double bestEdgeScore = 0.0;
        int bestEdgeIndex = -1;
        const int edgeBegin = (std::max)(0, bestIndex - edgeRadius);
        const int edgeEnd = (std::min)(maxStart, bestIndex + edgeRadius);
        for (int candidate = edgeBegin; candidate <= edgeEnd; ++candidate) {
            const int afterStart = candidate + afterOffset;
            const double afterSync = toneEnergy(sstvAudioBuffer,
                                                afterStart,
                                                afterLength,
                                                SSTV_SYNC_TONE_HZ + sstvToneOffsetHz);
            const double afterComp = (std::max)(
                toneEnergy(sstvAudioBuffer, afterStart, afterLength,
                           SSTV_MIN_TONE_HZ + sstvToneOffsetHz),
                toneEnergy(sstvAudioBuffer, afterStart, afterLength,
                           1900.0 + sstvToneOffsetHz));
            const int beforeStart = candidate - beforeOffset - beforeLength;
            const double beforeSync = beforeStart >= 0
                                          ? toneEnergy(sstvAudioBuffer,
                                                       beforeStart,
                                                       beforeLength,
                                                       SSTV_SYNC_TONE_HZ + sstvToneOffsetHz)
                                          : 0.0;
            const double beforeComp = beforeStart >= 0
                                          ? (std::max)(
                                                toneEnergy(sstvAudioBuffer,
                                                           beforeStart,
                                                           beforeLength,
                                                           SSTV_MIN_TONE_HZ + sstvToneOffsetHz),
                                                toneEnergy(sstvAudioBuffer,
                                                           beforeStart,
                                                           beforeLength,
                                                           1900.0 + sstvToneOffsetHz))
                                          : 1.0;
            const int porchStart = candidate + syncSamples + porchOffset;
            const double porchTone = toneEnergy(sstvAudioBuffer,
                                                porchStart,
                                                porchLength,
                                                SSTV_MIN_TONE_HZ + sstvToneOffsetHz);
            const double porchSync = toneEnergy(sstvAudioBuffer,
                                                porchStart,
                                                porchLength,
                                                SSTV_SYNC_TONE_HZ + sstvToneOffsetHz);
            const double afterRatio = afterSync / (afterComp + 1.0e-8);
            const double beforeRatio = beforeSync / (beforeComp + 1.0e-8);
            const double porchRatio = porchTone / (porchSync + 1.0e-8);
            const double edgeScore = afterRatio *
                                     (std::min)(3.0, porchRatio) /
                                     (1.0 + beforeRatio);
            if (afterRatio > 1.8 && porchRatio > 1.1 && edgeScore > bestEdgeScore) {
                bestEdgeScore = edgeScore;
                bestEdgeIndex = candidate;
            }
        }
        if (bestEdgeIndex >= 0) {
            bestIndex = bestEdgeIndex;
        }
    }

    if (bestScore) {
        *bestScore = best;
    }

    return bestIndex >= 0 && best > 4.0 && bestSyncEnergy > 5.0e-5 ? bestIndex : -1;
}

int VideoProcessor::detectSstvVisCode(double *confidence, int *headerEndSample) {
    const int leaderSamples = static_cast<int>(std::lround(SSTV_VIS_LEADER_SECONDS * sstvSampleRate));
    const int breakSamples = static_cast<int>(std::lround(SSTV_VIS_BREAK_SECONDS * sstvSampleRate));
    const int bitSamples = static_cast<int>(std::lround(SSTV_VIS_BIT_SECONDS * sstvSampleRate));
    const int bitCount = 10;
    const int headerSamples = leaderSamples + breakSamples + leaderSamples + bitSamples * bitCount;
    if (leaderSamples <= 0 || breakSamples <= 0 || bitSamples <= 0 ||
        sstvAudioBuffer.size() < static_cast<std::size_t>(headerSamples)) {
        return -1;
    }

    const int maxStart = static_cast<int>(sstvAudioBuffer.size()) - headerSamples;
    const int step = (std::max)(16, sstvSampleRate / 1000);
    const int firstStart = (std::clamp)(sstvVisSearchOffset, 0, maxStart);
    const int leaderProbeSamples = (std::max)(32, qRound(0.040 * sstvSampleRate));
    const int fullLeaderProbeSamples = (std::max)(64, qRound(0.200 * sstvSampleRate));
    const int fullLeaderProbeOffset = qRound(0.050 * sstvSampleRate);
    const int bitProbeSamples = (std::max)(32, qRound(0.020 * sstvSampleRate));
    const int leaderProbeOffset = qRound(0.120 * sstvSampleRate);
    const int bitProbeOffset = (bitSamples - bitProbeSamples) / 2;
    double bestConfidence = 0.0;
    double bestToneOffset = 0.0;
    int bestCode = -1;
    int bestHeaderEnd = -1;

    for (int start = firstStart; start <= maxStart; start += step) {
        const int breakStart = start + leaderSamples;
        const int secondLeaderStart = breakStart + breakSamples;
        const int visStart = secondLeaderStart + leaderSamples;

        const int probeStart = start + leaderProbeOffset;
        // Zero-crossing counts are unreliable on a discriminator output: a
        // small amount of wideband FM noise can add crossings even when the
        // 1900 Hz leader is clearly dominant. A compact Goertzel bank costs
        // a little more CPU during acquisition, but gives deterministic VIS
        // lock and supplies the common tone offset used for all VIS bits.
        double leaderProbeEnergy = 0.0;
        double measuredLeaderHz = SSTV_VIS_LEADER_TONE_HZ;
        for (double offsetHz = -175.0; offsetHz <= 175.0; offsetHz += 25.0) {
            const double candidateHz = SSTV_VIS_LEADER_TONE_HZ + offsetHz;
            const double energy = toneEnergy(sstvAudioBuffer,
                                             probeStart,
                                             leaderProbeSamples,
                                             candidateHz);
            if (energy > leaderProbeEnergy) {
                leaderProbeEnergy = energy;
                measuredLeaderHz = candidateHz;
            }
        }
        if (leaderProbeEnergy < 5.0e-6) {
            continue;
        }
        const double toneOffset = measuredLeaderHz - SSTV_VIS_LEADER_TONE_HZ;
        const double leader1 = toneEnergy(sstvAudioBuffer,
                                          probeStart,
                                          leaderProbeSamples,
                                          SSTV_VIS_LEADER_TONE_HZ + toneOffset);
        const int leader1Start = start + leaderProbeOffset;
        const int leader2Start = secondLeaderStart + leaderProbeOffset;
        const double leader1Comp = (std::max)(
            toneEnergy(sstvAudioBuffer, leader1Start, leaderProbeSamples,
                       SSTV_SYNC_TONE_HZ + toneOffset),
            toneEnergy(sstvAudioBuffer, leader1Start, leaderProbeSamples,
                       SSTV_VIS_ZERO_TONE_HZ + toneOffset));
        const double leader2 = toneEnergy(sstvAudioBuffer, leader2Start,
                                          leaderProbeSamples,
                                          SSTV_VIS_LEADER_TONE_HZ + toneOffset);
        const double leader2Comp = (std::max)(
            toneEnergy(sstvAudioBuffer, leader2Start, leaderProbeSamples,
                       SSTV_SYNC_TONE_HZ + toneOffset),
            toneEnergy(sstvAudioBuffer, leader2Start, leaderProbeSamples,
                       SSTV_VIS_ZERO_TONE_HZ + toneOffset));
        const int fullLeader1Start = start + fullLeaderProbeOffset;
        const int fullLeader2Start = secondLeaderStart + fullLeaderProbeOffset;
        const double fullLeader1 = toneEnergy(sstvAudioBuffer,
                                              fullLeader1Start,
                                              fullLeaderProbeSamples,
                                              SSTV_VIS_LEADER_TONE_HZ + toneOffset);
        const double fullLeader2 = toneEnergy(sstvAudioBuffer,
                                              fullLeader2Start,
                                              fullLeaderProbeSamples,
                                              SSTV_VIS_LEADER_TONE_HZ + toneOffset);
        const double fullLeader1Comp = (std::max)({
            toneEnergy(sstvAudioBuffer, fullLeader1Start, fullLeaderProbeSamples,
                       SSTV_SYNC_TONE_HZ + toneOffset),
            toneEnergy(sstvAudioBuffer, fullLeader1Start, fullLeaderProbeSamples,
                       SSTV_VIS_ZERO_TONE_HZ + toneOffset),
            toneEnergy(sstvAudioBuffer, fullLeader1Start, fullLeaderProbeSamples,
                       SSTV_MAX_TONE_HZ + toneOffset)});
        const double fullLeader2Comp = (std::max)({
            toneEnergy(sstvAudioBuffer, fullLeader2Start, fullLeaderProbeSamples,
                       SSTV_SYNC_TONE_HZ + toneOffset),
            toneEnergy(sstvAudioBuffer, fullLeader2Start, fullLeaderProbeSamples,
                       SSTV_VIS_ZERO_TONE_HZ + toneOffset),
            toneEnergy(sstvAudioBuffer, fullLeader2Start, fullLeaderProbeSamples,
                       SSTV_MAX_TONE_HZ + toneOffset)});
        const double shortLeaderRatio = (leader1 / (leader1Comp + 1.0e-8) +
                                         leader2 / (leader2Comp + 1.0e-8)) * 0.5;
        const double fullLeaderRatio = (fullLeader1 / (fullLeader1Comp + 1.0e-8) +
                                        fullLeader2 / (fullLeader2Comp + 1.0e-8)) * 0.5;
        const double leaderRatio = shortLeaderRatio * 0.25 + fullLeaderRatio * 0.75;
        if (shortLeaderRatio < 1.45 || fullLeaderRatio < 2.2 ||
            fullLeader1 < 5.0e-6 || fullLeader2 < 5.0e-6) {
            continue;
        }

        const double breakEnergy = toneEnergy(sstvAudioBuffer, breakStart, breakSamples,
                                              SSTV_SYNC_TONE_HZ + toneOffset);
        const double breakComp = (std::max)(
            toneEnergy(sstvAudioBuffer, breakStart, breakSamples,
                       SSTV_VIS_LEADER_TONE_HZ + toneOffset),
            toneEnergy(sstvAudioBuffer, breakStart, breakSamples,
                       SSTV_VIS_ZERO_TONE_HZ + toneOffset));
        const double startBit = toneEnergy(sstvAudioBuffer,
                                           visStart + bitProbeOffset,
                                           bitProbeSamples,
                                           SSTV_SYNC_TONE_HZ + toneOffset);
        const double startComp = (std::max)(
            toneEnergy(sstvAudioBuffer, visStart + bitProbeOffset, bitProbeSamples,
                       SSTV_VIS_ZERO_TONE_HZ + toneOffset),
            toneEnergy(sstvAudioBuffer, visStart + bitProbeOffset, bitProbeSamples,
                       SSTV_VIS_ONE_TONE_HZ + toneOffset));
        const double stopStart = visStart + bitSamples * 9;
        const double stopBit = toneEnergy(sstvAudioBuffer,
                                          stopStart + bitProbeOffset,
                                          bitProbeSamples,
                                          SSTV_SYNC_TONE_HZ + toneOffset);
        const double stopComp = (std::max)(
            toneEnergy(sstvAudioBuffer, stopStart + bitProbeOffset, bitProbeSamples,
                       SSTV_VIS_ZERO_TONE_HZ + toneOffset),
            toneEnergy(sstvAudioBuffer, stopStart + bitProbeOffset, bitProbeSamples,
                       SSTV_VIS_ONE_TONE_HZ + toneOffset));

        const double breakRatio = breakEnergy / (breakComp + 1.0e-8);
        const double startRatio = startBit / (startComp + 1.0e-8);
        const double stopRatio = stopBit / (stopComp + 1.0e-8);
        if (breakRatio < 1.45 || startRatio < 1.45 || stopRatio < 1.45) {
            continue;
        }
        const double framingRatio = (breakRatio + startRatio + stopRatio) / 3.0;

        int code = 0;
        int oneCount = 0;
        double bitConfidence = 0.0;
        double weakestBitEnergy = std::numeric_limits<double>::max();
        for (int bit = 0; bit < 8; ++bit) {
            const int bitStart = visStart + bitSamples * (bit + 1);
            const double oneEnergy = toneEnergy(sstvAudioBuffer,
                                                bitStart + bitProbeOffset,
                                                bitProbeSamples,
                                                SSTV_VIS_ONE_TONE_HZ + toneOffset);
            const double zeroEnergy = toneEnergy(sstvAudioBuffer,
                                                 bitStart + bitProbeOffset,
                                                 bitProbeSamples,
                                                 SSTV_VIS_ZERO_TONE_HZ + toneOffset);
            const bool one = oneEnergy > zeroEnergy;
            weakestBitEnergy = (std::min)(weakestBitEnergy,
                                          (std::max)(oneEnergy, zeroEnergy));
            const double ratio = (std::max)(oneEnergy, zeroEnergy) /
                                 ((std::min)(oneEnergy, zeroEnergy) + 1.0e-8);
            bitConfidence += (std::min)(ratio, 8.0);
            if (one) {
                if (bit < 7) {
                    code |= (1 << bit);
                }
                ++oneCount;
            }
        }
        bitConfidence /= 8.0;
        const bool evenParity = (oneCount % 2) == 0;
        const double combined = (std::min)(leaderRatio, 10.0) * 0.45 +
                                (std::min)(framingRatio, 10.0) * 0.35 +
                                bitConfidence * 0.20;

        if (evenParity && weakestBitEnergy > 5.0e-6 && combined > bestConfidence) {
            bestConfidence = combined;
            bestCode = code;
            bestHeaderEnd = start + headerSamples;
            bestToneOffset = toneOffset;
        }
    }

    if (confidence) {
        *confidence = bestConfidence;
    }
    if (headerEndSample) {
        *headerEndSample = bestHeaderEnd;
    }
    const int lastChecked = firstStart + ((maxStart - firstStart) / step) * step;
    sstvVisSearchOffset = lastChecked + step;
    if (bestConfidence >= 2.5) {
        sstvToneOffsetHz = bestToneOffset;
        return bestCode;
    }
    return -1;
}

bool VideoProcessor::isSupportedSstvVisCode(int visCode) const {
    return sstvModeSpecForVis(visCode) != nullptr;
}

QString VideoProcessor::sstvModeName(int visCode) const {
    if (const SstvModeSpec *mode = sstvModeSpecForVis(visCode)) {
        return QString::fromLatin1(mode->name);
    }
    return QStringLiteral("unknown mode");
}

int VideoProcessor::activeSstvWidth() const {
    if (const SstvModeSpec *mode = sstvModeSpecForVis(sstvLastVisCode)) {
        return mode->width;
    }
    return SSTV_WIDTH;
}

int VideoProcessor::activeSstvHeight() const {
    if (const SstvModeSpec *mode = sstvModeSpecForVis(sstvLastVisCode)) {
        return mode->height;
    }
    return SSTV_HEIGHT;
}

double VideoProcessor::activeSstvLineSeconds() const {
    if (const SstvModeSpec *mode = sstvModeSpecForVis(sstvLastVisCode)) {
        return mode->scanLineSeconds;
    }
    return ROBOT36_LINE_SECONDS;
}

double VideoProcessor::activeSstvSyncSeconds() const {
    if (const SstvModeSpec *mode = sstvModeSpecForVis(sstvLastVisCode)) {
        return mode->syncSeconds;
    }
    return ROBOT36_SYNC_SECONDS;
}

VideoProcessor::SstvDecodeFamily VideoProcessor::activeSstvFamily() const {
    if (const SstvModeSpec *mode = sstvModeSpecForVis(sstvLastVisCode)) {
        switch (mode->family) {
        case SstvSpecFamily::Robot72:
            return SstvDecodeFamily::Robot72;
        case SstvSpecFamily::Rgb:
            return SstvDecodeFamily::Rgb;
        case SstvSpecFamily::Pd:
            return SstvDecodeFamily::Pd;
        case SstvSpecFamily::Robot36:
        default:
            return SstvDecodeFamily::Robot36;
        }
    }
    return SstvDecodeFamily::Robot36;
}

void VideoProcessor::decodeSstvLine(int lineStart) {
    switch (activeSstvFamily()) {
    case SstvDecodeFamily::Robot72:
        decodeRobot72Line(lineStart);
        break;
    case SstvDecodeFamily::Rgb:
        decodeRgbSstvLine(lineStart);
        break;
    case SstvDecodeFamily::Pd:
        decodePdSstvLine(lineStart);
        break;
    case SstvDecodeFamily::Robot36:
    default:
        decodeRobot36Line(lineStart);
        break;
    }
}

void VideoProcessor::decodeRobot36Line(int lineStart) {
    if (sstvRaster.isNull() || sstvRaster.size() != QSize(SSTV_WIDTH, SSTV_HEIGHT)) {
        sstvRaster = QImage(SSTV_WIDTH, SSTV_HEIGHT, QImage::Format_RGB32);
        sstvRaster.fill(QColor(8, 10, 12));
    }

    const double timing = sstvLineTimingScale;
    const int syncSamples = static_cast<int>(std::lround(ROBOT36_SYNC_SECONDS * sstvSampleRate * timing));
    const int porchSamples = static_cast<int>(std::lround(ROBOT36_PORCH_SECONDS * sstvSampleRate * timing));
    const int lumaSamples = static_cast<int>(std::lround(ROBOT36_LUMA_SECONDS * sstvSampleRate * timing));
    const int separatorSamples = static_cast<int>(std::lround(ROBOT36_SEPARATOR_SECONDS * sstvSampleRate * timing));
    const int midPorchSamples = static_cast<int>(std::lround(ROBOT36_MID_PORCH_SECONDS * sstvSampleRate * timing));
    const int chromaSamples = static_cast<int>(std::lround(ROBOT36_CHROMA_SECONDS * sstvSampleRate * timing));
    const int lumaStart = lineStart + syncSamples + porchSamples;
    const int separatorStart = lumaStart + lumaSamples;
    const int chromaStart = separatorStart + separatorSamples + midPorchSamples;

    std::array<uchar, SSTV_WIDTH> luma = {};
    std::array<uchar, SSTV_WIDTH / 2> chroma = {};

    const std::vector<uchar> decodedLuma =
        decodeSstvToneSpan(lumaStart, lumaSamples, SSTV_WIDTH);
    const std::vector<uchar> decodedChroma =
        decodeSstvToneSpan(chromaStart, chromaSamples, SSTV_WIDTH / 2);
    std::copy_n(decodedLuma.begin(), SSTV_WIDTH, luma.begin());
    std::copy_n(decodedChroma.begin(), SSTV_WIDTH / 2, chroma.begin());
    sstvMedianFilter3(luma);
    sstvMedianFilter3(chroma);

    const int separatorCenter = separatorStart + separatorSamples / 2;
    const double separatorHz = estimateToneFrequency(sstvAudioBuffer,
                                                     separatorCenter,
                                                     (std::max)(24, separatorSamples),
                                                     1200.0 + sstvToneOffsetHz,
                                                     2300.0 + sstvToneOffsetHz);
    const bool currentLineCarriesV = separatorHz < 1900.0 + sstvToneOffsetHz;
    const int lineIndex = sstvNextLine % SSTV_HEIGHT;

    if (currentLineCarriesV) {
        sstvEvenLuma = luma;
        sstvEvenV = chroma;
        sstvHaveEvenLine = true;
        renderSstvLine(lineIndex, luma, nullptr, &chroma);
    } else {
        if (sstvHaveEvenLine) {
            const int evenLineIndex = (lineIndex + SSTV_HEIGHT - 1) % SSTV_HEIGHT;
            renderSstvLine(evenLineIndex, sstvEvenLuma, &chroma, &sstvEvenV);
            renderSstvLine(lineIndex, luma, &chroma, &sstvEvenV);
            sstvHaveEvenLine = false;
        } else {
            renderSstvLine(lineIndex, luma, &chroma, nullptr);
        }
    }

    sstvNextLine = (lineIndex + 1) % SSTV_HEIGHT;
    ++sstvDecodedRows;
    ++sstvLinesSinceFrame;
    if (sstvFrameTimer.elapsed() >= 250 || sstvLinesSinceFrame >= 4) {
        emit frameReady(sstvRaster.copy());
        sstvFrameTimer.restart();
        sstvLinesSinceFrame = 0;
    }

    if (!sstvStatusTimer.isValid() || sstvStatusTimer.elapsed() >= STATUS_INTERVAL_MS) {
        emitStatus(QStringLiteral("SSTV Robot36: line %1/%2, sync %3, sep %4 Hz")
                       .arg(lineIndex + 1)
                       .arg(SSTV_HEIGHT)
                       .arg(lastSstvSyncScore, 0, 'f', 1)
                       .arg(separatorHz, 0, 'f', 0),
                   true);
        sstvStatusTimer.restart();
    }
}

void VideoProcessor::decodeRobot72Line(int lineStart) {
    if (sstvRaster.isNull() || sstvRaster.size() != QSize(SSTV_WIDTH, SSTV_HEIGHT)) {
        sstvRaster = QImage(SSTV_WIDTH, SSTV_HEIGHT, QImage::Format_RGB32);
        sstvRaster.fill(QColor(8, 10, 12));
    }

    const double timing = sstvLineTimingScale;
    const int syncSamples = static_cast<int>(std::lround(ROBOT36_SYNC_SECONDS * sstvSampleRate * timing));
    const int porchSamples = static_cast<int>(std::lround(ROBOT36_PORCH_SECONDS * sstvSampleRate * timing));
    const int lumaSamples = static_cast<int>(std::lround(0.138 * sstvSampleRate * timing));
    const int separatorSamples = static_cast<int>(std::lround(ROBOT36_SEPARATOR_SECONDS * sstvSampleRate * timing));
    const int chromaSamples = static_cast<int>(std::lround(0.069 * sstvSampleRate * timing));
    const int lumaStart = lineStart + syncSamples + porchSamples;
    const int vStart = lumaStart + lumaSamples + separatorSamples;
    const int uStart = vStart + chromaSamples + separatorSamples;

    std::array<uchar, SSTV_WIDTH> luma = {};
    std::array<uchar, SSTV_WIDTH / 2> uChroma = {};
    std::array<uchar, SSTV_WIDTH / 2> vChroma = {};

    const std::vector<uchar> decodedLuma =
        decodeSstvToneSpan(lumaStart, lumaSamples, SSTV_WIDTH);
    const std::vector<uchar> decodedV =
        decodeSstvToneSpan(vStart, chromaSamples, SSTV_WIDTH / 2);
    const std::vector<uchar> decodedU =
        decodeSstvToneSpan(uStart, chromaSamples, SSTV_WIDTH / 2);
    std::copy_n(decodedLuma.begin(), SSTV_WIDTH, luma.begin());
    std::copy_n(decodedV.begin(), SSTV_WIDTH / 2, vChroma.begin());
    std::copy_n(decodedU.begin(), SSTV_WIDTH / 2, uChroma.begin());
    sstvMedianFilter3(luma);
    sstvMedianFilter3(uChroma);
    sstvMedianFilter3(vChroma);

    const int lineIndex = sstvNextLine % SSTV_HEIGHT;
    renderSstvLine(lineIndex, luma, &uChroma, &vChroma);
    sstvNextLine = (lineIndex + 1) % SSTV_HEIGHT;
    ++sstvDecodedRows;
    ++sstvLinesSinceFrame;
    if (sstvFrameTimer.elapsed() >= 250 || sstvLinesSinceFrame >= 4) {
        emit frameReady(sstvRaster.copy());
        sstvFrameTimer.restart();
        sstvLinesSinceFrame = 0;
    }
    if (!sstvStatusTimer.isValid() || sstvStatusTimer.elapsed() >= STATUS_INTERVAL_MS) {
        emitStatus(QStringLiteral("SSTV Robot72: line %1/%2, sync %3")
                       .arg(lineIndex + 1)
                       .arg(SSTV_HEIGHT)
                       .arg(lastSstvSyncScore, 0, 'f', 1),
                   true);
        sstvStatusTimer.restart();
    }
}

void VideoProcessor::decodeRgbSstvLine(int lineStart) {
    const SstvModeSpec *mode = sstvModeSpecForVis(sstvLastVisCode);
    if (!mode) {
        return;
    }
    if (sstvRaster.isNull() || sstvRaster.size() != QSize(mode->width, mode->height)) {
        sstvRaster = QImage(mode->width, mode->height, QImage::Format_RGB32);
        sstvRaster.fill(QColor(8, 10, 12));
    }

    const double timing = sstvLineTimingScale;
    const int syncSamples = static_cast<int>(std::lround(mode->syncSeconds * sstvSampleRate * timing));
    const int channelSamples = static_cast<int>(std::lround(mode->channelSeconds * sstvSampleRate * timing));
    const int separatorSamples = static_cast<int>(std::lround(mode->separatorSeconds * sstvSampleRate * timing));
    const int frequencyWindow = sstvPixelFrequencyWindow(channelSamples, mode->width, sstvSampleRate);

    auto decodeChannel = [&](int channelStart, std::array<uchar, SSTV_WIDTH> &dst) {
        for (int x = 0; x < mode->width; ++x) {
            const int center = channelStart + static_cast<int>(std::lround(
                                          (x + 0.5) * static_cast<double>(channelSamples) / mode->width));
            dst[static_cast<std::size_t>(x)] =
                sstvFrequencyToByte(estimateToneFrequency(sstvAudioBuffer,
                                                           center,
                                                           frequencyWindow,
                                                           SSTV_MIN_TONE_HZ + sstvToneOffsetHz,
                                                           SSTV_MAX_TONE_HZ + sstvToneOffsetHz));
        }
    };

    std::array<uchar, SSTV_WIDTH> red = {};
    std::array<uchar, SSTV_WIDTH> green = {};
    std::array<uchar, SSTV_WIDTH> blue = {};

    if (sstvLastVisCode == 60 || sstvLastVisCode == 56 || sstvLastVisCode == 76) {
        if (!sstvScottiePrimed) {
            // Scottie starts with an extra sync followed by G0/B0. Regular
            // line syncs then sit between B and R, so retain these two
            // channels until R0 arrives after the next sync.
            const int greenStart = lineStart + syncSamples + separatorSamples;
            const int blueStart = greenStart + channelSamples + separatorSamples;
            decodeChannel(greenStart, sstvScottieGreen);
            decodeChannel(blueStart, sstvScottieBlue);
            sstvMedianFilter3(sstvScottieGreen);
            sstvMedianFilter3(sstvScottieBlue);
            sstvLineAdvanceOverride = blueStart + channelSamples - lineStart;
            sstvScottiePrimed = true;
            return;
        }

        const int redStart = lineStart + syncSamples + separatorSamples;
        const int nextGreenStart = redStart + channelSamples + separatorSamples;
        const int nextBlueStart = nextGreenStart + channelSamples + separatorSamples;
        decodeChannel(redStart, red);
        decodeChannel(nextGreenStart, green);
        decodeChannel(nextBlueStart, blue);
        sstvMedianFilter3(red);
        sstvMedianFilter3(green);
        sstvMedianFilter3(blue);

        const int lineIndex = sstvNextLine % mode->height;
        renderRgbSstvLine(lineIndex, red, sstvScottieGreen, sstvScottieBlue);
        sstvScottieGreen = green;
        sstvScottieBlue = blue;
        sstvNextLine = (lineIndex + 1) % mode->height;
        ++sstvDecodedRows;
        ++sstvLinesSinceFrame;
        if (sstvFrameTimer.elapsed() >= 250 || sstvLinesSinceFrame >= 3) {
            emit frameReady(sstvRaster.copy());
            sstvFrameTimer.restart();
            sstvLinesSinceFrame = 0;
        }
        if (!sstvStatusTimer.isValid() || sstvStatusTimer.elapsed() >= STATUS_INTERVAL_MS) {
            emitStatus(QStringLiteral("SSTV %1: line %2/%3, sync %4, timing %5%")
                           .arg(sstvModeName(sstvLastVisCode))
                           .arg(lineIndex + 1)
                           .arg(mode->height)
                           .arg(lastSstvSyncScore, 0, 'f', 1)
                           .arg(sstvLineTimingScale * 100.0, 0, 'f', 3),
                       true);
            sstvStatusTimer.restart();
        }
        return;
    } else if (sstvLastVisCode == 55) {
        const int redStart = lineStart + syncSamples + separatorSamples;
        const int greenStart = redStart + channelSamples;
        const int blueStart = greenStart + channelSamples;
        decodeChannel(redStart, red);
        decodeChannel(greenStart, green);
        decodeChannel(blueStart, blue);
    } else {
        const int greenStart = lineStart + syncSamples + separatorSamples;
        const int blueStart = greenStart + channelSamples + separatorSamples;
        const int redStart = blueStart + channelSamples + separatorSamples;
        decodeChannel(redStart, red);
        decodeChannel(greenStart, green);
        decodeChannel(blueStart, blue);
    }

    sstvMedianFilter3(red);
    sstvMedianFilter3(green);
    sstvMedianFilter3(blue);

    const int lineIndex = sstvNextLine % mode->height;
    renderRgbSstvLine(lineIndex, red, green, blue);
    sstvNextLine = (lineIndex + 1) % mode->height;
    ++sstvDecodedRows;
    ++sstvLinesSinceFrame;
    if (sstvFrameTimer.elapsed() >= 250 || sstvLinesSinceFrame >= 3) {
        emit frameReady(sstvRaster.copy());
        sstvFrameTimer.restart();
        sstvLinesSinceFrame = 0;
    }
    if (!sstvStatusTimer.isValid() || sstvStatusTimer.elapsed() >= STATUS_INTERVAL_MS) {
        emitStatus(QStringLiteral("SSTV %1: line %2/%3, sync %4")
                       .arg(sstvModeName(sstvLastVisCode))
                       .arg(lineIndex + 1)
                       .arg(mode->height)
                       .arg(lastSstvSyncScore, 0, 'f', 1),
                   true);
        sstvStatusTimer.restart();
    }
}

void VideoProcessor::renderRgbSstvLine(int lineIndex,
                                       const std::array<uchar, 320> &red,
                                       const std::array<uchar, 320> &green,
                                       const std::array<uchar, 320> &blue) {
    if (sstvRaster.isNull() || lineIndex < 0 || lineIndex >= sstvRaster.height()) {
        return;
    }

    QRgb *line = reinterpret_cast<QRgb *>(sstvRaster.scanLine(lineIndex));
    const int renderWidth = (std::min)(sstvRaster.width(), SSTV_WIDTH);
    for (int x = 0; x < renderWidth; ++x) {
        line[x] = qRgb(red[static_cast<std::size_t>(x)],
                       green[static_cast<std::size_t>(x)],
                       blue[static_cast<std::size_t>(x)]);
    }
}

void VideoProcessor::decodePdSstvLine(int lineStart) {
    const SstvModeSpec *mode = sstvModeSpecForVis(sstvLastVisCode);
    if (!mode) {
        return;
    }
    if (sstvRaster.isNull() || sstvRaster.size() != QSize(mode->width, mode->height)) {
        sstvRaster = QImage(mode->width, mode->height, QImage::Format_RGB32);
        sstvRaster.fill(QColor(8, 10, 12));
    }

    const double timing = sstvLineTimingScale;
    const int syncSamples = static_cast<int>(std::lround(mode->syncSeconds * sstvSampleRate * timing));
    const int porchSamples = static_cast<int>(std::lround(mode->separatorSeconds * sstvSampleRate * timing));
    const int channelSamples = static_cast<int>(std::lround(mode->channelSeconds * sstvSampleRate * timing));
    const int frequencyWindow = sstvPixelFrequencyWindow(channelSamples, mode->width, sstvSampleRate);
    const int baseStart = lineStart + syncSamples + porchSamples;

    auto decodeChannelValue = [&](int channelStart, int x) -> uchar {
        const int center = channelStart + static_cast<int>(std::lround(
                                      (x + 0.5) * static_cast<double>(channelSamples) / mode->width));
        return sstvFrequencyToByte(estimateToneFrequency(sstvAudioBuffer,
                                                          center,
                                                          frequencyWindow,
                                                          SSTV_MIN_TONE_HZ + sstvToneOffsetHz,
                                                          SSTV_MAX_TONE_HZ + sstvToneOffsetHz));
    };

    const int yEvenStart = baseStart;
    const int vStart = yEvenStart + channelSamples;
    const int uStart = vStart + channelSamples;
    const int yOddStart = uStart + channelSamples;
    const int linePair = sstvNextLine % (std::max)(1, mode->height / 2);
    const int evenLineIndex = linePair * 2;
    const int oddLineIndex = (std::min)(evenLineIndex + 1, mode->height - 1);

    QRgb *evenLine = reinterpret_cast<QRgb *>(sstvRaster.scanLine(evenLineIndex));
    QRgb *oddLine = reinterpret_cast<QRgb *>(sstvRaster.scanLine(oddLineIndex));
    for (int x = 0; x < mode->width; ++x) {
        const double yEven = decodeChannelValue(yEvenStart, x);
        const double v = decodeChannelValue(vStart, x) - 128.0;
        const double u = decodeChannelValue(uStart, x) - 128.0;
        const double yOdd = decodeChannelValue(yOddStart, x);
        const int evenR = (std::clamp)(static_cast<int>(std::lround(yEven + 1.402 * v)), 0, 255);
        const int evenG = (std::clamp)(static_cast<int>(std::lround(yEven - 0.344136 * u - 0.714136 * v)), 0, 255);
        const int evenB = (std::clamp)(static_cast<int>(std::lround(yEven + 1.772 * u)), 0, 255);
        const int oddR = (std::clamp)(static_cast<int>(std::lround(yOdd + 1.402 * v)), 0, 255);
        const int oddG = (std::clamp)(static_cast<int>(std::lround(yOdd - 0.344136 * u - 0.714136 * v)), 0, 255);
        const int oddB = (std::clamp)(static_cast<int>(std::lround(yOdd + 1.772 * u)), 0, 255);
        evenLine[x] = qRgb(evenR, evenG, evenB);
        oddLine[x] = qRgb(oddR, oddG, oddB);
    }

    sstvNextLine = (linePair + 1) % (std::max)(1, mode->height / 2);
    sstvDecodedRows += (evenLineIndex == oddLineIndex) ? 1 : 2;
    ++sstvLinesSinceFrame;
    if (sstvFrameTimer.elapsed() >= 250 || sstvLinesSinceFrame >= 2) {
        emit frameReady(sstvRaster.copy());
        sstvFrameTimer.restart();
        sstvLinesSinceFrame = 0;
    }
    if (!sstvStatusTimer.isValid() || sstvStatusTimer.elapsed() >= STATUS_INTERVAL_MS) {
        emitStatus(QStringLiteral("SSTV %1: lines %2-%3/%4, sync %5")
                       .arg(sstvModeName(sstvLastVisCode))
                       .arg(evenLineIndex + 1)
                       .arg(oddLineIndex + 1)
                       .arg(mode->height)
                       .arg(lastSstvSyncScore, 0, 'f', 1),
                   true);
        sstvStatusTimer.restart();
    }
}

void VideoProcessor::renderSstvLine(int lineIndex,
                                    const std::array<uchar, 320> &luma,
                                    const std::array<uchar, 160> *uChroma,
                                    const std::array<uchar, 160> *vChroma) {
    if (sstvRaster.isNull() || lineIndex < 0 || lineIndex >= sstvRaster.height()) {
        return;
    }

    QRgb *line = reinterpret_cast<QRgb *>(sstvRaster.scanLine(lineIndex));
    for (int x = 0; x < SSTV_WIDTH; ++x) {
        const int chromaIndex = (std::min)(SSTV_WIDTH / 2 - 1, x / 2);
        const double y = luma[static_cast<std::size_t>(x)];
        const double u = (uChroma ? (*uChroma)[static_cast<std::size_t>(chromaIndex)] : 128) - 128.0;
        const double v = (vChroma ? (*vChroma)[static_cast<std::size_t>(chromaIndex)] : 128) - 128.0;
        const int r = (std::clamp)(static_cast<int>(std::lround(y + 1.402 * v)), 0, 255);
        const int g = (std::clamp)(static_cast<int>(std::lround(y - 0.344136 * u - 0.714136 * v)), 0, 255);
        const int b = (std::clamp)(static_cast<int>(std::lround(y + 1.772 * u)), 0, 255);
        line[x] = qRgb(r, g, b);
    }
}

double VideoProcessor::toneEnergy(const std::vector<float> &samples,
                                  int start,
                                  int length,
                                  double frequencyHz) const {
    if (length <= 0 || frequencyHz <= 0.0 || sstvSampleRate <= 0) {
        return 0.0;
    }
    start = (std::max)(0, start);
    const int end = (std::min)(static_cast<int>(samples.size()), start + length);
    if (end - start <= 4) {
        return 0.0;
    }

    double mean = 0.0;
    for (int i = start; i < end; ++i) {
        mean += samples[static_cast<std::size_t>(i)];
    }
    mean /= static_cast<double>(end - start);

    const double omega = TWO_PI * frequencyHz / sstvSampleRate;
    const double coefficient = 2.0 * std::cos(omega);
    double previous = 0.0;
    double previous2 = 0.0;
    for (int i = start; i < end; ++i) {
        const double sample = samples[static_cast<std::size_t>(i)] - mean;
        const double current = sample + coefficient * previous - previous2;
        previous2 = previous;
        previous = current;
    }
    const double norm = static_cast<double>(end - start);
    const double power = previous * previous + previous2 * previous2 -
                         coefficient * previous * previous2;
    return (std::max)(0.0, power) / (norm * norm + 1.0e-12);
}

double VideoProcessor::estimateToneFrequency(const std::vector<float> &samples,
                                             int center,
                                             int windowSamples,
                                             double minHz,
                                             double maxHz) const {
    if (samples.empty() || sstvSampleRate <= 0) {
        return minHz;
    }
    windowSamples = (std::max)(16, windowSamples);
    const int start = (std::clamp)(center - windowSamples / 2, 0, static_cast<int>(samples.size()) - 1);
    const int end = (std::clamp)(center + windowSamples / 2, start + 1, static_cast<int>(samples.size()));

    double firstCrossing = -1.0;
    double lastCrossing = -1.0;
    int crossingCount = 0;
    for (int i = start + 1; i < end; ++i) {
        const double prev = samples[static_cast<std::size_t>(i - 1)];
        const double current = samples[static_cast<std::size_t>(i)];
        if (prev < 0.0 && current >= 0.0) {
            const double denom = current - prev;
            const double frac = std::abs(denom) > 1.0e-9 ? -prev / denom : 0.0;
            const double crossing = static_cast<double>(i - 1) + frac;
            if (firstCrossing < 0.0) {
                firstCrossing = crossing;
            }
            lastCrossing = crossing;
            ++crossingCount;
        }
    }

    if (crossingCount >= 2 && lastCrossing > firstCrossing) {
        const double hz = (crossingCount - 1) * static_cast<double>(sstvSampleRate) /
                          (lastCrossing - firstCrossing);
        if (hz >= minHz * 0.85 && hz <= maxHz * 1.15) {
            return (std::clamp)(hz, minHz, maxHz);
        }
    }

    double bestEnergy = 0.0;
    double bestHz = minHz;
    for (double hz = minHz; hz <= maxHz + 0.1; hz += 50.0) {
        const double energy = toneEnergy(samples, start, end - start, hz);
        if (energy > bestEnergy) {
            bestEnergy = energy;
            bestHz = hz;
        }
    }

    // A short window is important for SSTV detail, but a 50 Hz-only bank
    // produces visible banding. Refine locally without widening the window
    // across neighbouring pixels.
    const double refineStart = (std::max)(minHz, bestHz - 50.0);
    const double refineEnd = (std::min)(maxHz, bestHz + 50.0);
    for (double hz = refineStart; hz <= refineEnd + 0.1; hz += 5.0) {
        const double energy = toneEnergy(samples, start, end - start, hz);
        if (energy > bestEnergy) {
            bestEnergy = energy;
            bestHz = hz;
        }
    }
    return bestHz;
}

std::vector<uchar> VideoProcessor::decodeSstvToneSpan(int start,
                                                      int spanSamples,
                                                      int pixelCount) const {
    std::vector<uchar> result(static_cast<std::size_t>((std::max)(0, pixelCount)), 0);
    if (pixelCount <= 0 || spanSamples <= 0 || sstvSampleRate <= 0 ||
        start < 0 || start >= static_cast<int>(sstvAudioBuffer.size())) {
        return result;
    }

    const int end = (std::min)(start + spanSamples,
                               static_cast<int>(sstvAudioBuffer.size()));
    const int mixerWindow = (std::clamp)(sstvSampleRate / 4000, 8, 24);
    const int warmupStart = (std::max)(0, start - mixerWindow * 3);
    std::vector<double> ringI(static_cast<std::size_t>(mixerWindow), 0.0);
    std::vector<double> ringQ(static_cast<std::size_t>(mixerWindow), 0.0);
    std::vector<double> frequencySum(static_cast<std::size_t>(pixelCount), 0.0);
    std::vector<double> weightSum(static_cast<std::size_t>(pixelCount), 0.0);

    const double mixerHz = 1900.0 + sstvToneOffsetHz;
    const double phaseStep = TWO_PI * mixerHz / sstvSampleRate;
    double phase = std::remainder(phaseStep * warmupStart, TWO_PI);
    double sumI = 0.0;
    double sumQ = 0.0;
    double previousI = 0.0;
    double previousQ = 0.0;
    bool havePrevious = false;
    int filled = 0;
    int ringIndex = 0;

    for (int sampleIndex = warmupStart; sampleIndex < end; ++sampleIndex) {
        const double sample = sstvAudioBuffer[static_cast<std::size_t>(sampleIndex)];
        const double mixedI = sample * std::cos(phase);
        const double mixedQ = -sample * std::sin(phase);
        phase = std::remainder(phase + phaseStep, TWO_PI);

        sumI -= ringI[static_cast<std::size_t>(ringIndex)];
        sumQ -= ringQ[static_cast<std::size_t>(ringIndex)];
        ringI[static_cast<std::size_t>(ringIndex)] = mixedI;
        ringQ[static_cast<std::size_t>(ringIndex)] = mixedQ;
        sumI += mixedI;
        sumQ += mixedQ;
        ringIndex = (ringIndex + 1) % mixerWindow;
        filled = (std::min)(mixerWindow, filled + 1);
        if (filled < mixerWindow) {
            continue;
        }

        const double currentI = sumI / mixerWindow;
        const double currentQ = sumQ / mixerWindow;
        const int effectiveIndex = sampleIndex - (mixerWindow - 1) / 2;
        if (havePrevious && effectiveIndex >= start && effectiveIndex < end) {
            const double real = currentI * previousI + currentQ * previousQ;
            const double imag = currentQ * previousI - currentI * previousQ;
            const double amplitude = std::sqrt((currentI * currentI + currentQ * currentQ) *
                                               (previousI * previousI + previousQ * previousQ));
            const double frequency = mixerHz +
                                     std::atan2(imag, real) * sstvSampleRate / TWO_PI;
            if (amplitude > 1.0e-7 &&
                frequency >= SSTV_MIN_TONE_HZ + sstvToneOffsetHz - 150.0 &&
                frequency <= SSTV_MAX_TONE_HZ + sstvToneOffsetHz + 150.0) {
                const int pixel = (std::clamp)(
                    static_cast<int>((static_cast<long long>(effectiveIndex - start) * pixelCount) /
                                     spanSamples),
                    0,
                    pixelCount - 1);
                frequencySum[static_cast<std::size_t>(pixel)] += frequency * amplitude;
                weightSum[static_cast<std::size_t>(pixel)] += amplitude;
            }
        }
        previousI = currentI;
        previousQ = currentQ;
        havePrevious = true;
    }

    for (int pixel = 0; pixel < pixelCount; ++pixel) {
        double frequency = 1900.0 + sstvToneOffsetHz;
        const double weight = weightSum[static_cast<std::size_t>(pixel)];
        if (weight > 0.0) {
            frequency = frequencySum[static_cast<std::size_t>(pixel)] / weight;
        } else {
            const int center = start + static_cast<int>(std::lround(
                (pixel + 0.5) * static_cast<double>(spanSamples) / pixelCount));
            frequency = estimateToneFrequency(sstvAudioBuffer,
                                              center,
                                              48,
                                              SSTV_MIN_TONE_HZ + sstvToneOffsetHz,
                                              SSTV_MAX_TONE_HZ + sstvToneOffsetHz);
        }
        result[static_cast<std::size_t>(pixel)] = sstvFrequencyToByte(frequency);
    }
    return result;
}

uchar VideoProcessor::sstvFrequencyToByte(double frequencyHz) const {
    const double correctedHz = frequencyHz - sstvToneOffsetHz;
    const double normalized = (correctedHz - SSTV_MIN_TONE_HZ) /
                              (SSTV_MAX_TONE_HZ - SSTV_MIN_TONE_HZ);
    return static_cast<uchar>((std::clamp)(static_cast<int>(std::lround(normalized * 255.0)), 0, 255));
}

void VideoProcessor::ensureSstvTestPatternTimer() {
    if (sstvTestPatternTimer) {
        return;
    }

    sstvTestPatternTimer = new QTimer(this);
    sstvTestPatternTimer->setTimerType(Qt::PreciseTimer);
    connect(sstvTestPatternTimer, &QTimer::timeout, this, &VideoProcessor::generateSstvTestPatternPcmFrame);
}

void VideoProcessor::generateSstvTestPatternPcmFrame() {
    if (!sstvTestPatternEnabled) {
        return;
    }

    constexpr int sampleRate = 48000;
    sstvSampleRate = sampleRate;
    QByteArray pcmData;
    pcmData.reserve(static_cast<int>(ROBOT36_LINE_SECONDS * sampleRate * 2.0 * SSTV_TEST_LINES_PER_TICK));

    if (sstvTestPatternLine == 0) {
        appendSstvVisHeader(pcmData, ROBOT36_VIS_CODE);
    }

    for (int line = 0; line < SSTV_TEST_LINES_PER_TICK; ++line) {
        const int y = sstvTestPatternLine % SSTV_HEIGHT;
        const bool vLine = (y % 2) == 0;
        appendSstvTone(pcmData, SSTV_SYNC_TONE_HZ,
                       static_cast<int>(std::lround(ROBOT36_SYNC_SECONDS * sampleRate)),
                       0.75);
        appendSstvTone(pcmData, SSTV_MIN_TONE_HZ,
                       static_cast<int>(std::lround(ROBOT36_PORCH_SECONDS * sampleRate)),
                       0.55);

        const int lumaSamples = static_cast<int>(std::lround(ROBOT36_LUMA_SECONDS * sampleRate));
        for (int n = 0; n < lumaSamples; ++n) {
            const int x = (std::clamp)(static_cast<int>(
                              static_cast<double>(n) * SSTV_WIDTH / (std::max)(1, lumaSamples)),
                              0,
                              SSTV_WIDTH - 1);
            const QRgb pixel = sstvTestPatternPixel(x, y);
            const double luminance = 0.299 * qRed(pixel) + 0.587 * qGreen(pixel) + 0.114 * qBlue(pixel);
            const double frequency = SSTV_MIN_TONE_HZ + (luminance / 255.0) * (SSTV_MAX_TONE_HZ - SSTV_MIN_TONE_HZ);
            appendSstvTone(pcmData, frequency, 1, 0.62);
        }

        appendSstvTone(pcmData, vLine ? 1500.0 : 2300.0,
                       static_cast<int>(std::lround(ROBOT36_SEPARATOR_SECONDS * sampleRate)),
                       0.62);
        appendSstvTone(pcmData, SSTV_MIN_TONE_HZ,
                       static_cast<int>(std::lround(ROBOT36_MID_PORCH_SECONDS * sampleRate)),
                       0.5);

        const int chromaSamples = static_cast<int>(std::lround(ROBOT36_CHROMA_SECONDS * sampleRate));
        for (int n = 0; n < chromaSamples; ++n) {
            const int x = (std::clamp)(static_cast<int>(
                              static_cast<double>(n) * (SSTV_WIDTH / 2) / (std::max)(1, chromaSamples)),
                              0,
                              SSTV_WIDTH / 2 - 1) * 2;
            const QRgb pixel = sstvTestPatternPixel(x, y);
            const double r = qRed(pixel);
            const double g = qGreen(pixel);
            const double b = qBlue(pixel);
            const double luminance = 0.299 * r + 0.587 * g + 0.114 * b;
            const double chromaValue = vLine
                                           ? (r - luminance) / 1.402 + 128.0
                                           : (b - luminance) / 1.772 + 128.0;
            const double clamped = (std::clamp)(chromaValue, 0.0, 255.0);
            const double frequency = SSTV_MIN_TONE_HZ + (clamped / 255.0) * (SSTV_MAX_TONE_HZ - SSTV_MIN_TONE_HZ);
            appendSstvTone(pcmData, frequency, 1, 0.62);
        }

        sstvTestPatternLine = (sstvTestPatternLine + 1) % SSTV_HEIGHT;
    }

    processSstvPcmFrame(pcmData, sampleRate);
}

void VideoProcessor::appendSstvVisHeader(QByteArray &pcmData, int visCode) {
    appendSstvTone(pcmData, SSTV_VIS_LEADER_TONE_HZ,
                   static_cast<int>(std::lround(SSTV_VIS_LEADER_SECONDS * 48000.0)),
                   0.72);
    appendSstvTone(pcmData, SSTV_SYNC_TONE_HZ,
                   static_cast<int>(std::lround(SSTV_VIS_BREAK_SECONDS * 48000.0)),
                   0.72);
    appendSstvTone(pcmData, SSTV_VIS_LEADER_TONE_HZ,
                   static_cast<int>(std::lround(SSTV_VIS_LEADER_SECONDS * 48000.0)),
                   0.72);
    appendSstvTone(pcmData, SSTV_SYNC_TONE_HZ,
                   static_cast<int>(std::lround(SSTV_VIS_BIT_SECONDS * 48000.0)),
                   0.72);

    int oneCount = 0;
    for (int bit = 0; bit < 7; ++bit) {
        const bool one = (visCode & (1 << bit)) != 0;
        if (one) {
            ++oneCount;
        }
        appendSstvTone(pcmData,
                       one ? SSTV_VIS_ONE_TONE_HZ : SSTV_VIS_ZERO_TONE_HZ,
                       static_cast<int>(std::lround(SSTV_VIS_BIT_SECONDS * 48000.0)),
                       0.72);
    }

    const bool parityOne = (oneCount % 2) != 0;
    appendSstvTone(pcmData,
                   parityOne ? SSTV_VIS_ONE_TONE_HZ : SSTV_VIS_ZERO_TONE_HZ,
                   static_cast<int>(std::lround(SSTV_VIS_BIT_SECONDS * 48000.0)),
                   0.72);
    appendSstvTone(pcmData, SSTV_SYNC_TONE_HZ,
                   static_cast<int>(std::lround(SSTV_VIS_BIT_SECONDS * 48000.0)),
                   0.72);
}

void VideoProcessor::appendSstvTone(QByteArray &pcmData,
                                    double frequencyHz,
                                    int sampleCount,
                                    double amplitude) {
    if (sampleCount <= 0) {
        return;
    }

    const double increment = TWO_PI * frequencyHz / 48000.0;
    const int oldSize = pcmData.size();
    pcmData.resize(oldSize + sampleCount * static_cast<int>(sizeof(qint16)));
    char *dst = pcmData.data() + oldSize;
    for (int i = 0; i < sampleCount; ++i) {
        const double sample = std::sin(sstvTestTonePhase) * amplitude;
        const qint16 value = static_cast<qint16>(std::lrint((std::clamp)(sample, -1.0, 1.0) * 32767.0));
        *dst++ = static_cast<char>(value & 0xff);
        *dst++ = static_cast<char>((value >> 8) & 0xff);
        sstvTestTonePhase = std::remainder(sstvTestTonePhase + increment, TWO_PI);
    }
}

QRgb VideoProcessor::sstvTestPatternPixel(int x, int y) const {
    static const QRgb bars[] = {
        qRgb(255, 255, 255),
        qRgb(255, 255, 0),
        qRgb(0, 255, 255),
        qRgb(0, 255, 0),
        qRgb(255, 0, 255),
        qRgb(255, 0, 0),
        qRgb(0, 0, 255),
        qRgb(16, 16, 16),
    };
    x = (std::clamp)(x, 0, SSTV_WIDTH - 1);
    y = (std::clamp)(y, 0, SSTV_HEIGHT - 1);
    if (y < SSTV_HEIGHT / 3) {
        return bars[(x * 8) / SSTV_WIDTH];
    }
    if (y < 2 * SSTV_HEIGHT / 3) {
        const int value = (x * 255) / (SSTV_WIDTH - 1);
        return qRgb(value, value, value);
    }

    const bool checker = (((x / 16) + (y / 16)) % 2) == 0;
    const int ramp = ((x + y) * 255) / (SSTV_WIDTH + SSTV_HEIGHT - 2);
    return checker ? qRgb(ramp, 80, 255 - ramp)
                   : qRgb(255 - ramp, ramp, 80);
}

void VideoProcessor::appendAptBrightnessSample(float brightness) {
    if (!std::isfinite(brightness)) {
        brightness = 0.0f;
    }
    const int value = (std::clamp)(static_cast<int>(std::lround(brightness * 255.0f)), 0, 255);
    aptLine.push_back(static_cast<uchar>(value));
    if (aptLine.size() >= APT_WIDTH) {
        renderAptLine();
        aptLine.clear();
    }
}

void VideoProcessor::renderAptLine() {
    if (aptRaster.isNull() || aptLine.size() < APT_WIDTH) {
        return;
    }

    uchar *line = aptRaster.scanLine(aptNextLine);
    std::memcpy(line, aptLine.data(), APT_WIDTH);
    aptNextLine = (aptNextLine + 1) % APT_DISPLAY_HEIGHT;
    ++aptLinesSinceFrame;

    if (aptFrameTimer.elapsed() >= 250 || aptLinesSinceFrame >= 4) {
        emit frameReady(aptRaster.copy());
        aptFrameTimer.restart();
        aptLinesSinceFrame = 0;
    }
    if (!aptStatusTimer.isValid() || aptStatusTimer.elapsed() >= STATUS_INTERVAL_MS) {
        emitStatus(QStringLiteral("NOAA APT: line %1, 2400 Hz envelope")
                       .arg(aptNextLine + 1),
                   true);
        aptStatusTimer.restart();
    }
}

void VideoProcessor::appendWefaxPixel(float brightness) {
    if (!std::isfinite(brightness)) {
        brightness = 0.0f;
    }
    const int value = (std::clamp)(static_cast<int>(std::lround(brightness * 255.0f)), 0, 255);
    wefaxLine.push_back(static_cast<uchar>(value));
    if (wefaxLine.size() >= WEFAX_WIDTH) {
        renderWefaxLine();
        wefaxLine.clear();
    }
}

void VideoProcessor::renderWefaxLine() {
    if (wefaxRaster.isNull() || wefaxLine.size() < WEFAX_WIDTH) {
        return;
    }

    uchar *line = wefaxRaster.scanLine(wefaxNextLine);
    std::memcpy(line, wefaxLine.data(), WEFAX_WIDTH);
    wefaxNextLine = (wefaxNextLine + 1) % WEFAX_DISPLAY_HEIGHT;
    ++wefaxLinesSinceFrame;
    if (wefaxFrameTimer.elapsed() >= 250 || wefaxLinesSinceFrame >= 4) {
        emit frameReady(wefaxRaster.copy());
        wefaxFrameTimer.restart();
        wefaxLinesSinceFrame = 0;
    }
    if (!wefaxStatusTimer.isValid() || wefaxStatusTimer.elapsed() >= STATUS_INTERVAL_MS) {
        emitStatus(QStringLiteral("WEFAX: line %1, 120 LPM beta")
                       .arg(wefaxNextLine + 1),
                   true);
        wefaxStatusTimer.restart();
    }
}

double VideoProcessor::estimateWefaxFrequency(const std::vector<float> &samples) const {
    if (samples.size() < 8 || wefaxSampleRate <= 0) {
        return WEFAX_BLACK_HZ;
    }

    double firstCrossing = -1.0;
    double lastCrossing = -1.0;
    int crossingCount = 0;
    for (int i = 1; i < static_cast<int>(samples.size()); ++i) {
        const double prev = samples[static_cast<std::size_t>(i - 1)];
        const double current = samples[static_cast<std::size_t>(i)];
        if (prev < 0.0 && current >= 0.0) {
            const double denom = current - prev;
            const double frac = std::abs(denom) > 1.0e-9 ? -prev / denom : 0.0;
            const double crossing = static_cast<double>(i - 1) + frac;
            if (firstCrossing < 0.0) {
                firstCrossing = crossing;
            }
            lastCrossing = crossing;
            ++crossingCount;
        }
    }

    if (crossingCount >= 2 && lastCrossing > firstCrossing) {
        return (std::clamp)((crossingCount - 1) * static_cast<double>(wefaxSampleRate) /
                                (lastCrossing - firstCrossing),
                            300.0,
                            2600.0);
    }

    return WEFAX_BLACK_HZ;
}

void VideoProcessor::ensureWefaxTestPatternTimer() {
    if (wefaxTestPatternTimer) {
        return;
    }

    wefaxTestPatternTimer = new QTimer(this);
    wefaxTestPatternTimer->setTimerType(Qt::PreciseTimer);
    connect(wefaxTestPatternTimer, &QTimer::timeout, this, &VideoProcessor::generateWefaxTestPatternPcmFrame);
}

void VideoProcessor::generateWefaxTestPatternPcmFrame() {
    if (!wefaxTestPatternEnabled) {
        return;
    }

    constexpr int sampleRate = 48000;
    wefaxSampleRate = sampleRate;
    QByteArray pcmData;
    pcmData.reserve(static_cast<int>(0.5 * sampleRate * sizeof(qint16)));
    const int sampleCount = static_cast<int>(std::lround(0.5 * sampleRate));
    const int y = wefaxTestLine;
    for (int n = 0; n < sampleCount; ++n) {
        const int x = (std::clamp)(static_cast<int>(
                          static_cast<double>(n) * WEFAX_WIDTH / (std::max)(1, sampleCount)),
                          0,
                          WEFAX_WIDTH - 1);
        const float brightness = wefaxTestPatternBrightness(x, y);
        const double frequency = WEFAX_BLACK_HZ + brightness * (WEFAX_WHITE_HZ - WEFAX_BLACK_HZ);
        appendWefaxTone(pcmData, x < 28 ? 300.0 : frequency, 1, 0.72);
    }
    wefaxTestLine = (wefaxTestLine + 1) % WEFAX_DISPLAY_HEIGHT;
    processWefaxPcmFrame(pcmData, sampleRate);
}

void VideoProcessor::appendWefaxTone(QByteArray &pcmData,
                                     double frequencyHz,
                                     int sampleCount,
                                     double amplitude) {
    if (sampleCount <= 0) {
        return;
    }

    const double increment = TWO_PI * frequencyHz / 48000.0;
    const int oldSize = pcmData.size();
    pcmData.resize(oldSize + sampleCount * static_cast<int>(sizeof(qint16)));
    char *dst = pcmData.data() + oldSize;
    for (int i = 0; i < sampleCount; ++i) {
        const double sample = std::sin(wefaxTestTonePhase) * amplitude;
        const qint16 value = static_cast<qint16>(std::lrint((std::clamp)(sample, -1.0, 1.0) * 32767.0));
        *dst++ = static_cast<char>(value & 0xff);
        *dst++ = static_cast<char>((value >> 8) & 0xff);
        wefaxTestTonePhase = std::remainder(wefaxTestTonePhase + increment, TWO_PI);
    }
}

float VideoProcessor::wefaxTestPatternBrightness(int x, int y) const {
    x = (std::clamp)(x, 0, WEFAX_WIDTH - 1);
    y = (std::max)(0, y);
    if (x < 28) {
        return 0.0f;
    }
    if (x < 96) {
        return ((x / 8) % 2) == 0 ? 0.1f : 0.9f;
    }
    if (y < WEFAX_DISPLAY_HEIGHT / 3) {
        return static_cast<float>(x - 96) / static_cast<float>((std::max)(1, WEFAX_WIDTH - 97));
    }
    if (y < 2 * WEFAX_DISPLAY_HEIGHT / 3) {
        return (0.5f + 0.35f * std::sin((x + y * 3) * 0.026f) +
                0.12f * std::sin((x - y * 5) * 0.061f));
    }
    return (((x / 24) + (y / 16)) % 2) == 0 ? 0.22f : 0.78f;
}

void VideoProcessor::ensureAptTestPatternTimer() {
    if (aptTestPatternTimer) {
        return;
    }

    aptTestPatternTimer = new QTimer(this);
    aptTestPatternTimer->setTimerType(Qt::PreciseTimer);
    connect(aptTestPatternTimer, &QTimer::timeout, this, &VideoProcessor::generateAptTestPatternPcmFrame);
}

void VideoProcessor::generateAptTestPatternPcmFrame() {
    if (!aptTestPatternEnabled) {
        return;
    }

    constexpr int sampleRate = 48000;
    aptSampleRate = sampleRate;
    QByteArray pcmData;
    pcmData.reserve(static_cast<int>(0.5 * sampleRate * sizeof(qint16) * APT_TEST_LINES_PER_TICK));

    for (int line = 0; line < APT_TEST_LINES_PER_TICK; ++line) {
        const int y = aptTestLine;
        const int sampleCount = static_cast<int>(std::lround(0.5 * sampleRate));
        for (int n = 0; n < sampleCount; ++n) {
            const int x = (std::clamp)(static_cast<int>(
                              static_cast<double>(n) * APT_WIDTH / (std::max)(1, sampleCount)),
                              0,
                              APT_WIDTH - 1);
            appendAptTone(pcmData, aptTestPatternBrightness(x, y), 1);
        }
        aptTestLine = (aptTestLine + 1) % APT_DISPLAY_HEIGHT;
    }

    processAptPcmFrame(pcmData, sampleRate);
}

void VideoProcessor::appendAptTone(QByteArray &pcmData, double amplitude, int sampleCount) {
    if (sampleCount <= 0) {
        return;
    }

    const double increment = TWO_PI * APT_SUBCARRIER_HZ / 48000.0;
    const int oldSize = pcmData.size();
    pcmData.resize(oldSize + sampleCount * static_cast<int>(sizeof(qint16)));
    char *dst = pcmData.data() + oldSize;
    const double clampedAmplitude = (std::clamp)(amplitude, 0.08, 0.95);
    for (int i = 0; i < sampleCount; ++i) {
        const double sample = std::sin(aptTestTonePhase) * clampedAmplitude;
        const qint16 value = static_cast<qint16>(std::lrint(sample * 32767.0));
        *dst++ = static_cast<char>(value & 0xff);
        *dst++ = static_cast<char>((value >> 8) & 0xff);
        aptTestTonePhase = std::remainder(aptTestTonePhase + increment, TWO_PI);
    }
}

float VideoProcessor::aptTestPatternBrightness(int x, int y) const {
    x = (std::clamp)(x, 0, APT_WIDTH - 1);
    y = (std::max)(0, y);
    if (x < 80) {
        return ((x / 8) % 2) == 0 ? 0.12f : 0.88f;
    }
    if (x < 1040) {
        const float cloud = 0.5f + 0.25f * std::sin((x + y * 7) * 0.018f) +
                            0.12f * std::sin((x - y * 4) * 0.047f);
        return (std::clamp)(cloud, 0.05f, 0.95f);
    }
    if (x < 1120) {
        return ((x / 10) % 2) == 0 ? 0.85f : 0.15f;
    }
    if (x < 2000) {
        const float gradient = static_cast<float>(x - 1120) / 880.0f;
        const float stripes = ((y / 16) % 2) == 0 ? 0.08f : -0.08f;
        return (std::clamp)(gradient + stripes, 0.05f, 0.95f);
    }
    return ((y / 8) % 2) == 0 ? 0.25f : 0.75f;
}

void VideoProcessor::configureLrptImage() {
    lrptRaster = QImage(LRPT_DISPLAY_SIZE, LRPT_DISPLAY_SIZE, QImage::Format_RGB32);
    lrptRaster.fill(QColor(8, 10, 12));
    QPainter painter(&lrptRaster);
    painter.setPen(QColor(60, 68, 74));
    painter.drawLine(LRPT_DISPLAY_SIZE / 2, 0, LRPT_DISPLAY_SIZE / 2, LRPT_DISPLAY_SIZE);
    painter.drawLine(0, LRPT_DISPLAY_SIZE / 2, LRPT_DISPLAY_SIZE, LRPT_DISPLAY_SIZE / 2);
    painter.drawEllipse(QPoint(LRPT_DISPLAY_SIZE / 2, LRPT_DISPLAY_SIZE / 2), 170, 170);
    painter.setPen(QColor(135, 145, 155));
    painter.drawText(lrptRaster.rect(), Qt::AlignCenter, QStringLiteral("Meteor LRPT beta\nQPSK IQ monitor"));
}

void VideoProcessor::processLrptFloatIqSnapshot(const std::vector<float> &iqSamples,
                                                const RadioSettings &settings) {
    if (!lrptEnabled || iqSamples.size() < 8 || settings.sampleRate <= 0.0) {
        return;
    }

    QImage image(LRPT_DISPLAY_SIZE, LRPT_DISPLAY_SIZE, QImage::Format_RGB32);
    image.fill(QColor(8, 10, 12));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QColor(46, 52, 58));
    painter.drawLine(LRPT_DISPLAY_SIZE / 2, 0, LRPT_DISPLAY_SIZE / 2, LRPT_DISPLAY_SIZE);
    painter.drawLine(0, LRPT_DISPLAY_SIZE / 2, LRPT_DISPLAY_SIZE, LRPT_DISPLAY_SIZE / 2);
    painter.drawEllipse(QPoint(LRPT_DISPLAY_SIZE / 2, LRPT_DISPLAY_SIZE / 2), 170, 170);

    const std::size_t iqCount = iqSamples.size() / 2;
    const std::size_t maxPoints = 14000;
    const std::size_t stride = (std::max<std::size_t>)(1, iqCount / maxPoints);
    const double fShift = settings.listeningFrequency - settings.centerFrequency;
    const double phaseIncrement = -TWO_PI * fShift / settings.sampleRate;
    double phase = ncoPhase;
    double power = 0.0;
    int points = 0;

    painter.setPen(QColor(70, 215, 180, 145));
    for (std::size_t n = 0; n < iqCount; n += stride) {
        float iSample = iqSamples[2 * n];
        float qSample = iqSamples[2 * n + 1];
        if (!std::isfinite(iSample)) {
            iSample = 0.0f;
        }
        if (!std::isfinite(qSample)) {
            qSample = 0.0f;
        }
        if (settings.inputMode == INPUT_HF1) {
            qSample = 0.0f;
        } else if (settings.inputMode == INPUT_HF2) {
            iSample = qSample;
            qSample = 0.0f;
        } else if (settings.inputMode == INPUT_HF_NOISE_CANCEL) {
            const float refGain = std::abs(hfNoiseCancelReferenceCoefficient(settings, settings.listeningFrequency));
            iSample -= static_cast<float>((std::clamp)(settings.hfNoiseCancelDepth, -2.0, 2.0)) * refGain * qSample;
            qSample = 0.0f;
        }

        const double rotI = std::cos(phase);
        const double rotQ = std::sin(phase);
        const double mixedI = iSample * rotI - qSample * rotQ;
        const double mixedQ = iSample * rotQ + qSample * rotI;
        power += mixedI * mixedI + mixedQ * mixedQ;
        const int x = (std::clamp)(static_cast<int>(std::lround(LRPT_DISPLAY_SIZE / 2 + mixedI * 190.0)),
                                   0,
                                   LRPT_DISPLAY_SIZE - 1);
        const int y = (std::clamp)(static_cast<int>(std::lround(LRPT_DISPLAY_SIZE / 2 - mixedQ * 190.0)),
                                   0,
                                   LRPT_DISPLAY_SIZE - 1);
        painter.drawPoint(x, y);
        phase += phaseIncrement * static_cast<double>(stride);
        phase = std::remainder(phase, TWO_PI);
        ++points;
    }
    ncoPhase = std::remainder(ncoPhase + phaseIncrement * static_cast<double>(iqCount), TWO_PI);

    painter.setPen(QColor(210, 220, 230));
    const double rms = points > 0 ? std::sqrt(power / static_cast<double>(points)) : 0.0;
    painter.drawText(12,
                     24,
                     QStringLiteral("Meteor LRPT beta: QPSK monitor, %1 kS/s, RMS %2")
                         .arg(settings.sampleRate / 1000.0, 0, 'f', 0)
                         .arg(rms, 0, 'f', 3));

    lrptRaster = image;
    if (lrptFrameTimer.elapsed() >= 120) {
        emit frameReady(lrptRaster.copy());
        lrptFrameTimer.restart();
    }
    if (!lrptStatusTimer.isValid() || lrptStatusTimer.elapsed() >= STATUS_INTERVAL_MS) {
        emitStatus(QStringLiteral("Meteor LRPT beta: QPSK monitor %1 kS/s, RMS %2")
                       .arg(settings.sampleRate / 1000.0, 0, 'f', 0)
                       .arg(rms, 0, 'f', 3),
                   true);
        lrptStatusTimer.restart();
    }
}

void VideoProcessor::processChannelSample(float iSample, float qSample) {
    float videoSample = 0.0f;
    if (mode == AmVideo) {
        videoSample = std::sqrt(iSample * iSample + qSample * qSample);
    } else {
        float limitedI = iSample;
        float limitedQ = qSample;
        const float magnitude = std::sqrt(limitedI * limitedI + limitedQ * limitedQ);
        if (magnitude > 0.000001f) {
            const float invMagnitude = 1.0f / magnitude;
            limitedI *= invMagnitude;
            limitedQ *= invMagnitude;
        }
        if (fmPreviousValid) {
            videoSample = limitedQ * fmPrevI - limitedI * fmPrevQ;
        }
        fmPrevI = limitedI;
        fmPrevQ = limitedQ;
        fmPreviousValid = true;
    }

    dcEstimate += 0.00008f * (videoSample - dcEstimate);
    appendVideoSample(videoSample - dcEstimate);
}

void VideoProcessor::appendVideoSample(float sample) {
    if (!std::isfinite(sample)) {
        sample = 0.0f;
    }
    currentLine.push_back(sample);

    bool lineComplete = false;
    if (activeSampleRate > 0.0 && configuredLineRate > 0.0) {
        // Preserve fractional line cadence (for example 153.6 samples at
        // 2.4 MS/s). Rounding every line to 154 accumulates almost a full
        // sample of timing error every 2.5 lines and makes HSync drift.
        lineSamplePhase += configuredLineRate;
        if (lineSamplePhase + 1.0e-9 >= activeSampleRate) {
            lineSamplePhase -= activeSampleRate;
            lineComplete = true;
        }
    } else {
        lineComplete = currentLine.size() >= samplesPerLine;
    }

    if (lineComplete) {
        renderCurrentLine();
        currentLine.clear();
    }
}

void VideoProcessor::renderCurrentLine() {
    if (raster.isNull() || width <= 0 || height <= 0 || currentLine.isEmpty()) {
        return;
    }

    const auto minmax = std::minmax_element(currentLine.cbegin(), currentLine.cend());
    uchar *line = raster.scanLine(nextLine);
    const int sourceCount = currentLine.size();
    int sourceOffset = 0;
    int syncWindow = (std::clamp)(static_cast<int>(std::lround(sourceCount * ANALOG_SYNC_FRACTION)),
                                  3,
                                  (std::max)(3, sourceCount / 4));
    bool lineAutoInvert = autoInvertActive;
    if (hSync && sourceCount > 0) {
        const float lineMin = *minmax.first;
        const float lineMax = *minmax.second;
        const float lineSpan = lineMax - lineMin;
        double lineSum = 0.0;
        for (float sample : currentLine) {
            lineSum += sample;
        }
        const float lineMean = static_cast<float>(lineSum / static_cast<double>((std::max)(1, sourceCount)));

        double windowSum = 0.0;
        const int maxWindowStart = (std::max)(0, sourceCount - syncWindow);
        for (int i = 0; i < syncWindow && i < sourceCount; ++i) {
            windowSum += currentLine[i];
        }
        double lowestWindowSum = windowSum;
        double highestWindowSum = windowSum;
        int lowestWindowStart = 0;
        int highestWindowStart = 0;
        for (int start = 1; start <= maxWindowStart; ++start) {
            windowSum += currentLine[start + syncWindow - 1];
            windowSum -= currentLine[start - 1];
            if (windowSum < lowestWindowSum) {
                lowestWindowSum = windowSum;
                lowestWindowStart = start;
            }
            if (windowSum > highestWindowSum) {
                highestWindowSum = windowSum;
                highestWindowStart = start;
            }
        }
        const float lowestWindowAverage = static_cast<float>(lowestWindowSum / syncWindow);
        const float highestWindowAverage = static_cast<float>(highestWindowSum / syncWindow);
        const float darkScore = lineMean - lowestWindowAverage;
        const float brightScore = highestWindowAverage - lineMean;
        const bool brightSync = brightScore > darkScore * 1.12f;
        const float syncScore = (std::max)(darkScore, brightScore);
        const int candidateOffset = brightSync ? highestWindowStart : lowestWindowStart;

        int lowSampleCount = 0;
        const float lowThreshold = lineMin + lineSpan * 0.12f;
        const float highThreshold = lineMax - lineSpan * 0.12f;
        int highSampleCount = 0;
        for (float sample : currentLine) {
            if (sample <= lowThreshold) {
                ++lowSampleCount;
            }
            if (sample >= highThreshold) {
                ++highSampleCount;
            }
        }
        const float lowFraction = static_cast<float>(lowSampleCount) /
                                  static_cast<float>((std::max)(1, sourceCount));
        const float highFraction = static_cast<float>(highSampleCount) /
                                   static_cast<float>((std::max)(1, sourceCount));
        const float syncFraction = brightSync ? highFraction : lowFraction;
        if (vSync && lineSpan >= HSYNC_MIN_SPAN && syncFraction > 0.32f) {
            nextLine = 0;
            currentLine.clear();
            emit analogLockMetrics(lastHSyncPercent, true, lineSpan);
            emitStatus(QStringLiteral("Video VSync lock"), false);
            return;
        }
        ++hSyncObservedLines;
        if (lineSpan >= HSYNC_MIN_SPAN && syncScore >= lineSpan * HSYNC_DARK_FRACTION) {
            if (!hSyncOffsetValid) {
                hSyncOffsetEstimate = candidateOffset;
                hSyncOffsetValid = true;
            } else {
                double delta = static_cast<double>(candidateOffset) - hSyncOffsetEstimate;
                const double halfLine = static_cast<double>(sourceCount) * 0.5;
                while (delta > halfLine) {
                    delta -= sourceCount;
                }
                while (delta < -halfLine) {
                    delta += sourceCount;
                }
                hSyncOffsetEstimate += delta * 0.35;
                while (hSyncOffsetEstimate < 0.0) {
                    hSyncOffsetEstimate += sourceCount;
                }
                while (hSyncOffsetEstimate >= sourceCount) {
                    hSyncOffsetEstimate -= sourceCount;
                }
            }
            sourceOffset = (std::clamp)(static_cast<int>(std::lround(hSyncOffsetEstimate)),
                                        0,
                                        sourceCount - 1);
            hSyncPolarityAccumulator += brightSync ? 1 : -1;
            hSyncPolarityAccumulator = (std::clamp)(hSyncPolarityAccumulator, -24, 24);
            if (hSyncPolarityAccumulator >= 8) {
                autoInvertActive = true;
            } else if (hSyncPolarityAccumulator <= -8) {
                autoInvertActive = false;
            }
            lineAutoInvert = autoInvertActive;
            ++hSyncLockedLines;
        }
        if (hSyncObservedLines >= 120) {
            const int lockPercent = static_cast<int>(std::lround(
                100.0 * static_cast<double>(hSyncLockedLines) /
                static_cast<double>((std::max)(1, hSyncObservedLines))));
            lastHSyncPercent = lockPercent;
            emit analogLockMetrics(lockPercent, false, levelMax - levelMin);
            emitStatus(QStringLiteral("Video HSync %1%, %2 samples/line%3, level %4")
                           .arg(lockPercent)
                           .arg(samplesPerLine)
                           .arg(autoInvertActive ? QStringLiteral(", auto invert") : QString())
                           .arg(levelMax - levelMin, 0, 'f', 3));
            hSyncObservedLines = 0;
            hSyncLockedLines = 0;
        }
    }

    std::vector<float> activeSamples;
    activeSamples.reserve(static_cast<std::size_t>(sourceCount));
    const int blankingSamples = hSync
                                    ? (std::clamp)(static_cast<int>(std::lround(sourceCount * ANALOG_BLANKING_FRACTION)),
                                                   syncWindow,
                                                   (std::max)(syncWindow, sourceCount / 3))
                                    : 0;
    for (int i = 0; i < sourceCount; ++i) {
        const int relative = hSync ? (i - sourceOffset + sourceCount) % sourceCount : i;
        if (relative < blankingSamples) {
            continue;
        }
        activeSamples.push_back(currentLine[i]);
    }
    if (activeSamples.size() >= 8) {
        std::sort(activeSamples.begin(), activeSamples.end());
        const int lowIndex = (std::clamp)(static_cast<int>(activeSamples.size() * 3 / 100),
                                          0,
                                          static_cast<int>(activeSamples.size()) - 1);
        const int highIndex = (std::clamp)(static_cast<int>(activeSamples.size() * 97 / 100),
                                           0,
                                           static_cast<int>(activeSamples.size()) - 1);
        levelMin += 0.06f * (activeSamples[static_cast<std::size_t>(lowIndex)] - levelMin);
        levelMax += 0.06f * (activeSamples[static_cast<std::size_t>(highIndex)] - levelMax);
    } else {
        levelMin += 0.04f * (*minmax.first - levelMin);
        levelMax += 0.04f * (*minmax.second - levelMax);
    }
    if (levelMax - levelMin < 0.02f) {
        const float mid = 0.5f * (levelMin + levelMax);
        levelMin = mid - 0.01f;
        levelMax = mid + 0.01f;
    }

    const float invSpan = 1.0f / (levelMax - levelMin);
    const int activeCount = (std::max)(1, sourceCount - blankingSamples);
    for (int x = 0; x < width; ++x) {
        const float sourcePos = width > 1
                                    ? static_cast<float>(x) * static_cast<float>(activeCount - 1) /
                                          static_cast<float>(width - 1)
                                    : 0.0f;
        const int rawIndex = (std::clamp)(static_cast<int>(sourcePos), 0, activeCount - 1);
        const int rawNextIndex = (std::min)(rawIndex + 1, activeCount - 1);
        const int index = (rawIndex + sourceOffset + blankingSamples) % sourceCount;
        const int nextIndex = (rawNextIndex + sourceOffset + blankingSamples) % sourceCount;
        const float frac = sourcePos - static_cast<float>(rawIndex);
        const float value = currentLine[index] * (1.0f - frac) + currentLine[nextIndex] * frac;
        float normalized = (value - levelMin) * invSpan;
        normalized = clampFloat(normalized, 0.0f, 1.0f);
        if (invert ^ lineAutoInvert) {
            normalized = 1.0f - normalized;
        }
        line[x] = static_cast<uchar>(std::lround(normalized * 255.0f));
    }

    nextLine = (nextLine + 1) % height;
    ++linesSinceFrame;
    if (frameTimer.elapsed() >= FRAME_INTERVAL_MS || linesSinceFrame >= height) {
        emit frameReady(raster.copy());
        linesSinceFrame = 0;
        frameTimer.restart();
    }
}

void VideoProcessor::emitStatus(const QString &status, bool force) {
    if (force || !statusTimer.isValid() || statusTimer.elapsed() >= STATUS_INTERVAL_MS) {
        if (status.startsWith(QStringLiteral("SSTV"))) {
            qDebug().noquote() << "[SSTV]" << status;
        } else if (status.startsWith(QStringLiteral("Video"))) {
            qDebug().noquote() << "[Analog video]" << status;
        }
        emit statusChanged(status);
        statusTimer.restart();
    }
}

void VideoProcessor::ensureTestPatternTimer() {
    if (testPatternTimer) {
        return;
    }

    testPatternTimer = new QTimer(this);
    testPatternTimer->setTimerType(Qt::PreciseTimer);
    connect(testPatternTimer, &QTimer::timeout, this, &VideoProcessor::generateTestPatternIqFrame);
}

void VideoProcessor::generateTestPatternIqFrame() {
    if (!testPatternEnabled) {
        return;
    }

    const int sampleCount = TEST_PATTERN_SAMPLES_PER_LINE * TEST_PATTERN_LINES_PER_TICK;
    const double sampleRate = configuredLineRate * TEST_PATTERN_SAMPLES_PER_LINE;
    QByteArray iqData;
    iqData.resize(sampleCount * 2 * static_cast<int>(sizeof(qint16)));
    char *dst = iqData.data();

    for (int n = 0; n < sampleCount; ++n) {
        const int x = n % TEST_PATTERN_SAMPLES_PER_LINE;
        const int y = (testPatternLine + n / TEST_PATTERN_SAMPLES_PER_LINE) % (std::max)(1, height);
        const float activeStart = 0.16f * TEST_PATTERN_SAMPLES_PER_LINE;
        float video = -0.75f;
        if (x < static_cast<int>(0.08f * TEST_PATTERN_SAMPLES_PER_LINE)) {
            video = -1.0f;
        } else if (x < static_cast<int>(activeStart)) {
            video = -0.55f;
        } else {
            const float tx = (x - activeStart) / (TEST_PATTERN_SAMPLES_PER_LINE - activeStart);
            const int bar = (std::clamp)(static_cast<int>(tx * 8.0f), 0, 7);
            const float bars[8] = {0.85f, 0.65f, 0.45f, 0.25f, 0.05f, -0.15f, -0.35f, -0.55f};
            const float diagonal = std::fmod(tx * 2.0f + y / static_cast<float>((std::max)(1, height)) +
                                             testPatternLine / 160.0f,
                                             1.0f);
            const float marker = diagonal < 0.04f ? 0.35f : 0.0f;
            video = clampFloat(bars[bar] + marker, -0.75f, 0.95f);
        }

        float iSample = 0.0f;
        float qSample = 0.0f;
        if (mode == AmVideo) {
            const float amplitude = clampFloat(0.48f + 0.36f * video, 0.08f, 0.85f);
            iSample = amplitude;
            qSample = 0.0f;
        } else {
            testPatternPhase += 0.20 * static_cast<double>(video);
            testPatternPhase = std::remainder(testPatternPhase, 6.28318530717958647692);
            iSample = 0.72f * static_cast<float>(std::cos(testPatternPhase));
            qSample = 0.72f * static_cast<float>(std::sin(testPatternPhase));
        }

        const auto writeSample = [&dst](float sample) {
            const float clamped = clampFloat(sample, -1.0f, 1.0f);
            const auto value = static_cast<qint16>(std::lrint(clamped * 32767.0f));
            *dst++ = static_cast<char>(value & 0xff);
            *dst++ = static_cast<char>((value >> 8) & 0xff);
        };
        writeSample(iSample);
        writeSample(qSample);
    }

    testPatternLine = (testPatternLine + TEST_PATTERN_LINES_PER_TICK) % (std::max)(1, height);
    processIqFrame(iqData, sampleRate, sampleCount);
}
