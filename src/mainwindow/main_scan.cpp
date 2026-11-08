#include "main.h"

#include "appconstants.h"
#include "appruntimeutils.h"
#include "diagnosticlogging.h"
#include "iqbuffer.h"
#include "modulationutils.h"
#include "presethelpers.h"
#include "scanvisualutils.h"
#include "spectrumfftworker.h"
#include "tuningutils.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHash>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <limits>

QVector<double> YourClassName::agileScanFrequencyList(QString *error) const {
    const QString ranges = agileScanRangesEdit
                               ? agileScanRangesEdit->text().trimmed()
                               : agileScanRangesMhz.trimmed();
    const double step = agileScanAutoStepSampleRate
                            ? agileScanAutoStepMhz()
                            : (agileScanStepSpin
                                   ? agileScanStepSpin->value()
                                   : agileScanStepMhz);
    return parseAgileScanFrequenciesMhz(ranges, step, error);
}

double YourClassName::agileScanAutoStepMhz() const {
    double sampleRate = pendingSettings.sampleRate;
    if ((!std::isfinite(sampleRate) || sampleRate <= 0.0) && sampleBox) {
        bool ok = false;
        const double currentRate = sampleBox->currentData().toDouble(&ok);
        if (ok) {
            sampleRate = currentRate;
        }
    }
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        sampleRate = globalSampleRate;
    }
    return (std::clamp)(sampleRate / 1000000.0,
                        AGILE_SCAN_MIN_STEP_MHZ,
                        AGILE_SCAN_MAX_STEP_MHZ);
}

void YourClassName::applyAgileScanAutoStep(bool updateSpin) {
    if (!agileScanAutoStepSampleRate) {
        return;
    }
    agileScanStepMhz = agileScanAutoStepMhz();
    if (updateSpin && agileScanStepSpin) {
        QSignalBlocker blocker(agileScanStepSpin);
        agileScanStepSpin->setValue(agileScanStepMhz);
    }
}

QVector<double> YourClassName::standardScanFrequencyList(QString *error) const {
    const QString centers = standardScanCentersEdit
                                ? standardScanCentersEdit->text().trimmed()
                                : standardScanCentersMhz.trimmed();
    QVector<double> frequencies = parseStandardScanCentersMhz(centers,
                                                               pendingSettings.sampleRate,
                                                               AGILE_SCAN_MIN_POINTS,
                                                               error,
                                                               nullptr);
    const double minimumFrequency = rfMinimumCenterFrequency(pendingSettings);
    const double maximumFrequency = rfMaximumCenterFrequency(pendingSettings);
    for (const double frequency : frequencies) {
        if (frequency < minimumFrequency || frequency > maximumFrequency) {
            if (error) {
                *error = uiText(
                    QStringLiteral("scan_receiver_frequency_range"),
                    QStringLiteral("A scan center is outside the selected receiver range: %1-%2 MHz"))
                             .arg(minimumFrequency / 1000000.0, 0, 'f', 3)
                             .arg(maximumFrequency / 1000000.0, 0, 'f', 3);
            }
            return {};
        }
    }
    return frequencies;
}

QVector<double> YourClassName::listeningScanFrequencyList(QString *error) const {
    const QString targets = listeningScanTargetsEdit
                                ? listeningScanTargetsEdit->text().trimmed()
                                : listeningScanTargetsMhz.trimmed();
    const QPair<double, double> span = listeningScanVisibleSpanHz(pendingSettings);
    return parseListeningScanTargetsMhz(targets,
                                        span.first,
                                        span.second,
                                        listeningScanEnabled ? 1 : 0,
                                        error);
}

void YourClassName::normalizeStandardScanCentersUi(bool requireTwoCenters) {
    const QString centers = standardScanCentersEdit
                                ? standardScanCentersEdit->text().trimmed()
                                : standardScanCentersMhz.trimmed();
    QString error;
    bool adjusted = false;
    const QVector<double> normalized =
        parseStandardScanCentersMhz(centers,
                                    pendingSettings.sampleRate,
                                    requireTwoCenters ? AGILE_SCAN_MIN_POINTS : 0,
                                    &error,
                                    &adjusted);
    if (!error.isEmpty() || normalized.isEmpty() || !adjusted) {
        return;
    }

    standardScanCentersMhz = formatMhzList(normalized);
    if (standardScanCentersEdit) {
        QSignalBlocker blocker(standardScanCentersEdit);
        standardScanCentersEdit->setText(standardScanCentersMhz);
    }
    if (standardScanStatusLabel) {
        standardScanStatusLabel->setText(
            uiText(QStringLiteral("standard_scan_adjusted"),
                   QStringLiteral("Standard scan: centers adjusted to sample-rate spacing")));
    }
    qDebug() << "[StandardScan] centers adjusted to sample-rate spacing"
             << "sampleRate" << pendingSettings.sampleRate
             << "centers" << standardScanCentersMhz;
}

void YourClassName::applyStandardScanRangeToCenters() {
    auto parseRangeMhz = [this](QString text, double *frequencyHz) -> bool {
        if (!frequencyHz) {
            return false;
        }
        text = text.trimmed();
        text.remove(QRegularExpression(QStringLiteral("mhz"), QRegularExpression::CaseInsensitiveOption));
        text.replace(QLatin1Char(','), QLatin1Char('.'));
        bool ok = false;
        const double mhz = text.toDouble(&ok);
        const double hz = mhz * 1000000.0;
        if (!ok ||
            !std::isfinite(hz) ||
            hz < rfMinimumCenterFrequency(pendingSettings) ||
            hz > rfMaximumCenterFrequency(pendingSettings)) {
            return false;
        }
        *frequencyHz = hz;
        return true;
    };

    if (standardScanRangeStartEdit) {
        standardScanRangeStartMhz = standardScanRangeStartEdit->text().trimmed();
    }
    if (standardScanRangeEndEdit) {
        standardScanRangeEndMhz = standardScanRangeEndEdit->text().trimmed();
    }

    double startHz = 0.0;
    double endHz = 0.0;
    if (!parseRangeMhz(standardScanRangeStartMhz, &startHz)) {
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_bad_range_start"),
                                                    QStringLiteral("Bad scan range start")));
        }
        return;
    }
    if (!parseRangeMhz(standardScanRangeEndMhz, &endHz)) {
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_bad_range_end"),
                                                    QStringLiteral("Bad scan range end")));
        }
        return;
    }

    if (endHz < startHz) {
        std::swap(startHz, endHz);
    }
    const double stepHz = pendingSettings.sampleRate;
    if (!std::isfinite(stepHz) || stepHz <= 0.0) {
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_bad_sample_rate"),
                                                    QStringLiteral("Standard scan: bad sample rate")));
        }
        return;
    }
    if (endHz - startHz < stepHz - 0.5) {
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_range_too_small"),
                                                    QStringLiteral("Scan range is smaller than one sample-rate step")));
        }
        return;
    }

    QVector<double> centers;
    constexpr int maxGeneratedCenters = 512;
    for (double centerHz = startHz;
         centerHz <= endHz + 0.5 && centers.size() < maxGeneratedCenters;
         centerHz += stepHz) {
        centers.push_back(centerHz);
    }
    if (centers.size() >= maxGeneratedCenters && centers.last() + stepHz <= endHz + 0.5) {
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_range_too_many"),
                                                    QStringLiteral("Scan range generated too many centers")));
        }
        return;
    }
    if (centers.size() < AGILE_SCAN_MIN_POINTS) {
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_range_too_small"),
                                                    QStringLiteral("Scan range is smaller than one sample-rate step")));
        }
        return;
    }

    standardScanCentersMhz = formatMhzList(centers);
    if (standardScanCentersEdit) {
        QSignalBlocker blocker(standardScanCentersEdit);
        standardScanCentersEdit->setText(standardScanCentersMhz);
    }
    if (standardScanStatusLabel) {
        standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_range_filled"),
                                                QStringLiteral("Standard scan: %1 centers from range"))
                                         .arg(centers.size()));
    }
}

