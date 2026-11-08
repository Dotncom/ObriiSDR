#pragma once

#include <QJsonObject>
#include <QWidget>

#include <cstdint>
#include <vector>

class QTimer;
class SignalLabConstellation3DWidget;

class DspResearchWidget final : public QWidget {
public:
    enum class Mode {
        Oscilloscope,
        Constellation,
        EyeDiagram,
        Synchronization
    };

    explicit DspResearchWidget(Mode mode, QWidget *parent = nullptr);

    void setSettings(const QJsonObject &settings);
    void setConnected(bool connected, bool ukrainian);
    void setExternalInput(bool enabled);
    void setExternalFrame(const std::vector<float> &iq, double sampleRate, std::uint64_t sequence);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void refreshSnapshot();
    void ingestIq(const std::vector<float> &iq, double sampleRate, std::uint64_t sequence);
    void updateConstellationView();
    void drawGrid(QPainter &painter, const QRectF &plot) const;
    void drawOscilloscope(QPainter &painter, const QRectF &plot) const;
    void drawConstellation(QPainter &painter, const QRectF &plot) const;
    void drawEye(QPainter &painter, const QRectF &plot) const;
    void drawSynchronization(QPainter &painter, const QRectF &plot) const;
    float interpolated(const std::vector<float> &values, double position) const;

    Mode mode_;
    QTimer *timer_ = nullptr;
    SignalLabConstellation3DWidget *constellation3D_ = nullptr;
    QJsonObject settings_;
    std::vector<float> iSamples_;
    std::vector<float> qSamples_;
    std::uint64_t sequence_ = 0;
    double sampleRate_ = 0.0;
    bool connected_ = false;
    bool externalInput_ = false;
    bool ukrainian_ = false;
};
