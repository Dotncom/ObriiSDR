#include "main.h"
#include "appconstants.h"
#include "diagnosticlogging.h"
#include "dspflowpanel.h"
#include "gnssqthhelpers.h"
#include "researchanalysisdialog.h"
#include "zerospandialog.h"
#include "zoomdensitydialog.h"

#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QMap>
#include <QMessageBox>
#include <QPair>
#include <QSignalBlocker>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace {
QString scienceFrequencyText(double frequencyHz) {
    if (!std::isfinite(frequencyHz)) return QStringLiteral("--");
    return QStringLiteral("%1 MHz").arg(frequencyHz / 1.0e6, 0, 'f', 6);
}

QString scienceLevelText(double levelDb, int unit) {
    if (!std::isfinite(levelDb)) return QStringLiteral("--");
    switch ((std::clamp)(unit, 0, 3)) {
    case 1:
        return QStringLiteral("%1 dBm").arg(levelDb, 0, 'f', 2);
    case 2:
        return QStringLiteral("%1 dBuV").arg(levelDb + 106.9897, 0, 'f', 2);
    case 3: {
        const double microvolts = std::pow(10.0, (levelDb + 106.9897) / 20.0);
        return QStringLiteral("%1 uV").arg(microvolts, 0, 'g', 6);
    }
    default:
        return QStringLiteral("%1 dBFS").arg(levelDb, 0, 'f', 2);
    }
}
}

void YourClassName::updateSpectrumScience(const std::vector<float> &frequencies,
                                          const std::vector<float> &levels) {
    spectrumScienceAnalyzer.setTraceEnabled(spectrumScienceMaxHoldEnabled,
                                            spectrumScienceMinHoldEnabled,
                                            spectrumScienceAverageEnabled);
    spectrumScienceAnalyzer.setAverageTimeSeconds(spectrumScienceAverageSeconds);
    spectrumScienceAnalyzer.setAverageFrameCount(spectrumAverageFrameCount);
    spectrumScienceAnalyzer.setDetector(spectrumDetectorMode, spectrumDetectorFrames);
    spectrumScienceAnalyzer.setVbwHz(spectrumVbwHz);
    spectrumScienceAnalyzer.setPercentileTraces(spectrumPercentile50Enabled,
                                                spectrumPercentile90Enabled,
                                                spectrumPercentile99Enabled);
    spectrumScienceAnalyzer.update(frequencies, levels);
    if (researchAnalysisDialog &&
        (researchAnalysisDialog->isVisible() ||
         researchAnalysisDialog->hasActiveMeasurementSession())) {
        researchAnalysisDialog->appendSpectrumFrame(frequencies,
                                                    levels,
                                                    spectrumScienceAnalyzer.metrics());
    }
    if (zoomDensityDialog && zoomDensityDialog->isVisible()) {
        zoomDensityDialog->appendSpectrumFrame(frequencies,
                                               levels,
                                               spectrumAmplitudeUnit,
                                               pendingSettings.listeningFrequency);
    }
    const SpectrumSciencePendingAction pendingAction = spectrumSciencePendingAction;
    spectrumSciencePendingAction = SpectrumScienceActionNone;
    if (pendingAction == SpectrumScienceActionPeak) {
        spectrumScienceAnalyzer.setMarker(spectrumScienceActiveMarker,
                                          spectrumScienceAnalyzer.strongestPeakFrequency());
    } else if (pendingAction == SpectrumScienceActionPreviousPeak) {
        spectrumScienceAnalyzer.setMarker(
            spectrumScienceActiveMarker,
            spectrumScienceAnalyzer.adjacentPeakFrequency(spectrumScienceActiveMarker, -1));
    } else if (pendingAction == SpectrumScienceActionNextPeak) {
        spectrumScienceAnalyzer.setMarker(
            spectrumScienceActiveMarker,
            spectrumScienceAnalyzer.adjacentPeakFrequency(spectrumScienceActiveMarker, 1));
    }
    const bool exportPending = spectrumScienceExportPending;
    spectrumScienceExportPending = false;
    updateSpectrumScienceUi();
    if (exportPending) {
        QTimer::singleShot(0, this, [this]() { exportSpectrumScienceCsv(); });
    }
}

bool YourClassName::spectrumScienceAnalysisRequired() const {
    return spectrumScienceAnalyzer.hasMarkers() ||
           spectrumScienceMaxHoldEnabled ||
           spectrumScienceMinHoldEnabled ||
           spectrumScienceAverageEnabled ||
           spectrumPercentile50Enabled ||
           spectrumPercentile90Enabled ||
           spectrumPercentile99Enabled ||
           spectrumDetectorMode != SPECTRUM_DETECTOR_SAMPLE ||
           spectrumVbwHz > 0.0 ||
           spectrumAverageFrameCount > 0 ||
           spectrumSciencePendingAction != SpectrumScienceActionNone ||
           spectrumScienceExportPending ||
           (researchAnalysisDialog &&
            (researchAnalysisDialog->isVisible() ||
             researchAnalysisDialog->hasActiveMeasurementSession())) ||
           (zeroSpanDialog && zeroSpanDialog->isVisible()) ||
           (zoomDensityDialog && zoomDensityDialog->isVisible());
}