double YourClassName::currentAgileScanCenterFrequencyHz() const {
    if (!agileScanRunning ||
        activeFobosApiKind != FobosApiKind::Agile ||
        !agileDevice ||
        activeAgileScanFrequencies.isEmpty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const int index = getFobosAgileScanIndexSafely(agileDevice);
    if (index < 0 || index >= activeAgileScanFrequencies.size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return activeAgileScanFrequencies.at(index);
}

double YourClassName::currentStandardScanCenterFrequencyHz() const {
    if (!standardScanRunning ||
        activeStandardScanFrequencies.isEmpty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const int index = (std::clamp)(standardScanIndex, 0, activeStandardScanFrequencies.size() - 1);
    return activeStandardScanFrequencies.at(index);
}

void YourClassName::updateAgileScanControls() {
    if (agileScanPresetCombo) {
        const QString currentText = agileScanPresetCombo->currentText();
        QSignalBlocker blocker(agileScanPresetCombo);
        agileScanPresetCombo->clear();
        agileScanPresetOrder =
            normalizedPresetOrder(agileScanPresetOrder,
                                  agileScanPresets,
                                  defaultAgileScanPresetOrder());
        for (const QString &name : std::as_const(agileScanPresetOrder)) {
            agileScanPresetCombo->addItem(name, name);
        }
        agileScanPresetCombo->setEditText(currentText);
    }
    if (standardScanPresetCombo) {
        const QString currentText = standardScanPresetCombo->currentText();
        QSignalBlocker blocker(standardScanPresetCombo);
        standardScanPresetCombo->clear();
        standardScanPresetOrder =
            normalizedPresetOrder(standardScanPresetOrder,
                                  standardScanPresets,
                                  defaultStandardScanPresetOrder());
        for (const QString &name : std::as_const(standardScanPresetOrder)) {
            standardScanPresetCombo->addItem(name, name);
        }
        standardScanPresetCombo->setEditText(currentText);
    }

    const FobosDeviceInfo selectedInfo = selectedFobosDeviceInfo();
    const bool agileScanSupported = selectedInfo.apiKind == FobosApiKind::Agile;
    const bool externalBackendSelected = isExternalReceiverBackendSelected();
    const bool standardScanSupported =
        externalBackendSelected ||
        selectedInfo.apiKind == FobosApiKind::Standard ||
        selectedInfo.apiKind == FobosApiKind::Agile;
    if (agileScanCheckbox) {
        agileScanCheckbox->setEnabled(agileScanSupported);
        agileScanCheckbox->setToolTip(agileScanSupported
                                          ? uiText(QStringLiteral("agile_scan_tooltip"),
                                                   QStringLiteral("Use Agile firmware scan mode"))
                                          : uiText(QStringLiteral("agile_receiver_required"),
                                                   QStringLiteral("Agile firmware receiver required")));
        if (!agileScanSupported && agileScanCheckbox->isChecked()) {
            QSignalBlocker blocker(agileScanCheckbox);
            agileScanCheckbox->setChecked(false);
            agileScanEnabled = false;
        }
    }
    if (standardScanCheckbox) {
        standardScanCheckbox->setEnabled(standardScanSupported);
        standardScanCheckbox->setToolTip(standardScanSupported
                                             ? uiText(QStringLiteral("standard_scan_tooltip"),
                                                      QStringLiteral("Slow manual retune scan by cycling through listed center frequencies"))
                                             : uiText(QStringLiteral("standard_receiver_required"),
                                                     QStringLiteral("Live-retune receiver required")));
        if (!standardScanSupported && standardScanCheckbox->isChecked()) {
            QSignalBlocker blocker(standardScanCheckbox);
            standardScanCheckbox->setChecked(false);
            standardScanEnabled = false;
            resetStandardScanState(true);
        }
    }
    if (scanListeningLockCheckbox) {
        scanListeningLockCheckbox->setEnabled(standardScanSupported || agileScanSupported);
    }

    const bool scanChecked = agileScanSupported && agileScanCheckbox && agileScanCheckbox->isChecked();
    if (agileScanRangesEdit) {
        agileScanRangesEdit->setEnabled(scanChecked);
    }
    if (agileScanAutoStepCheckbox) {
        agileScanAutoStepCheckbox->setEnabled(agileScanSupported);
        agileScanAutoStepCheckbox->setToolTip(uiText(
            QStringLiteral("agile_auto_step_tooltip"),
            QStringLiteral("Use the current sample rate in MHz as the Agile scan step.")));
    }
    applyAgileScanAutoStep(true);
    if (agileScanStepSpin) {
        agileScanStepSpin->setEnabled(scanChecked && !agileScanAutoStepSampleRate);
    }
    if (agileScanPresetCombo) {
        agileScanPresetCombo->setEnabled(agileScanSupported);
    }
    if (agileScanSavePresetButton) {
        agileScanSavePresetButton->setEnabled(agileScanSupported);
    }
    if (agileScanDeletePresetButton) {
        agileScanDeletePresetButton->setEnabled(agileScanSupported);
    }
    const bool standardScanChecked =
        standardScanSupported && standardScanCheckbox && standardScanCheckbox->isChecked();
    if (standardScanCentersEdit) {
        standardScanCentersEdit->setEnabled(standardScanChecked);
    }
    if (standardScanPresetCombo) {
        standardScanPresetCombo->setEnabled(standardScanSupported);
    }
    if (standardScanSavePresetButton) {
        standardScanSavePresetButton->setEnabled(standardScanSupported);
    }
    if (standardScanDeletePresetButton) {
        standardScanDeletePresetButton->setEnabled(standardScanSupported);
    }
    if (standardScanRangeStartEdit) {
        standardScanRangeStartEdit->setEnabled(standardScanSupported);
    }
    if (standardScanRangeEndEdit) {
        standardScanRangeEndEdit->setEnabled(standardScanSupported);
    }
    if (standardScanDwellSpin) {
        standardScanDwellSpin->setEnabled(standardScanSupported);
    }
    if (standardScanSettleSpin) {
        standardScanSettleSpin->setEnabled(standardScanSupported);
    }
    if (standardScanAddLowerButton) {
        standardScanAddLowerButton->setEnabled(standardScanSupported);
    }
    if (standardScanAddUpperButton) {
        standardScanAddUpperButton->setEnabled(standardScanSupported);
    }
    if (standardScanRemoveLowerButton) {
        standardScanRemoveLowerButton->setEnabled(standardScanSupported);
    }
    if (standardScanRemoveUpperButton) {
        standardScanRemoveUpperButton->setEnabled(standardScanSupported);
    }
    if (standardScanFillRangeButton) {
        standardScanFillRangeButton->setEnabled(standardScanSupported);
    }
    if (scanMeasurementBinSpin) {
        scanMeasurementBinSpin->setEnabled(scanMeasurementCheckbox && scanMeasurementCheckbox->isChecked());
    }
    if (scanMeasurementBaselineButton) {
        scanMeasurementBaselineButton->setEnabled(scanMeasurementCheckbox && scanMeasurementCheckbox->isChecked());
    }
    if (scanMeasurementResetPeakButton) {
        scanMeasurementResetPeakButton->setEnabled(scanMeasurementCheckbox && scanMeasurementCheckbox->isChecked());
    }
    if (scanMeasurementExportButton) {
        scanMeasurementExportButton->setEnabled(scanMeasurementCheckbox && scanMeasurementCheckbox->isChecked());
    }

    QString error;
    const QVector<double> frequencies = agileScanFrequencyList(&error);
    if (agileScanStatusLabel) {
        if (!agileScanSupported) {
            agileScanStatusLabel->setText(uiText(QStringLiteral("agile_firmware_required"),
                                                 QStringLiteral("Agile firmware required")));
        } else if (!error.isEmpty()) {
            agileScanStatusLabel->setText(error);
        } else if (scanChecked) {
            agileScanStatusLabel->setText(uiText(QStringLiteral("scan_list_points"),
                                                 QStringLiteral("Scan list: %1 points"))
                                          .arg(frequencies.size()));
        } else {
            agileScanStatusLabel->setText(uiText(QStringLiteral("agile_scan_off"),
                                                 QStringLiteral("Agile scan: off")));
        }
    }

    if (standardScanStatusLabel) {
        normalizeStandardScanCentersUi(false);
        QString standardError;
        const QVector<double> centers =
            parseStandardScanCentersMhz(standardScanCentersMhz,
                                        pendingSettings.sampleRate,
                                        standardScanChecked ? AGILE_SCAN_MIN_POINTS : 0,
                                        &standardError,
                                        nullptr);
        if (!standardScanSupported) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_firmware_required"),
                                                    QStringLiteral("Live-retune receiver required")));
        } else if (!standardError.isEmpty()) {
            standardScanStatusLabel->setText(standardError);
        } else if (standardScanChecked) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_list_points"),
                                                    QStringLiteral("Standard scan: %1 centers"))
                                             .arg(centers.size()));
        } else {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_off"),
                                                    QStringLiteral("Standard scan: off")));
        }
    }

    updateListeningScanControls();
}

