#include "zoomspectrumdialog.h"

#include "MyGraphWidget.h"
#include "MyWaterfallWidget.h"
#include "appsettingsutils.h"
#include "scalewidget.h"
#include "zoomspectrumprocessor.h"
#include "waterfall3dview.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

ZoomSpectrumDialog::ZoomSpectrumDialog(QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QString::fromUtf8(u8"Zoom spectrum / Вузькосмуговий аналіз"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1100, 760);

    graph = new MyGraphWidget(this);
    graph->setMinimumHeight(210);
    graph->setLevelRange(-140.0f, 0.0f);
    graph->setFrequencyAxisLabelsVisible(false);
    graph->setFpsOverlayEnabled(true);

    scale = new ScaleWidget(this);
    scale->setFixedHeight(52);
    scale->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    waterfall = new MyWaterfallWidget(this);
    waterfall->setMinimumHeight(310);
    waterfall->setRowsPerFrame(1);
    waterfall->setDisplayMode(MyWaterfallWidget::DisplayMode::Waterfall2D);
    waterfall->setFpsOverlayEnabled(true);
    waterfall3D = new Waterfall3DView(this);
    waterfall3D->setHistoryCapacity(160);
    waterfall3D->setResolutionDivisor(1);
    waterfallStack = new QStackedWidget(this);
    waterfallStack->addWidget(waterfall);
    waterfallStack->addWidget(waterfall3D);

    enabledCheck = new QCheckBox(QString::fromUtf8(u8"Аналізувати / Run"), this);
    enabledCheck->setChecked(true);
    displayModeCombo = new QComboBox(this);
    displayModeCombo->addItem(QStringLiteral("2D"), 0);
    displayModeCombo->addItem(QStringLiteral("3D"), 1);
    binWidthSpin = new QDoubleSpinBox(this);
    binWidthSpin->setRange(0.01, 100000.0);
    binWidthSpin->setDecimals(2);
    binWidthSpin->setSuffix(QStringLiteral(" Hz/bin"));
    binWidthSpin->setValue(1.0);
    binWidthSpin->setToolTip(QString::fromUtf8(u8"Бажана ширина одного FFT-біна у вузькій смузі"));
    updateIntervalSpin = new QSpinBox(this);
    updateIntervalSpin->setRange(1, 5000);
    updateIntervalSpin->setSuffix(QStringLiteral(" ms"));
    updateIntervalSpin->setValue(50);
    updateIntervalSpin->setToolTip(QString::fromUtf8(u8"Крок між сусідніми FFT-кадрами; короткий крок використовує перекриття"));
    levelMinSpin = new QSpinBox(this);
    levelMinSpin->setRange(-240, 20);
    levelMinSpin->setSuffix(QStringLiteral(" dB"));
    levelMinSpin->setValue(-140);
    levelMaxSpin = new QSpinBox(this);
    levelMaxSpin->setRange(-200, 80);
    levelMaxSpin->setSuffix(QStringLiteral(" dB"));
    levelMaxSpin->setValue(0);

    rangeLabel = new QLabel(this);
    statusLabel = new QLabel(QString::fromUtf8(u8"Очікування IQ..."), this);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *controls = new QHBoxLayout();
    controls->setContentsMargins(0, 0, 0, 0);
    controls->addWidget(enabledCheck);
    controls->addWidget(new QLabel(QString::fromUtf8(u8"Вигляд:"), this));
    controls->addWidget(displayModeCombo);
    controls->addWidget(new QLabel(QString::fromUtf8(u8"Роздільність:"), this));
    controls->addWidget(binWidthSpin);
    controls->addWidget(new QLabel(QString::fromUtf8(u8"Оновлення:"), this));
    controls->addWidget(updateIntervalSpin);
    controls->addWidget(new QLabel(QString::fromUtf8(u8"Мін. рівень:"), this));
    controls->addWidget(levelMinSpin);
    controls->addWidget(new QLabel(QString::fromUtf8(u8"Макс. рівень:"), this));
    controls->addWidget(levelMaxSpin);
    controls->addStretch();

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(controls);
    layout->addWidget(rangeLabel);
    layout->addWidget(graph, 2);
    layout->addWidget(scale);
    layout->addWidget(waterfallStack, 3);
    layout->addWidget(statusLabel);

    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    binWidthSpin->setValue(settings.value(QStringLiteral("zoomSpectrum/binWidthHz"), 1.0).toDouble());
    updateIntervalSpin->setValue(settings.value(QStringLiteral("zoomSpectrum/updateMs"), 50).toInt());
    levelMinSpin->setValue(settings.value(QStringLiteral("zoomSpectrum/levelMin"), -140).toInt());
    levelMaxSpin->setValue(settings.value(QStringLiteral("zoomSpectrum/levelMax"), 0).toInt());
    enabledCheck->setChecked(settings.value(QStringLiteral("zoomSpectrum/enabled"), true).toBool());
    displayModeCombo->setCurrentIndex((std::max)(0, displayModeCombo->findData(
        settings.value(QStringLiteral("zoomSpectrum/displayMode"), 0))));
    waterfallStack->setCurrentIndex(displayModeCombo->currentData().toInt());
    const QByteArray geometry = settings.value(QStringLiteral("zoomSpectrum/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }

    const auto configurationChanged = [this]() { applyConfiguration(); };
    connect(enabledCheck, &QCheckBox::toggled, this, configurationChanged);
    connect(displayModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        waterfallStack->setCurrentIndex(displayModeCombo->currentData().toInt());
        QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
        settings.setValue(QStringLiteral("zoomSpectrum/displayMode"), displayModeCombo->currentData());
    });
    connect(binWidthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [configurationChanged](double) { configurationChanged(); });
    connect(updateIntervalSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [configurationChanged](int) { configurationChanged(); });
    connect(levelMinSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        if (levelMaxSpin->value() <= levelMinSpin->value()) {
            levelMaxSpin->setValue(levelMinSpin->value() + 1);
        }
        graph->setLevelRange(levelMinSpin->value(), levelMaxSpin->value());
        waterfall->setLevelRange(levelMinSpin->value(), levelMaxSpin->value());
        QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
        settings.setValue(QStringLiteral("zoomSpectrum/levelMin"), levelMinSpin->value());
    });
    connect(levelMaxSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        if (levelMaxSpin->value() <= levelMinSpin->value()) {
            levelMinSpin->setValue(levelMaxSpin->value() - 1);
        }
        graph->setLevelRange(levelMinSpin->value(), levelMaxSpin->value());
        waterfall->setLevelRange(levelMinSpin->value(), levelMaxSpin->value());
        QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
        settings.setValue(QStringLiteral("zoomSpectrum/levelMax"), levelMaxSpin->value());
    });

    pollTimer = new QTimer(this);
    pollTimer->setTimerType(Qt::PreciseTimer);
    pollTimer->setInterval(8);
    connect(pollTimer, &QTimer::timeout, this, &ZoomSpectrumDialog::pollFrames);
    pollTimer->start();
    rateTimer.start();
}

