#include "scientificsessiontools.h"
#include "hdf5spectrumexportui.h"
#include "experimentbundledialog.h"

#include "appsettingsutils.h"
#include "fft.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
#include <utility>

namespace {
QString frequencyText(double hz) {
    if (!std::isfinite(hz)) return QStringLiteral("--");
    if (std::abs(hz) >= 1.0e9) return QStringLiteral("%1 GHz").arg(hz / 1.0e9, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e6) return QStringLiteral("%1 MHz").arg(hz / 1.0e6, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e3) return QStringLiteral("%1 kHz").arg(hz / 1.0e3, 0, 'f', 3);
    return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 1);
}

QString levelWithUnit(double db, int unit) {
    switch ((std::clamp)(unit, 0, 3)) {
    case 1: return QStringLiteral("%1 dBm").arg(db, 0, 'f', 2);
    case 2: return QStringLiteral("%1 dBuV").arg(db + 106.9897, 0, 'f', 2);
    case 3:
        return QStringLiteral("%1 uV")
            .arg(std::pow(10.0, (db + 106.9897) / 20.0), 0, 'g', 6);
    default: return QStringLiteral("%1 dBFS").arg(db, 0, 'f', 2);
    }
}

struct PulseSample {
    qint64 utcMs = 0;
    float levelDb = -160.0f;
};

struct PulseEvent {
    qint64 startMs = 0;
    qint64 endMs = 0;
    double riseMs = 0.0;
    double fallMs = 0.0;
    float peakDb = -160.0f;
};

struct PulseMetrics {
    double thresholdDb = -80.0;
    double averageWidthMs = 0.0;
    double averagePriMs = 0.0;
    double repetitionHz = 0.0;
    double dutyPercent = 0.0;
    double riseMs = 0.0;
    double fallMs = 0.0;
    int pulseCount = 0;
};

QColor pulseColor(double normalized) {
    normalized = (std::clamp)(normalized, 0.0, 1.0);
    if (normalized < 0.35) {
        const double t = normalized / 0.35;
        return QColor(0, static_cast<int>(45 + 150 * t), static_cast<int>(90 + 165 * t));
    }
    if (normalized < 0.72) {
        const double t = (normalized - 0.35) / 0.37;
        return QColor(static_cast<int>(255 * t), 225, static_cast<int>(255 * (1.0 - t)));
    }
    const double t = (normalized - 0.72) / 0.28;
    return QColor(255, static_cast<int>(225 * (1.0 - t)), 0);
}

class PulsePlotCanvas : public QWidget {
public:
    explicit PulsePlotCanvas(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(640, 400);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setData(const std::deque<PulseSample> &newSamples,
                 double newThreshold,
                 const QImage &newFolded,
                 double newMinimum,
                 double newMaximum,
                 double newFrequency,
                 double newFoldPeriodMs) {
        samples = newSamples;
        thresholdDb = newThreshold;
        folded = newFolded;
        minimumDb = newMinimum;
        maximumDb = newMaximum;
        frequencyHz = newFrequency;
        foldPeriodMs = newFoldPeriodMs;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(12, 14, 18));
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        const int left = 72;
        const int right = 18;
        const QRect trace(left, 14, (std::max)(1, width() - left - right),
                          (std::max)(80, static_cast<int>(height() * 0.53)));
        const QRect fold(left,
                         trace.bottom() + 34,
                         trace.width(),
                         (std::max)(70, height() - trace.bottom() - 72));
        painter.setPen(QColor(52, 59, 69));
        for (int i = 0; i <= 4; ++i) {
            const int y = trace.top() + trace.height() * i / 4;
            painter.drawLine(trace.left(), y, trace.right(), y);
            const double level = maximumDb - (maximumDb - minimumDb) * i / 4.0;
            painter.setPen(QColor(190, 198, 210));
            painter.drawText(QRect(2, y - 10, left - 8, 20),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(level, 'f', 1));
            painter.setPen(QColor(52, 59, 69));
        }
        if (samples.size() >= 2) {
            const qint64 firstMs = samples.front().utcMs;
            const qint64 lastMs = samples.back().utcMs;
            const double timeRange = (std::max)(1.0, static_cast<double>(lastMs - firstMs));
            const double levelRange = (std::max)(1.0, maximumDb - minimumDb);
            QPainterPath path;
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const double x = trace.left() + trace.width() *
                    (samples[i].utcMs - firstMs) / timeRange;
                const double y = trace.top() + trace.height() *
                    (maximumDb - samples[i].levelDb) / levelRange;
                if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
            }
            painter.setPen(QPen(QColor(80, 245, 125), 1.4));
            painter.drawPath(path);
            const double thresholdY = trace.top() + trace.height() *
                (maximumDb - thresholdDb) / levelRange;
            painter.setPen(QPen(QColor(255, 100, 70), 1.0, Qt::DashLine));
            painter.drawLine(trace.left(), static_cast<int>(thresholdY),
                             trace.right(), static_cast<int>(thresholdY));
            painter.setPen(QColor(205, 212, 222));
            painter.drawText(QRect(trace.left(), trace.bottom() + 5, trace.width(), 20),
                             Qt::AlignLeft,
                             QStringLiteral("%1  |  %2 s")
                                 .arg(frequencyText(frequencyHz))
                                 .arg(timeRange / 1000.0, 0, 'f', 2));
        }
        painter.setPen(QColor(100, 112, 128));
        painter.drawRect(trace.adjusted(0, 0, -1, -1));
        if (!folded.isNull()) painter.drawImage(fold, folded);
        painter.setPen(QColor(100, 112, 128));
        painter.drawRect(fold.adjusted(0, 0, -1, -1));
        painter.setPen(QColor(205, 212, 222));
        painter.drawText(QRect(fold.left(), fold.bottom() + 4, fold.width(), 20),
                         Qt::AlignCenter,
                         QStringLiteral("0 ... %1 ms").arg(foldPeriodMs, 0, 'f', 3));
    }

private:
    std::deque<PulseSample> samples;
    QImage folded;
    double thresholdDb = -80.0;
    double minimumDb = -140.0;
    double maximumDb = -20.0;
    double frequencyHz = 0.0;
    double foldPeriodMs = 0.0;
};

