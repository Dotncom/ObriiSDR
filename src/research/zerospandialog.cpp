#include "zerospandialog.h"

#include "appsettingsutils.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
double dbToPower(float db) {
    return std::pow(10.0, static_cast<double>(db) / 10.0);
}

float powerToDb(double power) {
    return power > 1.0e-20
               ? static_cast<float>(10.0 * std::log10(power))
               : -200.0f;
}

QString frequencyText(double hz) {
    if (!std::isfinite(hz)) return QStringLiteral("--");
    if (std::abs(hz) >= 1.0e9) return QStringLiteral("%1 GHz").arg(hz / 1.0e9, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e6) return QStringLiteral("%1 MHz").arg(hz / 1.0e6, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e3) return QStringLiteral("%1 kHz").arg(hz / 1.0e3, 0, 'f', 3);
    return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 0);
}

struct PlotSample {
    qint64 utcMs = 0;
    float levelDb = -160.0f;
};
}

class ZeroSpanPlotWidget : public QWidget {
public:
    explicit ZeroSpanPlotWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(560, 280);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setTrace(const std::vector<PlotSample> &newSamples,
                  double spanSeconds,
                  int pretriggerPercent,
                  qint64 triggerMs,
                  float triggerLevel,
                  bool showTrigger,
                  bool lockWindowToTrigger,
                  bool autoScale,
                  float fixedMin,
                  float fixedMax) {
        samples = newSamples;
        timeSpanSeconds = std::clamp(spanSeconds, 0.1, 300.0);
        pretrigger = std::clamp(pretriggerPercent, 0, 90);
        triggerUtcMs = triggerMs;
        triggerLevelDb = triggerLevel;
        triggerVisible = showTrigger;
        triggerLockedWindow = lockWindowToTrigger;
        autoScaleY = autoScale;
        fixedLevelMin = fixedMin;
        fixedLevelMax = fixedMax;
        update();
    }

protected:
    void mouseMoveEvent(QMouseEvent *event) override {
        hoverPos = event->pos();
        hoverVisible = true;
        update();
    }