void YourClassName::updateListeningScanControls() {
    if (listeningScanPresetCombo) {
        const QString currentText = listeningScanPresetCombo->currentText();
        QSignalBlocker blocker(listeningScanPresetCombo);
        listeningScanPresetCombo->clear();
        listeningScanPresetOrder =
            normalizedPresetOrder(listeningScanPresetOrder,
                                  listeningScanPresets,
                                  defaultListeningScanPresetOrder());
        for (const QString &name : std::as_const(listeningScanPresetOrder)) {
            listeningScanPresetCombo->addItem(name, name);
        }
        listeningScanPresetCombo->setEditText(currentText);
    }

    if (listeningScanCheckbox) {
        QSignalBlocker blocker(listeningScanCheckbox);
        listeningScanCheckbox->setChecked(listeningScanEnabled);
    }
    if (listeningScanTargetsEdit) {
        listeningScanTargetsEdit->setEnabled(listeningScanEnabled);
    }
    if (listeningScanDwellSpin) {
        listeningScanDwellSpin->setEnabled(true);
    }
    if (listeningScanSettleSpin) {
        listeningScanSettleSpin->setEnabled(true);
    }
    if (listeningScanPresetCombo) {
        listeningScanPresetCombo->setEnabled(true);
    }
    if (listeningScanSavePresetButton) {
        listeningScanSavePresetButton->setEnabled(true);
    }
    if (listeningScanDeletePresetButton) {
        listeningScanDeletePresetButton->setEnabled(true);
    }

    if (!listeningScanStatusLabel) {
        return;
    }

    if (!listeningScanEnabled) {
        listeningScanStatusLabel->setText(uiText(QStringLiteral("listening_scan_off"),
                                                QStringLiteral("Listening scan: off")));
        return;
    }

    QString error;
    const QVector<double> targets = listeningScanFrequencyList(&error);
    if (!error.isEmpty()) {
        listeningScanStatusLabel->setText(error);
    } else if (listeningScanEnabled && listeningScanRunning && !activeListeningScanFrequencies.isEmpty()) {
        const int index = (std::clamp)(listeningScanIndex, 0, activeListeningScanFrequencies.size() - 1);
        listeningScanStatusLabel->setText(
            uiText(QStringLiteral("listening_scan_active"),
                   QStringLiteral("Listening scan active: %1 targets, #%2 %3 MHz"))
                .arg(activeListeningScanFrequencies.size())
                .arg(index + 1)
                .arg(activeListeningScanFrequencies.at(index) / 1000000.0, 0, 'f', 6));
    } else if (listeningScanEnabled) {
        listeningScanStatusLabel->setText(
            uiText(QStringLiteral("listening_scan_list_points"),
                   QStringLiteral("Listening scan: %1 targets"))
                .arg(targets.size()));
    }
}

