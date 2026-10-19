#ifndef MYWATERFALLWIDGET_H
#define MYWATERFALLWIDGET_H

#include <QObject>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShader>
#include <QOpenGLShaderProgram>
#include <vector>
#include <deque>
#include <array>
#include <QColor>
#include <QElapsedTimer>
#include <QOpenGLTexture>
#include <QMutex>
#include <QWheelEvent>
#include <QPainter>
#include <QMouseEvent>
#include <QPoint>
#include <cmath>
#include <memory>
#include "scanvisualassembler.h"
#include "spectrumoverlaytypes.h"

class Waterfall3DRenderer;
class QLabel;

class MyWaterfallWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    enum class RenderBackend {
        CpuTexture,
        GpuPrepared
    };

    enum class DisplayMode {
        Waterfall2D = 0,
        Waterfall3D = 1,
        Waterfall3DWithMini = 2
    };

    enum class DiscontinuityKind {
        ProcessingQueueDrop,
        VisualSnapshotSkip
    };

    explicit MyWaterfallWidget(QWidget *parent = nullptr);
    ~MyWaterfallWidget();
    bool initialized;
    void setData(const std::vector<float> &xData, const std::vector<float> &yData, double minFrequency, double maxFrequency, int fftLength, bool secondGraph, bool colorSpectrum, float contrast, float sensitivity, float levelMin, float levelMax, bool displayOrdered = false);
    void queueDiscontinuityRow(DiscontinuityKind kind);
    void setRowsPerFrame(int rows);
    void setRenderBackend(RenderBackend backend);
    RenderBackend renderBackend() const;
    void setDisplayMode(DisplayMode mode);
    DisplayMode displayMode() const;
    void set3DResolutionDivisor(int divisor);
    void set3DHistoryRows(int rows);
    void set3DSurfaceStyle(int style);
    void set3DSurfaceSmoothing(int smoothing);
    void set3DSurfaceLighting(int lighting);
    void set3DSliceScrollStep(int points);
    void set3DSliceWidth(int points);
    void set3DSpectrumSliceScrollStep(int rows);
    void set3DSpectrumSliceWidth(int rows);
    void set3DSpectrumSliceCapture(bool enabled);
    void set3DSpectrumSliceCaptureFixed(bool enabled);
    void set3DModifierFreeSliceInput(bool enabled);
    void set3DFixedPlane(bool enabled);
    void set3DMonochrome(bool enabled);
    void set3DHeightGradient(bool enabled);
    void set3DMonochromeColor(const QColor &color);
    void set3DMonochromeGradientDepth(int percent);
    void setAlternativeInterfaceMode(bool enabled);
    void setAlternativeSpectrumGradientFill(bool enabled);
    void setAlternativeSpectrumGradientOpacity(int percent);
    void setLevelRange(float minLevel, float maxLevel);
    void setBandMarkersEnabled(bool generalEnabled, bool amateurEnabled);
    void setBandMarkersCompact(bool compact);
    void setBandMarkers(const QVector<GraphBandMarker> &markers);
    void setScanSegments(const QVector<ScanVisualSegment> &segments);
    void setScanSegmentMarkersVisible(bool visible);
    void setSpectrumMetadata(double centerFrequencyHz,
                             double listeningFrequencyHz,
                             double sampleRateHz,
                             int sourceFftLength,
                             int fftWindowType);
    void setFpsOverlayEnabled(bool enabled);
    void setExtendedInfoOverlayEnabled(bool enabled);
    void setAreaMeasurementEnabled(bool enabled);
    void setPauseControlVisible(bool visible);
    void setDisplayPaused(bool paused);
    void setTuneContextEnabled(bool enabled) noexcept { tuneContextEnabled = enabled; }
    bool isDisplayPaused() const noexcept { return displayPaused; }
    void setScienceAnalysisData(const std::vector<float> &maxHold,
                                const std::vector<float> &minHold,
                                const std::vector<float> &average,
                                const std::vector<float> &percentile50,
                                const std::vector<float> &percentile90,
                                const std::vector<float> &percentile99,
                                bool showMaxHold,
                                bool showMinHold,
                                bool showAverage,
                                bool showPercentile50,
                                bool showPercentile90,
                                bool showPercentile99,
                                const QVector<SpectrumScienceMarker> &markers);
    void clearData();
    void computeLineData();