float integratedLevel(const std::vector<float> &frequencies,
                      const std::vector<float> &levels,
                      double targetHz,
                      double bandwidthHz) {
    const std::size_t count = (std::min)(frequencies.size(), levels.size());
    if (count == 0 || !std::isfinite(targetHz)) return -200.0f;
    const double half = (std::max)(0.0, bandwidthHz * 0.5);
    double power = 0.0;
    int used = 0;
    double nearestDistance = std::numeric_limits<double>::max();
    float nearest = -200.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const double distance = std::abs(static_cast<double>(frequencies[i]) - targetHz);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = levels[i];
        }
        if (half > 0.0 && distance <= half) {
            power += std::pow(10.0, static_cast<double>(levels[i]) / 10.0);
            ++used;
        }
    }
    return used > 0 && power > 1.0e-20
               ? static_cast<float>(10.0 * std::log10(power))
               : nearest;
}
}

struct PulseAnalysisWidget::Impl {
    Translator translator;
    PulseAnalysisWidget *owner = nullptr;
    PulsePlotCanvas *plot = nullptr;
    QLabel *status = nullptr;
    QComboBox *source = nullptr;
    QDoubleSpinBox *bandwidth = nullptr;
    QComboBox *thresholdMode = nullptr;
    QDoubleSpinBox *threshold = nullptr;
    QDoubleSpinBox *hysteresis = nullptr;
    QDoubleSpinBox *historySeconds = nullptr;
    QDoubleSpinBox *foldPeriod = nullptr;
    QSpinBox *foldRows = nullptr;
    QDoubleSpinBox *minimum = nullptr;
    QDoubleSpinBox *maximum = nullptr;
    QCheckBox *paused = nullptr;
    std::deque<PulseSample> samples;
    std::vector<PulseEvent> events;
    QImage foldedImage;
    QElapsedTimer displayClock;
    PulseMetrics metrics;
    double currentFrequencyHz = 0.0;
    int amplitudeUnit = 0;

    QString text(const QString &key, const QString &fallback) const {
        return translator ? translator(key, fallback) : fallback;
    }

    void clear() {
        samples.clear();
        events.clear();
        foldedImage = QImage();
        metrics = PulseMetrics{};
        status->setText(text(QStringLiteral("research_pulse_waiting"),
                             QStringLiteral("Waiting for spectrum frames")));
        plot->setData(samples, threshold->value(), foldedImage,
                      minimum->value(), maximum->value(), currentFrequencyHz, 0.0);
    }

    void saveSettings() const {
        QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
        settings.beginGroup(QStringLiteral("researchPulse"));
        settings.setValue(QStringLiteral("source"), source->currentData());
        settings.setValue(QStringLiteral("bandwidthHz"), bandwidth->value());
        settings.setValue(QStringLiteral("thresholdMode"), thresholdMode->currentData());
        settings.setValue(QStringLiteral("thresholdDb"), threshold->value());
        settings.setValue(QStringLiteral("hysteresisDb"), hysteresis->value());
        settings.setValue(QStringLiteral("historySeconds"), historySeconds->value());
        settings.setValue(QStringLiteral("foldPeriodMs"), foldPeriod->value());
        settings.setValue(QStringLiteral("foldRows"), foldRows->value());
        settings.setValue(QStringLiteral("minimumDb"), minimum->value());
        settings.setValue(QStringLiteral("maximumDb"), maximum->value());
        settings.endGroup();
    }

    double effectiveThreshold() const {
        if (thresholdMode->currentData().toInt() == 0 || samples.empty()) {
            return threshold->value();
        }
        std::vector<float> ordered;
        ordered.reserve(samples.size());
        for (const PulseSample &sample : samples) ordered.push_back(sample.levelDb);
        const std::size_t index = ordered.size() / 5;
        std::nth_element(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(index),
                         ordered.end());
        return ordered[index] + threshold->value();
    }

    void analyze();
    void exportCsv();
};