void YourClassName::updateScanMeasurement(const std::vector<float> &frequencies,
                                          const std::vector<float> &magnitudes) {
    if (!scanMeasurementEnabled || frequencies.empty() || magnitudes.empty()) {
        return;
    }
    if (!scanMeasurementUpdateClock.isValid()) {
        scanMeasurementUpdateClock.start();
    } else if (scanMeasurementUpdateClock.elapsed() <
               (std::clamp)(scanMeasurementUpdateIntervalMs,
                            SCAN_MEASUREMENT_MIN_UPDATE_MS,
                            SCAN_MEASUREMENT_MAX_UPDATE_MS)) {
        return;
    } else {
        scanMeasurementUpdateClock.restart();
    }

    const int dataCount = std::min(static_cast<int>(frequencies.size()),
                                   static_cast<int>(magnitudes.size()));
    if (dataCount <= 0) {
        return;
    }

    const double binHz = (std::clamp)(scanMeasurementBinMhz,
                                      SCAN_MEASUREMENT_MIN_BIN_MHZ,
                                      SCAN_MEASUREMENT_MAX_BIN_MHZ) * 1000000.0;
    if (!std::isfinite(binHz) || binHz <= 0.0) {
        return;
    }

    const double firstFrequency = frequencies.front();
    const double lastFrequency = frequencies[static_cast<std::size_t>(dataCount - 1)];
    if (!std::isfinite(firstFrequency) || !std::isfinite(lastFrequency)) {
        return;
    }
    const qint64 firstKey = static_cast<qint64>(std::llround(firstFrequency / binHz));
    const qint64 lastKey = static_cast<qint64>(std::llround(lastFrequency / binHz));
    const qint64 minimumKey = (std::min)(firstKey, lastKey);
    const qint64 maximumKey = (std::max)(firstKey, lastKey);
    const qint64 frameBinCount64 = maximumKey - minimumKey + 1;
    if (frameBinCount64 <= 0) {
        return;
    }

    ++scanMeasurementSequence;
    const bool useDenseFrameBins = frameBinCount64 <= 1000000;
    std::vector<float> framePeakByBin;
    QHash<qint64, float> sparseFramePeakByBin;
    if (useDenseFrameBins) {
        framePeakByBin.assign(static_cast<std::size_t>(frameBinCount64),
                              -std::numeric_limits<float>::infinity());
    } else {
        sparseFramePeakByBin.reserve((std::min)(dataCount, 65536));
    }
    for (int i = 0; i < dataCount; ++i) {
        const double frequency = frequencies[static_cast<std::size_t>(i)];
        const float level = magnitudes[static_cast<std::size_t>((i + dataCount / 2) % dataCount)];
        if (!std::isfinite(frequency) || !std::isfinite(level)) {
            continue;
        }
        const qint64 key = static_cast<qint64>(std::llround(frequency / binHz));
        const qint64 frameIndex = key - minimumKey;
        if (useDenseFrameBins && frameIndex >= 0 && frameIndex < frameBinCount64) {
            float &peak = framePeakByBin[static_cast<std::size_t>(frameIndex)];
            peak = (std::max)(peak, level);
        } else if (!useDenseFrameBins) {
            const auto existing = sparseFramePeakByBin.constFind(key);
            if (existing == sparseFramePeakByBin.constEnd() || level > existing.value()) {
                sparseFramePeakByBin.insert(key, level);
            }
        }
    }

    auto applyFramePeak = [this, binHz](qint64 key, float framePeak) {
        ScanMeasurementBin &bin = scanMeasurementBins[key];
        bin.frequencyHz = static_cast<double>(key) * binHz;
        bin.currentDb = framePeak;
        bin.peakDb = (std::max)(bin.peakDb, framePeak);
        bin.seenCount += 1;
        bin.lastSequence = scanMeasurementSequence;
        if (scanMeasurementBaselineRecording) {
            if (bin.baselineCount <= 0) {
                bin.baselineDb = framePeak;
                bin.baselineCount = 1;
            } else {
                const float alpha = bin.baselineCount < 8 ? 0.35f : 0.12f;
                bin.baselineDb += alpha * (framePeak - bin.baselineDb);
                ++bin.baselineCount;
            }
        }
    };
    if (useDenseFrameBins) {
        for (qint64 frameIndex = 0; frameIndex < frameBinCount64; ++frameIndex) {
            const float framePeak = framePeakByBin[static_cast<std::size_t>(frameIndex)];
            if (std::isfinite(framePeak)) {
                applyFramePeak(minimumKey + frameIndex, framePeak);
            }
        }
    } else {
        for (auto it = sparseFramePeakByBin.constBegin(); it != sparseFramePeakByBin.constEnd(); ++it) {
            applyFramePeak(it.key(), it.value());
        }
    }

    if (!scanMeasurementStatusClock.isValid()) {
        scanMeasurementStatusClock.start();
        updateScanMeasurementStatus();
    } else if (scanMeasurementStatusClock.elapsed() >=
               (std::max)(500,
                          (std::clamp)(scanMeasurementUpdateIntervalMs,
                                       SCAN_MEASUREMENT_MIN_UPDATE_MS,
                                       SCAN_MEASUREMENT_MAX_UPDATE_MS))) {
        scanMeasurementStatusClock.restart();
        updateScanMeasurementStatus();
    }
}

std::vector<float> YourClassName::scanMeasurementOverlay(const std::vector<float> &frequencies,
                                                         int dataCount) const {
    std::vector<float> overlay;
    if (!scanMeasurementEnabled || scanMeasurementBins.isEmpty() || dataCount <= 0) {
        scanMeasurementOverlayCache.clear();
        return overlay;
    }

    const double firstHz = !frequencies.empty() ? frequencies.front() : std::numeric_limits<double>::quiet_NaN();
    const double lastHz = !frequencies.empty() ? frequencies.back() : std::numeric_limits<double>::quiet_NaN();
    const bool cacheMatches = scanMeasurementOverlayCacheCount == dataCount &&
                              scanMeasurementOverlayCacheSequence == scanMeasurementSequence &&
                              scanMeasurementOverlayCacheFirstHz == firstHz &&
                              scanMeasurementOverlayCacheLastHz == lastHz &&
                              !scanMeasurementOverlayCache.empty();
    if (cacheMatches) {
        if (!scanMeasurementOverlayClock.isValid() ||
            scanMeasurementOverlayClock.elapsed() <
                (std::clamp)(scanMeasurementUpdateIntervalMs,
                             SCAN_MEASUREMENT_MIN_UPDATE_MS,
                             SCAN_MEASUREMENT_MAX_UPDATE_MS)) {
            return scanMeasurementOverlayCache;
        }
    }

    const double binHz = (std::clamp)(scanMeasurementBinMhz,
                                      SCAN_MEASUREMENT_MIN_BIN_MHZ,
                                      SCAN_MEASUREMENT_MAX_BIN_MHZ) * 1000000.0;
    if (!std::isfinite(binHz) || binHz <= 0.0) {
        return overlay;
    }

    overlay.assign(static_cast<std::size_t>(dataCount), -160.0f);
    for (int i = 0; i < dataCount && i < static_cast<int>(frequencies.size()); ++i) {
        const double frequency = frequencies[static_cast<std::size_t>(i)];
        if (!std::isfinite(frequency)) {
            continue;
        }
        const qint64 key = static_cast<qint64>(std::llround(frequency / binHz));
        const auto it = scanMeasurementBins.constFind(key);
        if (it == scanMeasurementBins.constEnd()) {
            continue;
        }
        overlay[static_cast<std::size_t>((i + dataCount / 2) % dataCount)] = it.value().peakDb;
    }
    scanMeasurementOverlayCache = overlay;
    scanMeasurementOverlayCacheCount = dataCount;
    scanMeasurementOverlayCacheSequence = scanMeasurementSequence;
    scanMeasurementOverlayCacheFirstHz = firstHz;
    scanMeasurementOverlayCacheLastHz = lastHz;
    if (!scanMeasurementOverlayClock.isValid()) {
        scanMeasurementOverlayClock.start();
    } else {
        scanMeasurementOverlayClock.restart();
    }
    return overlay;
}

void YourClassName::updateScanMeasurementStatus() {
    if (!scanMeasurementStatusLabel) {
        return;
    }

    auto setScanStatus = [this](const QString &text) {
        scanMeasurementStatusLabel->setToolTip(text);
        scanMeasurementStatusLabel->setText(text);
    };

    if (!scanMeasurementEnabled) {
        setScanStatus(uiText(QStringLiteral("scan_measurement_off"),
                             QStringLiteral("Spectrum measurement: off")));
        return;
    }

    if (scanMeasurementBins.isEmpty()) {
        setScanStatus(scanMeasurementBaselineRecording
                          ? uiText(QStringLiteral("scan_measurement_recording_baseline"),
                                   QStringLiteral("Spectrum measurement: recording baseline..."))
                          : uiText(QStringLiteral("scan_measurement_waiting"),
                                   QStringLiteral("Spectrum measurement: waiting for spectrum")));
        return;
    }

    int currentBins = 0;
    int baselineBins = 0;
    int coveredBins = 0;
    double peakSum = 0.0;
    double deltaSum = 0.0;
    float maxPeak = -160.0f;
    for (const ScanMeasurementBin &bin : scanMeasurementBins) {
        if (bin.seenCount <= 0) {
            continue;
        }
        ++currentBins;
        peakSum += bin.peakDb;
        maxPeak = (std::max)(maxPeak, bin.peakDb);
        if (bin.baselineCount > 0) {
            ++baselineBins;
            const float delta = bin.peakDb - bin.baselineDb;
            deltaSum += delta;
            if (delta >= SCAN_MEASUREMENT_COVERAGE_DELTA_DB) {
                ++coveredBins;
            }
        }
    }

    const double avgPeak = currentBins > 0 ? peakSum / currentBins : -160.0;
    const double avgDelta = baselineBins > 0 ? deltaSum / baselineBins : 0.0;
    const double coverage = baselineBins > 0
                                ? (100.0 * static_cast<double>(coveredBins) / baselineBins)
                                : 0.0;
    setScanStatus(
        baselineBins > 0
            ? uiText(QStringLiteral("scan_measurement_with_baseline"),
                     QStringLiteral("Spectrum measurement: %1 bins, peak %2 dB, avg %3 dB, delta %4 dB, coverage %5% >+%6 dB%7"))
                  .arg(currentBins)
                  .arg(maxPeak, 0, 'f', 1)
                  .arg(avgPeak, 0, 'f', 1)
                  .arg(avgDelta, 0, 'f', 1)
                  .arg(coverage, 0, 'f', 0)
                  .arg(SCAN_MEASUREMENT_COVERAGE_DELTA_DB, 0, 'f', 0)
                  .arg(scanMeasurementBaselineRecording
                           ? uiText(QStringLiteral("bg_rec_suffix"), QStringLiteral(" (BG rec)"))
                           : QString())
            : uiText(QStringLiteral("scan_measurement_without_baseline"),
                     QStringLiteral("Spectrum measurement: %1 bins, peak %2 dB, avg %3 dB%4"))
                  .arg(currentBins)
                  .arg(maxPeak, 0, 'f', 1)
                  .arg(avgPeak, 0, 'f', 1)
                  .arg(scanMeasurementBaselineRecording
                           ? uiText(QStringLiteral("bg_rec_suffix"), QStringLiteral(" (BG rec)"))
                           : QString()));
}