void YourClassName::openResearchAnalysis(int tabIndex) {
    if (!researchAnalysisDialog) {
        researchAnalysisDialog = new ResearchAnalysisDialog(
            [this](const QString &key, const QString &fallback) {
                return uiText(key, fallback);
            },
            [this]() {
                ResearchRadioContext context;
                context.sampleRateHz = pendingSettings.sampleRate;
                context.centerFrequencyHz = pendingSettings.centerFrequency;
                context.listeningFrequencyHz = pendingSettings.listeningFrequency;
                context.inputMode = pendingSettings.inputMode;
                context.modulationType = pendingSettings.modulationType;
                context.bandwidthHz = pendingSettings.bandwidth;
                context.fftLength = pendingSettings.fftLength;
                context.fftWindowType = pendingSettings.fftWindowType;
                return context;
            },
            [this]() {
                ResearchSpectrumSettings settings;
                settings.detectorMode = spectrumDetectorMode;
                settings.detectorFrames = spectrumDetectorFrames;
                settings.vbwHz = spectrumVbwHz;
                settings.fftOverlapPercent = spectrumFftOverlapPercent;
                settings.averageFrameCount = spectrumAverageFrameCount;
                settings.percentile50 = spectrumPercentile50Enabled;
                settings.percentile90 = spectrumPercentile90Enabled;
                settings.percentile99 = spectrumPercentile99Enabled;
                settings.amplitudeUnit = spectrumAmplitudeUnit;
                return settings;
            },
            [this](const ResearchSpectrumSettings &settings) {
                spectrumDetectorMode = normalizedSpectrumDetectorMode(settings.detectorMode);
                spectrumDetectorFrames = (std::clamp)(settings.detectorFrames, 1, 256);
                spectrumVbwHz = (std::clamp)(settings.vbwHz, 0.0, 10000.0);
                spectrumFftOverlapPercent = settings.fftOverlapPercent == 25 ||
                                                    settings.fftOverlapPercent == 50 ||
                                                    settings.fftOverlapPercent == 75
                                                ? settings.fftOverlapPercent
                                                : 0;
                spectrumAverageFrameCount = (std::clamp)(settings.averageFrameCount, 0, 10000);
                spectrumPercentile50Enabled = settings.percentile50;
                spectrumPercentile90Enabled = settings.percentile90;
                spectrumPercentile99Enabled = settings.percentile99;
                spectrumAmplitudeUnit = (std::clamp)(settings.amplitudeUnit, 0, 3);
                spectrumScienceAnalyzer.setDetector(spectrumDetectorMode, spectrumDetectorFrames);
                spectrumScienceAnalyzer.setVbwHz(spectrumVbwHz);
                spectrumScienceAnalyzer.setAverageFrameCount(spectrumAverageFrameCount);
                spectrumScienceAnalyzer.setPercentileTraces(spectrumPercentile50Enabled,
                                                            spectrumPercentile90Enabled,
                                                            spectrumPercentile99Enabled);
                savePersistentSettings();
                updateSpectrumScienceUi();
            },
            [this](bool enabled,
                   double carrierOffsetHz,
                   double phaseRadians,
                   double timingPhase,
                   double confidence) {
                pendingSettings.liveDigitalSyncEnabled = enabled;
                pendingSettings.liveDigitalSyncCarrierOffsetHz =
                    enabled && std::isfinite(carrierOffsetHz)
                        ? carrierOffsetHz
                        : 0.0;
                pendingSettings.liveDigitalSyncPhaseRadians =
                    enabled && std::isfinite(phaseRadians)
                        ? phaseRadians
                        : 0.0;
                pendingSettings.liveDigitalSyncTimingPhase =
                    enabled && std::isfinite(timingPhase)
                        ? (std::clamp)(timingPhase, 0.0, 1.0)
                        : 0.5;
                pendingSettings.liveDigitalSyncConfidence =
                    enabled && std::isfinite(confidence)
                        ? (std::clamp)(confidence, 0.0, 1.0)
                        : 0.0;
                if (audioProcessor) {
                    audioProcessor->configure(audioProcessorSettings());
                }
            },
            [this]() {
                if (!spectrumFrameRecorder.isRecording()) {
                    startSpectrumFrameRecording();
                }
            },
            [this](double frequencyHz) {
                if (listeningFrequencyControl && std::isfinite(frequencyHz)) {
                    listeningFrequencyControl->setValueHz(frequencyHz);
                }
            },
            this);
    }
    const int clampedTab = (std::clamp)(tabIndex,
                                        static_cast<int>(ResearchAnalysisDialog::InterferenceTab),
                                        static_cast<int>(ResearchAnalysisDialog::SessionTab));
    researchAnalysisDialog->selectTab(static_cast<ResearchAnalysisDialog::Tab>(clampedTab));
    if (!spectrumScienceAnalyzer.frequencies().empty() &&
        !spectrumScienceAnalyzer.levels().empty()) {
        researchAnalysisDialog->appendSpectrumFrame(spectrumScienceAnalyzer.frequencies(),
                                                    spectrumScienceAnalyzer.levels(),
                                                    spectrumScienceAnalyzer.metrics());
    }
}

void YourClassName::feedZeroSpanFrame(const std::vector<float> &frequencies,
                                      const std::vector<float> &levels,
                                      bool fftShiftedStorage) {
    if (zeroSpanDialog && zeroSpanDialog->isVisible()) {
        zeroSpanDialog->appendSpectrumFrame(frequencies,
                                            levels,
                                            pendingSettings.listeningFrequency,
                                            spectrumScienceAnalyzer.marker(0),
                                            spectrumScienceAnalyzer.marker(1),
                                            spectrumScienceAnalyzer.metrics(),
                                            fftShiftedStorage);
    }
}

void YourClassName::openZeroSpanDialog() {
    if (!zeroSpanDialog) {
        zeroSpanDialog = new ZeroSpanDialog(
            [this](const QString &key, const QString &fallback) {
                return uiText(key, fallback);
            },
            this);
    }
    zeroSpanDialog->show();
    zeroSpanDialog->raise();
    zeroSpanDialog->activateWindow();
}

void YourClassName::setSpectrumScienceMarker(double frequencyHz) {
    if (!std::isfinite(frequencyHz)) return;
    spectrumScienceAnalyzer.setMarker(spectrumScienceActiveMarker, frequencyHz);
    updateSpectrumScienceUi();
}

