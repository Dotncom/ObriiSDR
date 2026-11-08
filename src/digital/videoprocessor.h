#ifndef VIDEOPROCESSOR_H
#define VIDEOPROCESSOR_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <array>
#include <complex>
#include <vector>

#include "radiosettings.h"

class VideoProcessor : public QObject {
    Q_OBJECT

public:
    enum DemodMode {
        FmVideo = 0,
        AmVideo = 1
    };

    explicit VideoProcessor(QObject *parent = nullptr);

public slots:
    void configure(bool enabled,
                   int demodMode,
                   double lineRate,
                   int frameWidth,
                   int frameHeight,
                   bool invertVideo,
                   bool hSyncEnabled,
                   bool vSyncEnabled);
    void setTestPatternEnabled(bool enabled);
    void configureSstv(bool enabled);
    void resetSstvDecoder();
    void setSstvTestPatternEnabled(bool enabled);
    void configureApt(bool enabled);
    void setAptTestPatternEnabled(bool enabled);
    void configureWefax(bool enabled);
    void setWefaxTestPatternEnabled(bool enabled);
    void configureLrpt(bool enabled);
    void processSstvPcmFrame(const QByteArray &pcmData, int sampleRate);
    void processAptPcmFrame(const QByteArray &pcmData, int sampleRate);
    void processWefaxPcmFrame(const QByteArray &pcmData, int sampleRate);
    void processIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount);
    void processFloatIqSnapshot(const std::vector<float> &iqSamples, const RadioSettings &settings);
    void reset();

signals:
    void frameReady(const QImage &frame);
    void statusChanged(const QString &status);
    void analogLockMetrics(int hsyncPercent, bool verticalSync, double levelSpan);

