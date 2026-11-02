#ifndef MYGRAPHWIDGET_H
#define MYGRAPHWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QVector>
#include <QColor>
#include <QElapsedTimer>
#include <QString>
#include <vector>
#include <QWheelEvent>
#include <QPainter>
#include <QMouseEvent>
#include <QPoint>
#include "spectrumpersistence.h"
#include "scanvisualassembler.h"
#include "spectrumoverlaytypes.h"

class MyGraphWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    explicit MyGraphWidget(QWidget *parent = nullptr);
    ~MyGraphWidget();

    void setData(const std::vector<float> &xData, const std::vector<float> &yData, double xMin, double xMax, int fftLength, bool colorf, bool displayOrdered = false, bool preserveFrequencyGaps = false);
    void setOverlayData(const std::vector<float> &yData, bool enabled, bool displayOrdered = false);
    void setLevelRange(float minLevel, float maxLevel);
    void setPersistenceEnabled(bool enabled);
    void setPersistenceParameters(double seconds, int intensityPercent, int thresholdPercent = 0, int smoothingMs = 0);
    void setBandMarkersEnabled(bool generalEnabled, bool amateurEnabled);
    void setBandMarkersCompact(bool compact);
    void setBandMarkers(const QVector<GraphBandMarker> &markers);
    void setScanSegments(const QVector<ScanVisualSegment> &segments);
    void setScanSegmentMarkersVisible(bool visible);
    void setTuningMarker(double frequencyHz, bool visible);
    void setSpectrumMetadata(double centerFrequencyHz,
                             double listeningFrequencyHz,
                             double sampleRateHz,
                             int sourceFftLength,
                             int fftWindowType);
    void setFpsOverlayEnabled(bool enabled);
    void setExtendedInfoOverlayEnabled(bool enabled);
    void setAnalogPeakMeterEnabled(bool enabled);
    void setAnalogPeakMeterStyle(int style);
    void setAnalogPeakMeterTarget(double frequencyHz, bool valid);
    void setFrequencyAxisLabelsVisible(bool visible);
    void setPauseControlVisible(bool visible);
    void setDisplayPaused(bool paused);
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
    bool bandwidthSelection(double &lowHz, double &highHz) const;

signals:
    void scaleChanged(int direction);
    void tuneContextRequested(double frequency, const QPoint &globalPos);
    void autoTuneRequested(double frequency);
    void panRequested(int deltaPixels, int widthPixels);
    void scienceMarkerRequested(double frequency);
    void bandwidthSelectionChanged(double lowHz, double highHz);
    void multiVfoSelectionRequested(double lowHz, double highHz);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void accumulatePersistence();
    bool persistenceEnabled = false;
    SpectrumPersistence persistence;
    QElapsedTimer persistenceClock;
    void updatePauseControl();
    struct CursorPeak {
        bool valid = false;
        double frequency = 0.0;
        double displayFrequency = 0.0;
        float level = 0.0f;
        int dataIndex = -1;
        bool fromOverlay = false;
        int x = 0;
        int y = 0;
    };

    void drawBandMarkers(QPainter &painter) const;
    void drawScanSegments(QPainter &painter) const;
    void drawYAxis(QPainter &painter) const;
    void drawXAxis(QPainter &painter) const;
    void drawTuningMarker(QPainter &painter) const;
    void drawBandwidthMeasurement(QPainter &painter) const;
    void drawHoverCursor(QPainter &painter) const;
    void drawScienceTraces(QPainter &painter) const;
    void drawScienceMarkers(QPainter &painter) const;
    void updateFpsCounter();
    void drawFpsOverlay(QPainter &painter) const;
    void drawExtendedInfoOverlay(QPainter &painter) const;
    void drawAnalogPeakMeter(QPainter &painter) const;
    float normalizedLevel(float value) const;
    float displayLevelAt(const std::vector<float> &levels, int index, int count, bool ordered) const;
    double displayFrequencyAt(int index, int count) const;
    int bottomMargin() const;
    double displayFrequencyAtX(int x) const;
    double actualFrequencyForDisplayFrequency(double displayFrequency) const;
    double displayFrequencyForActualFrequency(double actualFrequency) const;
    double frequencyAtX(int x) const;
    int xForFrequency(double frequency) const;
    CursorPeak cursorPeakAtX(int x) const;
    QString formatFrequencyLabel(double frequencyHz) const;
    QString formatFrequencySpanLabel(double spanHz) const;
    double signalCenterNearFrequency(double frequency) const;

    double xMin, xMax, yMin, yMax;
    int fftLength;
    bool initialized;
    bool colorf;

    std::vector<float> xData, yData, overlayYData;
    std::vector<float> renderFrequencyScratch;
    std::vector<float> renderLevelScratch;
    std::vector<float> renderOverlayLevelScratch;
    bool overlayEnabled = false;
    bool dataDisplayOrdered = false;
    bool preserveDataFrequencyGaps = false;
    bool overlayDisplayOrdered = false;
    bool generalBandMarkersEnabled = false;
    bool amateurBandMarkersEnabled = false;
    bool compactBandMarkersEnabled = false;
    bool hoverCursorVisible = false;
    QPoint hoverCursorPos;
    bool bandwidthMeasurementActive = false;
    bool bandwidthMeasurementVisible = false;
    bool multiVfoSelectionActive = false;
    QPoint bandwidthMeasureStartPos;
    QPoint bandwidthMeasureEndPos;
    bool spectrumPanActive = false;
    bool spectrumPanMoved = false;
    QPoint spectrumPanLastPos;
    QVector<GraphBandMarker> bandMarkers;
    QVector<ScanVisualSegment> scanSegments;
    bool scanSegmentMarkersVisible = true;
    bool tuningMarkerVisible = false;
    double tuningMarkerFrequencyHz = 0.0;
    double metadataCenterFrequencyHz = 0.0;
    double metadataListeningFrequencyHz = 0.0;
    double metadataSampleRateHz = 0.0;
    int metadataFftLength = 0;
    int metadataFftWindowType = 0;
    bool fpsOverlayEnabled = false;
    bool extendedInfoOverlayEnabled = false;
    bool analogPeakMeterEnabled = false;
    int analogPeakMeterStyle = 0;
    bool analogPeakMeterTargetValid = false;
    double analogPeakMeterTargetHz = 0.0;
    bool frequencyAxisLabelsVisible = false;
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
    int fpsFrameCount = 0;
    double displayedFps = 0.0;
    class QToolButton *pauseControl = nullptr;
    bool displayPaused = false;
    QColor valueToColor(float value);
};

#endif // MYGRAPHWIDGET_H