void YourClassName::updateSpectrumScienceUi() {
    if (spectrumScienceMarkerCombo && spectrumScienceMarkerCombo->count() != spectrumScienceAnalyzer.markerCount()) {
        const QSignalBlocker blocker(spectrumScienceMarkerCombo);
        spectrumScienceMarkerCombo->clear();
        for (int i = 0; i < spectrumScienceAnalyzer.markerCount(); ++i)
            spectrumScienceMarkerCombo->addItem(spectrumScienceAnalyzer.marker(i).label, i);
        spectrumScienceActiveMarker = (std::clamp)(spectrumScienceActiveMarker, 0, spectrumScienceAnalyzer.markerCount() - 1);
        spectrumScienceMarkerCombo->setCurrentIndex(spectrumScienceActiveMarker);
    }
    QVector<SpectrumScienceMarker> markers;
    markers.reserve(spectrumScienceAnalyzer.markerCount());
    for (int i = 0; i < spectrumScienceAnalyzer.markerCount(); ++i)
        markers.append(spectrumScienceAnalyzer.marker(i));
    const int activeMarkerIndex = std::clamp(spectrumScienceActiveMarker, 0, markers.size() - 1);
    const SpectrumScienceMarker activeMarker = markers.at(activeMarkerIndex);
    if (graphWidget) {
        graphWidget->setAnalogPeakMeterTarget(activeMarker.frequencyHz, activeMarker.enabled);
        graphWidget->setScienceAnalysisData(spectrumScienceAnalyzer.maxHoldTrace(),
                                            spectrumScienceAnalyzer.minHoldTrace(),
                                            spectrumScienceAnalyzer.averageTrace(),
                                            spectrumScienceAnalyzer.percentile50Trace(),
                                            spectrumScienceAnalyzer.percentile90Trace(),
                                            spectrumScienceAnalyzer.percentile99Trace(),
                                            spectrumScienceMaxHoldEnabled,
                                            spectrumScienceMinHoldEnabled,
                                            spectrumScienceAverageEnabled,
                                            spectrumPercentile50Enabled,
                                            spectrumPercentile90Enabled,
                                            spectrumPercentile99Enabled,
                                            markers);
    }
    if (dspFlowPanel) {
        dspFlowPanel->setAnalogPeakMeterTarget(activeMarker.frequencyHz, activeMarker.enabled);
    }
    if (waterfallWidget) {
        waterfallWidget->setScienceAnalysisData(spectrumScienceAnalyzer.maxHoldTrace(),
                                                spectrumScienceAnalyzer.minHoldTrace(),
                                                spectrumScienceAnalyzer.averageTrace(),
                                                spectrumScienceAnalyzer.percentile50Trace(),
                                                spectrumScienceAnalyzer.percentile90Trace(),
                                                spectrumScienceAnalyzer.percentile99Trace(),
                                                spectrumScienceMaxHoldEnabled,
                                                spectrumScienceMinHoldEnabled,
                                                spectrumScienceAverageEnabled,
                                                spectrumPercentile50Enabled,
                                                spectrumPercentile90Enabled,
                                                spectrumPercentile99Enabled,
                                                markers);
    }

    if (spectrumScienceMarkerStatusLabel) {
        auto markerText = [this](const SpectrumScienceMarker &marker) {
            return marker.enabled
                       ? QStringLiteral("%1 %2 / %3")
                             .arg(marker.label)
                             .arg(scienceFrequencyText(marker.frequencyHz))
                             .arg(scienceLevelText(marker.levelDb, spectrumAmplitudeUnit))
                       : QStringLiteral("%1 --").arg(marker.label);
        };
        QString status = markerText(markers.at(activeMarkerIndex));
        QStringList allMarkers;
        for (const auto &marker : markers) if (marker.enabled) allMarkers << markerText(marker);
        if (markers.at(0).enabled && markers.at(1).enabled) {
            status += QStringLiteral(" | dF %1 kHz | dL %2 dB")
                          .arg(std::abs(markers.at(1).frequencyHz - markers.at(0).frequencyHz) / 1.0e3, 0, 'f', 3)
                          .arg(markers.at(1).levelDb - markers.at(0).levelDb, 0, 'f', 1);
        }
        spectrumScienceMarkerStatusLabel->setText(status);
        spectrumScienceMarkerStatusLabel->setToolTip(allMarkers.join(QLatin1Char('\n')));
    }

    if (spectrumScienceMetricsLabel) {
        const SpectrumScienceMetrics &metrics = spectrumScienceAnalyzer.metrics();
        QString status = metrics.valid
            ? uiText(QStringLiteral("science_metrics_format_units"),
                     QStringLiteral("Peak %1 / %2 | Noise %3 | SNR %4 dB | Power %5 | OBW 90/95/99: %6/%7/%8 kHz"))
                  .arg(scienceFrequencyText(metrics.peakFrequencyHz))
                  .arg(scienceLevelText(metrics.peakDb, spectrumAmplitudeUnit))
                  .arg(scienceLevelText(metrics.noiseFloorDb, spectrumAmplitudeUnit))
                  .arg(metrics.snrDb, 0, 'f', 1)
                  .arg(scienceLevelText(metrics.channelPowerDb, spectrumAmplitudeUnit))
                  .arg(metrics.occupiedBandwidth90Hz / 1.0e3, 0, 'f', 3)
                  .arg(metrics.occupiedBandwidth95Hz / 1.0e3, 0, 'f', 3)
                  .arg(metrics.occupiedBandwidthHz / 1.0e3, 0, 'f', 3)
            : uiText(QStringLiteral("science_waiting"),
                     QStringLiteral("Scientific analysis: waiting for spectrum"));
        if (metrics.valid && calibrationTableEnabled) {
            const ReceiverCalibrationCorrection correction =
                receiverCalibrationTable.correctionAt(metrics.peakFrequencyHz);
            if (correction.valid && correction.uncertaintyDb > 0.0) {
                status += uiText(QStringLiteral("science_uncertainty_suffix"),
                                 QStringLiteral(" | uncertainty +/- %1 dB"))
                              .arg(correction.uncertaintyDb, 0, 'f', 2);
            }
        }
        spectrumScienceMetricsLabel->setText(status);
        spectrumScienceMetricsLabel->setToolTip(status);
    }
}