void PulseAnalysisWidget::Impl::analyze() {
    events.clear();
    metrics = PulseMetrics{};
    if (samples.size() < 3) return;
    metrics.thresholdDb = effectiveThreshold();
    const double releaseThreshold = metrics.thresholdDb - hysteresis->value();
    bool active = false;
    std::size_t startIndex = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (!active && samples[i].levelDb >= metrics.thresholdDb) {
            active = true;
            startIndex = i;
        } else if (active && samples[i].levelDb <= releaseThreshold) {
            PulseEvent event;
            event.startMs = samples[startIndex].utcMs;
            event.endMs = samples[i].utcMs;
            std::size_t peakIndex = startIndex;
            for (std::size_t j = startIndex; j <= i; ++j) {
                if (samples[j].levelDb > samples[peakIndex].levelDb) peakIndex = j;
            }
            event.peakDb = samples[peakIndex].levelDb;
            const double baseline = releaseThreshold;
            const double lowLevel = baseline + (event.peakDb - baseline) * 0.1;
            const double highLevel = baseline + (event.peakDb - baseline) * 0.9;
            std::size_t riseLow = startIndex > 12 ? startIndex - 12 : 0;
            std::size_t riseHigh = peakIndex;
            bool lowFound = false;
            for (std::size_t j = riseLow; j <= peakIndex; ++j) {
                if (!lowFound && samples[j].levelDb >= lowLevel) {
                    riseLow = j;
                    lowFound = true;
                }
                if (lowFound && samples[j].levelDb >= highLevel) {
                    riseHigh = j;
                    break;
                }
            }
            event.riseMs = static_cast<double>(
                samples[riseHigh].utcMs - samples[riseLow].utcMs);
            std::size_t fallHigh = peakIndex;
            std::size_t fallLow = i;
            bool highFound = false;
            for (std::size_t j = peakIndex; j <= i; ++j) {
                if (!highFound && samples[j].levelDb <= highLevel) {
                    fallHigh = j;
                    highFound = true;
                }
                if (highFound && samples[j].levelDb <= lowLevel) {
                    fallLow = j;
                    break;
                }
            }
            event.fallMs = static_cast<double>(
                samples[fallLow].utcMs - samples[fallHigh].utcMs);
            events.push_back(event);
            active = false;
        }
    }
    metrics.pulseCount = static_cast<int>(events.size());
    if (!events.empty()) {
        double totalWidth = 0.0;
        double totalRise = 0.0;
        double totalFall = 0.0;
        for (const PulseEvent &event : events) {
            totalWidth += static_cast<double>(event.endMs - event.startMs);
            totalRise += event.riseMs;
            totalFall += event.fallMs;
        }
        metrics.averageWidthMs = totalWidth / events.size();
        metrics.riseMs = totalRise / events.size();
        metrics.fallMs = totalFall / events.size();
        const double observedMs = static_cast<double>(
            samples.back().utcMs - samples.front().utcMs);
        metrics.dutyPercent = observedMs > 0.0 ? totalWidth * 100.0 / observedMs : 0.0;
    }
    if (events.size() >= 2) {
        double totalPri = 0.0;
        for (std::size_t i = 1; i < events.size(); ++i) {
            totalPri += static_cast<double>(events[i].startMs - events[i - 1].startMs);
        }
        metrics.averagePriMs = totalPri / static_cast<double>(events.size() - 1);
        metrics.repetitionHz = metrics.averagePriMs > 0.0
                                   ? 1000.0 / metrics.averagePriMs
                                   : 0.0;
    }

    const double periodMs = foldPeriod->value() > 0.0
                                ? foldPeriod->value()
                                : metrics.averagePriMs;
    const int imageWidth = 512;
    const int requestedRows = foldRows->value();
    foldedImage = QImage(imageWidth, requestedRows, QImage::Format_RGB32);
    foldedImage.fill(QColor(0, 0, 0));
    if (periodMs > 0.0 && !samples.empty()) {
        const qint64 anchor = !events.empty() ? events.front().startMs : samples.front().utcMs;
        const qint64 newestCycle = static_cast<qint64>(
            std::floor((samples.back().utcMs - anchor) / periodMs));
        const qint64 firstCycle = (std::max)(qint64(0), newestCycle - requestedRows + 1);
        const double range = (std::max)(1.0, maximum->value() - minimum->value());
        for (const PulseSample &sample : samples) {
            const double cycleValue = (sample.utcMs - anchor) / periodMs;
            const qint64 cycle = static_cast<qint64>(std::floor(cycleValue));
            if (cycle < firstCycle || cycle > newestCycle) continue;
            double phase = cycleValue - std::floor(cycleValue);
            if (phase < 0.0) phase += 1.0;
            const int x = (std::clamp)(
                static_cast<int>(phase * imageWidth), 0, imageWidth - 1);
            const int y = (std::clamp)(
                static_cast<int>(cycle - firstCycle), 0, requestedRows - 1);
            const double normalized = (sample.levelDb - minimum->value()) / range;
            foldedImage.setPixelColor(x, y, pulseColor(normalized));
        }
    }
    std::deque<PulseSample> displaySamples;
    const std::size_t displayLimit = 4096;
    const std::size_t displayStep =
        (std::max)(std::size_t(1), samples.size() / displayLimit);
    for (std::size_t i = 0; i < samples.size(); i += displayStep) {
        displaySamples.push_back(samples[i]);
    }
    if (!samples.empty() &&
        (displaySamples.empty() ||
         displaySamples.back().utcMs != samples.back().utcMs)) {
        displaySamples.push_back(samples.back());
    }
    plot->setData(displaySamples,
                  metrics.thresholdDb,
                  foldedImage,
                  minimum->value(),
                  maximum->value(),
                  currentFrequencyHz,
                  periodMs);
    status->setText(
        text(QStringLiteral("research_pulse_status"),
             QStringLiteral("Pulses %1 | width %2 ms | PRI %3 ms (%4 Hz) | duty %5% | rise/fall %6/%7 ms | threshold %8"))
            .arg(metrics.pulseCount)
            .arg(metrics.averageWidthMs, 0, 'f', 3)
            .arg(metrics.averagePriMs, 0, 'f', 3)
            .arg(metrics.repetitionHz, 0, 'f', 3)
            .arg(metrics.dutyPercent, 0, 'f', 2)
            .arg(metrics.riseMs, 0, 'f', 3)
            .arg(metrics.fallMs, 0, 'f', 3)
            .arg(levelWithUnit(metrics.thresholdDb, amplitudeUnit)));
}

void PulseAnalysisWidget::Impl::exportCsv() {
    if (samples.empty()) return;
    const QString path = QFileDialog::getSaveFileName(
        owner,
        text(QStringLiteral("research_pulse_export"), QStringLiteral("Export pulse analysis")),
        QStringLiteral("pulse-analysis-%1.csv")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))),
        QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream stream(&file);
    stream << "# frequency_hz," << currentFrequencyHz << '\n';
    stream << "# threshold_db," << metrics.thresholdDb << '\n';
    stream << "# pulse_count," << metrics.pulseCount << '\n';
    stream << "# average_width_ms," << metrics.averageWidthMs << '\n';
    stream << "# average_pri_ms," << metrics.averagePriMs << '\n';
    stream << "# repetition_hz," << metrics.repetitionHz << '\n';
    stream << "# duty_percent," << metrics.dutyPercent << '\n';
    stream << "utc_ms,utc_iso,level_db,active\n";
    for (const PulseSample &sample : samples) {
        stream << sample.utcMs << ','
               << QDateTime::fromMSecsSinceEpoch(sample.utcMs, Qt::UTC)
                      .toString(Qt::ISODateWithMs)
               << ',' << sample.levelDb << ','
               << (sample.levelDb >= metrics.thresholdDb ? 1 : 0) << '\n';
    }
}