signals:
    void displayPausedChanged(bool paused);
    void scaleChanged(int delta);
    void tuneContextRequested(double frequency, const QPoint &globalPos);
    void autoTuneRequested(double frequency);
    void panRequested(int deltaPixels, int widthPixels);
protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
	void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    void updatePauseControl();
    void ensureLineBuffer();
    void resetWaterfallTexture(int w, int h);
    void resizeWaterfallTexturePreserve(int w, int h);
    void queueCurrentTextureLine();
    bool ensureGpuWaterfallProgram();
    bool drawGpuPreparedWaterfall(float vStart);
    void drawMiniWaterfallOverlay(float vStart);
    void uploadPendingTextureLine();
    void drawScanSegments(QPainter &painter) const;
    void drawWaterfallAreaMeasurement(QPainter &painter) const;
    void drawAlternativeSpectrumOverlay(QPainter &painter,
                                        const std::vector<float> &normalizedLevels) const;
    void drawAlternativeBandMarkers(QPainter &painter, const QRect &plotRect) const;
    void drawAlternativeScienceOverlays(QPainter &painter, const QRect &plotRect) const;
    QRect alternativeSpectrumPlotRect() const;
    QString formatFrequencyLabel(double frequencyHz) const;
    QString formatFrequencySpanLabel(double spanHz) const;
    void updateFpsCounter();
    void positionInfoOverlays();
    void updateSliceOverlay(const QPoint &anchor);
    void hideSliceOverlay();
    void updateAlternativeDbLabels();
    void updateAlternativeBandLabels();
    void updateAlternativeInteractionLabels(const std::vector<float> &normalizedLevels);
    double displayFrequencyAtX(int x) const;
    double actualFrequencyForDisplayFrequency(double displayFrequency) const;
    double displayFrequencyForActualFrequency(double actualFrequency) const;
    double frequencyAtX(int x) const;
    qint64 waterfallTimeAtY(int y) const;
    double signalCenterNearFrequency(double frequency);
    mutable QMutex mutex;
    QOpenGLBuffer waterfallVbo;
    QOpenGLShaderProgram waterfallProgram;
    GLuint waterfallTexture;
    std::vector<unsigned char> lineData;
    std::vector<unsigned char> textureUploadRows;
    std::vector<unsigned char> waterfallTexturePixels;
    std::deque<std::vector<unsigned char>> pendingTextureLines;
    std::deque<qint64> pendingTextureLineTimesMs;
    std::deque<qint64> waterfallRowTimesMs;
    std::vector<float> pixelMaxData;
    std::vector<float> pixelFrequencyData;
    std::vector<float> pixelLevelData;
    QColor valueToColor(float value, float contrastFactor, float sensitivityFactor);
    QColor valueToColors(float value) const;
    float normalizedLevel(float value) const;
    float yMin, yMax, contrast, sensitivity, levelMin, levelMax;
    double xMin, xMax;
    double metadataCenterFrequencyHz = 0.0;
    double metadataListeningFrequencyHz = 0.0;
    double metadataSampleRateHz = 0.0;
    int metadataFftLength = 0;
    int metadataFftWindowType = 0;
    int fftLength;
    int textureWidth = 0;
    int textureHeight = 0;
    int waterfallWriteRow = 0;
    int rowsPerFrame = 2;
    RenderBackend activeRenderBackend = RenderBackend::CpuTexture;
    DisplayMode activeDisplayMode = DisplayMode::Waterfall2D;
    std::unique_ptr<Waterfall3DRenderer> waterfall3DRenderer;
    bool waterfallProgramReady = false;
    bool waterfallProgramTried = false;
    bool secondGraph;
    bool colorSpectrum = true;
    bool changebit;
    bool spectrumPanActive = false;
    bool spectrumPanMoved = false;
    Qt::MouseButton spectrumPanButton = Qt::NoButton;
    QPoint spectrumPanLastPos;
    bool cameraOrbitActive = false;
    QPoint cameraOrbitLastPos;
    bool cameraPanActive = false;
    QPoint cameraPanLastPos;
    bool frequencySliceMouseActive = false;
    bool spectrumFrameSliceMouseActive = false;
    bool modifierFreeSliceInput = false;
    bool tuneContextEnabled = true;
    bool alternativeInterfaceMode = false;
    bool fixed3DPlane = false;
    bool alternativeSpectrumGradientFill = false;
    int alternativeSpectrumGradientOpacity = 70;
    bool alternativeSpectrumHoverVisible = false;
    QPoint alternativeSpectrumHoverPos;
    bool alternativeSpectrumMeasurementActive = false;
    bool alternativeSpectrumMeasurementVisible = false;
    QPoint alternativeSpectrumMeasureStartPos;
    QPoint alternativeSpectrumMeasureEndPos;
    bool areaMeasurementEnabled = false;
    bool areaMeasurementActive = false;
    bool areaMeasurementVisible = false;
    QPoint areaMeasurementStartPos;
    QPoint areaMeasurementEndPos;
    bool pendingTextureLine = false;
    bool textureClearRequested = false;
    bool updateQueued = false;
    QVector<ScanVisualSegment> scanSegments;
    QVector<GraphBandMarker> bandMarkers;
    bool generalBandMarkersEnabled = false;
    bool amateurBandMarkersEnabled = false;
    bool compactBandMarkersEnabled = false;
    bool scanSegmentMarkersVisible = true;
    bool fpsOverlayEnabled = false;
    bool extendedInfoOverlayEnabled = false;
    std::vector<float> scienceMaxHoldData;
    std::vector<float> scienceMinHoldData;
    std::vector<float> scienceAverageData;
    std::vector<float> sciencePercentile50Data;
    std::vector<float> sciencePercentile90Data;
    std::vector<float> sciencePercentile99Data;
    QVector<SpectrumScienceMarker> scienceMarkers;
    bool scienceMaxHoldVisible = false;
    bool scienceMinHoldVisible = false;
    bool scienceAverageVisible = false;
    bool sciencePercentile50Visible = false;
    bool sciencePercentile90Visible = false;
    bool sciencePercentile99Visible = false;
    QElapsedTimer fpsElapsedTimer;
    QElapsedTimer waterfallRowsElapsedTimer;
    QElapsedTimer sliceOverlayUpdateTimer;
    int fpsFrameCount = 0;
    double displayedFps = 0.0;
    quint64 waterfallFftFramesSinceRateUpdate = 0;
    double displayedWaterfallFftFramesPerSecond = 0.0;
    quint64 waterfallRowsSinceRateUpdate = 0;
    double displayedWaterfallRowsPerSecond = 0.0;
    QLabel *fpsOverlayLabel = nullptr;
    QLabel *sliceOverlayLabel = nullptr;
    QLabel *sliceDetailsOverlayLabel = nullptr;
    QLabel *areaMeasurementOverlayLabel = nullptr;
    class QToolButton *pauseControl = nullptr;
    bool displayPaused = false;
    std::array<QLabel*, 6> alternativeDbLabels{};
    std::vector<QLabel*> alternativeBandLabels;
    QLabel *alternativeMeasurementLabel = nullptr;
    QLabel *alternativeHoverLabel = nullptr;
    QPoint sliceOverlayAnchor;
};

#endif // MYWATERFALLWIDGET_H