    void leaveEvent(QEvent *event) override {
        hoverVisible = false;
        update();
        QWidget::leaveEvent(event);
    }

    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(4, 8, 12));
        const QRect plotRect = rect().adjusted(58, 16, -16, -36);
        if (plotRect.width() < 20 || plotRect.height() < 20) return;

        qint64 viewEndMs = QDateTime::currentMSecsSinceEpoch();
        qint64 viewStartMs = viewEndMs - qRound64(timeSpanSeconds * 1000.0);
        if (triggerLockedWindow && triggerUtcMs >= 0) {
            viewStartMs = triggerUtcMs - qRound64(timeSpanSeconds * pretrigger * 10.0);
            viewEndMs = viewStartMs + qRound64(timeSpanSeconds * 1000.0);
        } else if (!samples.empty()) {
            viewEndMs = samples.back().utcMs;
            viewStartMs = viewEndMs - qRound64(timeSpanSeconds * 1000.0);
        }

        float levelMin = fixedLevelMin;
        float levelMax = fixedLevelMax;
        if (autoScaleY && !samples.empty()) {
            levelMin = std::numeric_limits<float>::infinity();
            levelMax = -std::numeric_limits<float>::infinity();
            for (const PlotSample &sample : samples) {
                if (sample.utcMs < viewStartMs || sample.utcMs > viewEndMs || !std::isfinite(sample.levelDb)) continue;
                levelMin = std::min(levelMin, sample.levelDb);
                levelMax = std::max(levelMax, sample.levelDb);
            }
            if (!std::isfinite(levelMin) || !std::isfinite(levelMax)) {
                levelMin = -140.0f;
                levelMax = 0.0f;
            } else {
                const float margin = std::max(2.0f, (levelMax - levelMin) * 0.12f);
                levelMin -= margin;
                levelMax += margin;
            }
        }
        if (levelMax <= levelMin + 0.1f) levelMax = levelMin + 1.0f;

        painter.setPen(QPen(QColor(80, 105, 120, 100), 1));
        constexpr int xDivisions = 10;
        constexpr int yDivisions = 8;
        for (int i = 0; i <= xDivisions; ++i) {
            const int x = plotRect.left() + i * plotRect.width() / xDivisions;
            painter.drawLine(x, plotRect.top(), x, plotRect.bottom());
            const double seconds = (viewStartMs + (viewEndMs - viewStartMs) * i / xDivisions -
                                    (triggerUtcMs >= 0 ? triggerUtcMs : viewEndMs)) / 1000.0;
            painter.setPen(QColor(180, 200, 210));
            painter.drawText(QRect(x - 35, plotRect.bottom() + 5, 70, 20), Qt::AlignHCenter | Qt::AlignTop,
                             QStringLiteral("%1 s").arg(seconds, 0, 'f', 1));
            painter.setPen(QPen(QColor(80, 105, 120, 100), 1));
        }
        for (int i = 0; i <= yDivisions; ++i) {
            const int y = plotRect.top() + i * plotRect.height() / yDivisions;
            painter.drawLine(plotRect.left(), y, plotRect.right(), y);
            const float level = levelMax - (levelMax - levelMin) * i / yDivisions;
            painter.setPen(QColor(180, 200, 210));
            painter.drawText(QRect(2, y - 9, 52, 18), Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("%1").arg(level, 0, 'f', 1));
            painter.setPen(QPen(QColor(80, 105, 120, 100), 1));
        }
        painter.setPen(QColor(205, 220, 225));
        painter.drawText(4, 13, QStringLiteral("dBFS"));

        auto xForTime = [&](qint64 utcMs) {
            const double ratio = static_cast<double>(utcMs - viewStartMs) /
                                 static_cast<double>(std::max<qint64>(1, viewEndMs - viewStartMs));
            return plotRect.left() + ratio * plotRect.width();
        };
        auto yForLevel = [&](float db) {
            const double ratio = (db - levelMin) / (levelMax - levelMin);
            return plotRect.bottom() - std::clamp(ratio, 0.0, 1.0) * plotRect.height();
        };

        if (triggerVisible) {
            painter.setPen(QPen(QColor(255, 110, 70, 190), 1, Qt::DashLine));
            const int y = qRound(yForLevel(triggerLevelDb));
            painter.drawLine(plotRect.left(), y, plotRect.right(), y);
        }
        if (triggerUtcMs >= viewStartMs && triggerUtcMs <= viewEndMs) {
            painter.setPen(QPen(QColor(255, 210, 60, 220), 1, Qt::DashLine));
            const int x = qRound(xForTime(triggerUtcMs));
            painter.drawLine(x, plotRect.top(), x, plotRect.bottom());
            painter.drawText(x + 4, plotRect.top() + 14, QStringLiteral("T"));
        }

        int visibleSamples = 0;
        for (const PlotSample &sample : samples) {
            if (sample.utcMs >= viewStartMs && sample.utcMs <= viewEndMs && std::isfinite(sample.levelDb)) ++visibleSamples;
        }
        painter.setPen(QPen(QColor(80, 235, 150), visibleSamples > plotRect.width() * 2 ? 1 : 2));
        if (visibleSamples > plotRect.width() * 2) {
            std::vector<float> pixelMin(static_cast<std::size_t>(plotRect.width() + 1),
                                        std::numeric_limits<float>::infinity());
            std::vector<float> pixelMax(static_cast<std::size_t>(plotRect.width() + 1),
                                        -std::numeric_limits<float>::infinity());
            for (const PlotSample &sample : samples) {
                if (sample.utcMs < viewStartMs || sample.utcMs > viewEndMs || !std::isfinite(sample.levelDb)) continue;
                const int pixel = std::clamp(qRound(xForTime(sample.utcMs)) - plotRect.left(), 0, plotRect.width());
                pixelMin[static_cast<std::size_t>(pixel)] = std::min(pixelMin[static_cast<std::size_t>(pixel)], sample.levelDb);
                pixelMax[static_cast<std::size_t>(pixel)] = std::max(pixelMax[static_cast<std::size_t>(pixel)], sample.levelDb);
            }
            for (int pixel = 0; pixel <= plotRect.width(); ++pixel) {
                const float minimum = pixelMin[static_cast<std::size_t>(pixel)];
                const float maximum = pixelMax[static_cast<std::size_t>(pixel)];
                if (!std::isfinite(minimum) || !std::isfinite(maximum)) continue;
                const int x = plotRect.left() + pixel;
                painter.drawLine(x, qRound(yForLevel(maximum)), x, qRound(yForLevel(minimum)));
            }
        } else {
            QPainterPath path;
            bool started = false;
            for (const PlotSample &sample : samples) {
                if (sample.utcMs < viewStartMs || sample.utcMs > viewEndMs || !std::isfinite(sample.levelDb)) continue;
                const QPointF point(xForTime(sample.utcMs), yForLevel(sample.levelDb));
                if (!started) { path.moveTo(point); started = true; }
                else path.lineTo(point);
            }
            if (started) {
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.drawPath(path);
            }
        }

        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QColor(155, 180, 195));
        painter.drawRect(plotRect);
        if (hoverVisible && plotRect.contains(hoverPos) && !samples.empty()) {
            const qint64 targetMs = viewStartMs + qRound64((hoverPos.x() - plotRect.left()) /
                                                           static_cast<double>(plotRect.width()) *
                                                           (viewEndMs - viewStartMs));
            const PlotSample *nearest = nullptr;
            qint64 bestDistance = std::numeric_limits<qint64>::max();
            for (const PlotSample &sample : samples) {
                const qint64 distance = std::abs(sample.utcMs - targetMs);
                if (distance < bestDistance) { bestDistance = distance; nearest = &sample; }
            }
            if (nearest) {
                const int x = qRound(xForTime(nearest->utcMs));
                const int y = qRound(yForLevel(nearest->levelDb));
                painter.setPen(QPen(QColor(210, 240, 255, 180), 1, Qt::DashLine));
                painter.drawLine(x, plotRect.top(), x, plotRect.bottom());
                painter.setBrush(QColor(255, 255, 255));
                painter.drawEllipse(QPoint(x, y), 3, 3);
                const double relativeSeconds = (nearest->utcMs -
                    (triggerUtcMs >= 0 ? triggerUtcMs : viewEndMs)) / 1000.0;
                const QString label = QStringLiteral("%1 s / %2 dB")
                                          .arg(relativeSeconds, 0, 'f', 3)
                                          .arg(nearest->levelDb, 0, 'f', 2);
                const QRect labelRect = painter.fontMetrics().boundingRect(label).adjusted(-5, -3, 5, 3);
                QRect placed = labelRect;
                placed.moveTopLeft(QPoint(std::min(x + 8, width() - placed.width() - 4),
                                           std::max(4, y - placed.height() - 6)));
                painter.fillRect(placed, QColor(0, 0, 0, 210));
                painter.setPen(QColor(230, 245, 255));
                painter.drawText(placed, Qt::AlignCenter, label);
            }
        }
    }