PulseAnalysisWidget::PulseAnalysisWidget(Translator translator, QWidget *parent)
    : QWidget(parent), impl(std::make_unique<Impl>()) {
    impl->translator = std::move(translator);
    impl->owner = this;
    QVBoxLayout *root = new QVBoxLayout(this);
    QGridLayout *controls = new QGridLayout();
    impl->source = new QComboBox(this);
    impl->source->addItem(impl->text(QStringLiteral("research_pulse_listening"),
                                     QStringLiteral("Listening frequency")), 0);
    impl->source->addItem(impl->text(QStringLiteral("research_pulse_peak"),
                                     QStringLiteral("Strongest peak")), 1);
    impl->bandwidth = new QDoubleSpinBox(this);
    impl->bandwidth->setRange(0.0, 10000000.0);
    impl->bandwidth->setDecimals(1);
    impl->bandwidth->setSuffix(QStringLiteral(" Hz"));
    impl->thresholdMode = new QComboBox(this);
    impl->thresholdMode->addItem(impl->text(QStringLiteral("research_pulse_fixed"),
                                            QStringLiteral("Fixed threshold")), 0);
    impl->thresholdMode->addItem(impl->text(QStringLiteral("research_pulse_auto"),
                                            QStringLiteral("Noise + margin")), 1);
    impl->threshold = new QDoubleSpinBox(this);
    impl->threshold->setRange(-240.0, 100.0);
    impl->threshold->setDecimals(2);
    impl->threshold->setSuffix(QStringLiteral(" dB"));
    impl->hysteresis = new QDoubleSpinBox(this);
    impl->hysteresis->setRange(0.0, 60.0);
    impl->hysteresis->setSuffix(QStringLiteral(" dB"));
    impl->historySeconds = new QDoubleSpinBox(this);
    impl->historySeconds->setRange(0.5, 3600.0);
    impl->historySeconds->setSuffix(QStringLiteral(" s"));
    impl->foldPeriod = new QDoubleSpinBox(this);
    impl->foldPeriod->setRange(0.0, 100000.0);
    impl->foldPeriod->setDecimals(3);
    impl->foldPeriod->setSpecialValueText(
        impl->text(QStringLiteral("auto"), QStringLiteral("Auto")));
    impl->foldPeriod->setSuffix(QStringLiteral(" ms"));
    impl->foldRows = new QSpinBox(this);
    impl->foldRows->setRange(4, 256);
    impl->minimum = new QDoubleSpinBox(this);
    impl->minimum->setRange(-240.0, 80.0);
    impl->minimum->setSuffix(QStringLiteral(" dB"));
    impl->maximum = new QDoubleSpinBox(this);
    impl->maximum->setRange(-240.0, 100.0);
    impl->maximum->setSuffix(QStringLiteral(" dB"));
    impl->paused = new QCheckBox(
        impl->text(QStringLiteral("pause"), QStringLiteral("Pause")), this);
    QPushButton *clearButton = new QPushButton(
        impl->text(QStringLiteral("clear"), QStringLiteral("Clear")), this);
    QPushButton *exportButton = new QPushButton(
        impl->text(QStringLiteral("research_export_csv"), QStringLiteral("Export CSV")), this);

    controls->addWidget(new QLabel(impl->text(QStringLiteral("source"),
                                               QStringLiteral("Source:")), this), 0, 0);
    controls->addWidget(impl->source, 0, 1);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("bandwidth"),
                                               QStringLiteral("Bandwidth:")), this), 0, 2);
    controls->addWidget(impl->bandwidth, 0, 3);
    controls->addWidget(impl->thresholdMode, 0, 4);
    controls->addWidget(impl->threshold, 0, 5);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_pulse_hysteresis"),
                                               QStringLiteral("Hysteresis:")), this), 1, 0);
    controls->addWidget(impl->hysteresis, 1, 1);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_pulse_history"),
                                               QStringLiteral("History:")), this), 1, 2);
    controls->addWidget(impl->historySeconds, 1, 3);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_pulse_fold"),
                                               QStringLiteral("Fold period:")), this), 1, 4);
    controls->addWidget(impl->foldPeriod, 1, 5);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_pulse_rows"),
                                               QStringLiteral("Fold rows:")), this), 1, 6);
    controls->addWidget(impl->foldRows, 1, 7);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("minimum"),
                                               QStringLiteral("Minimum:")), this), 2, 0);
    controls->addWidget(impl->minimum, 2, 1);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("maximum"),
                                               QStringLiteral("Maximum:")), this), 2, 2);
    controls->addWidget(impl->maximum, 2, 3);
    controls->addWidget(impl->paused, 2, 4);
    controls->addWidget(clearButton, 2, 5);
    controls->addWidget(exportButton, 2, 6);
    controls->setColumnStretch(8, 1);
    root->addLayout(controls);
    impl->status = new QLabel(this);
    impl->status->setWordWrap(true);
    root->addWidget(impl->status);
    impl->plot = new PulsePlotCanvas(this);
    root->addWidget(impl->plot, 1);

    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("researchPulse"));
    impl->source->setCurrentIndex((std::max)(0, impl->source->findData(
        settings.value(QStringLiteral("source"), 0))));
    impl->bandwidth->setValue(settings.value(QStringLiteral("bandwidthHz"), 12500.0).toDouble());
    impl->thresholdMode->setCurrentIndex((std::max)(0, impl->thresholdMode->findData(
        settings.value(QStringLiteral("thresholdMode"), 1))));
    impl->threshold->setValue(settings.value(QStringLiteral("thresholdDb"), 8.0).toDouble());
    impl->hysteresis->setValue(settings.value(QStringLiteral("hysteresisDb"), 3.0).toDouble());
    impl->historySeconds->setValue(settings.value(QStringLiteral("historySeconds"), 30.0).toDouble());
    impl->foldPeriod->setValue(settings.value(QStringLiteral("foldPeriodMs"), 0.0).toDouble());
    impl->foldRows->setValue(settings.value(QStringLiteral("foldRows"), 64).toInt());
    impl->minimum->setValue(settings.value(QStringLiteral("minimumDb"), -140.0).toDouble());
    impl->maximum->setValue(settings.value(QStringLiteral("maximumDb"), -20.0).toDouble());
    settings.endGroup();
    impl->displayClock.start();
    impl->clear();

    auto changed = [this]() {
        if (impl->maximum->value() <= impl->minimum->value() + 1.0) {
            impl->maximum->setValue(impl->minimum->value() + 1.0);
        }
        impl->saveSettings();
        impl->analyze();
    };
    connect(impl->source, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        impl->saveSettings();
        impl->clear();
    });
    connect(impl->bandwidth, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        impl->saveSettings();
        impl->clear();
    });
    connect(impl->thresholdMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [changed](int) { changed(); });
    connect(impl->threshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
    connect(impl->hysteresis, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
    connect(impl->historySeconds, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
    connect(impl->foldPeriod, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
    connect(impl->foldRows, QOverload<int>::of(&QSpinBox::valueChanged), this, [changed](int) { changed(); });
    connect(impl->minimum, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
    connect(impl->maximum, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
    connect(clearButton, &QPushButton::clicked, this, [this]() { impl->clear(); });
    connect(exportButton, &QPushButton::clicked, this, [this]() { impl->exportCsv(); });
}

PulseAnalysisWidget::~PulseAnalysisWidget() = default;

void PulseAnalysisWidget::appendSpectrumFrame(const std::vector<float> &frequencies,
                                              const std::vector<float> &levels,
                                              const ScientificSessionContext &context,
                                              const SpectrumScienceMetrics &metrics,
                                              int amplitudeUnit) {
    if (!impl || impl->paused->isChecked() || frequencies.empty() || levels.empty()) return;
    const double targetHz = impl->source->currentData().toInt() == 1 && metrics.valid
                                ? metrics.peakFrequencyHz
                                : context.listeningFrequencyHz;
    if (!std::isfinite(targetHz)) return;
    const double resetDistance = (std::max)(1.0, impl->bandwidth->value() * 0.5);
    if (!impl->samples.empty() &&
        std::abs(targetHz - impl->currentFrequencyHz) > resetDistance) {
        impl->clear();
    }
    impl->currentFrequencyHz = targetHz;
    impl->amplitudeUnit = amplitudeUnit;
    PulseSample sample;
    sample.utcMs = QDateTime::currentMSecsSinceEpoch();
    sample.levelDb = integratedLevel(frequencies, levels, targetHz, impl->bandwidth->value());
    impl->samples.push_back(sample);
    const qint64 oldestMs = sample.utcMs -
        static_cast<qint64>(impl->historySeconds->value() * 1000.0);
    while (!impl->samples.empty() && impl->samples.front().utcMs < oldestMs) {
        impl->samples.pop_front();
    }
    if (!impl->displayClock.isValid() || impl->displayClock.elapsed() >= 50) {
        impl->displayClock.restart();
        impl->analyze();
    }
}

struct MeasurementSessionWidget::Impl {
    struct Snapshot {
        qint64 utcMs = 0;
        ScientificSessionContext context;
        SpectrumScienceMetrics metrics;
        int amplitudeUnit = 0;
    };

    Translator translator;
    FrequencySetter frequencySetter;
    ExperimentBundleDialog::Snapshot experimentSnapshot;
    ExperimentBundleDialog::ApplyWorkspace experimentWorkspace;
    MeasurementSessionWidget *owner = nullptr;
    QLineEdit *name = nullptr;
    QPlainTextEdit *notes = nullptr;
    QDoubleSpinBox *cursorMHz = nullptr;
    QSpinBox *captureInterval = nullptr;
    QPushButton *startStop = nullptr;
    QPushButton *capture = nullptr;
    QPushButton *clear = nullptr;
    QPushButton *exportButton = nullptr;
    QLabel *status = nullptr;
    QTableWidget *table = nullptr;
    std::vector<Snapshot> snapshots;
    ScientificSessionContext latestContext;
    SpectrumScienceMetrics latestMetrics;
    int latestAmplitudeUnit = 0;
    qint64 startedUtcMs = 0;
    qint64 lastCaptureUtcMs = 0;
    bool haveLatest = false;
    bool running = false;

    QString text(const QString &key, const QString &fallback) const {
        return translator ? translator(key, fallback) : fallback;
    }

    void saveSettings() const {
        QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
        settings.beginGroup(QStringLiteral("measurementSession"));
        settings.setValue(QStringLiteral("name"), name->text());
        settings.setValue(QStringLiteral("notes"), notes->toPlainText());
        settings.setValue(QStringLiteral("captureInterval"), captureInterval->value());
        settings.endGroup();
    }

    void updateButton() {
        startStop->setText(
            running
                ? text(QStringLiteral("research_session_stop"), QStringLiteral("Stop session"))
                : text(QStringLiteral("research_session_start"), QStringLiteral("Start session")));
        startStop->setStyleSheet(
            running
                ? QStringLiteral("QPushButton { background:#a52d2d; color:white; }")
                : QString());
    }

    void updateStatus() {
        const double elapsed = running && startedUtcMs > 0
                                   ? (QDateTime::currentMSecsSinceEpoch() - startedUtcMs) / 1000.0
                                   : 0.0;
        status->setText(
            text(QStringLiteral("research_session_status"),
                 QStringLiteral("%1 snapshots | elapsed %2 s | synchronized cursor %3"))
                .arg(snapshots.size())
                .arg(elapsed, 0, 'f', 1)
                .arg(frequencyText(cursorMHz->value() * 1.0e6)));
    }

    void rebuildTable() {
        table->setRowCount(static_cast<int>(snapshots.size()));
        for (int row = 0; row < static_cast<int>(snapshots.size()); ++row) {
            const Snapshot &snapshot = snapshots[static_cast<std::size_t>(row)];
            const QStringList cells = {
                QDateTime::fromMSecsSinceEpoch(snapshot.utcMs, Qt::UTC)
                    .toString(QStringLiteral("HH:mm:ss.zzz")),
                frequencyText(snapshot.context.listeningFrequencyHz),
                frequencyText(snapshot.metrics.peakFrequencyHz),
                levelWithUnit(snapshot.metrics.peakDb, snapshot.amplitudeUnit),
                levelWithUnit(snapshot.metrics.noiseFloorDb, snapshot.amplitudeUnit),
                QStringLiteral("%1 dB").arg(snapshot.metrics.snrDb, 0, 'f', 2),
                levelWithUnit(snapshot.metrics.channelPowerDb, snapshot.amplitudeUnit),
                frequencyText(snapshot.metrics.occupiedBandwidth95Hz)
            };
            for (int column = 0; column < cells.size(); ++column) {
                table->setItem(row, column, new QTableWidgetItem(cells[column]));
            }
        }
        if (!snapshots.empty()) table->scrollToBottom();
        updateStatus();
    }

    void captureSnapshot() {
        if (!haveLatest) {
            status->setText(text(QStringLiteral("research_session_waiting"),
                                 QStringLiteral("Waiting for spectrum data")));
            return;
        }
        Snapshot snapshot;
        snapshot.utcMs = QDateTime::currentMSecsSinceEpoch();
        snapshot.context = latestContext;
        snapshot.metrics = latestMetrics;
        snapshot.amplitudeUnit = latestAmplitudeUnit;
        snapshots.push_back(snapshot);
        lastCaptureUtcMs = snapshot.utcMs;
        rebuildTable();
    }

    void setRunning(bool enabled) {
        running = enabled;
        if (running) {
            startedUtcMs = QDateTime::currentMSecsSinceEpoch();
            lastCaptureUtcMs = 0;
            if (snapshots.empty() && haveLatest) captureSnapshot();
        }
        updateButton();
        updateStatus();
    }

    void exportReport();
    QJsonObject experimentSession() const;
    bool restoreExperimentSession(const QJsonObject &context, QString *error);
};

QJsonObject MeasurementSessionWidget::Impl::experimentSession() const {
    QJsonArray rows;
    const auto begin = snapshots.size() > 4096 ? snapshots.size() - 4096 : 0;
    for (std::size_t i = begin; i < snapshots.size(); ++i) {
        const auto &s = snapshots[i];
        rows.append(QJsonObject{{"utcMs", double(s.utcMs)}, {"amplitudeUnit", s.amplitudeUnit},
            {"context", QJsonObject{{"sampleRateHz", s.context.sampleRateHz},
                {"centerFrequencyHz", s.context.centerFrequencyHz}, {"listeningFrequencyHz", s.context.listeningFrequencyHz},
                {"bandwidthHz", s.context.bandwidthHz}, {"inputMode", s.context.inputMode},
                {"modulationType", s.context.modulationType}, {"fftLength", s.context.fftLength},
                {"fftWindowType", s.context.fftWindowType}}},
            {"metrics", QJsonObject{{"valid", s.metrics.valid}, {"peakFrequencyHz", s.metrics.peakFrequencyHz},
                {"peakDb", s.metrics.peakDb}, {"noiseFloorDb", s.metrics.noiseFloorDb},
                {"snrDb", s.metrics.snrDb}, {"channelPowerDb", s.metrics.channelPowerDb},
                {"occupiedBandwidth95Hz", s.metrics.occupiedBandwidth95Hz}}}});
    }
    return {{"name", name->text().left(1024)}, {"notes", notes->toPlainText().left(65536)}, {"snapshots", rows},
        {"totalSnapshotCount", double(snapshots.size())}, {"exportedSnapshotCount", rows.size()}, {"omittedSnapshotCount", double(begin)}};
}

bool MeasurementSessionWidget::Impl::restoreExperimentSession(const QJsonObject &context, QString *error) {
    const bool uk = QSettings(persistentSettingsFilePath(), QSettings::IniFormat).value("ui/language", "en").toString().startsWith("uk");
    const auto fail = [&](const char *en, const char *ua) { if (error) *error = QString::fromUtf8(uk ? ua : en); return false; };
    const auto session = context.value("session").toObject();
    const auto rows = session.value("snapshots").toArray();
    if (session.isEmpty() || !session.value("snapshots").isArray() || rows.size() > 4096) {
        return fail("Invalid measurement session (maximum 4096 snapshots).", "Некоректна сесія вимірювань (не більше 4096 знімків).");
    }
    std::vector<Snapshot> restored;
    for (const auto &value : rows) {
        const auto row = value.toObject(), c = row.value("context").toObject(), m = row.value("metrics").toObject();
        if (c.isEmpty() || m.isEmpty()) return fail("Invalid session snapshot", "Некоректний знімок сесії");
        for (auto object : {c, m, row}) for (auto it = object.begin(); it != object.end(); ++it)
            if (it->isDouble() && !std::isfinite(it->toDouble())) return fail("Non-finite session value", "Нескінченне або невизначене значення сесії");
        for (const auto *key : {"peakDb", "noiseFloorDb", "snrDb", "channelPowerDb"})
            if (!m.value(key).isDouble() || std::abs(m.value(key).toDouble()) > 1000000)
                return fail("Invalid snapshot level", "Некоректний рівень знімка");
        const double utc = row.value("utcMs").toDouble(-1);
        if (utc < 0 || utc > 8640000000000000.0) return fail("Invalid snapshot time", "Некоректний час знімка");
        Snapshot s;
        s.utcMs = qint64(utc); s.amplitudeUnit = std::clamp(row.value("amplitudeUnit").toInt(), 0, 3);
        s.context = {c.value("sampleRateHz").toDouble(), c.value("centerFrequencyHz").toDouble(),
            c.value("listeningFrequencyHz").toDouble(), c.value("bandwidthHz").toDouble(),
            c.value("inputMode").toInt(), c.value("modulationType").toInt(),
            c.value("fftLength").toInt(), c.value("fftWindowType").toInt()};
        s.metrics.valid = m.value("valid").toBool(); s.metrics.peakFrequencyHz = m.value("peakFrequencyHz").toDouble();
        s.metrics.peakDb = float(m.value("peakDb").toDouble()); s.metrics.noiseFloorDb = float(m.value("noiseFloorDb").toDouble());
        s.metrics.snrDb = float(m.value("snrDb").toDouble()); s.metrics.channelPowerDb = float(m.value("channelPowerDb").toDouble());
        s.metrics.occupiedBandwidth95Hz = m.value("occupiedBandwidth95Hz").toDouble();
        restored.push_back(s);
    }
    setRunning(false);
    snapshots = std::move(restored);
    { QSignalBlocker a(name), b(notes); name->setText(session.value("name").toString().left(1024));
      notes->setPlainText(session.value("notes").toString().left(65536)); }
    // No cursor assignment: its valueChanged handler retunes the live receiver.
    rebuildTable();
    return true;
}

void MeasurementSessionWidget::Impl::exportReport() {
    if (snapshots.empty()) return;
    QString path = QFileDialog::getSaveFileName(
        owner,
        text(QStringLiteral("research_session_export"), QStringLiteral("Export measurement session")),
        QStringLiteral("measurement-session-%1.html")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))),
        QStringLiteral("HTML report (*.html)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".html"), Qt::CaseInsensitive)) path += QStringLiteral(".html");
    QFile htmlFile(path);
    if (!htmlFile.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream html(&htmlFile);
    html.setCodec("UTF-8");
    html << "<!doctype html><meta charset=\"utf-8\"><title>"
         << name->text().toHtmlEscaped()
         << "</title><style>body{font-family:system-ui;margin:32px;color:#17202a}"
            "table{border-collapse:collapse;width:100%}th,td{border:1px solid #aeb6bf;"
            "padding:5px;text-align:right}th{background:#eaf0f5}h1{text-align:left}"
            ".notes{white-space:pre-wrap;background:#f4f6f7;padding:12px}</style>";
    html << "<h1>" << name->text().toHtmlEscaped() << "</h1>";
    html << "<p>Export UTC: "
         << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
         << "</p><div class=\"notes\">" << notes->toPlainText().toHtmlEscaped()
         << "</div><h2>Measurements</h2><table><tr>"
            "<th>UTC</th><th>Cursor Hz</th><th>Center Hz</th><th>Sample rate</th>"
            "<th>FFT</th><th>Window</th><th>Peak Hz</th><th>Peak dB</th>"
            "<th>Noise dB</th><th>SNR dB</th><th>Power dB</th><th>OBW95 Hz</th></tr>";
    for (const Snapshot &snapshot : snapshots) {
        html << "<tr><td>"
             << QDateTime::fromMSecsSinceEpoch(snapshot.utcMs, Qt::UTC)
                    .toString(Qt::ISODateWithMs)
             << "</td><td>" << snapshot.context.listeningFrequencyHz
             << "</td><td>" << snapshot.context.centerFrequencyHz
             << "</td><td>" << snapshot.context.sampleRateHz
             << "</td><td>" << snapshot.context.fftLength
             << "</td><td>" << QString::fromLatin1(fftWindowTypeName(snapshot.context.fftWindowType))
             << "</td><td>" << snapshot.metrics.peakFrequencyHz
             << "</td><td>" << snapshot.metrics.peakDb
             << "</td><td>" << snapshot.metrics.noiseFloorDb
             << "</td><td>" << snapshot.metrics.snrDb
             << "</td><td>" << snapshot.metrics.channelPowerDb
             << "</td><td>" << snapshot.metrics.occupiedBandwidth95Hz
             << "</td></tr>";
    }
    html << "</table>";
    htmlFile.close();

    QString csvPath = path;
    csvPath.chop(5);
    csvPath += QStringLiteral(".csv");
    QFile csvFile(csvPath);
    if (csvFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream csv(&csvFile);
        csv << "utc_iso,cursor_hz,center_hz,sample_rate_hz,fft_length,fft_window,"
               "peak_hz,peak_db,noise_db,snr_db,channel_power_db,obw95_hz\n";
        for (const Snapshot &snapshot : snapshots) {
            csv << QDateTime::fromMSecsSinceEpoch(snapshot.utcMs, Qt::UTC)
                       .toString(Qt::ISODateWithMs)
                << ',' << snapshot.context.listeningFrequencyHz
                << ',' << snapshot.context.centerFrequencyHz
                << ',' << snapshot.context.sampleRateHz
                << ',' << snapshot.context.fftLength
                << ',' << fftWindowTypeName(snapshot.context.fftWindowType)
                << ',' << snapshot.metrics.peakFrequencyHz
                << ',' << snapshot.metrics.peakDb
                << ',' << snapshot.metrics.noiseFloorDb
                << ',' << snapshot.metrics.snrDb
                << ',' << snapshot.metrics.channelPowerDb
                << ',' << snapshot.metrics.occupiedBandwidth95Hz << '\n';
        }
    }
}

MeasurementSessionWidget::MeasurementSessionWidget(Translator translator,
                                                   FrequencySetter frequencySetter,
                                                   QWidget *parent)
    : QWidget(parent), impl(std::make_unique<Impl>()) {
    impl->translator = std::move(translator);
    impl->frequencySetter = std::move(frequencySetter);
    impl->owner = this;
    QVBoxLayout *root = new QVBoxLayout(this);
    QGridLayout *controls = new QGridLayout();
    impl->name = new QLineEdit(this);
    impl->name->setPlaceholderText(
        impl->text(QStringLiteral("research_session_name_hint"),
                   QStringLiteral("Experiment name")));
    impl->cursorMHz = new QDoubleSpinBox(this);
    impl->cursorMHz->setRange(0.0, 100000.0);
    impl->cursorMHz->setDecimals(6);
    impl->cursorMHz->setSuffix(QStringLiteral(" MHz"));
    impl->captureInterval = new QSpinBox(this);
    impl->captureInterval->setRange(0, 3600);
    impl->captureInterval->setSpecialValueText(
        impl->text(QStringLiteral("research_session_manual"), QStringLiteral("Manual")));
    impl->captureInterval->setSuffix(QStringLiteral(" s"));
    impl->startStop = new QPushButton(this);
    impl->capture = new QPushButton(
        impl->text(QStringLiteral("research_session_snapshot"), QStringLiteral("Snapshot")), this);
    impl->clear = new QPushButton(
        impl->text(QStringLiteral("clear"), QStringLiteral("Clear")), this);
    impl->exportButton = new QPushButton(
        impl->text(QStringLiteral("research_session_export"), QStringLiteral("Export report")), this);
    controls->addWidget(new QLabel(
        impl->text(QStringLiteral("name"), QStringLiteral("Name:")), this), 0, 0);
    controls->addWidget(impl->name, 0, 1, 1, 3);
    controls->addWidget(new QLabel(
        impl->text(QStringLiteral("research_session_cursor"),
                   QStringLiteral("Synchronized cursor:")), this), 0, 4);
    controls->addWidget(impl->cursorMHz, 0, 5);
    controls->addWidget(new QLabel(
        impl->text(QStringLiteral("research_session_interval"),
                   QStringLiteral("Auto snapshot:")), this), 1, 0);
    controls->addWidget(impl->captureInterval, 1, 1);
    controls->addWidget(impl->startStop, 1, 2);
    controls->addWidget(impl->capture, 1, 3);
    controls->addWidget(impl->clear, 1, 4);
    controls->addWidget(impl->exportButton, 1, 5);
    auto *hdf5Button = new QPushButton(impl->text("hdf5_export", "HDF5 export..."), this);
    controls->addWidget(hdf5Button, 2, 5);
    const bool experimentUk = QSettings(persistentSettingsFilePath(), QSettings::IniFormat).value("ui/language", "en").toString().startsWith("uk");
    auto *exportExperiment = new QPushButton(impl->text("experiment_export", experimentUk ? QString::fromUtf8("Експортувати експеримент...") : QStringLiteral("Export experiment...")), this);
    auto *openExperiment = new QPushButton(impl->text("experiment_open", experimentUk ? QString::fromUtf8("Відкрити експеримент...") : QStringLiteral("Open experiment...")), this);
    controls->addWidget(exportExperiment, 2, 2, 1, 2);
    controls->addWidget(openExperiment, 2, 4);
    const auto bundle = [this](bool importing) {
        auto *dialog = new ExperimentBundleDialog(importing, [this] {
            auto context = impl->experimentSnapshot ? impl->experimentSnapshot() : QJsonObject();
            context["session"] = impl->experimentSession();
            return context;
        }, impl->experimentWorkspace, [this](const QJsonObject &context, QString *error) {
            return impl->restoreExperimentSession(context, error);
        }, impl->translator, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    };
    connect(exportExperiment, &QPushButton::clicked, this, [bundle] { bundle(false); });
    connect(openExperiment, &QPushButton::clicked, this, [bundle] { bundle(true); });
    connect(hdf5Button, &QPushButton::clicked, this, [this]() { showSpectrumHdf5Export(this, impl->translator); });
    controls->setColumnStretch(6, 1);
    root->addLayout(controls);
    impl->notes = new QPlainTextEdit(this);
    impl->notes->setMaximumHeight(72);
    impl->notes->setPlaceholderText(
        impl->text(QStringLiteral("research_session_notes"),
                   QStringLiteral("Conditions, antenna, generator level, observations...")));
    root->addWidget(impl->notes);
    impl->status = new QLabel(this);
    root->addWidget(impl->status);
    impl->table = new QTableWidget(this);
    impl->table->setColumnCount(8);
    impl->table->setHorizontalHeaderLabels({
        QStringLiteral("UTC"),
        impl->text(QStringLiteral("research_session_cursor_short"), QStringLiteral("Cursor")),
        impl->text(QStringLiteral("research_session_peak"), QStringLiteral("Peak frequency")),
        impl->text(QStringLiteral("research_session_peak_level"), QStringLiteral("Peak level")),
        impl->text(QStringLiteral("research_session_noise"), QStringLiteral("Noise")),
        QStringLiteral("SNR"),
        impl->text(QStringLiteral("research_session_power"), QStringLiteral("Channel power")),
        QStringLiteral("OBW95")
    });
    impl->table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    impl->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    impl->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    root->addWidget(impl->table, 1);

    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("measurementSession"));
    impl->name->setText(settings.value(QStringLiteral("name"),
                                        QStringLiteral("Obrii SDR measurement")).toString());
    impl->notes->setPlainText(settings.value(QStringLiteral("notes")).toString());
    impl->captureInterval->setValue(
        settings.value(QStringLiteral("captureInterval"), 1).toInt());
    settings.endGroup();
    impl->updateButton();
    impl->updateStatus();

    connect(impl->startStop, &QPushButton::clicked, this, [this]() {
        impl->setRunning(!impl->running);
    });
    connect(impl->capture, &QPushButton::clicked, this, [this]() {
        impl->captureSnapshot();
    });
    connect(impl->clear, &QPushButton::clicked, this, [this]() {
        impl->snapshots.clear();
        impl->rebuildTable();
    });
    connect(impl->exportButton, &QPushButton::clicked, this, [this]() {
        impl->exportReport();
    });
    connect(impl->cursorMHz, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double mhz) {
        if (impl->frequencySetter) impl->frequencySetter(mhz * 1.0e6);
        impl->updateStatus();
    });
    connect(impl->name, &QLineEdit::editingFinished, this, [this]() {
        impl->saveSettings();
    });
    connect(impl->notes, &QPlainTextEdit::textChanged, this, [this]() {
        impl->saveSettings();
    });
    connect(impl->captureInterval, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { impl->saveSettings(); });
}

MeasurementSessionWidget::~MeasurementSessionWidget() = default;

bool MeasurementSessionWidget::isRunning() const {
    return impl && impl->running;
}

void MeasurementSessionWidget::setExperimentBundleCallbacks(
    std::function<QJsonObject()> snapshot,
    std::function<bool(const QJsonObject &, QString *)> applyWorkspace) {
    impl->experimentSnapshot = std::move(snapshot);
    impl->experimentWorkspace = std::move(applyWorkspace);
}

void MeasurementSessionWidget::appendSpectrumFrame(
    const std::vector<float> &frequencies,
    const std::vector<float> &levels,
    const ScientificSessionContext &context,
    const SpectrumScienceMetrics &metrics,
    int amplitudeUnit) {
    Q_UNUSED(frequencies);
    Q_UNUSED(levels);
    if (!impl) return;
    impl->latestContext = context;
    impl->latestMetrics = metrics;
    impl->latestAmplitudeUnit = amplitudeUnit;
    impl->haveLatest = metrics.valid;
    if (!impl->cursorMHz->hasFocus()) {
        QSignalBlocker blocker(impl->cursorMHz);
        impl->cursorMHz->setValue(context.listeningFrequencyHz / 1.0e6);
    }
    if (impl->running && impl->captureInterval->value() > 0) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (impl->lastCaptureUtcMs <= 0 ||
            now - impl->lastCaptureUtcMs >=
                static_cast<qint64>(impl->captureInterval->value()) * 1000) {
            impl->captureSnapshot();
        }
    } else {
        impl->updateStatus();
    }
}