void YourClassName::exportSpectrumScienceCsv() {
    const auto &frequencies = spectrumScienceAnalyzer.frequencies();
    const auto &levels = spectrumScienceAnalyzer.levels();
    if (frequencies.empty() || levels.empty()) {
        QMessageBox::information(this,
                                 uiText(QStringLiteral("science_title"), QStringLiteral("Scientific spectrum analysis")),
                                 uiText(QStringLiteral("science_no_data"), QStringLiteral("No spectrum data to export.")));
        return;
    }
    const QString defaultPath = QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("spectrum_science_%1.csv").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    const QString path = QFileDialog::getSaveFileName(
        this,
        uiText(QStringLiteral("science_export_title"), QStringLiteral("Export scientific spectrum report")),
        defaultPath,
        uiText(QStringLiteral("csv_files_filter"), QStringLiteral("CSV files (*.csv)")));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this,
                             uiText(QStringLiteral("science_title"), QStringLiteral("Scientific spectrum analysis")),
                             uiText(QStringLiteral("scan_measurement_csv_write_failed"), QStringLiteral("Cannot write CSV file.")));
        return;
    }

    const SpectrumScienceMetrics &metrics = spectrumScienceAnalyzer.metrics();
    const SpectrumScienceMarker markerA = spectrumScienceAnalyzer.marker(0);
    const SpectrumScienceMarker markerB = spectrumScienceAnalyzer.marker(1);
    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << "# Obrii SDR scientific spectrum report\n";
    out << "# created," << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << '\n';
    out << "# center_hz," << QString::number(pendingSettings.centerFrequency, 'f', 3) << '\n';
    out << "# listening_hz," << QString::number(pendingSettings.listeningFrequency, 'f', 3) << '\n';
    out << "# sample_rate_hz," << QString::number(pendingSettings.sampleRate, 'f', 3) << '\n';
    out << "# fft_length," << pendingSettings.fftLength << '\n';
    out << "# detector_mode," << spectrumDetectorMode << '\n';
    out << "# detector_frames," << spectrumDetectorFrames << '\n';
    out << "# vbw_hz," << QString::number(spectrumVbwHz, 'f', 3) << '\n';
    out << "# fft_overlap_percent," << spectrumFftOverlapPercent << '\n';
    out << "# amplitude_unit," << (spectrumAmplitudeUnit == 1 ? "dBm" :
                                      spectrumAmplitudeUnit == 2 ? "dBuV_50ohm" :
                                      spectrumAmplitudeUnit == 3 ? "uV_50ohm" : "dBFS") << '\n';
    out << "# marker_a_hz," << (markerA.enabled ? QString::number(markerA.frequencyHz, 'f', 3) : QString()) << '\n';
    out << "# marker_b_hz," << (markerB.enabled ? QString::number(markerB.frequencyHz, 'f', 3) : QString()) << '\n';
    for (int i = 2; i < spectrumScienceAnalyzer.markerCount(); ++i) {
        const auto marker = spectrumScienceAnalyzer.marker(i);
        out << "# marker_" << marker.label << "_hz," << (marker.enabled ? QString::number(marker.frequencyHz, 'f', 3) : QString()) << '\n';
    }
    if (metrics.valid) {
        out << "# peak_hz," << QString::number(metrics.peakFrequencyHz, 'f', 3) << '\n';
        out << "# peak_db," << QString::number(metrics.peakDb, 'f', 3) << '\n';
        out << "# noise_floor_db," << QString::number(metrics.noiseFloorDb, 'f', 3) << '\n';
        out << "# snr_db," << QString::number(metrics.snrDb, 'f', 3) << '\n';
        out << "# channel_power_dbfs," << QString::number(metrics.channelPowerDb, 'f', 3) << '\n';
        out << "# occupied_bandwidth_99_hz," << QString::number(metrics.occupiedBandwidthHz, 'f', 3) << '\n';
        out << "# occupied_bandwidth_90_hz," << QString::number(metrics.occupiedBandwidth90Hz, 'f', 3) << '\n';
        out << "# occupied_bandwidth_95_hz," << QString::number(metrics.occupiedBandwidth95Hz, 'f', 3) << '\n';
        out << "# width_3db_hz," << QString::number(metrics.width3DbHz, 'f', 3) << '\n';
        out << "# width_6db_hz," << QString::number(metrics.width6DbHz, 'f', 3) << '\n';
        out << "# width_20db_hz," << QString::number(metrics.width20DbHz, 'f', 3) << '\n';
        out << "# acpr_lower_db," << QString::number(metrics.acprLowerDb, 'f', 3) << '\n';
        out << "# acpr_upper_db," << QString::number(metrics.acprUpperDb, 'f', 3) << '\n';
        out << "# centroid_hz," << QString::number(metrics.centroidFrequencyHz, 'f', 3) << '\n';
    }
    out << "frequency_hz,current_db,max_hold_db,min_hold_db,linear_average_db,p50_db,p90_db,p99_db\n";
    const auto &maxHold = spectrumScienceAnalyzer.maxHoldTrace();
    const auto &minHold = spectrumScienceAnalyzer.minHoldTrace();
    const auto &average = spectrumScienceAnalyzer.averageTrace();
    const auto &p50 = spectrumScienceAnalyzer.percentile50Trace();
    const auto &p90 = spectrumScienceAnalyzer.percentile90Trace();
    const auto &p99 = spectrumScienceAnalyzer.percentile99Trace();
    const std::size_t count = std::min(frequencies.size(), levels.size());
    for (std::size_t i = 0; i < count; ++i) {
        out << QString::number(frequencies[i], 'f', 3) << ','
            << QString::number(levels[i], 'f', 3) << ','
            << (i < maxHold.size() ? QString::number(maxHold[i], 'f', 3) : QString()) << ','
            << (i < minHold.size() ? QString::number(minHold[i], 'f', 3) : QString()) << ','
            << (i < average.size() ? QString::number(average[i], 'f', 3) : QString()) << ','
            << (i < p50.size() ? QString::number(p50[i], 'f', 3) : QString()) << ','
            << (i < p90.size() ? QString::number(p90[i], 'f', 3) : QString()) << ','
            << (i < p99.size() ? QString::number(p99[i], 'f', 3) : QString()) << '\n';
    }
}