private:
    std::vector<PlotSample> samples;
    double timeSpanSeconds = 10.0;
    int pretrigger = 25;
    qint64 triggerUtcMs = -1;
    float triggerLevelDb = -60.0f;
    bool triggerVisible = false;
    bool triggerLockedWindow = false;
    bool autoScaleY = true;
    float fixedLevelMin = -140.0f;
    float fixedLevelMax = 0.0f;
    bool hoverVisible = false;
    QPoint hoverPos;
};

ZeroSpanDialog::ZeroSpanDialog(Translator translator, QWidget *parent)
    : QDialog(parent), translator(std::move(translator)) {
    setWindowTitle(text(QStringLiteral("zero_span_title"), QStringLiteral("Zero Span")));
    resize(920, 570);

    sourceCombo = new QComboBox(this);
    sourceCombo->addItem(text(QStringLiteral("zero_span_source_listen"), QStringLiteral("Listening")), static_cast<int>(Source::Listening));
    sourceCombo->addItem(QStringLiteral("A"), static_cast<int>(Source::MarkerA));
    sourceCombo->addItem(QStringLiteral("B"), static_cast<int>(Source::MarkerB));
    sourceCombo->addItem(text(QStringLiteral("zero_span_source_peak"), QStringLiteral("Peak")), static_cast<int>(Source::Peak));
    bandwidthSpin = new QDoubleSpinBox(this);
    bandwidthSpin->setRange(0.0, 10000.0);
    bandwidthSpin->setDecimals(3);
    bandwidthSpin->setSingleStep(1.0);
    bandwidthSpin->setSuffix(QStringLiteral(" kHz"));
    bandwidthSpin->setToolTip(text(QStringLiteral("zero_span_bandwidth_tooltip"),
                                   QStringLiteral("Integration bandwidth around the selected frequency; zero uses the nearest displayed bin.")));
    timeSpanSpin = new QDoubleSpinBox(this);
    timeSpanSpin->setRange(0.1, 300.0);
    timeSpanSpin->setDecimals(1);
    timeSpanSpin->setSingleStep(1.0);
    timeSpanSpin->setSuffix(QStringLiteral(" s"));
    frequencyLabel = new QLabel(QStringLiteral("--"), this);
    frequencyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    triggerModeCombo = new QComboBox(this);
    triggerModeCombo->addItem(text(QStringLiteral("off"), QStringLiteral("Off")), static_cast<int>(TriggerMode::Off));
    triggerModeCombo->addItem(text(QStringLiteral("auto"), QStringLiteral("Auto")), static_cast<int>(TriggerMode::Auto));
    triggerModeCombo->addItem(text(QStringLiteral("zero_span_normal"), QStringLiteral("Normal")), static_cast<int>(TriggerMode::Normal));
    triggerEdgeCombo = new QComboBox(this);
    triggerEdgeCombo->addItem(text(QStringLiteral("zero_span_rising"), QStringLiteral("Rising")), static_cast<int>(TriggerEdge::Rising));
    triggerEdgeCombo->addItem(text(QStringLiteral("zero_span_falling"), QStringLiteral("Falling")), static_cast<int>(TriggerEdge::Falling));
    triggerLevelSpin = new QDoubleSpinBox(this);
    triggerLevelSpin->setRange(-200.0, 50.0);
    triggerLevelSpin->setDecimals(1);
    triggerLevelSpin->setSingleStep(1.0);
    triggerLevelSpin->setSuffix(QStringLiteral(" dB"));
    pretriggerSpin = new QSpinBox(this);
    pretriggerSpin->setRange(0, 90);
    pretriggerSpin->setSuffix(QStringLiteral(" %"));

    autoScaleCheckbox = new QCheckBox(text(QStringLiteral("zero_span_auto_y"), QStringLiteral("Auto Y")), this);
    levelMinSpin = new QDoubleSpinBox(this);
    levelMinSpin->setRange(-200.0, 49.0);
    levelMinSpin->setSuffix(QStringLiteral(" dB"));
    levelMaxSpin = new QDoubleSpinBox(this);
    levelMaxSpin->setRange(-199.0, 50.0);
    levelMaxSpin->setSuffix(QStringLiteral(" dB"));

    runButton = new QPushButton(text(QStringLiteral("zero_span_pause"), QStringLiteral("Pause")), this);
    runButton->setCheckable(true);
    runButton->setChecked(true);
    armButton = new QPushButton(text(QStringLiteral("zero_span_arm"), QStringLiteral("Arm")), this);
    clearButton = new QPushButton(text(QStringLiteral("clear"), QStringLiteral("Clear")), this);
    exportButton = new QPushButton(QStringLiteral("CSV"), this);
    exportButton->setToolTip(text(QStringLiteral("zero_span_export_tooltip"), QStringLiteral("Export the current Zero Span trace to CSV")));
    statusLabel = new QLabel(text(QStringLiteral("zero_span_waiting"), QStringLiteral("Waiting for spectrum data")), this);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    plot = new ZeroSpanPlotWidget(this);

    auto *topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel(text(QStringLiteral("zero_span_source"), QStringLiteral("Source:")), this));
    topRow->addWidget(sourceCombo);
    topRow->addWidget(new QLabel(text(QStringLiteral("zero_span_bandwidth"), QStringLiteral("Measure BW:")), this));
    topRow->addWidget(bandwidthSpin);
    topRow->addWidget(new QLabel(text(QStringLiteral("zero_span_time"), QStringLiteral("Time:")), this));
    topRow->addWidget(timeSpanSpin);
    topRow->addWidget(frequencyLabel, 1);
    topRow->addWidget(runButton);

    auto *triggerRow = new QHBoxLayout();
    triggerRow->addWidget(new QLabel(text(QStringLiteral("zero_span_trigger"), QStringLiteral("Trigger:")), this));
    triggerRow->addWidget(triggerModeCombo);
    triggerRow->addWidget(triggerEdgeCombo);
    triggerRow->addWidget(triggerLevelSpin);
    triggerRow->addWidget(new QLabel(text(QStringLiteral("zero_span_pretrigger"), QStringLiteral("Pre:")), this));
    triggerRow->addWidget(pretriggerSpin);
    triggerRow->addWidget(armButton);
    triggerRow->addSpacing(12);
    triggerRow->addWidget(autoScaleCheckbox);
    triggerRow->addWidget(levelMinSpin);
    triggerRow->addWidget(levelMaxSpin);
    triggerRow->addStretch(1);
    triggerRow->addWidget(clearButton);
    triggerRow->addWidget(exportButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(topRow);
    layout->addLayout(triggerRow);
    layout->addWidget(plot, 1);
    layout->addWidget(statusLabel);

    loadSettings();
    connect(sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { clearTrace(); saveSettings(); });
    connect(bandwidthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { clearTrace(); saveSettings(); });
    connect(timeSpanSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { trimRollingSamples(QDateTime::currentMSecsSinceEpoch()); updateStatus(); saveSettings(); });
    connect(triggerModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { clearTrace(); updateControlState(); saveSettings(); });
    connect(triggerEdgeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { havePreviousLevel = false; saveSettings(); });
    connect(triggerLevelSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { havePreviousLevel = false; updateStatus(); saveSettings(); });
    connect(pretriggerSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { clearTrace(); saveSettings(); });
    connect(autoScaleCheckbox, &QCheckBox::toggled, this, [this](bool) { updateControlState(); updateStatus(); saveSettings(); });
    connect(levelMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (value >= levelMaxSpin->value()) levelMaxSpin->setValue(value + 1.0);
        updateStatus(); saveSettings();
    });
    connect(levelMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (value <= levelMinSpin->value()) levelMinSpin->setValue(value - 1.0);
        updateStatus(); saveSettings();
    });
    connect(runButton, &QPushButton::toggled, this, [this](bool checked) {
        running = checked;
        runButton->setText(checked ? text(QStringLiteral("zero_span_pause"), QStringLiteral("Pause"))
                                   : text(QStringLiteral("zero_span_run"), QStringLiteral("Run")));
        updateControlState();
    });
    connect(armButton, &QPushButton::clicked, this, &ZeroSpanDialog::armTrigger);
    connect(clearButton, &QPushButton::clicked, this, &ZeroSpanDialog::clearTrace);
    connect(exportButton, &QPushButton::clicked, this, &ZeroSpanDialog::exportCsv);
    updateControlState();
    updateStatus();
}

