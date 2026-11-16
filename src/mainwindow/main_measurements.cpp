#include "main.h"
#include "appconstants.h"
#include "diagnosticlogging.h"
#include "dspflowpanel.h"
#include "gnssqthhelpers.h"
#include "researchanalysisdialog.h"
#include "zerospandialog.h"
#include "zoomdensitydialog.h"
#include "analyzerframechannel.h"
#include "spectrummarkerpairsdialog.h"
#include "core/spurmaskresolution.h"
#include "core/spectrumordering.h"

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
    if (analyzerFramePublisher && analyzerFramePublisher->hasSubscribers()) {
        analyzerFramePublisher->publish(frequencies, levels, pendingSettings.fftLength);
    }
    if (!spectrumScienceLocalAnalysisRequired()) return;
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
    return (analyzerFramePublisher && analyzerFramePublisher->hasSubscribers()) ||
           spectrumScienceLocalAnalysisRequired();
}

bool YourClassName::spectrumScienceLocalAnalysisRequired() const {
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
        researchAnalysisDialog->setExperimentBundleCallbacks(
            [this]() { return experimentBundleContext(true); },
            [this](const QJsonObject &context, QString *error) {
                return applyExperimentWorkspace(context, error);
            });
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
    if (spectrumMarkerPairsDialog) spectrumMarkerPairsDialog->setMarkers(markers);
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
        const int firstPairMarker = spectrumScienceAnalyzer.measurementMarkerFirst();
        const int secondPairMarker = spectrumScienceAnalyzer.measurementMarkerSecond();
        if (markers.at(firstPairMarker).enabled && markers.at(secondPairMarker).enabled) {
            status += QStringLiteral(" | dF %1 kHz | dL %2 dB")
                          .arg(std::abs(markers.at(secondPairMarker).frequencyHz -
                                        markers.at(firstPairMarker).frequencyHz) / 1.0e3, 0, 'f', 3)
                          .arg(markers.at(secondPairMarker).levelDb -
                                   markers.at(firstPairMarker).levelDb,
                               0, 'f', 1);
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

void YourClassName::clearSpurMask() {
    if (spurSweepActive) return;
    spurCalibrationActive = false;
    spurCalibrationBins.clear();
    spurMaskEntries.clear();
    spurCalibrationProfile = {};
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
                                          double centerFrequency, uint64_t epoch, uint64_t endFloatCount) {
    if (!spurCalibrationActive || frequencies.size() != magnitudes.size() ||
        !std::isfinite(centerFrequency)) return;
    if (spurSweepActive && !spurFrameGate.accepts(epoch, endFloatCount, centerFrequency,
            spurSettlingClock.isValid() && spurSettlingClock.elapsed() >= 350)) return;
    if (!spurLineCalibrator.addFrame(magnitudes, pendingSettings.sampleRate)) return;
    if (spurSweepActive && spurSweepNoiseBaseline.isCapturing())
        spurSweepNoiseBaseline.addFrame(frequencies, spectrumInFrequencyOrder(magnitudes), receiverNoiseBaselineKey(pendingSettings));
    spurCalibrationFramesDone = spurLineCalibrator.frameCount();
    updateSpurSuppressionStatus();
    if (spurCalibrationFramesDone >= spurCalibrationTargetFrames) finishSpurCalibration();
}

void YourClassName::finishSpurCalibration() {
    spurCalibrationActive = false;
    const auto entries = spurLineCalibrator.finish(spurDetectionThresholdDb);
    spurLineCalibrator.reset();
    qInfo() << "[Spur] averaged narrow-line calibration" << "centerHz" << pendingSettings.centerFrequency
            << "frames" << spurCalibrationFramesDone << "lines" << entries.size();
    if (spurSweepActive) {
        spurSweepProfile.points.append({spurSweepCenters.at(spurSweepIndex), entries});
        QTimer::singleShot(0, this, [this] { if (spurSweepActive) advanceSpurCalibration(); });
        return;
    }
    spurMaskEntries = entries;
    spurSuppressionEnabled = !entries.isEmpty();
    if (spurSuppressionCheckbox) {
        const QSignalBlocker blocker(spurSuppressionCheckbox);
        spurSuppressionCheckbox->setChecked(spurSuppressionEnabled);
    }
    updateSpurSuppressionStatus();
    savePersistentSettings();
}

void YourClassName::applySpurSuppression(const std::vector<float> &frequencies,
                                         std::vector<float> &magnitudes,
                                         double centerFrequency) const {
    if (!spurSuppressionEnabled || pendingSettings.inputMode != INPUT_RF ||
        !std::isfinite(centerFrequency) || frequencies.size() != magnitudes.size()) return;
    auto mask = spurCalibrationProfile.points.isEmpty() ? spurMaskEntries :
        spurCalibrationProfile.maskFor(centerFrequency, spurCalibrationContext());
    mask.erase(std::remove_if(mask.begin(), mask.end(), [this](const auto &entry) {
        return entry.prominenceDb < spurDetectionThresholdDb;
    }), mask.end());
    adaptSpurMaskToResolution(mask, pendingSettings.sampleRate, static_cast<int>(magnitudes.size()), pendingSettings.fftWindowType);
    applyCalibratedSpurLines(magnitudes, mask, pendingSettings.sampleRate);
}

void YourClassName::updateSpurSuppressionStatus() {
    if (!spurSuppressionStatusLabel) {
        return;
    }

    auto setSpurStatus = [this](const QString &text) {
        spurSuppressionStatusLabel->setToolTip(text);
        spurSuppressionStatusLabel->setText(text);
    };

    if (!spurCalibrationProfile.points.isEmpty() && !spurSweepActive) {
        const auto *mask = spurCalibrationProfile.maskAt(pendingSettings.centerFrequency, spurCalibrationContext());
        const bool uk = uiLanguage == QStringLiteral("uk");
        if (spurSuppressionEnabled && pendingSettings.sampleRate * 3 / (std::max)(1, pendingSettings.fftLength) > 20000) {
            setSpurStatus(uk ? QStringLiteral("Профіль збережено. Збільшіть FFT: поточна роздільність не відокремлює вузькі спури.")
                : QStringLiteral("Profile retained. Increase FFT size: current resolution cannot separate narrow spurs."));
            return;
        }
        setSpurStatus((uk ? QStringLiteral("Профіль спурів: %1 точок, %2–%3 МГц. %4")
                         : QStringLiteral("Spur profile: %1 points, %2–%3 MHz. %4"))
            .arg(spurCalibrationProfile.points.size())
            .arg(spurCalibrationProfile.points.front().centerHz / 1e6, 0, 'f', 1)
            .arg(spurCalibrationProfile.points.back().centerHz / 1e6, 0, 'f', 1)
            .arg(!spurSuppressionEnabled ? (uk ? QStringLiteral("Вимкнено") : QStringLiteral("Disabled")) :
                mask ? (uk ? QStringLiteral("Застосовується: %1 вузьких ліній").arg(std::count_if(mask->cbegin(), mask->cend(), [this](const auto &e) { return e.prominenceDb >= spurDetectionThresholdDb; }))
                           : QStringLiteral("Applied: %1 narrow lines").arg(std::count_if(mask->cbegin(), mask->cend(), [this](const auto &e) { return e.prominenceDb >= spurDetectionThresholdDb; }))) :
                (uk ? QStringLiteral("Інші Fs/ПЧ/приймач або апаратний режим — потрібна калібровка")
                    : QStringLiteral("Different Fs/IF/receiver or hardware mode — calibration required"))));
        return;
    }

    if (spurCalibrationActive) {
        setSpurStatus(
            uiText(QStringLiteral("spur_cal_status"),
                   QStringLiteral("Spur calibration: %1/%2 frames, %3 candidates"))
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