void YourClassName::startSpurCalibration() {
    spurCalibrationBins.clear();
    spurCalibrationFramesDone = 0;
    spurCalibrationTargetFrames = SPUR_CALIBRATION_TARGET_FRAMES;
    spurCalibrationBinHz = 0.0;
    spurCombStepHz = 0.0;
    spurCombSpacingHits = 0;
    spurCalibrationActive = true;
    updateSpurSuppressionStatus();
    qDebug() << "[Spur] calibration started"
             << "targetFrames" << spurCalibrationTargetFrames
             << "sampleRate" << pendingSettings.sampleRate
             << "fftLength" << pendingSettings.fftLength
             << "inputMode" << pendingSettings.inputMode;
}

void YourClassName::clearSpurMask() {
    spurCalibrationActive = false;
    spurCalibrationBins.clear();
    spurMaskEntries.clear();
    spurCombStepHz = 0.0;
    spurCombSpacingHits = 0;
    spurSuppressionEnabled = false;
    if (spurSuppressionCheckbox) {
        QSignalBlocker blocker(spurSuppressionCheckbox);
        spurSuppressionCheckbox->setChecked(false);
    }
    updateSpurSuppressionStatus();
    savePersistentSettings();
}

void YourClassName::updateSpurCalibration(const std::vector<float> &frequencies,
                                          const std::vector<float> &magnitudes,
                                          double centerFrequency) {
    if (!spurCalibrationActive ||
        !std::isfinite(centerFrequency) ||
        frequencies.empty() ||
        magnitudes.empty()) {
        return;
    }

    const int dataCount = std::min(static_cast<int>(frequencies.size()),
                                   static_cast<int>(magnitudes.size()));
    if (dataCount <= SPUR_CALIBRATION_OUTER_BINS * 2 + 4 ||
        qFuzzyCompare(frequencies.front(), frequencies.back())) {
        return;
    }

    const double spanHz = std::abs(static_cast<double>(frequencies.back()) -
                                   static_cast<double>(frequencies.front()));
    const double binHz = (std::max)(25.0, spanHz / static_cast<double>((std::max)(1, dataCount)) * 3.0);
    if (spurCalibrationBinHz <= 0.0) {
        spurCalibrationBinHz = binHz;
    }

    std::vector<float> levels(static_cast<std::size_t>(dataCount), -160.0f);
    std::vector<double> prefixSum(static_cast<std::size_t>(dataCount + 1), 0.0);
    std::vector<int> prefixCount(static_cast<std::size_t>(dataCount + 1), 0);
    for (int i = 0; i < dataCount; ++i) {
        float level = magnitudes[static_cast<std::size_t>((i + dataCount / 2) % dataCount)];
        if (!std::isfinite(level)) {
            level = -160.0f;
        }
        levels[static_cast<std::size_t>(i)] = level;
        prefixSum[static_cast<std::size_t>(i + 1)] =
            prefixSum[static_cast<std::size_t>(i)] + static_cast<double>(level);
        prefixCount[static_cast<std::size_t>(i + 1)] =
            prefixCount[static_cast<std::size_t>(i)] + (std::isfinite(level) ? 1 : 0);
    }

    struct FrameCandidate {
        double offsetHz = 0.0;
        float level = -160.0f;
        float prominenceDb = 0.0f;
    };
    QMap<qint64, FrameCandidate> frameCandidates;

    auto rangeAverage = [&](int start, int end, int *countOut) {
        start = (std::clamp)(start, 0, dataCount);
        end = (std::clamp)(end, 0, dataCount);
        if (end <= start) {
            if (countOut) {
                *countOut = 0;
            }
            return -160.0;
        }
        const double sum = prefixSum[static_cast<std::size_t>(end)] -
                           prefixSum[static_cast<std::size_t>(start)];
        const int count = prefixCount[static_cast<std::size_t>(end)] -
                          prefixCount[static_cast<std::size_t>(start)];
        if (countOut) {
            *countOut = count;
        }
        return count > 0 ? sum / static_cast<double>(count) : -160.0;
    };

    for (int i = SPUR_CALIBRATION_OUTER_BINS;
         i < dataCount - SPUR_CALIBRATION_OUTER_BINS;
         ++i) {
        const float level = levels[static_cast<std::size_t>(i)];
        if (!std::isfinite(level) ||
            level < levels[static_cast<std::size_t>(i - 1)] ||
            level < levels[static_cast<std::size_t>(i + 1)]) {
            continue;
        }

        const float sideMax = (std::max)(levels[static_cast<std::size_t>(i - SPUR_CALIBRATION_INNER_BINS)],
                                         levels[static_cast<std::size_t>(i + SPUR_CALIBRATION_INNER_BINS)]);
        if (level - sideMax < SPUR_CALIBRATION_MIN_NARROW_DB) {
            continue;
        }

        int leftCount = 0;
        int rightCount = 0;
        const double leftAverage = rangeAverage(i - SPUR_CALIBRATION_OUTER_BINS,
                                                i - SPUR_CALIBRATION_INNER_BINS,
                                                &leftCount);
        const double rightAverage = rangeAverage(i + SPUR_CALIBRATION_INNER_BINS + 1,
                                                 i + SPUR_CALIBRATION_OUTER_BINS + 1,
                                                 &rightCount);
        if (leftCount + rightCount < 12) {
            continue;
        }
        const double baseline = (leftAverage * leftCount + rightAverage * rightCount) /
                                static_cast<double>(leftCount + rightCount);
        const float prominence = static_cast<float>(level - baseline);
        if (!std::isfinite(prominence) ||
            prominence < SPUR_CALIBRATION_MIN_PROMINENCE_DB) {
            continue;
        }

        const double offsetHz = static_cast<double>(frequencies[static_cast<std::size_t>(i)]) - centerFrequency;
        if (!std::isfinite(offsetHz)) {
            continue;
        }

        const qint64 key = static_cast<qint64>(std::llround(offsetHz / binHz));
        auto candidateIt = frameCandidates.find(key);
        if (candidateIt == frameCandidates.end() ||
            prominence > candidateIt.value().prominenceDb) {
            frameCandidates[key] = {offsetHz, level, prominence};
        }
    }

    for (auto it = frameCandidates.constBegin(); it != frameCandidates.constEnd(); ++it) {
        const FrameCandidate &candidate = it.value();
        const double weight = (std::max)(1.0, static_cast<double>(candidate.prominenceDb));
        SpurCalibrationBin &bin = spurCalibrationBins[it.key()];
        bin.offsetWeightedSum += candidate.offsetHz * weight;
        bin.weightSum += weight;
        bin.maxProminenceDb = (std::max)(bin.maxProminenceDb, candidate.prominenceDb);
        ++bin.hits;
    }

    ++spurCalibrationFramesDone;
    updateSpurSuppressionStatus();
    if (spurCalibrationFramesDone >= spurCalibrationTargetFrames) {
        finishSpurCalibration();
    }
}