ZeroSpanDialog::~ZeroSpanDialog() {
    saveSettings();
}

QString ZeroSpanDialog::text(const QString &key, const QString &fallback) const {
    return translator ? translator(key, fallback) : fallback;
}

void ZeroSpanDialog::loadSettings() {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    sourceCombo->setCurrentIndex((std::clamp)(settings.value(QStringLiteral("zeroSpan/source"), 0).toInt(), 0, 3));
    bandwidthSpin->setValue((std::clamp)(settings.value(QStringLiteral("zeroSpan/bandwidthKhz"), 0.0).toDouble(), 0.0, 10000.0));
    timeSpanSpin->setValue((std::clamp)(settings.value(QStringLiteral("zeroSpan/timeSpanSeconds"), 10.0).toDouble(), 0.1, 300.0));
    triggerModeCombo->setCurrentIndex((std::clamp)(settings.value(QStringLiteral("zeroSpan/triggerMode"), 0).toInt(), 0, 2));
    triggerEdgeCombo->setCurrentIndex((std::clamp)(settings.value(QStringLiteral("zeroSpan/triggerEdge"), 0).toInt(), 0, 1));
    triggerLevelSpin->setValue((std::clamp)(settings.value(QStringLiteral("zeroSpan/triggerLevelDb"), -60.0).toDouble(), -200.0, 50.0));
    pretriggerSpin->setValue((std::clamp)(settings.value(QStringLiteral("zeroSpan/pretriggerPercent"), 25).toInt(), 0, 90));
    autoScaleCheckbox->setChecked(settings.value(QStringLiteral("zeroSpan/autoScaleY"), true).toBool());
    levelMinSpin->setValue((std::clamp)(settings.value(QStringLiteral("zeroSpan/levelMinDb"), -140.0).toDouble(), -200.0, 49.0));
    levelMaxSpin->setValue((std::clamp)(settings.value(QStringLiteral("zeroSpan/levelMaxDb"), 0.0).toDouble(), -199.0, 50.0));
    const QByteArray geometry = settings.value(QStringLiteral("zeroSpan/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
}

void ZeroSpanDialog::saveSettings() const {
    if (!sourceCombo) return;
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("zeroSpan/source"), sourceCombo->currentIndex());
    settings.setValue(QStringLiteral("zeroSpan/bandwidthKhz"), bandwidthSpin->value());
    settings.setValue(QStringLiteral("zeroSpan/timeSpanSeconds"), timeSpanSpin->value());
    settings.setValue(QStringLiteral("zeroSpan/triggerMode"), triggerModeCombo->currentIndex());
    settings.setValue(QStringLiteral("zeroSpan/triggerEdge"), triggerEdgeCombo->currentIndex());
    settings.setValue(QStringLiteral("zeroSpan/triggerLevelDb"), triggerLevelSpin->value());
    settings.setValue(QStringLiteral("zeroSpan/pretriggerPercent"), pretriggerSpin->value());
    settings.setValue(QStringLiteral("zeroSpan/autoScaleY"), autoScaleCheckbox->isChecked());
    settings.setValue(QStringLiteral("zeroSpan/levelMinDb"), levelMinSpin->value());
    settings.setValue(QStringLiteral("zeroSpan/levelMaxDb"), levelMaxSpin->value());
    settings.setValue(QStringLiteral("zeroSpan/geometry"), saveGeometry());
}

void ZeroSpanDialog::closeEvent(QCloseEvent *event) {
    saveSettings();
    QDialog::closeEvent(event);
}

double ZeroSpanDialog::selectedFrequency(double listeningFrequencyHz,
                                         const SpectrumScienceMarker &markerA,
                                         const SpectrumScienceMarker &markerB,
                                         const SpectrumScienceMetrics &metrics) const {
    switch (static_cast<Source>(sourceCombo->currentData().toInt())) {
    case Source::MarkerA: return markerA.enabled ? markerA.frequencyHz : std::numeric_limits<double>::quiet_NaN();
    case Source::MarkerB: return markerB.enabled ? markerB.frequencyHz : std::numeric_limits<double>::quiet_NaN();
    case Source::Peak: return metrics.valid ? metrics.peakFrequencyHz : std::numeric_limits<double>::quiet_NaN();
    case Source::Listening:
    default: return listeningFrequencyHz;
    }
}

float ZeroSpanDialog::measuredLevel(const std::vector<float> &frequencies,
                                    const std::vector<float> &levels,
                                    double targetFrequencyHz,
                                    bool fftShiftedStorage) const {
    const std::size_t count = std::min(frequencies.size(), levels.size());
    if (count == 0 || !std::isfinite(targetFrequencyHz)) return std::numeric_limits<float>::quiet_NaN();
    const double bandwidthHz = bandwidthSpin->value() * 1000.0;
    auto levelAtDisplayIndex = [&](int displayIndex) {
        const int normalizedIndex = std::clamp(displayIndex, 0, static_cast<int>(count) - 1);
        const int sourceIndex = fftShiftedStorage
                                    ? (normalizedIndex + static_cast<int>(count) / 2) % static_cast<int>(count)
                                    : normalizedIndex;
        return levels[static_cast<std::size_t>(sourceIndex)];
    };

    if (count >= 2) {
        const double firstFrequency = frequencies.front();
        const double lastFrequency = frequencies[count - 1];
        const double stepHz = (lastFrequency - firstFrequency) / static_cast<double>(count - 1);
        const double quarterExpected = firstFrequency + stepHz * static_cast<double>(count / 4);
        const bool regularGrid = std::isfinite(firstFrequency) && std::isfinite(lastFrequency) &&
                                 std::isfinite(stepHz) && stepHz > 0.0 &&
                                 std::abs(static_cast<double>(frequencies[count / 4]) - quarterExpected) <=
                                     std::max(1.0, stepHz * 0.35);
        if (regularGrid && (targetFrequencyHz < firstFrequency - stepHz ||
                            targetFrequencyHz > lastFrequency + stepHz)) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        if (regularGrid) {
            const int nearestIndex = std::clamp(
                static_cast<int>(std::lround((targetFrequencyHz - firstFrequency) / stepHz)),
                0,
                static_cast<int>(count) - 1);
            if (bandwidthHz <= 0.0) return levelAtDisplayIndex(nearestIndex);
            const int firstIndex = std::clamp(
                static_cast<int>(std::ceil((targetFrequencyHz - bandwidthHz * 0.5 - firstFrequency) / stepHz)),
                0,
                static_cast<int>(count) - 1);
            const int lastIndex = std::clamp(
                static_cast<int>(std::floor((targetFrequencyHz + bandwidthHz * 0.5 - firstFrequency) / stepHz)),
                firstIndex,
                static_cast<int>(count) - 1);
            double powerSum = 0.0;
            int included = 0;
            for (int index = firstIndex; index <= lastIndex; ++index) {
                const float level = levelAtDisplayIndex(index);
                if (!std::isfinite(level)) continue;
                powerSum += dbToPower(level);
                ++included;
            }
            return included > 0 ? powerToDb(powerSum) : levelAtDisplayIndex(nearestIndex);
        }
    }

    int nearest = -1;
    double nearestDistance = std::numeric_limits<double>::infinity();
    double powerSum = 0.0;
    int included = 0;
    for (int i = 0; i < static_cast<int>(count); ++i) {
        const double frequency = frequencies[static_cast<std::size_t>(i)];
        const float level = levelAtDisplayIndex(i);
        if (!std::isfinite(frequency) || !std::isfinite(level)) continue;
        const double distance = std::abs(frequency - targetFrequencyHz);
        if (distance < nearestDistance) { nearestDistance = distance; nearest = i; }
        if (bandwidthHz > 0.0 && distance <= bandwidthHz * 0.5) {
            powerSum += dbToPower(level);
            ++included;
        }
    }
    if (included > 0) return powerToDb(powerSum);
    return nearest >= 0 ? levelAtDisplayIndex(nearest)
                        : std::numeric_limits<float>::quiet_NaN();
}

bool ZeroSpanDialog::triggerCrossed(float previous, float current) const {
    const float threshold = static_cast<float>(triggerLevelSpin->value());
    return static_cast<TriggerEdge>(triggerEdgeCombo->currentData().toInt()) == TriggerEdge::Rising
               ? previous < threshold && current >= threshold
               : previous > threshold && current <= threshold;
}

void ZeroSpanDialog::appendSpectrumFrame(const std::vector<float> &frequencies,
                                         const std::vector<float> &levels,
                                         double listeningFrequencyHz,
                                         const SpectrumScienceMarker &markerA,
                                         const SpectrumScienceMarker &markerB,
                                         const SpectrumScienceMetrics &metrics,
                                         bool fftShiftedStorage) {
    if (!isVisible() || !running) return;
    const double frequencyHz = selectedFrequency(listeningFrequencyHz, markerA, markerB, metrics);
    const float levelDb = measuredLevel(frequencies, levels, frequencyHz, fftShiftedStorage);
    if (!std::isfinite(frequencyHz) || !std::isfinite(levelDb)) {
        frequencyLabel->setText(text(QStringLiteral("zero_span_source_unavailable"), QStringLiteral("Selected source is unavailable")));
        return;
    }
    currentFrequencyHz = frequencyHz;
    frequencyLabel->setText(QStringLiteral("%1 / %2 dB").arg(frequencyText(frequencyHz)).arg(levelDb, 0, 'f', 2));
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    samples.push_back({nowMs, frequencyHz, levelDb});

    const TriggerMode mode = static_cast<TriggerMode>(triggerModeCombo->currentData().toInt());
    const bool crossing = havePreviousLevel && triggerCrossed(previousLevelDb, levelDb);
    previousLevelDb = levelDb;
    havePreviousLevel = true;
    if (mode == TriggerMode::Off) {
        triggered = false;
        triggerUtcMs = -1;
        trimRollingSamples(nowMs);
    } else if (mode == TriggerMode::Auto) {
        if (crossing) { triggered = true; triggerUtcMs = nowMs; }
        trimRollingSamples(nowMs);
    } else if (!triggered) {
        const qint64 preMs = qRound64(timeSpanSpin->value() * pretriggerSpin->value() * 10.0);
        while (!samples.empty() && samples.front().utcMs < nowMs - preMs) samples.pop_front();
        if (armed && crossing) {
            triggered = true;
            triggerUtcMs = nowMs;
        }
    } else {
        const double postSeconds = timeSpanSpin->value() * (100 - pretriggerSpin->value()) / 100.0;
        if (nowMs >= triggerUtcMs + qRound64(postSeconds * 1000.0)) {
            running = false;
            armed = false;
            QSignalBlocker blocker(runButton);
            runButton->setChecked(false);
            runButton->setText(text(QStringLiteral("zero_span_run"), QStringLiteral("Run")));
        }
    }
    if (!displayUpdateClock.isValid()) displayUpdateClock.start();
    if (!running || displayUpdateClock.elapsed() >= 33) {
        displayUpdateClock.restart();
        updateStatus();
    }
}

void ZeroSpanDialog::trimRollingSamples(qint64 newestMs) {
    const qint64 cutoff = newestMs - qRound64(timeSpanSpin->value() * 1000.0);
    while (!samples.empty() && samples.front().utcMs < cutoff) samples.pop_front();
}

void ZeroSpanDialog::clearTrace() {
    samples.clear();
    armed = false;
    triggered = false;
    triggerUtcMs = -1;
    havePreviousLevel = false;
    displayUpdateClock.invalidate();
    updateControlState();
    updateStatus();
}

void ZeroSpanDialog::armTrigger() {
    clearTrace();
    armed = true;
    running = true;
    QSignalBlocker blocker(runButton);
    runButton->setChecked(true);
    runButton->setText(text(QStringLiteral("zero_span_pause"), QStringLiteral("Pause")));
    updateControlState();
    updateStatus();
}

void ZeroSpanDialog::updateControlState() {
    const TriggerMode mode = static_cast<TriggerMode>(triggerModeCombo->currentData().toInt());
    const bool triggerEnabled = mode != TriggerMode::Off;
    triggerEdgeCombo->setEnabled(triggerEnabled);
    triggerLevelSpin->setEnabled(triggerEnabled);
    pretriggerSpin->setEnabled(mode == TriggerMode::Normal);
    armButton->setEnabled(mode == TriggerMode::Normal);
    levelMinSpin->setEnabled(!autoScaleCheckbox->isChecked());
    levelMaxSpin->setEnabled(!autoScaleCheckbox->isChecked());
}

void ZeroSpanDialog::updateStatus() {
    std::vector<PlotSample> plotSamples;
    plotSamples.reserve(samples.size());
    float minimum = std::numeric_limits<float>::infinity();
    float maximum = -std::numeric_limits<float>::infinity();
    double powerSum = 0.0;
    for (const Sample &sample : samples) {
        plotSamples.push_back({sample.utcMs, sample.levelDb});
        minimum = std::min(minimum, sample.levelDb);
        maximum = std::max(maximum, sample.levelDb);
        powerSum += dbToPower(sample.levelDb);
    }
    const TriggerMode mode = static_cast<TriggerMode>(triggerModeCombo->currentData().toInt());
    plot->setTrace(plotSamples,
                   timeSpanSpin->value(),
                   pretriggerSpin->value(),
                   triggerUtcMs,
                   static_cast<float>(triggerLevelSpin->value()),
                   mode != TriggerMode::Off,
                   mode == TriggerMode::Normal && triggered,
                   autoScaleCheckbox->isChecked(),
                   static_cast<float>(levelMinSpin->value()),
                   static_cast<float>(levelMaxSpin->value()));
    QString state;
    if (mode == TriggerMode::Normal && armed && !triggered) state = text(QStringLiteral("zero_span_armed"), QStringLiteral("ARMED"));
    else if (triggered) state = text(QStringLiteral("zero_span_triggered"), QStringLiteral("TRIGGERED"));
    else if (running) state = text(QStringLiteral("zero_span_running"), QStringLiteral("RUN"));
    else state = text(QStringLiteral("zero_span_paused"), QStringLiteral("PAUSED"));
    if (samples.empty()) {
        statusLabel->setText(QStringLiteral("%1 | %2").arg(state, text(QStringLiteral("zero_span_waiting"), QStringLiteral("Waiting for spectrum data"))));
        return;
    }
    const float average = powerToDb(powerSum / samples.size());
    const double duration = samples.size() > 1 ? (samples.back().utcMs - samples.front().utcMs) / 1000.0 : 0.0;
    const double rate = duration > 0.0 ? (samples.size() - 1) / duration : 0.0;
    statusLabel->setText(text(QStringLiteral("zero_span_status_format"),
                              QStringLiteral("%1 | %2 samples / %3 Hz | min %4 dB | avg %5 dB | max %6 dB | p-p %7 dB"))
                             .arg(state)
                             .arg(samples.size())
                             .arg(rate, 0, 'f', 1)
                             .arg(minimum, 0, 'f', 2)
                             .arg(average, 0, 'f', 2)
                             .arg(maximum, 0, 'f', 2)
                             .arg(maximum - minimum, 0, 'f', 2));
}

void ZeroSpanDialog::exportCsv() {
    if (samples.empty()) {
        QMessageBox::information(this,
                                 text(QStringLiteral("zero_span_title"), QStringLiteral("Zero Span")),
                                 text(QStringLiteral("zero_span_no_data"), QStringLiteral("No Zero Span samples to export.")));
        return;
    }
    const QString defaultName = QStringLiteral("zero_span_%1.csv")
                                    .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(this,
                                                      text(QStringLiteral("zero_span_export_title"), QStringLiteral("Export Zero Span CSV")),
                                                      QCoreApplication::applicationDirPath() + QLatin1Char('/') + defaultName,
                                                      text(QStringLiteral("csv_files_filter"), QStringLiteral("CSV files (*.csv)")));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this,
                             text(QStringLiteral("zero_span_title"), QStringLiteral("Zero Span")),
                             text(QStringLiteral("scan_measurement_csv_write_failed"), QStringLiteral("Cannot write CSV file.")));
        return;
    }
    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << "utc_iso,utc_ms,relative_seconds,frequency_hz,level_dbfs\n";
    const qint64 referenceMs = triggerUtcMs >= 0 ? triggerUtcMs : samples.front().utcMs;
    for (const Sample &sample : samples) {
        out << QDateTime::fromMSecsSinceEpoch(sample.utcMs).toString(Qt::ISODateWithMs) << ','
            << sample.utcMs << ','
            << QString::number((sample.utcMs - referenceMs) / 1000.0, 'f', 6) << ','
            << QString::number(sample.frequencyHz, 'f', 3) << ','
            << QString::number(sample.levelDb, 'f', 3) << '\n';
    }
}