ZoomSpectrumDialog::~ZoomSpectrumDialog() {
    if (processor) {
        processor->setEnabled(false);
    }
}

void ZoomSpectrumDialog::setSource(const std::shared_ptr<ZoomSpectrumProcessor> &newProcessor,
                                   double selectedLowHz,
                                   double selectedHighHz) {
    if (processor && processor != newProcessor) {
        processor->setEnabled(false);
    }
    processor = newProcessor;
    lowHz = std::min(selectedLowHz, selectedHighHz);
    highHz = std::max(selectedLowHz, selectedHighHz);
    rangeLabel->setText(QString::fromUtf8(u8"Виділена смуга: %1 – %2  |  ширина %3")
                            .arg(formatFrequency(lowHz),
                                 formatFrequency(highHz),
                                 formatFrequency(highHz - lowHz)));
    scale->setRange(lowHz, highHz);
    scale->setTuning((lowHz + highHz) * 0.5,
                     (lowHz + highHz) * 0.5,
                     highHz - lowHz);
    applyConfiguration();
}

void ZoomSpectrumDialog::applyConfiguration() {
    graph->setLevelRange(levelMinSpin->value(), levelMaxSpin->value());
    waterfall->setLevelRange(levelMinSpin->value(), levelMaxSpin->value());
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("zoomSpectrum/binWidthHz"), binWidthSpin->value());
    settings.setValue(QStringLiteral("zoomSpectrum/updateMs"), updateIntervalSpin->value());
    settings.setValue(QStringLiteral("zoomSpectrum/enabled"), enabledCheck->isChecked());
    if (processor) {
        processor->configure(lowHz,
                             highHz,
                             binWidthSpin->value(),
                             updateIntervalSpin->value(),
                             enabledCheck->isChecked() && isVisible());
    }
}

