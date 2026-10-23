#include "main.h"

#include "appsettingsutils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
double powerDb(float power) {
    return power > 0.0f && std::isfinite(power)
               ? 10.0 * std::log10(power)
               : std::numeric_limits<double>::quiet_NaN();
}

QString numberOrDash(double value, int decimals) {
    return std::isfinite(value) ? QString::number(value, 'f', decimals) : QStringLiteral("-");
}

class YFactorPlotWidget final : public QWidget {
public:
    explicit YFactorPlotWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(190);
        setMouseTracking(true);
    }

    void setProfile(const ReceiverYFactorProfile &value) {
        profile = value;
        update();
    }

    void setMetric(int value) {
        metric = (std::clamp)(value, 0, 2);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(14, 17, 21));
        const QRectF plot(64.0, 14.0, (std::max)(20.0, width() - 78.0),
                          (std::max)(20.0, height() - 48.0));
        painter.setPen(QColor(70, 76, 84));
        for (int division = 0; division <= 5; ++division) {
            const double x = plot.left() + plot.width() * division / 5.0;
            const double y = plot.top() + plot.height() * division / 5.0;
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
        if (!profile.isValid()) {
            painter.setPen(QColor(170, 175, 182));
            painter.drawText(plot, Qt::AlignCenter, QStringLiteral("No Y-factor data"));
            return;
        }

        QVector<double> values(profile.coldPower.size(), std::numeric_limits<double>::quiet_NaN());
        QVector<double> finiteValues;
        finiteValues.reserve(values.size());
        for (int index = 0; index < values.size(); ++index) {
            double value = std::numeric_limits<double>::quiet_NaN();
            if (metric == 0) {
                const double coldDb = powerDb(profile.coldPower[index]);
                const double hotDb = powerDb(profile.hotPower[index]);
                value = hotDb - coldDb;
            } else if (metric == 1) {
                value = profile.noiseFigureDb[index];
            } else {
                value = profile.equivalentNoiseTemperatureKelvin[index];
            }
            values[index] = value;
            if (std::isfinite(value)) finiteValues.append(value);
        }
        if (finiteValues.isEmpty()) return;
        std::sort(finiteValues.begin(), finiteValues.end());
        const int lowIndex = finiteValues.size() > 20 ? finiteValues.size() / 50 : 0;
        const int highIndex = finiteValues.size() > 20
                                  ? finiteValues.size() - 1 - finiteValues.size() / 50
                                  : finiteValues.size() - 1;
        double minimum = finiteValues[lowIndex];
        double maximum = finiteValues[highIndex];
        if (maximum <= minimum) {
            minimum -= 0.5;
            maximum += 0.5;
        } else {
            const double padding = (maximum - minimum) * 0.08;
            minimum -= padding;
            maximum += padding;
        }

        painter.setPen(QColor(190, 196, 204));
        const QString unit = metric == 2 ? QStringLiteral("K") : QStringLiteral("dB");
        for (int division = 0; division <= 5; ++division) {
            const double value = maximum - (maximum - minimum) * division / 5.0;
            const double y = plot.top() + plot.height() * division / 5.0;
            painter.drawText(QRectF(2.0, y - 9.0, 57.0, 18.0), Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("%1 %2").arg(value, 0, 'f', metric == 2 ? 0 : 2).arg(unit));
            const double frequencyMhz = profile.minimumFrequencyHz / 1.0e6 +
                (profile.maximumFrequencyHz - profile.minimumFrequencyHz) / 1.0e6 * division / 5.0;
            const double x = plot.left() + plot.width() * division / 5.0;
            painter.drawText(QRectF(x - 50.0, plot.bottom() + 4.0, 100.0, 20.0),
                             Qt::AlignHCenter | Qt::AlignTop,
                             QStringLiteral("%1").arg(frequencyMhz, 0, 'f', 3));
        }
        painter.drawText(QRectF(plot.left(), 0.0, plot.width(), 16.0), Qt::AlignCenter,
                         metric == 0 ? QStringLiteral("Y-factor, dB")
                                     : metric == 1 ? QStringLiteral("Noise figure, dB")
                                                   : QStringLiteral("Equivalent noise temperature, K"));

        QPainterPath path;
        bool pathStarted = false;
        const int last = values.size() - 1;
        for (int index = 0; index <= last; ++index) {
            const double value = values[index];
            if (!std::isfinite(value)) {
                pathStarted = false;
                continue;
            }
            const double x = plot.left() + plot.width() * index / (std::max)(1, last);
            const double clipped = (std::clamp)(value, minimum, maximum);
            const double y = plot.bottom() - plot.height() * (clipped - minimum) / (maximum - minimum);
            if (!pathStarted) {
                path.moveTo(x, y);
                pathStarted = true;
            } else {
                path.lineTo(x, y);
            }
        }
        painter.setPen(QPen(QColor(255, 190, 55), 1.6));
        painter.drawPath(path);
        plotRect = plot;
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (!profile.isValid() || !plotRect.contains(event->pos())) return;
        const int count = profile.coldPower.size();
        const double ratio = (std::clamp)((event->localPos().x() - plotRect.left()) /
                                              (std::max)(1.0, plotRect.width()),
                                          0.0, 1.0);
        const int index = (std::clamp)(static_cast<int>(std::llround(ratio * (count - 1))),
                                       0, count - 1);
        const double frequencyMhz = (profile.minimumFrequencyHz +
            (profile.maximumFrequencyHz - profile.minimumFrequencyHz) * index /
                (std::max)(1, count - 1)) / 1.0e6;
        const double coldDb = powerDb(profile.coldPower[index]);
        const double hotDb = powerDb(profile.hotPower[index]);
        const double value = metric == 0 ? hotDb - coldDb
                            : metric == 1 ? profile.noiseFigureDb[index]
                                          : profile.equivalentNoiseTemperatureKelvin[index];
        QToolTip::showText(event->globalPos(),
                           QStringLiteral("%1 MHz\n%2 %3")
                               .arg(frequencyMhz, 0, 'f', 6)
                               .arg(numberOrDash(value, metric == 2 ? 1 : 3))
                               .arg(metric == 2 ? QStringLiteral("K") : QStringLiteral("dB")),
                           this);
    }