private:
    enum class SstvDecodeFamily {
        Robot36,
        Robot72,
        Rgb,
        Pd
    };

    void resetRaster();
    void resetChannelizer();
    void resetSstvState(bool clearImage);
    void processSstvBuffer();
    int findSstvLineStart(double *bestScore) const;
    int detectSstvVisCode(double *confidence, int *headerEndSample);
    bool isSupportedSstvVisCode(int visCode) const;
    QString sstvModeName(int visCode) const;
    int activeSstvWidth() const;
    int activeSstvHeight() const;
    double activeSstvLineSeconds() const;
    double activeSstvSyncSeconds() const;
    SstvDecodeFamily activeSstvFamily() const;
    void decodeSstvLine(int lineStart);
    void decodeRobot36Line(int lineStart);
    void decodeRobot72Line(int lineStart);
    void decodeRgbSstvLine(int lineStart);
    void decodePdSstvLine(int lineStart);
    void renderRgbSstvLine(int lineIndex,
                           const std::array<uchar, 320> &red,
                           const std::array<uchar, 320> &green,
                           const std::array<uchar, 320> &blue);
    void renderSstvLine(int lineIndex,
                        const std::array<uchar, 320> &luma,
                        const std::array<uchar, 160> *uChroma,
                        const std::array<uchar, 160> *vChroma);
    double toneEnergy(const std::vector<float> &samples, int start, int length, double frequencyHz) const;
    double estimateToneFrequency(const std::vector<float> &samples,
                                 int center,
                                 int windowSamples,
                                 double minHz,
                                 double maxHz) const;
    std::vector<uchar> decodeSstvToneSpan(int start,
                                         int spanSamples,
                                         int pixelCount) const;
    uchar sstvFrequencyToByte(double frequencyHz) const;
    void ensureSstvTestPatternTimer();
    void generateSstvTestPatternPcmFrame();
    void appendSstvVisHeader(QByteArray &pcmData, int visCode);
    void appendSstvTone(QByteArray &pcmData, double frequencyHz, int sampleCount, double amplitude);
    QRgb sstvTestPatternPixel(int x, int y) const;
    void resetAptState(bool clearImage);
    void appendAptBrightnessSample(float brightness);
    void renderAptLine();
    void ensureAptTestPatternTimer();
    void generateAptTestPatternPcmFrame();
    void appendAptTone(QByteArray &pcmData, double amplitude, int sampleCount);
    float aptTestPatternBrightness(int x, int y) const;
    void resetWefaxState(bool clearImage);
    void appendWefaxPixel(float brightness);
    void renderWefaxLine();
    double estimateWefaxFrequency(const std::vector<float> &samples) const;
    void ensureWefaxTestPatternTimer();
    void generateWefaxTestPatternPcmFrame();
    void appendWefaxTone(QByteArray &pcmData, double frequencyHz, int sampleCount, double amplitude);
    float wefaxTestPatternBrightness(int x, int y) const;
    void configureLrptImage();
    void processLrptFloatIqSnapshot(const std::vector<float> &iqSamples, const RadioSettings &settings);
    void appendVideoSample(float sample);
    void processChannelSample(float iSample, float qSample);
    void renderCurrentLine();
    void emitStatus(const QString &status, bool force = false);
    void ensureTestPatternTimer();
    void generateTestPatternIqFrame();

    bool enabled = false;
    bool testPatternEnabled = false;
    int mode = FmVideo;
    double configuredLineRate = 15625.0;
    int width = 384;
    int height = 288;
    bool invert = false;
    bool hSync = true;
    bool vSync = true;

    double activeSampleRate = 0.0;
    double activeInputSampleRate = 0.0;
    double activeCenterFrequency = 0.0;
    double activeListeningFrequency = 0.0;
    int activeInputMode = 0;
    int samplesPerLine = 0;
    double lineSamplePhase = 0.0;
    QVector<float> currentLine;
    QImage raster;
    int nextLine = 0;
    int linesSinceFrame = 0;

    bool fmPreviousValid = false;
    float fmPrevI = 1.0f;
    float fmPrevQ = 0.0f;
    float dcEstimate = 0.0f;
    float levelMin = -0.25f;
    float levelMax = 0.25f;
    bool hSyncOffsetValid = false;
    double hSyncOffsetEstimate = 0.0;
    int hSyncPolarityAccumulator = 0;
    bool autoInvertActive = false;

    QElapsedTimer frameTimer;
    QElapsedTimer statusTimer;
    QTimer *testPatternTimer = nullptr;
    double testPatternPhase = 0.0;
    int testPatternLine = 0;
    int hSyncLockedLines = 0;
    int hSyncObservedLines = 0;
    int lastHSyncPercent = 0;

    double ncoPhase = 0.0;
    std::complex<float> decimationSum = {0.0f, 0.0f};
    int decimationCount = 0;
    std::complex<float> lowPassState = {0.0f, 0.0f};

    bool sstvEnabled = false;
    int sstvSampleRate = 48000;
    std::vector<float> sstvAudioBuffer;
    QImage sstvRaster;
    int sstvNextLine = 0;
    bool sstvHaveEvenLine = false;
    std::array<uchar, 320> sstvEvenLuma = {};
    std::array<uchar, 160> sstvEvenV = {};
    QElapsedTimer sstvFrameTimer;
    QElapsedTimer sstvStatusTimer;
    int sstvLinesSinceFrame = 0;
    double lastSstvSyncScore = 0.0;
    bool sstvFrameActive = false;
    int sstvSyncLockCount = 0;
    int sstvLostSyncCount = 0;
    int sstvSamplesSinceLastSync = 0;
    bool sstvLineOverlapActive = false;
    double sstvLineTimingScale = 1.0;
    int sstvPreviousAdvanceSamples = 0;
    bool sstvHaveLinePeriodReference = false;
    int sstvLineAdvanceOverride = -1;
    int sstvDecodedRows = 0;
    bool sstvScottiePrimed = false;
    std::array<uchar, 320> sstvScottieGreen = {};
    std::array<uchar, 320> sstvScottieBlue = {};
    int sstvLastVisCode = 8;
    int sstvVisSearchOffset = 0;
    double sstvToneOffsetHz = 0.0;
    bool sstvTestPatternEnabled = false;
    QTimer *sstvTestPatternTimer = nullptr;
    int sstvTestPatternLine = 0;
    double sstvTestTonePhase = 0.0;

    bool aptEnabled = false;
    bool aptTestPatternEnabled = false;
    int aptSampleRate = 48000;
    QImage aptRaster;
    std::vector<uchar> aptLine;
    int aptNextLine = 0;
    double aptCarrierPhase = 0.0;
    double aptCarrierI = 0.0;
    double aptCarrierQ = 0.0;
    double aptEnvelopeDc = 0.0;
    double aptPixelPhase = 0.0;
    double aptTestTonePhase = 0.0;
    int aptTestLine = 0;
    QTimer *aptTestPatternTimer = nullptr;
    QElapsedTimer aptFrameTimer;
    QElapsedTimer aptStatusTimer;
    int aptLinesSinceFrame = 0;

    bool wefaxEnabled = false;
    bool wefaxTestPatternEnabled = false;
    int wefaxSampleRate = 48000;
    QImage wefaxRaster;
    std::vector<uchar> wefaxLine;
    std::vector<float> wefaxPixelSamples;
    int wefaxNextLine = 0;
    double wefaxPixelPhase = 0.0;
    double wefaxTestTonePhase = 0.0;
    int wefaxTestLine = 0;
    QTimer *wefaxTestPatternTimer = nullptr;
    QElapsedTimer wefaxFrameTimer;
    QElapsedTimer wefaxStatusTimer;
    int wefaxLinesSinceFrame = 0;

    bool lrptEnabled = false;
    QImage lrptRaster;
    QElapsedTimer lrptFrameTimer;
    QElapsedTimer lrptStatusTimer;
};

#endif // VIDEOPROCESSOR_H