void YourClassName::resetScanMeasurementPeaks() {
    for (auto &bin : scanMeasurementBins) {
        bin.peakDb = bin.currentDb;
    }
    scanMeasurementOverlayCache.clear();
    scanMeasurementOverlayCacheSequence = 0;
    updateScanMeasurementStatus();
}

void YourClassName::clearScanMeasurement() {
    scanMeasurementBins.clear();
    scanMeasurementSequence = 0;
    scanMeasurementOverlayCache.clear();
    scanMeasurementOverlayCacheSequence = 0;
    updateScanMeasurementStatus();
}

void YourClassName::exportScanMeasurementCsv() {
    if (scanMeasurementBins.isEmpty()) {
        QMessageBox::information(this,
                                 uiText(QStringLiteral("scan_measurement_title"),
                                        QStringLiteral("Spectrum measurement")),
                                 uiText(QStringLiteral("scan_measurement_no_data_export"),
                                        QStringLiteral("No spectrum measurement data to export.")));
        return;
    }

    const QString defaultPath =
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("scan_measurement_%1.csv").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    const QString path = QFileDialog::getSaveFileName(this,
                                                     uiText(QStringLiteral("export_scan_measurement_csv"),
                                                             QStringLiteral("Export spectrum measurement CSV")),
                                                      defaultPath,
                                                      uiText(QStringLiteral("csv_files_filter"),
                                                             QStringLiteral("CSV files (*.csv)")));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this,
                             uiText(QStringLiteral("scan_measurement_title"),
                                    QStringLiteral("Spectrum measurement")),
                             uiText(QStringLiteral("scan_measurement_csv_write_failed"),
                                    QStringLiteral("Cannot write CSV file.")));
        return;
    }

    QTextStream out(&file);
    out << "frequency_mhz,current_db,peak_db,baseline_db,delta_db,seen_count,baseline_count\n";
    for (const ScanMeasurementBin &bin : scanMeasurementBins) {
        const bool hasBaseline = bin.baselineCount > 0;
        const float delta = hasBaseline ? (bin.peakDb - bin.baselineDb) : 0.0f;
        out << QString::number(bin.frequencyHz / 1000000.0, 'f', 6) << ','
            << QString::number(bin.currentDb, 'f', 2) << ','
            << QString::number(bin.peakDb, 'f', 2) << ','
            << (hasBaseline ? QString::number(bin.baselineDb, 'f', 2) : QString()) << ','
            << (hasBaseline ? QString::number(delta, 'f', 2) : QString()) << ','
            << bin.seenCount << ','
            << bin.baselineCount << '\n';
    }
    updateScanMeasurementStatus();
}

bool YourClassName::applyAgileScanSettings(bool forceStop) {
    if (forceStop || !agileScanEnabled) {
        if (activeFobosApiKind == FobosApiKind::Agile && agileDevice && agileScanRunning) {
            const int result = stopFobosAgileScanSafely(agileDevice);
            qDebug() << "[AgileScan] stop" << "result" << result;
            agileScanRunning = false;
            activeAgileScanFrequencies.clear();
            scanVisualAssembler.reset();
            if (graphWidget) {
                graphWidget->setScanSegments({});
            }
            if (waterfallWidget) {
                waterfallWidget->setScanSegments({});
            }
            if (result != FOBOS_ERR_OK) {
                return false;
            }
        }
        if (agileScanStatusLabel) {
            agileScanStatusLabel->setText(QStringLiteral("Agile scan: off"));
        }
        return true;
    }

    if (activeFobosApiKind != FobosApiKind::Agile || !agileDevice) {
        if (agileScanStatusLabel) {
            agileScanStatusLabel->setText(QStringLiteral("Agile receiver required"));
        }
        if (agileScanEnabled) {
            qDebug() << "[AgileScan] disabling saved scan flag because active receiver is not Agile"
                     << "apiKind" << fobosApiKindName(activeFobosApiKind);
        }
        agileScanEnabled = false;
        agileScanRunning = false;
        activeAgileScanFrequencies.clear();
        scanVisualAssembler.reset();
        if (agileScanCheckbox && agileScanCheckbox->isChecked()) {
            QSignalBlocker blocker(agileScanCheckbox);
            agileScanCheckbox->setChecked(false);
        }
        return true;
    }

    if (pendingSettings.inputMode != INPUT_RF) {
        if (agileScanRunning) {
            const int result = stopFobosAgileScanSafely(agileDevice);
            qDebug() << "[AgileScan] stopped outside RF mode" << "result" << result;
            agileScanRunning = false;
            activeAgileScanFrequencies.clear();
        }
        if (agileScanStatusLabel) {
            agileScanStatusLabel->setText(QStringLiteral("Agile scan works in RF mode"));
        }
        return true;
    }

    QString error;
    QVector<double> frequencies = agileScanFrequencyList(&error);
    if (!error.isEmpty() || frequencies.isEmpty()) {
        if (agileScanStatusLabel) {
            agileScanStatusLabel->setText(error.isEmpty() ? QStringLiteral("Bad Agile scan list") : error);
        }
        qDebug() << "[AgileScan] invalid scan list" << error;
        return false;
    }
    bool scanListChanged = activeAgileScanFrequencies.size() != frequencies.size();
    if (!scanListChanged) {
        for (int i = 0; i < frequencies.size(); ++i) {
            if (std::abs(activeAgileScanFrequencies.at(i) - frequencies.at(i)) > 0.5) {
                scanListChanged = true;
                break;
            }
        }
    }
    if (scanListChanged && !scanMeasurementBins.isEmpty()) {
        clearScanMeasurement();
    }

    if (agileScanRunning) {
        const int stopResult = stopFobosAgileScanSafely(agileDevice);
        qDebug() << "[AgileScan] restart stop" << "result" << stopResult;
        agileScanRunning = false;
        activeAgileScanFrequencies.clear();
        if (stopResult != FOBOS_ERR_OK) {
            if (agileScanStatusLabel) {
                agileScanStatusLabel->setText(QStringLiteral("Scan stop failed: %1").arg(stopResult));
            }
            return false;
        }
    }

    pendingSettings.centerFrequency = frequencies.first();
    pendingSettings.actualFrequency = frequencies.first();
    if (!scanListeningLockEnabled &&
        (pendingSettings.listeningFrequency < pendingSettings.centerFrequency - pendingSettings.sampleRate / 2.0 ||
         pendingSettings.listeningFrequency > pendingSettings.centerFrequency + pendingSettings.sampleRate / 2.0)) {
        pendingSettings.listeningFrequency = pendingSettings.centerFrequency;
    }

    QVector<double> hardwareFrequencies;
    hardwareFrequencies.reserve(frequencies.size());
    for (double frequency : frequencies) {
        hardwareFrequencies.append(calibratedHardwareFrequency(frequency));
    }
    const int result = startFobosAgileScanSafely(agileDevice,
                                                hardwareFrequencies.data(),
                                                static_cast<unsigned int>(hardwareFrequencies.size()));
    const int scanning = result == FOBOS_ERR_OK ? isFobosAgileScanningSafely(agileDevice) : result;
    const int index = result == FOBOS_ERR_OK ? getFobosAgileScanIndexSafely(agileDevice) : -1;
    qDebug() << "[AgileScan] start"
             << "result" << result
             << "points" << frequencies.size()
             << "firstHz" << frequencies.first()
             << "lastHz" << frequencies.last()
             << "baseCalibrationHz" << frequencyCalibrationOffsetHz
             << "tableCalibration" << calibrationTableEnabled
             << "isScanning" << scanning
             << "index" << index;
    agileScanRunning = result == FOBOS_ERR_OK;
    activeAgileScanFrequencies = agileScanRunning ? frequencies : QVector<double>();
    if (agileScanStatusLabel) {
        agileScanStatusLabel->setText(result == FOBOS_ERR_OK
                                          ? QStringLiteral("Scan active: %1 points").arg(frequencies.size())
                                          : QStringLiteral("Scan start failed: %1").arg(result));
    }
    return result == FOBOS_ERR_OK;
}

