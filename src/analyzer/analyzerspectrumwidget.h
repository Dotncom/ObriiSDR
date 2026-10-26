#ifndef ANALYZERSPECTRUMWIDGET_H
#define ANALYZERSPECTRUMWIDGET_H

#include "spectrumoverlaytypes.h"

#include <QWidget>
#include <vector>

class AnalyzerSpectrumWidget final : public QWidget {
    Q_OBJECT
public:
    explicit AnalyzerSpectrumWidget(QWidget *parent = nullptr);

    void setFrame(const std::vector<float> &frequencies,
                  const std::vector<float> &levels);
    void setTraces(const std::vector<float> &maxHold,
                   const std::vector<float> &minHold,
                   const std::vector<float> &average,
                   const std::vector<float> &percentile50,
                   const std::vector<float> &percentile90,
                   const std::vector<float> &percentile99,
                   bool showMax,
                   bool showMin,
                   bool showAverage,
                   bool showP50,
                   bool showP90,
                   bool showP99,
                   bool showCurrent);
    void setMarkers(const QVector<SpectrumScienceMarker> &markers);
    void setAmplitudeRange(double referenceLevelDb, double dbPerDivision);
    void setAmplitudeBounds(double topDb, double bottomDb);
    void setMeasurementDetails(const QString &details);
    void clear();

signals:
    void frequencyClicked(double frequencyHz);
    void frequencyPanRequested(double deltaHz);
    void frequencyRangeSelected(double startHz, double endHz);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRect plotRect() const;
    double frequencyAtX(int x) const;
    double levelAtY(int y) const;
    int xForFrequency(double hz) const;
    int yForLevel(double db) const;
    QString frequencyText(double hz) const;
    void drawTrace(QPainter &painter, const std::vector<float> &trace,
                   const QColor &color, qreal width, bool preserveMinimum = false) const;

    std::vector<float> frequencies;
    std::vector<float> levels;
    std::vector<float> maxHold;
    std::vector<float> minHold;
    std::vector<float> average;
    std::vector<float> percentile50;
    std::vector<float> percentile90;
    std::vector<float> percentile99;
    QVector<SpectrumScienceMarker> markers;
    bool maxVisible = false;
    bool minVisible = false;
    bool averageVisible = false;
    bool p50Visible = false;
    bool p90Visible = false;
    bool p99Visible = false;
    bool currentVisible = true;
    double referenceLevel = 0.0;
    double divisionDb = 10.0;
    bool hoverVisible = false;
    QPoint hoverPosition;
    bool dragging = false;
    bool dragMoved = false;
    bool measuring = false;
    QPoint dragStartPosition;
    QPoint dragLastPosition;
    QString measurementDetails;
};

#endif