private:
    ReceiverYFactorProfile profile;
    int metric = 0;
    QRectF plotRect;
};

} // namespace

void YourClassName::saveReceiverYFactorProfiles() const {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("calibration/yFactorProfiles"),
                      receiverYFactorCalibration.serialize());
    settings.sync();
}

void YourClassName::openReceiverYFactorDialog() {
    const bool ukrainian = normalizedUiLanguage(uiLanguage) == QStringLiteral("uk");
    QDialog dialog(this);
    dialog.setWindowTitle(uiText(QStringLiteral("receiver_y_factor"), QStringLiteral("Y-factor measurement")));
    dialog.resize(900, 680);
    auto *root = new QVBoxLayout(&dialog);

    auto *instructions = new QLabel(uiText(
        QStringLiteral("receiver_y_factor_instructions"),
        QStringLiteral("Y-factor compares a cold and a hot known noise source without changing receiver settings. Capture Cold first, switch only the source, then capture Hot. A calibrated ENR source is preferred. Results are receiver noise figure and equivalent noise temperature versus frequency.")),
        &dialog);
    instructions->setWordWrap(true);
    instructions->setStyleSheet(QStringLiteral("QLabel { padding: 6px; background: #182c34; color: #bfe7ef; border: 1px solid #386779; }"));
    root->addWidget(instructions);

    auto *contextLabel = new QLabel(&dialog);
    contextLabel->setWordWrap(true);
    contextLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(contextLabel);

    auto *form = new QFormLayout();
    auto *applyCheck = new QCheckBox(
        uiText(QStringLiteral("receiver_y_factor_apply"), QStringLiteral("Apply input-referred noise view")), &dialog);
    applyCheck->setChecked(receiverYFactorApplyEnabled);
    applyCheck->setToolTip(uiText(
        QStringLiteral("receiver_y_factor_apply_tooltip"),
        QStringLiteral("Visual mode only. It removes the measured receiver noise contribution in the temperature/power domain and maps the known cold source to the selected reference floor. Raw IQ, audio and measurements stay unchanged.")));

    auto *targetSpin = new QDoubleSpinBox(&dialog);
    targetSpin->setRange(-220.0, 20.0);
    targetSpin->setDecimals(1);
    targetSpin->setSuffix(QStringLiteral(" dB"));
    targetSpin->setValue(receiverYFactorTargetDb);
    targetSpin->setKeyboardTracking(false);

    auto *sourceCombo = new QComboBox(&dialog);
    sourceCombo->addItem(uiText(QStringLiteral("receiver_y_factor_enr"), QStringLiteral("Calibrated ENR source")),
                         static_cast<int>(YFactorSourceMode::Enr));
    sourceCombo->addItem(uiText(QStringLiteral("receiver_y_factor_temperatures"), QStringLiteral("Known cold/hot temperatures")),
                         static_cast<int>(YFactorSourceMode::Temperatures));
    sourceCombo->setCurrentIndex((std::max)(0, sourceCombo->findData(receiverYFactorSourceMode)));

    auto *coldSpin = new QDoubleSpinBox(&dialog);
    coldSpin->setRange(1.0, 100000.0);
    coldSpin->setDecimals(2);
    coldSpin->setSuffix(QStringLiteral(" K"));
    coldSpin->setValue(receiverYFactorColdTemperatureKelvin);
    coldSpin->setKeyboardTracking(false);
    auto *hotSpin = new QDoubleSpinBox(&dialog);
    hotSpin->setRange(1.0, 1000000.0);
    hotSpin->setDecimals(2);
    hotSpin->setSuffix(QStringLiteral(" K"));
    hotSpin->setValue(receiverYFactorHotTemperatureKelvin);
    hotSpin->setKeyboardTracking(false);
    auto *enrSpin = new QDoubleSpinBox(&dialog);
    enrSpin->setRange(-20.0, 100.0);
    enrSpin->setDecimals(2);
    enrSpin->setSuffix(QStringLiteral(" dB"));
    enrSpin->setValue(receiverYFactorEnrDb);
    enrSpin->setKeyboardTracking(false);
    auto *framesSpin = new QSpinBox(&dialog);
    framesSpin->setRange(8, 512);
    framesSpin->setSingleStep(8);
    framesSpin->setValue(receiverYFactorCaptureFrames);

    form->addRow(applyCheck);
    form->addRow(uiText(QStringLiteral("receiver_y_factor_target"), QStringLiteral("Cold reference floor")), targetSpin);
    form->addRow(uiText(QStringLiteral("receiver_y_factor_source"), QStringLiteral("Source definition")), sourceCombo);
    form->addRow(uiText(QStringLiteral("receiver_y_factor_cold_temperature"), QStringLiteral("Cold temperature")), coldSpin);
    form->addRow(uiText(QStringLiteral("receiver_y_factor_hot_temperature"), QStringLiteral("Hot temperature")), hotSpin);
    form->addRow(QStringLiteral("ENR"), enrSpin);
    form->addRow(uiText(QStringLiteral("receiver_noise_baseline_frames"), QStringLiteral("Capture frames")), framesSpin);
    root->addLayout(form);

    auto *progress = new QProgressBar(&dialog);
    root->addWidget(progress);
    auto *statusLabel = new QLabel(&dialog);
    statusLabel->setWordWrap(true);
    root->addWidget(statusLabel);

    auto *actions = new QHBoxLayout();
    auto *captureColdButton = new QPushButton(
        uiText(QStringLiteral("receiver_y_factor_capture_cold"), QStringLiteral("Capture Cold")), &dialog);
    auto *captureHotButton = new QPushButton(
        uiText(QStringLiteral("receiver_y_factor_capture_hot"), QStringLiteral("Capture Hot")), &dialog);
    auto *cancelButton = new QPushButton(uiText(QStringLiteral("cancel"), QStringLiteral("Cancel")), &dialog);
    auto *clearButton = new QPushButton(
        uiText(QStringLiteral("receiver_noise_baseline_clear_current"), QStringLiteral("Clear current")), &dialog);
    auto *exportButton = new QPushButton(
        uiText(QStringLiteral("receiver_y_factor_export"), QStringLiteral("Export CSV...")), &dialog);
    actions->addWidget(captureColdButton);
    actions->addWidget(captureHotButton);
    actions->addWidget(cancelButton);
    actions->addStretch(1);
    actions->addWidget(clearButton);
    actions->addWidget(exportButton);
    root->addLayout(actions);

    auto *plotControls = new QHBoxLayout();
    auto *plotMetricCombo = new QComboBox(&dialog);
    plotMetricCombo->addItem(QStringLiteral("Y-factor, dB"), 0);
    plotMetricCombo->addItem(QStringLiteral("Noise figure, dB"), 1);
    plotMetricCombo->addItem(QStringLiteral("Te, K"), 2);
    plotControls->addWidget(new QLabel(uiText(QStringLiteral("receiver_y_factor_graph"), QStringLiteral("Graph")), &dialog));
    plotControls->addWidget(plotMetricCombo);
    plotControls->addStretch(1);
    root->addLayout(plotControls);
    auto *plot = new YFactorPlotWidget(&dialog);
    root->addWidget(plot);

    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels({
        uiText(QStringLiteral("frequency"), QStringLiteral("Frequency")),
        uiText(QStringLiteral("receiver_y_factor_cold_db"), QStringLiteral("Cold dB")),
        uiText(QStringLiteral("receiver_y_factor_hot_db"), QStringLiteral("Hot dB")),
        QStringLiteral("Y dB"),
        QStringLiteral("Te K"),
        QStringLiteral("NF dB")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    root->addWidget(table, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto currentKey = [this]() { return receiverNoiseBaselineKey(spectrumProcessingSettings()); };
    auto updateSourceFields = [sourceCombo, hotSpin, enrSpin]() {
        const bool enr = sourceCombo->currentData().toInt() == static_cast<int>(YFactorSourceMode::Enr);
        enrSpin->setEnabled(enr);
        hotSpin->setEnabled(!enr);
    };
    updateSourceFields();

    qint64 displayedProfileTimestamp = -1;
    auto populateTable = [table, plot, &displayedProfileTimestamp](const ReceiverYFactorProfile &profile) {
        if (!profile.isValid()) {
            table->setRowCount(0);
            plot->setProfile(profile);
            displayedProfileTimestamp = -1;
            return;
        }
        if (displayedProfileTimestamp == profile.capturedUtcMs) return;
        displayedProfileTimestamp = profile.capturedUtcMs;
        plot->setProfile(profile);
        const int sourceRows = profile.coldPower.size();
        const int rows = (std::min)(512, sourceRows);
        table->setRowCount(rows);
        for (int row = 0; row < rows; ++row) {
            const int index = rows > 1 ? row * (sourceRows - 1) / (rows - 1) : 0;
            const double frequencyHz = profile.minimumFrequencyHz +
                (profile.maximumFrequencyHz - profile.minimumFrequencyHz) * index /
                    (std::max)(1, sourceRows - 1);
            const double coldDb = powerDb(profile.coldPower[index]);
            const double hotDb = powerDb(profile.hotPower[index]);
            const double yDb = hotDb - coldDb;
            const double te = profile.equivalentNoiseTemperatureKelvin[index];
            const double nf = profile.noiseFigureDb[index];
            const QStringList values{
                QString::number(frequencyHz / 1.0e6, 'f', 6),
                numberOrDash(coldDb, 2), numberOrDash(hotDb, 2),
                numberOrDash(yDb, 2), numberOrDash(te, 1), numberOrDash(nf, 2)};
            for (int column = 0; column < values.size(); ++column) {
                table->setItem(row, column, new QTableWidgetItem(values[column]));
            }
        }
    };

    auto refresh = [this, ukrainian, currentKey, contextLabel, statusLabel, progress,
                    captureColdButton, captureHotButton, cancelButton, clearButton,
                    exportButton, populateTable]() {
        const ReceiverNoiseBaselineKey key = currentKey();
        contextLabel->setText((ukrainian ? QStringLiteral("Поточна конфігурація: ")
                                         : QStringLiteral("Current configuration: ")) + key.description());
        const bool capturing = receiverYFactorCalibration.isCapturing();
        progress->setMaximum((std::max)(1, receiverYFactorCalibration.targetFrames()));
        progress->setValue(receiverYFactorCalibration.capturedFrames());
        captureColdButton->setEnabled(!capturing);
        captureHotButton->setEnabled(!capturing && receiverYFactorCalibration.coldReady(key));
        cancelButton->setEnabled(capturing);
        const ReceiverYFactorProfile profile = receiverYFactorCalibration.matchingProfile(key);
        clearButton->setEnabled(!capturing && profile.isValid());
        exportButton->setEnabled(profile.isValid());
        populateTable(profile);
        if (capturing) {
            const bool cold = receiverYFactorCalibration.stage() == YFactorCaptureStage::Cold;
            statusLabel->setText(ukrainian
                ? QStringLiteral("Накопичення %1: %2/%3 кадрів")
                      .arg(cold ? QStringLiteral("Cold") : QStringLiteral("Hot"))
                      .arg(receiverYFactorCalibration.capturedFrames())
                      .arg(receiverYFactorCalibration.targetFrames())
                : QStringLiteral("Capturing %1: %2/%3 frames")
                      .arg(cold ? QStringLiteral("Cold") : QStringLiteral("Hot"))
                      .arg(receiverYFactorCalibration.capturedFrames())
                      .arg(receiverYFactorCalibration.targetFrames()));
        } else if (receiverYFactorCalibration.coldReady(key)) {
            statusLabel->setText(ukrainian
                ? QStringLiteral("Cold накопичено. Не змінюючи налаштувань приймача, під'єднайте/увімкніть Hot-джерело та натисніть Capture Hot.")
                : QStringLiteral("Cold is ready. Without changing receiver settings, connect/enable the Hot source and press Capture Hot."));
        } else if (profile.isValid()) {
            statusLabel->setText(ukrainian
                ? QStringLiteral("Результат: дійсних точок %1/%2, профілів %3, %4")
                      .arg(profile.validBins).arg(profile.coldPower.size())
                      .arg(receiverYFactorCalibration.profileCount())
                      .arg(QDateTime::fromMSecsSinceEpoch(profile.capturedUtcMs).toString(Qt::ISODate))
                : QStringLiteral("Result: %1/%2 valid bins, %3 profile(s), %4")
                      .arg(profile.validBins).arg(profile.coldPower.size())
                      .arg(receiverYFactorCalibration.profileCount())
                      .arg(QDateTime::fromMSecsSinceEpoch(profile.capturedUtcMs).toString(Qt::ISODate)));
        } else {
            statusLabel->setText(ukrainian
                ? QStringLiteral("Для поточної конфігурації результатів Y-factor немає.")
                : QStringLiteral("No Y-factor result matches the current configuration."));
        }
    };

    auto captureStateValid = [this, ukrainian]() {
        QString message;
        if (runState != RadioRunState::Running)
            message = ukrainian ? QStringLiteral("Спочатку запустіть приймач.") : QStringLiteral("Start the receiver first.");
        else if (agileScanRunning || standardScanRunning || listeningScanRunning)
            message = ukrainian ? QStringLiteral("Зупиніть сканування перед вимірюванням.") : QStringLiteral("Stop scanning before measurement.");
        else if (isHackRfNativeSelected() && pendingSettings.hackRfBiasTeeEnabled)
            message = ukrainian ? QStringLiteral("Вимкніть bias tee, якщо Hot-джерело явно не потребує його живлення.")
                                : QStringLiteral("Turn bias tee off unless the Hot source explicitly requires it.");
        if (message.isEmpty()) return true;
        QMessageBox::warning(this, uiText(QStringLiteral("receiver_y_factor"), QStringLiteral("Y-factor measurement")), message);
        return false;
    };

    connect(sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [this, sourceCombo, updateSourceFields](int) {
                receiverYFactorSourceMode = sourceCombo->currentData().toInt();
                updateSourceFields();
                savePersistentSettings();
            });
    connect(plotMetricCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [plot, plotMetricCombo](int) { plot->setMetric(plotMetricCombo->currentData().toInt()); });
    connect(applyCheck, &QCheckBox::toggled, &dialog, [this](bool checked) {
        receiverYFactorApplyEnabled = checked;
        savePersistentSettings();
    });
    connect(targetSpin, &QDoubleSpinBox::editingFinished, &dialog, [this, targetSpin]() {
        receiverYFactorTargetDb = targetSpin->value();
        savePersistentSettings();
    });
    auto saveSourceValues = [this, coldSpin, hotSpin, enrSpin, framesSpin]() {
        receiverYFactorColdTemperatureKelvin = coldSpin->value();
        receiverYFactorHotTemperatureKelvin = hotSpin->value();
        receiverYFactorEnrDb = enrSpin->value();
        receiverYFactorCaptureFrames = framesSpin->value();
        savePersistentSettings();
    };
    connect(coldSpin, &QDoubleSpinBox::editingFinished, &dialog, saveSourceValues);
    connect(hotSpin, &QDoubleSpinBox::editingFinished, &dialog, saveSourceValues);
    connect(enrSpin, &QDoubleSpinBox::editingFinished, &dialog, saveSourceValues);
    connect(framesSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
            [saveSourceValues](int) { saveSourceValues(); });

    connect(captureColdButton, &QPushButton::clicked, &dialog,
            [this, currentKey, captureStateValid, refresh]() {
                if (!captureStateValid()) return;
                receiverNoiseBaseline.cancelCapture();
                receiverYFactorCalibration.beginColdCapture(currentKey(), receiverYFactorCaptureFrames);
                receiverYFactorCaptureTimer.invalidate();
                refresh();
            });
    connect(captureHotButton, &QPushButton::clicked, &dialog,
            [this, ukrainian, currentKey, captureStateValid, refresh]() {
                if (!captureStateValid()) return;
                QString error;
                if (!receiverYFactorCalibration.beginHotCapture(
                        currentKey(), receiverYFactorCaptureFrames,
                        static_cast<YFactorSourceMode>(receiverYFactorSourceMode),
                        receiverYFactorColdTemperatureKelvin,
                        receiverYFactorHotTemperatureKelvin,
                        receiverYFactorEnrDb, &error)) {
                    QMessageBox::warning(this,
                                         uiText(QStringLiteral("receiver_y_factor"), QStringLiteral("Y-factor measurement")),
                                         ukrainian ? QStringLiteral("Не вдалося почати Hot: %1").arg(error)
                                                   : QStringLiteral("Could not start Hot: %1").arg(error));
                    return;
                }
                receiverYFactorCaptureTimer.invalidate();
                refresh();
            });
    connect(cancelButton, &QPushButton::clicked, &dialog, [this, refresh]() {
        receiverYFactorCalibration.cancelCapture();
        refresh();
    });
    connect(clearButton, &QPushButton::clicked, &dialog, [this, currentKey, refresh]() {
        if (receiverYFactorCalibration.clearMatching(currentKey())) saveReceiverYFactorProfiles();
        refresh();
    });
    connect(exportButton, &QPushButton::clicked, &dialog, [this, ukrainian, currentKey, &dialog]() {
        const ReceiverYFactorProfile profile = receiverYFactorCalibration.matchingProfile(currentKey());
        if (!profile.isValid()) return;
        const QString fileName = QFileDialog::getSaveFileName(
            this,
            uiText(QStringLiteral("receiver_y_factor_export"), QStringLiteral("Export Y-factor CSV")),
            QStringLiteral("obrii_y_factor.csv"), QStringLiteral("CSV (*.csv)"));
        if (fileName.isEmpty()) return;
        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, dialog.windowTitle(),
                                 ukrainian ? QStringLiteral("Не вдалося відкрити файл.")
                                           : QStringLiteral("Could not open the file."));
            return;
        }
        QTextStream stream(&file);
        stream.setCodec("UTF-8");
        stream << "frequency_hz,cold_db,hot_db,y_db,equivalent_noise_temperature_k,noise_figure_db\n";
        const int count = profile.coldPower.size();
        for (int index = 0; index < count; ++index) {
            const double frequencyHz = profile.minimumFrequencyHz +
                (profile.maximumFrequencyHz - profile.minimumFrequencyHz) * index /
                    (std::max)(1, count - 1);
            const double coldDb = powerDb(profile.coldPower[index]);
            const double hotDb = powerDb(profile.hotPower[index]);
            stream << QString::number(frequencyHz, 'f', 3) << ','
                   << numberOrDash(coldDb, 6) << ',' << numberOrDash(hotDb, 6) << ','
                   << numberOrDash(hotDb - coldDb, 6) << ','
                   << numberOrDash(profile.equivalentNoiseTemperatureKelvin[index], 6) << ','
                   << numberOrDash(profile.noiseFigureDb[index], 6) << '\n';
        }
    });

    QTimer timer(&dialog);
    timer.setInterval(100);
    connect(&timer, &QTimer::timeout, &dialog, refresh);
    timer.start();
    refresh();
    dialog.exec();
}