void YourClassName::finishSpurCalibration() {
    spurCalibrationActive = false;

    QVector<SpurMaskEntry> candidates;
    const int minHits = (std::max)(4, spurCalibrationTargetFrames / 5);
    const double widthHz = (std::clamp)(spurCalibrationBinHz * 3.5,
                                        SPUR_MIN_MASK_WIDTH_HZ,
                                        SPUR_MAX_MASK_WIDTH_HZ);
    for (auto it = spurCalibrationBins.constBegin(); it != spurCalibrationBins.constEnd(); ++it) {
        const SpurCalibrationBin &bin = it.value();
        if (bin.hits < minHits || bin.weightSum <= 0.0) {
            continue;
        }
        SpurMaskEntry entry;
        entry.offsetHz = bin.offsetWeightedSum / bin.weightSum;
        entry.widthHz = widthHz;
        entry.prominenceDb = bin.maxProminenceDb;
        entry.hits = bin.hits;
        if (std::isfinite(entry.offsetHz) && std::isfinite(entry.widthHz)) {
            candidates.append(entry);
        }
    }

    spurCombStepHz = 0.0;
    spurCombSpacingHits = 0;
    if (candidates.size() >= 5) {
        constexpr double kMinCombStepHz = 1000.0;
        constexpr double kMaxCombStepHz = 2000000.0;
        constexpr double kCombStepBinHz = 250.0;
        QMap<qint64, int> spacingBins;
        QVector<double> offsets;
        offsets.reserve(candidates.size());
        for (const SpurMaskEntry &candidate : std::as_const(candidates)) {
            offsets.append(candidate.offsetHz);
        }
        std::sort(offsets.begin(), offsets.end());
        const int offsetCount = offsets.size();
        for (int i = 0; i < offsetCount; ++i) {
            for (int j = i + 1; j < offsetCount; ++j) {
                const double spacingHz = std::abs(offsets.at(j) - offsets.at(i));
                if (spacingHz < kMinCombStepHz) {
                    continue;
                }
                if (spacingHz > kMaxCombStepHz) {
                    break;
                }
                const qint64 bin = static_cast<qint64>(std::llround(spacingHz / kCombStepBinHz));
                ++spacingBins[bin];
            }
        }
        for (auto it = spacingBins.constBegin(); it != spacingBins.constEnd(); ++it) {
            if (it.value() > spurCombSpacingHits) {
                spurCombSpacingHits = it.value();
                spurCombStepHz = static_cast<double>(it.key()) * kCombStepBinHz;
            }
        }
        if (spurCombSpacingHits < 6) {
            spurCombStepHz = 0.0;
            spurCombSpacingHits = 0;
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const SpurMaskEntry &a, const SpurMaskEntry &b) {
        if (a.hits != b.hits) {
            return a.hits > b.hits;
        }
        return a.prominenceDb > b.prominenceDb;
    });

    QVector<SpurMaskEntry> merged;
    for (const SpurMaskEntry &candidate : std::as_const(candidates)) {
        bool mergedIntoExisting = false;
        for (SpurMaskEntry &existing : merged) {
            const double mergeDistance = (std::max)(existing.widthHz, candidate.widthHz);
            if (std::abs(existing.offsetHz - candidate.offsetHz) <= mergeDistance) {
                if (candidate.prominenceDb > existing.prominenceDb || candidate.hits > existing.hits) {
                    existing.offsetHz = candidate.offsetHz;
                    existing.prominenceDb = (std::max)(existing.prominenceDb, candidate.prominenceDb);
                    existing.hits = (std::max)(existing.hits, candidate.hits);
                    existing.widthHz = (std::max)(existing.widthHz, candidate.widthHz);
                }
                mergedIntoExisting = true;
                break;
            }
        }
        if (!mergedIntoExisting) {
            merged.append(candidate);
        }
        if (merged.size() >= SPUR_MAX_MASK_ENTRIES) {
            break;
        }
    }

    std::sort(merged.begin(), merged.end(), [](const SpurMaskEntry &a, const SpurMaskEntry &b) {
        return a.offsetHz < b.offsetHz;
    });

    spurMaskEntries = merged;
    spurCalibrationBins.clear();
    spurSuppressionEnabled = !spurMaskEntries.isEmpty();
    if (spurSuppressionCheckbox) {
        QSignalBlocker blocker(spurSuppressionCheckbox);
        spurSuppressionCheckbox->setChecked(spurSuppressionEnabled);
    }
    updateSpurSuppressionStatus();
    savePersistentSettings();

    QStringList offsets;
    for (const SpurMaskEntry &entry : std::as_const(spurMaskEntries)) {
        offsets << QStringLiteral("%1 kHz").arg(entry.offsetHz / 1000.0, 0, 'f', 1);
    }
    qDebug() << "[Spur] calibration finished"
             << "entries" << spurMaskEntries.size()
             << "combStepHz" << spurCombStepHz
             << "combHits" << spurCombSpacingHits
             << "offsets" << offsets.join(QStringLiteral(", "));
}