void YourClassName::resetStandardScanState(bool clearSegments) {
    standardScanRunning = false;
    activeStandardScanFrequencies.clear();
    standardScanIndex = 0;
    standardScanDwellTimer.invalidate();
    if (standardScanAdvanceTimer) {
        standardScanAdvanceTimer->stop();
    }

    if (!clearSegments) {
        return;
    }

    scanVisualAssembler.reset();
    if (graphWidget) {
        graphWidget->setScanSegments({});
    }
    if (waterfallWidget) {
        waterfallWidget->setScanSegments({});
    }
    if (scaleWidget) {
        scaleWidget->setScanSegments({});
    }
}

bool YourClassName::applyStandardScanRetune(double targetFrequencyHz, const char *reason) {
    const bool externalBackendSelected = isExternalReceiverBackendSelected();
    const bool fobosBackendAvailable = hasActiveFobosDevice();
    if ((!fobosBackendAvailable && !externalBackendSelected) ||
        pendingSettings.inputMode != INPUT_RF ||
        (fobosBackendAvailable && activeFobosApiKind == FobosApiKind::Agile && agileScanRunning) ||
        !std::isfinite(targetFrequencyHz) ||
        targetFrequencyHz <= 0.0) {
        return false;
    }

    const double requestedFrequency = (std::clamp)(targetFrequencyHz,
                                                   rfMinimumCenterFrequency(pendingSettings),
                                                   rfMaximumCenterFrequency(pendingSettings));
    double tunedFrequency = requestedFrequency;

    if (externalBackendSelected) {
        const bool streamRunning = processor && processor->isRunning();
        const uint64_t preRetuneIqEpoch =
            processor ? processor->beginIqRetuneBarrier() : 0;
        if (streamRunning) {
            clearLiveSpectrumSnapshot(false, preRetuneIqEpoch);
            IqBuffer::armRetuneTrace(preRetuneIqEpoch, 6, 4, 6, 6);
        }

        QElapsedTimer retuneTimer;
        retuneTimer.start();
        if (streamRunning) {
            processor->setFrequencyCalibrationOffset(
                effectiveFrequencyCalibrationOffsetHz(requestedFrequency));
        }
        const bool retuned = !streamRunning ||
                             processor->retuneCenterFrequency(requestedFrequency);
        const qint64 retuneCallMs = retuneTimer.elapsed();
        if (!retuned) {
            qDebug() << "[StandardScan] external retune failed"
                     << "reason" << (reason ? reason : "")
                     << "requested" << requestedFrequency
                     << "callMs" << retuneCallMs;
            if (standardScanStatusLabel) {
                standardScanStatusLabel->setText(
                    uiText(QStringLiteral("standard_scan_retune_failed"),
                           QStringLiteral("Standard scan retune failed: %1"))
                        .arg(-1));
            }
            return false;
        }

        pendingSettings.centerFrequency = requestedFrequency;
        pendingSettings.actualFrequency = tunedFrequency;
        actualFrequency = tunedFrequency;
        const uint64_t postRetuneIqEpoch =
            processor ? processor->beginIqRetuneBarrier() : 0;
        if (streamRunning) {
            clearLiveSpectrumSnapshot(false, postRetuneIqEpoch);
            IqBuffer::armRetuneTrace(postRetuneIqEpoch);
        }
        if (hardwareSettingsApplied) {
            appliedHardwareSettings.centerFrequency = requestedFrequency;
            appliedHardwareSettings.actualFrequency = tunedFrequency;
        }
        publishSettingsToGlobals();
        if (frequencyControl) {
            QSignalBlocker blocker(frequencyControl);
            frequencyControl->setValueHz(pendingSettings.centerFrequency);
        }
        if (!scanListeningLockEnabled) {
            settingRange();
        } else {
            updateFineTuneLabel();
        }

        networkSpectrumFrameMetadataValid = false;
        networkSpectrumFrameMinFrequency = 0.0;
        networkSpectrumFrameMaxFrequency = 0.0;
        networkSpectrumFrameFftLength = 0;
        liveRetuneSettleDurationMs = (std::clamp)(standardScanSettleMs,
                                                  STANDARD_SCAN_MIN_SETTLE_MS,
                                                  STANDARD_SCAN_MAX_SETTLE_MS);
        if (streamRunning) {
            liveRetuneSettleTimer.start();
        } else {
            liveRetuneSettleTimer.invalidate();
        }
        standardScanDwellTimer.restart();
        spectrumTuningDebugFramesRemaining = fobosVerboseLoggingEnabled() ? 4 : 0;

        qDebug() << "[StandardScan] external retune"
                 << "reason" << (reason ? reason : "")
                 << "index" << standardScanIndex
                 << "requested" << requestedFrequency
                 << "actual" << tunedFrequency
                 << "streamRunning" << streamRunning
                 << "callMs" << retuneCallMs
                 << "iqEpoch" << postRetuneIqEpoch
                 << "settleMs" << liveRetuneSettleDurationMs;
        return true;
    }

    const uint64_t preRetuneIqEpoch =
        processor ? processor->beginIqRetuneBarrier() : 0;
    IqBuffer::clear(preRetuneIqEpoch);
    QElapsedTimer retuneTimer;
    retuneTimer.start();
    const int result = setCalibratedActiveFrequencySafely(requestedFrequency, &tunedFrequency);
    const qint64 retuneCallMs = retuneTimer.elapsed();
    if (result != FOBOS_ERR_OK) {
        qDebug() << "[StandardScan] retune failed"
                 << "reason" << (reason ? reason : "")
                 << "requested" << requestedFrequency
                 << "callMs" << retuneCallMs
                 << "error" << result;
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(
                uiText(QStringLiteral("standard_scan_retune_failed"),
                       QStringLiteral("Standard scan retune failed: %1"))
                    .arg(result));
        }
        return false;
    }

    pendingSettings.centerFrequency = requestedFrequency;
    pendingSettings.actualFrequency = tunedFrequency;
    actualFrequency = tunedFrequency;
    const uint64_t postRetuneIqEpoch =
        processor ? processor->beginIqRetuneBarrier() : 0;
    if (hardwareSettingsApplied) {
        appliedHardwareSettings.centerFrequency = requestedFrequency;
        appliedHardwareSettings.actualFrequency = tunedFrequency;
    }
    publishSettingsToGlobals();
    if (frequencyControl) {
        QSignalBlocker blocker(frequencyControl);
        frequencyControl->setValueHz(pendingSettings.centerFrequency);
    }
    if (!scanListeningLockEnabled) {
        settingRange();
    } else {
        updateFineTuneLabel();
    }

    IqBuffer::clear(postRetuneIqEpoch);
    fftResult = std::make_unique<FFTResult>();
    if (spectrumFftWorker) {
        spectrumFftWorker->resetHfNoiseCancelState();
    }
    networkSpectrumFrameMetadataValid = false;
    networkSpectrumFrameMinFrequency = 0.0;
    networkSpectrumFrameMaxFrequency = 0.0;
    networkSpectrumFrameFftLength = 0;
    liveRetuneSettleDurationMs = (std::clamp)(standardScanSettleMs,
                                              STANDARD_SCAN_MIN_SETTLE_MS,
                                              STANDARD_SCAN_MAX_SETTLE_MS);
    liveRetuneSettleTimer.start();
    standardScanDwellTimer.restart();
    spectrumTuningDebugFramesRemaining = fobosVerboseLoggingEnabled() ? 4 : 0;

    qDebug() << "[StandardScan] retune"
             << "reason" << (reason ? reason : "")
             << "index" << standardScanIndex
             << "requested" << requestedFrequency
             << "actual" << tunedFrequency
             << "callMs" << retuneCallMs
             << "iqEpoch" << postRetuneIqEpoch
             << "settleMs" << liveRetuneSettleDurationMs;
    return true;
}

