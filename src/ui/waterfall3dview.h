#ifndef WATERFALL3DVIEW_H
#define WATERFALL3DVIEW_H

#include <QImage>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>

#include <memory>
#include <vector>

class Waterfall3DRenderer;

class Waterfall3DView : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    explicit Waterfall3DView(QWidget *parent = nullptr);
    ~Waterfall3DView() override;

    void clearHistory();
    void appendHistoryRow(const std::vector<float> &levels,
                          const std::vector<unsigned char> &rgb,
                          float levelMin,
                          float levelMax);
    void setHistoryCapacity(int rows);
    void setHighlightedHistoryRow(int row);
    void setResolutionDivisor(int divisor);
    void setSliceScrollStep(int points);
    void setSliceWidth(int points);
    void setSpectrumSliceScrollStep(int rows);
    void setSpectrumSliceWidth(int rows);
    void setSpectrumSliceCapture(bool enabled);
    void setSpectrumSliceCaptureFixed(bool enabled);
    void setModifierFreeSliceInput(bool enabled);
    void setDensityAxes(bool enabled);
    void setDensityAxisMapping(int xDimension, int yDimension);
    void setDensityFrontProfile(const std::vector<float> &normalizedLevels);
    void setDensityFrontProfileStyle(int style);
    void setSurfaceStyle(int style);
    void setSurfaceSmoothing(int smoothing);
    void setSurfaceLighting(int lighting);
    void setDensityAxisLabels(const QString &frequency,
                              const QString &density,
                              const QString &level);
    void setDensityAxisRanges(double firstFrequencyHz,
                              double lastFrequencyHz,
                              double minimumDb,
                              double maximumDb,
                              double maximumDensity);
    void setOverview(const QImage &image,
                     int windowStart,
                     int windowEnd,
                     int selectedRow,
                     int totalRows);
    void setOverviewVisible(bool visible);

signals:
    void frequencySliceSelected(double ratio);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void drawOverview();
    void drawDensityAxes();
    void emitSelectedFrequency();

    std::unique_ptr<Waterfall3DRenderer> renderer;
    QPoint lastMousePosition;
    bool cameraDragActive = false;
    bool cameraPanActive = false;
    bool frequencySliceActive = false;
    bool spectrumSliceActive = false;
    bool modifierFreeSliceInput = false;
    bool densityAxes = false;
    QString densityFrequencyLabel;
    QString densityAccumulationLabel;
    QString densityLevelLabel;
    double densityFirstFrequencyHz = 0.0;
    double densityLastFrequencyHz = 0.0;
    double densityMinimumDb = -140.0;
    double densityMaximumDb = -20.0;
    double densityMaximumValue = 1.0;
    bool overviewVisible = false;
    QImage overviewImage;
    QImage overviewThumbnail;
    int overviewWindowStart = 0;
    int overviewWindowEnd = 0;
    int overviewSelectedRow = 0;
    int overviewTotalRows = 0;
};

#endif // WATERFALL3DVIEW_H