void YourClassName::applySpurSuppression(const std::vector<float> &frequencies,
                                         std::vector<float> &magnitudes,
                                         double centerFrequency) const {
    if (!spurSuppressionEnabled ||
        spurMaskEntries.isEmpty() ||
        !std::isfinite(centerFrequency) ||
        frequencies.empty() ||
        magnitudes.empty()) {
        return;
    }

    const int dataCount = std::min(static_cast<int>(frequencies.size()),
                                   static_cast<int>(magnitudes.size()));
    if (dataCount <= 8) {
        return;
    }

    for (const SpurMaskEntry &entry : spurMaskEntries) {
        if (!std::isfinite(entry.offsetHz) || !std::isfinite(entry.widthHz) || entry.widthHz <= 0.0) {
            continue;
        }
        const double targetFrequency = centerFrequency + entry.offsetHz;
        const double halfWidth = entry.widthHz * 0.5;
        const auto lower = std::lower_bound(frequencies.begin(),
                                            frequencies.begin() + dataCount,
                                            static_cast<float>(targetFrequency - halfWidth));
        const auto upper = std::upper_bound(frequencies.begin(),
                                            frequencies.begin() + dataCount,
                                            static_cast<float>(targetFrequency + halfWidth));
        int start = static_cast<int>(std::distance(frequencies.begin(), lower));
        int end = static_cast<int>(std::distance(frequencies.begin(), upper));
        start = (std::clamp)(start, 0, dataCount);
        end = (std::clamp)(end, 0, dataCount);
        if (end <= start) {
            continue;
        }

        const int guardBins = (std::max)(2, end - start);
        const int leftStart = (std::max)(0, start - guardBins * 3);
        const int leftEnd = (std::max)(leftStart, start - guardBins);
        const int rightStart = (std::min)(dataCount, end + guardBins);
        const int rightEnd = (std::min)(dataCount, end + guardBins * 3);

        double replacementSum = 0.0;
        int replacementCount = 0;
        auto accumulate = [&](int from, int to) {
            for (int i = from; i < to; ++i) {
                const int magnitudeIndex = (i + dataCount / 2) % dataCount;
                const float level = magnitudes[static_cast<std::size_t>(magnitudeIndex)];
                if (std::isfinite(level)) {
                    replacementSum += level;
                    ++replacementCount;
                }
            }
        };
        accumulate(leftStart, leftEnd);
        accumulate(rightStart, rightEnd);
        if (replacementCount <= 0) {
            continue;
        }

        const float replacement = static_cast<float>(replacementSum / replacementCount);
        for (int i = start; i < end; ++i) {
            const int magnitudeIndex = (i + dataCount / 2) % dataCount;
            float &level = magnitudes[static_cast<std::size_t>(magnitudeIndex)];
            if (!std::isfinite(level) || level > replacement) {
                level = replacement;
            }
        }
    }
}

void YourClassName::updateSpurSuppressionStatus() {
    if (!spurSuppressionStatusLabel) {
        return;
    }

    auto setSpurStatus = [this](const QString &text) {
        spurSuppressionStatusLabel->setToolTip(text);
        spurSuppressionStatusLabel->setText(text);
    };

    if (spurCalibrationActive) {
        setSpurStatus(
            uiText(QStringLiteral("spur_cal_status"),
                   QStringLiteral("Spur cal: %1/%2 frames, %3 candidates"))
                .arg(spurCalibrationFramesDone)
                .arg(spurCalibrationTargetFrames)
                .arg(spurCalibrationBins.size()));
        return;
    }

    if (spurMaskEntries.isEmpty()) {
        setSpurStatus(uiText(QStringLiteral("spur_mask_no_profile"),
                             QStringLiteral("Spur mask: no profile")));
        return;
    }

    QStringList offsets;
    for (int i = 0; i < spurMaskEntries.size() && i < 6; ++i) {
        offsets << QStringLiteral("%1k").arg(spurMaskEntries.at(i).offsetHz / 1000.0, 0, 'f', 1);
    }
    const QString suffix = spurMaskEntries.size() > 6 ? QStringLiteral(", ...") : QString();
    const QString combSuffix =
        spurCombStepHz > 0.0
            ? QStringLiteral(", comb %1k/h%2")
                  .arg(spurCombStepHz / 1000.0, 0, 'f', 2)
                  .arg(spurCombSpacingHits)
            : QString();
    setSpurStatus(
        uiText(QStringLiteral("spur_mask_status"),
               QStringLiteral("Spur mask: %1, %2 offsets [%3%4]"))
            .arg(spurSuppressionEnabled
                     ? uiText(QStringLiteral("on"), QStringLiteral("on"))
                     : uiText(QStringLiteral("off"), QStringLiteral("off")))
            .arg(spurMaskEntries.size())
            .arg(offsets.join(QStringLiteral(", ")))
            .arg(suffix) +
        combSuffix);
}