bool YourClassName::applyStandardScanSettings(bool forceStop) {
    if (forceStop || !standardScanEnabled) {
        resetStandardScanState(true);
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_off"),
                                                    QStringLiteral("Standard scan: off")));
        }
        return true;
    }

    const bool standardScanBackendAvailable =
        hasActiveFobosDevice() || isExternalReceiverBackendSelected();
    if (!standardScanBackendAvailable) {
        resetStandardScanState(true);
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_firmware_required"),
                                                    QStringLiteral("Live-retune receiver required")));
        }
        return true;
    }

    if (hasActiveFobosDevice() &&
        activeFobosApiKind == FobosApiKind::Agile &&
        agileScanRunning) {
        resetStandardScanState(true);
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_disable_agile"),
                                                    QStringLiteral("Disable Agile firmware scan first")));
        }
        return true;
    }

    if (pendingSettings.inputMode != INPUT_RF) {
        resetStandardScanState(true);
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_rf_only"),
                                                    QStringLiteral("Standard scan works in RF mode")));
        }
        return true;
    }

    normalizeStandardScanCentersUi(true);
    QString error;
    const QVector<double> frequencies = standardScanFrequencyList(&error);
    if (!error.isEmpty() || frequencies.size() < AGILE_SCAN_MIN_POINTS) {
        resetStandardScanState(true);
        if (standardScanStatusLabel) {
            standardScanStatusLabel->setText(error.isEmpty()
                                                 ? uiText(QStringLiteral("standard_scan_bad_list"),
                                                          QStringLiteral("Bad standard scan list"))
                                                 : error);
        }
        qDebug() << "[StandardScan] invalid list" << error;
        return false;
    }

    bool scanListChanged = activeStandardScanFrequencies.size() != frequencies.size();
    if (!scanListChanged) {
        for (int i = 0; i < frequencies.size(); ++i) {
            if (std::abs(activeStandardScanFrequencies.at(i) - frequencies.at(i)) > 0.5) {
                scanListChanged = true;
                break;
            }
        }
    }
    if (scanListChanged && !scanMeasurementBins.isEmpty()) {
        clearScanMeasurement();
    }

    activeStandardScanFrequencies = frequencies;
    if (scanListChanged) {
        standardScanIndex = 0;
        scanVisualAssembler.reset();
    } else {
        standardScanIndex = (std::clamp)(standardScanIndex, 0, activeStandardScanFrequencies.size() - 1);
    }
    standardScanRunning = true;

    const double firstCenter = activeStandardScanFrequencies.at(standardScanIndex);
    pendingSettings.centerFrequency = firstCenter;
    pendingSettings.actualFrequency = firstCenter;
    if (!scanListeningLockEnabled &&
        (pendingSettings.listeningFrequency < firstCenter - pendingSettings.sampleRate * 0.5 ||
         pendingSettings.listeningFrequency > firstCenter + pendingSettings.sampleRate * 0.5)) {
        pendingSettings.listeningFrequency = firstCenter;
    }

    if (!applyStandardScanRetune(firstCenter, scanListChanged ? "standard scan start" : "standard scan refresh")) {
        resetStandardScanState(true);
        return false;
    }
    if (standardScanAdvanceTimer && !standardScanAdvanceTimer->isActive()) {
        standardScanAdvanceTimer->start();
    }

    if (standardScanStatusLabel) {
        standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_active"),
                                                QStringLiteral("Standard scan active: %1 centers"))
                                         .arg(activeStandardScanFrequencies.size()));
    }
    return true;
}

void YourClassName::advanceStandardScanIfNeeded() {
    if (!standardScanRunning ||
        !standardScanEnabled ||
        (!hasActiveFobosDevice() && !isExternalReceiverBackendSelected()) ||
        pendingSettings.inputMode != INPUT_RF ||
        activeStandardScanFrequencies.size() < AGILE_SCAN_MIN_POINTS ||
        runState != RadioRunState::Running ||
        (hasActiveFobosDevice() && activeFobosApiKind == FobosApiKind::Agile && agileScanRunning)) {
        return;
    }

    if (liveRetuneSettleTimer.isValid()) {
        const qint64 elapsedMs = liveRetuneSettleTimer.elapsed();
        const qint64 settleMs = liveRetuneSettleDurationMs > 0
                                    ? liveRetuneSettleDurationMs
                                    : STANDARD_SCAN_SETTLE_MS;
        IqBuffer::clear();
        if (elapsedMs < settleMs) {
            return;
        }
        liveRetuneSettleTimer.invalidate();
        standardScanDwellTimer.restart();
        return;
    }

    if (!standardScanDwellTimer.isValid()) {
        standardScanDwellTimer.start();
        return;
    }
    const qint64 dwellMs = (std::clamp)(standardScanDwellMs,
                                        STANDARD_SCAN_MIN_DWELL_MS,
                                        STANDARD_SCAN_MAX_DWELL_MS);
    if (standardScanDwellTimer.elapsed() < dwellMs) {
        return;
    }

    standardScanIndex = (standardScanIndex + 1) % activeStandardScanFrequencies.size();
    const double nextCenter = activeStandardScanFrequencies.at(standardScanIndex);
    applyStandardScanRetune(nextCenter, "standard scan advance");
}