void ZoomSpectrumDialog::pollFrames() {
    if (!processor || !isVisible()) {
        return;
    }
    std::vector<ZoomSpectrumFrame> frames;
    if (!processor->takeFrames(frames) || frames.empty()) {
        return;
    }
    rateFrameCount += static_cast<int>(frames.size());
    const qint64 elapsed = rateTimer.elapsed();
    if (elapsed >= 750) {
        measuredRowsPerSecond = rateFrameCount * 1000.0 / std::max<qint64>(1, elapsed);
        rateFrameCount = 0;
        rateTimer.restart();
    }

    for (const ZoomSpectrumFrame &frame : frames) {
        if (frame.frequencies.empty() || frame.levels.empty()) {
            continue;
        }
        waterfall->setData(frame.frequencies,
                           frame.levels,
                           frame.selectedLowHz,
                           frame.selectedHighHz,
                           static_cast<int>(frame.levels.size()),
                           false,
                           true,
                           10.0f,
                           10.0f,
                           levelMinSpin->value(),
                           levelMaxSpin->value(),
                           true);
        if (displayModeCombo->currentData().toInt() == 1) {
            colorScratch.resize(frame.levels.size() * 3U);
            const double low = levelMinSpin->value();
            const double span = (std::max)(1.0, levelMaxSpin->value() - low);
            for (std::size_t i = 0; i < frame.levels.size(); ++i) {
                const double t = std::clamp((frame.levels[i] - low) / span, 0.0, 1.0);
                const QColor color = QColor::fromHsvF((1.0 - t) * 0.66, 1.0, t > 0.02 ? 1.0 : 0.0);
                colorScratch[i * 3U] = static_cast<unsigned char>(color.red());
                colorScratch[i * 3U + 1U] = static_cast<unsigned char>(color.green());
                colorScratch[i * 3U + 2U] = static_cast<unsigned char>(color.blue());
            }
            waterfall3D->appendHistoryRow(frame.levels,
                                          colorScratch,
                                          levelMinSpin->value(),
                                          levelMaxSpin->value());
        }
    }
    const ZoomSpectrumFrame &latest = frames.back();
    graph->setData(latest.frequencies,
                   latest.levels,
                   latest.selectedLowHz,
                   latest.selectedHighHz,
                   static_cast<int>(latest.levels.size()),
                   true,
                   true);
    graph->setSpectrumMetadata((latest.selectedLowHz + latest.selectedHighHz) * 0.5,
                               (latest.selectedLowHz + latest.selectedHighHz) * 0.5,
                               latest.channelSampleRateHz,
                               latest.fftLength,
                               1);
    waterfall->setSpectrumMetadata((latest.selectedLowHz + latest.selectedHighHz) * 0.5,
                                   (latest.selectedLowHz + latest.selectedHighHz) * 0.5,
                                   latest.channelSampleRateHz,
                                   latest.fftLength,
                                   1);
    updateStatus(latest.fftLength,
                 latest.channelSampleRateHz,
                 latest.binWidthHz,
                 latest.decimation,
                 static_cast<int>(frames.size()));
}

void ZoomSpectrumDialog::updateStatus(int fftLength,
                                      double sampleRateHz,
                                      double binWidthHz,
                                      int decimation,
                                      int deliveredFrames) {
    statusLabel->setText(QString::fromUtf8(u8"Zoom SR %1 | FFT %2 | %3 Hz/bin | decimation ×%4 | %5 rows/s | пакет %6")
                             .arg(formatFrequency(sampleRateHz))
                             .arg(fftLength)
                             .arg(binWidthHz, 0, 'f', binWidthHz < 1.0 ? 3 : 2)
                             .arg(decimation)
                             .arg(measuredRowsPerSecond, 0, 'f', 1)
                             .arg(deliveredFrames));
}

QString ZoomSpectrumDialog::formatFrequency(double hz) const {
    const double absolute = std::abs(hz);
    if (absolute >= 1.0e9) return QStringLiteral("%1 GHz").arg(hz / 1.0e9, 0, 'f', 6);
    if (absolute >= 1.0e6) return QStringLiteral("%1 MHz").arg(hz / 1.0e6, 0, 'f', 6);
    if (absolute >= 1.0e3) return QStringLiteral("%1 kHz").arg(hz / 1.0e3, 0, 'f', 3);
    return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 2);
}

void ZoomSpectrumDialog::closeEvent(QCloseEvent *event) {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("zoomSpectrum/geometry"), saveGeometry());
    settings.sync();
    if (processor) {
        processor->setEnabled(false);
    }
    QDialog::closeEvent(event);
}