void YourClassName::updateGnssSpurWatch(const std::vector<float> &frequencies,
                                        const std::vector<float> &magnitudes,
                                        double centerFrequency) {
    if (!fobosVerboseLoggingEnabled() ||
        frequencies.empty() || magnitudes.empty() || frequencies.size() != magnitudes.size()) {
        return;
    }

    const GnssSystemPreset preset = gnssSystemPreset(gnssSystemId);
    const double targetHz = preset.id == QStringLiteral("all_l1")
                                ? GNSS_GPS_L1_HZ
                                : preset.targetHz;
    if (!std::isfinite(targetHz) || targetHz <= 0.0 ||
        !std::isfinite(centerFrequency) || centerFrequency <= 0.0) {
        return;
    }
    const double minHz = static_cast<double>(*std::min_element(frequencies.begin(), frequencies.end()));
    const double maxHz = static_cast<double>(*std::max_element(frequencies.begin(), frequencies.end()));
    if (targetHz < minHz || targetHz > maxHz) {
        return;
    }

    if (!gnssSpurWatchTimer.isValid()) {
        gnssSpurWatchTimer.start();
    }
    if (!gnssSpurLogTimer.isValid()) {
        gnssSpurLogTimer.start();
    }

    constexpr double watchSpanHz = 25000000.0;
    constexpr double binHz = 10000.0;
    QVector<QPair<int, float>> peaks;
    double average = 0.0;
    int averageCount = 0;
    for (int i = 0; i < static_cast<int>(frequencies.size()); ++i) {
        const double frequency = static_cast<double>(frequencies[static_cast<std::size_t>(i)]);
        const float level = magnitudes[static_cast<std::size_t>(i)];
        if (!std::isfinite(frequency) || !std::isfinite(level) ||
            std::abs(frequency - targetHz) > watchSpanHz) {
            continue;
        }
        average += static_cast<double>(level);
        ++averageCount;
    }
    if (averageCount <= 0) {
        return;
    }
    average /= static_cast<double>(averageCount);

    for (int i = 1; i + 1 < static_cast<int>(frequencies.size()); ++i) {
        const double frequency = static_cast<double>(frequencies[static_cast<std::size_t>(i)]);
        const float level = magnitudes[static_cast<std::size_t>(i)];
        if (!std::isfinite(frequency) || !std::isfinite(level) ||
            std::abs(frequency - targetHz) > watchSpanHz ||
            level < average + 8.0f) {
            continue;
        }
        const float prev = magnitudes[static_cast<std::size_t>(i - 1)];
        const float next = magnitudes[static_cast<std::size_t>(i + 1)];
        if (level < prev || level < next) {
            continue;
        }
        peaks.append(qMakePair(i, level));
    }
    std::sort(peaks.begin(), peaks.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });
    while (peaks.size() > 8) {
        peaks.removeLast();
    }

    const qint64 nowMs = gnssSpurWatchTimer.elapsed();
    for (const auto &peak : std::as_const(peaks)) {
        const double frequency = static_cast<double>(frequencies[static_cast<std::size_t>(peak.first)]);
        const qint64 bin = static_cast<qint64>(std::llround((frequency - targetHz) / binHz));
        GnssSpurWatchBin &entry = gnssSpurWatchBins[bin];
        entry.averageDb = entry.hits == 0
                              ? peak.second
                              : static_cast<float>(entry.averageDb * 0.85f + peak.second * 0.15f);
        entry.hits = (std::min)(9999, entry.hits + 1);
        entry.lastSeenMs = nowMs;
    }

    if (gnssSpurLogTimer.elapsed() < 3000) {
        return;
    }
    gnssSpurLogTimer.restart();

    QVector<QPair<qint64, GnssSpurWatchBin>> activeBins;
    for (auto it = gnssSpurWatchBins.begin(); it != gnssSpurWatchBins.end();) {
        if (nowMs - it.value().lastSeenMs > 15000) {
            it = gnssSpurWatchBins.erase(it);
            continue;
        }
        activeBins.append(qMakePair(it.key(), it.value()));
        ++it;
    }
    if (activeBins.isEmpty()) {
        return;
    }
    std::sort(activeBins.begin(), activeBins.end(), [](const auto &a, const auto &b) {
        if (a.second.hits != b.second.hits) {
            return a.second.hits > b.second.hits;
        }
        return a.second.averageDb > b.second.averageDb;
    });

    constexpr int kMaxLoggedSpurBins = 24;
    QStringList summary;
    const int loggedBins = (std::min)(kMaxLoggedSpurBins, activeBins.size());
    summary.reserve(loggedBins);
    for (int i = 0; i < loggedBins; ++i) {
        const auto &entry = activeBins.at(i);
        const double offsetHz = static_cast<double>(entry.first) * binHz;
        summary << QStringLiteral("%1k/%2dB/h%3")
                       .arg(offsetHz / 1000.0, 0, 'f', 1)
                       .arg(entry.second.averageDb, 0, 'f', 1)
                       .arg(entry.second.hits);
    }
    qDebug() << "[GNSS spur watch]"
             << "system" << preset.id
             << "targetHz" << targetHz
             << "centerHz" << centerFrequency
             << "averageDb" << average
             << "trackedBins" << activeBins.size()
             << "loggedBins" << loggedBins
             << "peaks" << summary.join(QStringLiteral(", "));
}