void YourClassName::resetListeningScanState() {
    listeningScanRunning = false;
    activeListeningScanFrequencies.clear();
    listeningScanIndex = 0;
    listeningScanDwellTimer.invalidate();
    listeningScanSettleTimer.invalidate();
    if (listeningScanAdvanceTimer) {
        listeningScanAdvanceTimer->stop();
    }
}

bool YourClassName::applyListeningScanTarget(double targetFrequencyHz, const char *reason) {
    if (!std::isfinite(targetFrequencyHz) || targetFrequencyHz <= 0.0) {
        return false;
    }

    const QPair<double, double> span = listeningScanVisibleSpanHz(pendingSettings);
    if (targetFrequencyHz < span.first - 0.5 || targetFrequencyHz > span.second + 0.5) {
        if (listeningScanStatusLabel) {
            listeningScanStatusLabel->setText(
                uiText(QStringLiteral("listening_scan_target_out_of_span"),
                       QStringLiteral("Listening scan target is outside the visible span: %1-%2 MHz"))
                    .arg(span.first / 1000000.0, 0, 'f', 6)
                    .arg(span.second / 1000000.0, 0, 'f', 6));
        }
        qDebug() << "[ListeningScan] target outside visible span"
                 << "reason" << (reason ? reason : "")
                 << "targetHz" << targetFrequencyHz
                 << "spanLowHz" << span.first
                 << "spanHighHz" << span.second
                 << "centerHz" << pendingSettings.centerFrequency
                 << "sampleRate" << pendingSettings.sampleRate;
        return false;
    }

    pendingSettings.listeningFrequency = targetFrequencyHz;
    normalizeTuning(pendingSettings, true);
    publishSettingsToGlobals();
    if (listeningFrequencyControl) {
        QSignalBlocker blocker(listeningFrequencyControl);
        listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
    }
    settingRange();
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand(80);
    } else if (networkMode == NetworkMode::Server) {
        sendServerStateToClients();
    }

    listeningScanSettleTimer.restart();
    listeningScanDwellTimer.invalidate();
    qDebug() << "[ListeningScan] target"
             << "reason" << (reason ? reason : "")
             << "index" << listeningScanIndex
             << "targetHz" << targetFrequencyHz
             << "dwellMs" << listeningScanDwellMs
             << "settleMs" << listeningScanSettleMs
             << "centerHz" << pendingSettings.centerFrequency
             << "sampleRate" << pendingSettings.sampleRate;
    updateListeningScanControls();
    return true;
}

bool YourClassName::applyListeningScanSettings(bool forceStop) {
    if (forceStop || !listeningScanEnabled) {
        resetListeningScanState();
        if (listeningScanStatusLabel) {
            listeningScanStatusLabel->setText(uiText(QStringLiteral("listening_scan_off"),
                                                    QStringLiteral("Listening scan: off")));
        }
        return true;
    }

    QString error;
    const QVector<double> targets = listeningScanFrequencyList(&error);
    if (!error.isEmpty() || targets.isEmpty()) {
        resetListeningScanState();
        if (listeningScanStatusLabel) {
            listeningScanStatusLabel->setText(error.isEmpty()
                                                  ? uiText(QStringLiteral("listening_scan_bad_list"),
                                                           QStringLiteral("Bad listening scan list"))
                                                  : error);
        }
        qDebug() << "[ListeningScan] invalid list" << error;
        return false;
    }

    const bool listChanged = frequencyListChanged(activeListeningScanFrequencies, targets);
    activeListeningScanFrequencies = targets;
    if (listChanged) {
        listeningScanIndex = 0;
    } else {
        listeningScanIndex = (std::clamp)(listeningScanIndex, 0, activeListeningScanFrequencies.size() - 1);
    }
    listeningScanRunning = true;

    const double target = activeListeningScanFrequencies.at(listeningScanIndex);
    if (!applyListeningScanTarget(target, listChanged ? "listening scan start" : "listening scan refresh")) {
        resetListeningScanState();
        return false;
    }
    if (listeningScanAdvanceTimer && !listeningScanAdvanceTimer->isActive()) {
        listeningScanAdvanceTimer->start();
    }

    updateListeningScanControls();
    return true;
}

void YourClassName::advanceListeningScanIfNeeded() {
    if (!listeningScanRunning ||
        !listeningScanEnabled ||
        activeListeningScanFrequencies.isEmpty()) {
        return;
    }

    if (listeningScanSettleTimer.isValid()) {
        const qint64 settleMs = (std::clamp)(listeningScanSettleMs,
                                             LISTENING_SCAN_MIN_SETTLE_MS,
                                             LISTENING_SCAN_MAX_SETTLE_MS);
        if (listeningScanSettleTimer.elapsed() < settleMs) {
            return;
        }
        listeningScanSettleTimer.invalidate();
        listeningScanDwellTimer.restart();
        return;
    }

    if (!listeningScanDwellTimer.isValid()) {
        listeningScanDwellTimer.start();
        return;
    }

    const qint64 dwellMs = (std::clamp)(listeningScanDwellMs,
                                        LISTENING_SCAN_MIN_DWELL_MS,
                                        LISTENING_SCAN_MAX_DWELL_MS);
    if (listeningScanDwellTimer.elapsed() < dwellMs) {
        return;
    }

    listeningScanIndex = (listeningScanIndex + 1) % activeListeningScanFrequencies.size();
    const double nextTarget = activeListeningScanFrequencies.at(listeningScanIndex);
    if (!applyListeningScanTarget(nextTarget, "listening scan advance")) {
        resetListeningScanState();
    }
}

bool YourClassName::stopAgileScanForNormalRf(const char *reason) {
    if (activeFobosApiKind != FobosApiKind::Agile || !agileDevice) {
        agileScanRunning = false;
        activeAgileScanFrequencies.clear();
        scanVisualAssembler.reset();
        return true;
    }

    const int scanning = isFobosAgileScanningSafely(agileDevice);
    if (scanning < 0 && scanning != FOBOS_ERR_NOT_OPEN) {
        qDebug() << "[AgileScan] normal RF guard could not query scan state"
                 << "reason" << (reason ? reason : "")
                 << "result" << scanning
                 << "flag" << agileScanRunning;
        return false;
    }

    if (scanning <= 0 && !agileScanRunning) {
        return true;
    }

    qDebug() << "[AgileScan] normal RF guard stopping active scan before tuning"
             << "reason" << (reason ? reason : "")
             << "isScanning" << scanning
             << "flag" << agileScanRunning;
    const int stopResult = stopFobosAgileScanSafely(agileDevice);
    const int afterStop = stopResult == FOBOS_ERR_OK
                              ? isFobosAgileScanningSafely(agileDevice)
                              : stopResult;
    qDebug() << "[AgileScan] normal RF guard stop complete"
             << "reason" << (reason ? reason : "")
             << "result" << stopResult
             << "isScanningAfter" << afterStop;

    agileScanRunning = false;
    activeAgileScanFrequencies.clear();
    scanVisualAssembler.reset();
    if (graphWidget) {
        graphWidget->setScanSegments({});
    }
    if (waterfallWidget) {
        waterfallWidget->setScanSegments({});
    }
    if (stopResult == FOBOS_ERR_OK) {
        clearLiveSpectrumSnapshot();
        return true;
    }
    return false;
}
