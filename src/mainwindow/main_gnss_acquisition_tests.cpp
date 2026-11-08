#include "main.h"

#include "appconstants.h"
#include "diagnosticlogging.h"
#include "gnssqthhelpers.h"
#include "qthmapwidget.h"
#include "samplefileutils.h"
#include "tuningutils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace {

bool isGnssAcquisitionImplemented(GnssAcquisitionKind kind) {
    return kind == GnssAcquisitionKind::GpsL1Ca ||
           kind == GnssAcquisitionKind::GlonassL1Of ||
           kind == GnssAcquisitionKind::GpsGlonassL1;
}

GnssAcquisitionResult acquireForPreset(const GnssSystemPreset &preset,
                                       const std::vector<float> &snapshot,
                                       double sampleRate,
                                       double centerFrequency,
                                       double targetFrequency,
                                       int integrationMs,
                                       double channelFilterCutoffHz,
                                       const std::atomic_bool *cancelFlag,
                                       int dopplerMinHz,
                                       int dopplerMaxHz,
                                       int dopplerStepHz) {
    switch (preset.acquisitionKind) {
    case GnssAcquisitionKind::GpsL1Ca:
        return GnssAcquisition::acquireGpsL1Ca(snapshot,
                                              sampleRate,
                                              centerFrequency,
                                              targetFrequency,
                                              integrationMs,
                                              channelFilterCutoffHz,
                                              cancelFlag,
                                              dopplerMinHz,
                                              dopplerMaxHz,
                                              dopplerStepHz);
    case GnssAcquisitionKind::GlonassL1Of:
        return GnssAcquisition::acquireGlonassL1Of(snapshot,
                                                  sampleRate,
                                                  centerFrequency,
                                                  targetFrequency,
                                                  integrationMs,
                                                  channelFilterCutoffHz,
                                                  cancelFlag,
                                                  dopplerMinHz,
                                                  dopplerMaxHz,
                                                  dopplerStepHz);
    case GnssAcquisitionKind::GpsGlonassL1:
        return GnssAcquisition::acquireGpsGlonassL1(snapshot,
                                                   sampleRate,
                                                   centerFrequency,
                                                   targetFrequency,
                                                   integrationMs,
                                                   channelFilterCutoffHz,
                                                   cancelFlag,
                                                   dopplerMinHz,
                                                   dopplerMaxHz,
                                                   dopplerStepHz);
    case GnssAcquisitionKind::None:
        break;
    }

    GnssAcquisitionResult result;
    result.status = QStringLiteral("%1 acquisition parser is not implemented yet.")
                        .arg(preset.fallbackName);
    return result;
}

double candidateMetricDb(const GnssAcquisitionCandidate &candidate) {
    return 10.0 * std::log10((std::max)(candidate.metric,
                                        std::numeric_limits<double>::min()));
}

double candidatePeakToSecondDb(const GnssAcquisitionCandidate &candidate) {
    return 10.0 * std::log10((std::max)(candidate.peakToSecond,
                                        std::numeric_limits<double>::min()));
}

} // namespace

void YourClassName::runGnssAcquisitionTest() {
    if (gnssAcquisitionRunning) {
        return;
    }

    updateGnssSystemSelection();
    if (gnssIntegrationSpin) {
        gnssAcquisitionIntegrationMs =
            (std::clamp)(gnssIntegrationSpin->value(),
                         GNSS_ACQUISITION_MIN_INTEGRATION_MS,
                         GNSS_ACQUISITION_MAX_INTEGRATION_MS);
    }
    if (gnssChannelFilterSpin) {
        const double cutoffHz = gnssChannelFilterSpin->value() * 1000000.0;
        gnssChannelFilterCutoffHz =
            (std::clamp)(std::isfinite(cutoffHz) ? cutoffHz : 1800000.0,
                         GNSS_CHANNEL_FILTER_MIN_HZ,
                         GNSS_CHANNEL_FILTER_MAX_HZ);
    }
    if (gnssDopplerSpanSpin) {
        gnssDopplerSpanHz = (std::clamp)(gnssDopplerSpanSpin->value(), 1, 50) * 1000;
    }
    if (gnssDopplerStepSpin) {
        gnssDopplerStepHz = (std::clamp)(gnssDopplerStepSpin->value(), 250, 5000);
    }
    const GnssSystemPreset selectedPreset = gnssSystemPreset(gnssSystemId);
    const GnssSystemPreset acquisitionPreset = selectedPreset;
    const QString selectedName = uiText(selectedPreset.textKey, selectedPreset.fallbackName);
    const QString acquisitionName = uiText(acquisitionPreset.textKey, acquisitionPreset.fallbackName);
    if (!isGnssAcquisitionImplemented(acquisitionPreset.acquisitionKind)) {
        const QString status =
            uiText(QStringLiteral("gnss_acq_not_implemented"),
                   QStringLiteral("%1 acquisition parser is not implemented yet. RF tune/scan, IQ monitor and IQ saving are ready."))
                .arg(selectedName);
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(status);
        }
        GnssAcquisitionResult skippedResult;
        skippedResult.status = status;
        skippedResult.centerFrequency = pendingSettings.centerFrequency;
        skippedResult.targetFrequency = acquisitionPreset.targetHz;
        skippedResult.inputSampleRate = pendingSettings.sampleRate;
        updateGnssAcquisitionPlot(skippedResult);
        scheduleGnssContinuousAcquisition();
        qDebug() << "[GNSS acquisition] skipped"
                 << "system" << selectedPreset.id
                 << "name" << selectedName
                 << "reason" << "parser-not-implemented";
        return;
    }

    const double measuredSampleRate = IqBuffer::sampleRateEstimate();
    const double configuredSampleRate = pendingSettings.sampleRate;
    const bool haveConfiguredSampleRate =
        std::isfinite(configuredSampleRate) && configuredSampleRate > 0.0;
    const bool haveMeasuredSampleRate =
        std::isfinite(measuredSampleRate) && measuredSampleRate > 0.0;
    double sampleRate = haveConfiguredSampleRate ? configuredSampleRate : measuredSampleRate;
    const QString sampleRateSource =
        haveConfiguredSampleRate ? QStringLiteral("configured") : QStringLiteral("measured");
    const double centerFrequency = pendingSettings.centerFrequency > 0.0
                                       ? pendingSettings.centerFrequency
                                       : pendingSettings.actualFrequency;
    const double targetFrequency = acquisitionPreset.targetHz;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 ||
        !std::isfinite(centerFrequency) || centerFrequency <= 0.0 ||
        !std::isfinite(targetFrequency) || targetFrequency <= 0.0) {
        const QString status = uiText(QStringLiteral("gps_ca_scan_no_iq"),
                                      QStringLiteral("GPS C/A accumulation: no IQ snapshot yet"));
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(status);
        }
        GnssAcquisitionResult skippedResult;
        skippedResult.status = status;
        skippedResult.inputSampleRate = sampleRate;
        skippedResult.centerFrequency = centerFrequency;
        skippedResult.targetFrequency = targetFrequency;
        updateGnssAcquisitionPlot(skippedResult);
        scheduleGnssContinuousAcquisition();
        qDebug() << "[GNSS acquisition] skipped"
                 << "system" << selectedPreset.id
                 << "acquisitionSystem" << acquisitionPreset.id
                 << "sampleRate" << sampleRate
                 << "centerHz" << centerFrequency
                 << "targetHz" << targetFrequency
                 << "reason" << "invalid-iq-context";
        return;
    }
    const double targetOffsetHz = targetFrequency - centerFrequency;
    const bool targetSpanCheckRequired =
        acquisitionPreset.acquisitionKind != GnssAcquisitionKind::GpsGlonassL1;
    if (targetSpanCheckRequired && std::abs(targetOffsetHz) > sampleRate * 0.48) {
        const QString status =
            uiText(QStringLiteral("gnss_acq_target_out_of_span"),
                   QStringLiteral("%1 target %2 MHz is outside the current IQ span. Tune this system or wait for its scan center."))
                .arg(acquisitionName,
                     QString::number(targetFrequency / 1000000.0, 'f', 6));
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(status);
        }
        GnssAcquisitionResult skippedResult;
        skippedResult.status = status;
        skippedResult.inputSampleRate = sampleRate;
        skippedResult.centerFrequency = centerFrequency;
        skippedResult.targetFrequency = targetFrequency;
        skippedResult.frequencyOffset = targetOffsetHz;
        updateGnssAcquisitionPlot(skippedResult);
        scheduleGnssContinuousAcquisition();
        qDebug() << "[GNSS acquisition] skipped"
                 << "system" << selectedPreset.id
                 << "acquisitionSystem" << acquisitionPreset.id
                 << "targetHz" << targetFrequency
                 << "centerHz" << centerFrequency
                 << "sampleRate" << sampleRate
                 << "sampleRateSource" << sampleRateSource
                 << "configuredSampleRate" << configuredSampleRate
                 << "measuredSampleRate" << measuredSampleRate
                 << "reason" << "target-out-of-span";
        return;
    }

    int requestedIntegrationMs =
        (std::clamp)(gnssAcquisitionIntegrationMs,
                     GNSS_ACQUISITION_MIN_INTEGRATION_MS,
                     GNSS_ACQUISITION_MAX_INTEGRATION_MS);
    const bool allowGpsFocusedContinuous =
        acquisitionPreset.acquisitionKind == GnssAcquisitionKind::GpsL1Ca &&
        requestedIntegrationMs >= 80;
    if (gnssContinuousAcquisitionTickActive &&
        requestedIntegrationMs > GNSS_CONTINUOUS_ACQUISITION_MAX_INTEGRATION_MS &&
        !allowGpsFocusedContinuous) {
        qDebug() << "[GNSS acquisition] continuous integration capped"
                 << "configuredMs" << requestedIntegrationMs
                 << "autoMs" << GNSS_CONTINUOUS_ACQUISITION_MAX_INTEGRATION_MS;
        requestedIntegrationMs = GNSS_CONTINUOUS_ACQUISITION_MAX_INTEGRATION_MS;
    } else if (gnssContinuousAcquisitionTickActive && allowGpsFocusedContinuous) {
        qDebug() << "[GNSS acquisition] continuous GPS focused integration enabled"
                 << "integrationMs" << requestedIntegrationMs;
    }
    const QString acquisitionSource =
        gnssContinuousAcquisitionTickActive
            ? (requestedIntegrationMs >= GNSS_DEEP_ACQUISITION_MS
                   ? QStringLiteral("continuous-deep-live")
                   : QStringLiteral("continuous-live"))
            : (requestedIntegrationMs >= GNSS_DEEP_ACQUISITION_MS
                   ? QStringLiteral("deep-live")
                   : QStringLiteral("live"));
    gnssAcquisitionSource = acquisitionSource;
    const double requestedIqSamplesDouble =
        std::ceil((sampleRate * static_cast<double>(requestedIntegrationMs)) / 1000.0) +
        8192.0;
    const std::size_t requestedSnapshotFloats =
        static_cast<std::size_t>((std::max)(8192.0, requestedIqSamplesDouble)) * 2U;
    std::vector<float> snapshot;
    std::uint64_t sequence = 0;
    if (!IqBuffer::snapshotRecent(snapshot, requestedSnapshotFloats, &sequence) ||
        snapshot.size() < 2 * 4092) {
        const QString status = uiText(QStringLiteral("gps_ca_scan_no_iq"),
                                      QStringLiteral("GPS C/A accumulation: no IQ snapshot yet"));
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(status);
        }
        GnssAcquisitionResult skippedResult;
        skippedResult.status = status;
        skippedResult.inputSampleRate = sampleRate;
        skippedResult.centerFrequency = centerFrequency;
        skippedResult.targetFrequency = targetFrequency;
        skippedResult.frequencyOffset = targetOffsetHz;
        updateGnssAcquisitionPlot(skippedResult);
        scheduleGnssContinuousAcquisition();
        qDebug() << "[GNSS acquisition] skipped"
                 << "system" << selectedPreset.id
                 << "acquisitionSystem" << acquisitionPreset.id
                 << "sequence" << sequence
                 << "requestedSnapshotFloats" << static_cast<qulonglong>(requestedSnapshotFloats)
                 << "actualSnapshotFloats" << static_cast<qulonglong>(snapshot.size())
                 << "sampleRate" << sampleRate
                 << "requestedIntegrationMs" << requestedIntegrationMs
                 << "reason" << "no-iq-snapshot";
        return;
    }
    if (gnssContinuousAcquisitionTickActive &&
        sequence != 0 &&
        sequence == gnssLastContinuousAcquisitionSequence) {
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gps_ca_scan_no_iq"),
                                                  QStringLiteral("GPS C/A accumulation: no IQ snapshot yet")));
        }
        qDebug() << "[GNSS acquisition] skipped"
                 << "system" << selectedPreset.id
                 << "acquisitionSystem" << acquisitionPreset.id
                 << "sequence" << static_cast<qulonglong>(sequence)
                 << "requestedSnapshotFloats" << static_cast<qulonglong>(requestedSnapshotFloats)
                 << "actualSnapshotFloats" << static_cast<qulonglong>(snapshot.size())
                 << "reason" << "duplicate-continuous-sequence";
        scheduleGnssContinuousAcquisition();
        return;
    }
    if (gnssContinuousAcquisitionTickActive) {
        gnssLastContinuousAcquisitionSequence = sequence;
    }
    const double snapshotMs =
        (static_cast<double>(snapshot.size() / 2) / sampleRate) * 1000.0;

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    gnssAcquisitionCancelFlag = cancelFlag;
    gnssAcquisitionRunning = true;
    if (gnssAcquireButton) {
        gnssAcquireButton->setEnabled(false);
    }
    if (gnssOfflineAcquireButton) {
        gnssOfflineAcquireButton->setEnabled(false);
    }
    if (gnssSelfTestButton) {
        gnssSelfTestButton->setEnabled(false);
    }
    if (gnssAcquireStatusLabel) {
        gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gps_ca_scan_running"),
                                              QStringLiteral("GPS C/A accumulation: running")));
    }

    const int focusedPrn = gnssGpsFocusedPrn;
    const double focusedDopplerHz = gnssGpsFocusedDopplerHz;
    const int focusedStableCount = gnssGpsFocusedStableCount;
    const bool continuousAcquisition = gnssContinuousAcquisitionTickActive;
    int workerIntegrationMs = requestedIntegrationMs;
    if (continuousAcquisition &&
        focusedPrn >= 1 &&
        focusedPrn <= 32 &&
        focusedStableCount >= 3) {
        workerIntegrationMs = (std::min)(workerIntegrationMs, 80);
        if (workerIntegrationMs != requestedIntegrationMs) {
            qDebug() << "[GNSS acquisition] continuous focused integration reduced"
                     << "requestedMs" << requestedIntegrationMs
                     << "workerMs" << workerIntegrationMs
                     << "focusedPrn" << focusedPrn
                     << "focusedStableCount" << focusedStableCount;
        }
    }

    qDebug() << "[GNSS acquisition] start"
             << "system" << selectedPreset.id
             << "systemName" << selectedName
             << "acquisitionSystem" << acquisitionPreset.id
             << "acquisitionName" << acquisitionName
             << "sequence" << sequence
             << "inputSamples" << static_cast<int>(snapshot.size() / 2)
             << "snapshotMs" << snapshotMs
             << "requestedSnapshotFloats" << static_cast<qulonglong>(requestedSnapshotFloats)
             << "sampleRate" << sampleRate
             << "sampleRateSource" << sampleRateSource
             << "configuredSampleRate" << configuredSampleRate
             << "measuredSampleRate" << measuredSampleRate
             << "centerHz" << centerFrequency
             << "targetHz" << targetFrequency
             << "requestedIntegrationMs" << workerIntegrationMs
             << "uiIntegrationMs" << requestedIntegrationMs
             << "channelFilterCutoffHz" << gnssChannelFilterCutoffHz
             << "dopplerMinHz" << -gnssDopplerSpanHz
             << "dopplerMaxHz" << gnssDopplerSpanHz
             << "dopplerStepHz" << gnssDopplerStepHz
             << "focusedPrn" << focusedPrn
             << "focusedDopplerHz" << focusedDopplerHz
             << "focusedStableCount" << focusedStableCount
             << "continuous" << continuousAcquisition;

    QPointer<YourClassName> self(this);
    QtConcurrent::run([self,
                       snapshot = std::move(snapshot),
                       sampleRate,
                       centerFrequency,
                       targetFrequency,
                       integrationMs = workerIntegrationMs,
                       channelFilterCutoffHz = gnssChannelFilterCutoffHz,
                       dopplerSpanHz = gnssDopplerSpanHz,
                       dopplerStepHz = gnssDopplerStepHz,
                       focusedPrn,
                       focusedDopplerHz,
                       focusedStableCount,
                       continuousAcquisition,
                       sequence,
                       snapshotMs,
                       acquisitionPreset,
                       acquisitionSource,
                       cancelFlag]() mutable {
        QElapsedTimer acquisitionTimer;
        acquisitionTimer.start();
        GnssAcquisitionResult result;
        if (acquisitionPreset.acquisitionKind == GnssAcquisitionKind::GpsL1Ca &&
            integrationMs >= 80) {
            if (focusedPrn >= 1 && focusedPrn <= 32) {
                const int focusedSpanHz = focusedStableCount >= 6 ? 250
                                       : focusedStableCount >= 3 ? 500
                                                                 : 1500;
                const int focusedStepHz = 250;
                qDebug() << "[GNSS acquisition] focused PRN retry"
                         << "system" << acquisitionPreset.id
                         << "prn" << focusedPrn
                         << "dopplerCenterHz" << focusedDopplerHz
                         << "stableCount" << focusedStableCount
                         << "dopplerSpanHz" << focusedSpanHz
                         << "dopplerStepHz" << focusedStepHz
                         << "focusedIntegrationMs" << integrationMs;
                result = GnssAcquisition::acquireGpsL1CaFocused(snapshot,
                                                                sampleRate,
                                                                centerFrequency,
                                                                targetFrequency,
                                                                focusedPrn,
                                                                integrationMs,
                                                                channelFilterCutoffHz,
                                                                cancelFlag.get(),
                                                                static_cast<int>(std::lround(focusedDopplerHz)),
                                                                focusedSpanHz,
                                                                focusedStepHz);
            }

            const int coarseMs = continuousAcquisition ? 4 : 20;
            if (!result.valid && !cancelFlag->load(std::memory_order_relaxed)) {
                GnssAcquisitionResult coarse =
                    GnssAcquisition::acquireGpsL1Ca(snapshot,
                                                   sampleRate,
                                                   centerFrequency,
                                                   targetFrequency,
                                                   coarseMs,
                                                   channelFilterCutoffHz,
                                                   cancelFlag.get(),
                                                   -dopplerSpanHz,
                                                   dopplerSpanHz,
                                                   dopplerStepHz);
            if (!coarse.topCandidates.isEmpty()) {
                const GnssAcquisitionCandidate best = coarse.topCandidates.first();
                const double coarseMetricDb = candidateMetricDb(best);
                const double coarsePeak2Db = candidatePeakToSecondDb(best);
                const double focusedFollowupCoarseMetricDb = continuousAcquisition ? 7.5 : 7.0;
                const double focusedFollowupCoarsePeak2Db = continuousAcquisition ? 2.2 : 3.0;
                const bool strongCoarseCandidate =
                    coarsePeak2Db >= (continuousAcquisition ? 3.2 : 3.5);
                const int focusedFollowupMs =
                    continuousAcquisition && !strongCoarseCandidate
                        ? (std::min)(integrationMs, 40)
                        : integrationMs;
                if (coarseMetricDb >= focusedFollowupCoarseMetricDb &&
                    coarsePeak2Db >= focusedFollowupCoarsePeak2Db &&
                    !cancelFlag->load(std::memory_order_relaxed)) {
                    qDebug() << "[GNSS acquisition] focused follow-up"
                             << "system" << acquisitionPreset.id
                             << "coarsePrn" << best.prn
                             << "coarseLabel" << best.label
                             << "coarseDopplerHz" << best.dopplerHz
                             << "coarseMs" << coarseMs
                             << "coarseMetricDb" << coarseMetricDb
                             << "coarsePeakToSecondDb" << coarsePeak2Db
                             << "metricThresholdDb" << focusedFollowupCoarseMetricDb
                             << "peakToSecondThresholdDb" << focusedFollowupCoarsePeak2Db
                             << "strongCoarseCandidate" << strongCoarseCandidate
                             << "focusedIntegrationMs" << focusedFollowupMs;
                    result = GnssAcquisition::acquireGpsL1CaFocused(snapshot,
                                                                    sampleRate,
                                                                    centerFrequency,
                                                                    targetFrequency,
                                                                    best.prn,
                                                                    focusedFollowupMs,
                                                                    channelFilterCutoffHz,
                                                                    cancelFlag.get(),
                                                                    static_cast<int>(std::lround(best.dopplerHz)),
                                                                    1000,
                                                                    250);
                } else {
                    qDebug() << "[GNSS acquisition] focused follow-up skipped"
                             << "system" << acquisitionPreset.id
                             << "coarsePrn" << best.prn
                             << "coarseLabel" << best.label
                             << "coarseDopplerHz" << best.dopplerHz
                             << "coarseMs" << coarseMs
                             << "coarseMetricDb" << coarseMetricDb
                             << "coarsePeakToSecondDb" << coarsePeak2Db
                             << "metricThresholdDb" << focusedFollowupCoarseMetricDb
                             << "peakToSecondThresholdDb" << focusedFollowupCoarsePeak2Db;
                    result = coarse;
                }
            } else {
                result = coarse;
            }
            }
        } else {
            result =
                acquireForPreset(acquisitionPreset,
                                 snapshot,
                                 sampleRate,
                                 centerFrequency,
                                 targetFrequency,
                                 integrationMs,
                                 channelFilterCutoffHz,
                                 cancelFlag.get(),
                                 -dopplerSpanHz,
                                 dopplerSpanHz,
                                 dopplerStepHz);
        }
        result.inputSequence = sequence;
        result.snapshotMs = snapshotMs;
        result.processingElapsedMs = acquisitionTimer.elapsed();
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, result, acquisitionSource, cancelFlag]() {
            if (!self) {
                return;
            }
            self->gnssAcquisitionRunning = false;
            if (self->gnssAcquisitionCancelFlag == cancelFlag) {
                self->gnssAcquisitionCancelFlag.reset();
            }
            if (self->gnssAcquireButton) {
                self->gnssAcquireButton->setEnabled(true);
            }
            if (self->gnssOfflineAcquireButton) {
                self->gnssOfflineAcquireButton->setEnabled(true);
            }
            if (self->gnssSelfTestButton) {
                self->gnssSelfTestButton->setEnabled(true);
            }
            self->gnssAcquisitionSource = acquisitionSource;
            self->updateGnssAcquisitionStatus(result);
        }, Qt::QueuedConnection);
    });
}

void YourClassName::runGnssDeepAcquisitionTest() {
    if (gnssAcquisitionRunning) {
        return;
    }
    gnssAcquisitionIntegrationMs = GNSS_DEEP_ACQUISITION_MS;
    if (gnssIntegrationSpin) {
        QSignalBlocker blocker(gnssIntegrationSpin);
        gnssIntegrationSpin->setValue(gnssAcquisitionIntegrationMs);
    }
    qDebug() << "[GNSS acquisition] deep requested"
             << "integrationMs" << gnssAcquisitionIntegrationMs;
    savePersistentSettings();
    runGnssAcquisitionTest();
}

void YourClassName::setGnssContinuousAcquisitionEnabled(bool enabled) {
    if (gnssContinuousAcquisitionEnabled == enabled &&
        (!gnssDeepAcquireButton || gnssDeepAcquireButton->isChecked() == enabled)) {
        if (enabled && gnssContinuousAcquireTimer && !gnssContinuousAcquireTimer->isActive()) {
            scheduleGnssContinuousAcquisition(GNSS_CONTINUOUS_ACQUISITION_FIRST_DELAY_MS);
            qDebug() << "[GNSS acquisition] continuous re-armed"
                     << "intervalMs" << gnssContinuousAcquisitionIntervalMs;
        }
        return;
    }

    if (!enabled) {
        stopGnssSdrAcquisition(QStringLiteral("ui-toggle"), true);
        return;
    }

    gnssContinuousAcquisitionEnabled = enabled;
    if (gnssDeepAcquireButton && gnssDeepAcquireButton->isChecked() != enabled) {
        QSignalBlocker blocker(gnssDeepAcquireButton);
        gnssDeepAcquireButton->setChecked(enabled);
    }

    if (gnssAcquireStatusLabel) {
        gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gnss_continuous_acq_enabled"),
                                              QStringLiteral("GNSS continuous acquisition: waiting for IQ")));
    }
    scheduleGnssContinuousAcquisition(GNSS_CONTINUOUS_ACQUISITION_FIRST_DELAY_MS);
    qDebug() << "[GNSS acquisition] continuous enabled"
             << "intervalMs" << gnssContinuousAcquisitionIntervalMs;
    savePersistentSettings();
}

void YourClassName::stopGnssSdrAcquisition(const QString &reason, bool persistSettings) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this,
                                  [this, reason, persistSettings]() {
                                      stopGnssSdrAcquisition(reason, persistSettings);
                                  },
                                  Qt::QueuedConnection);
        return;
    }

    const bool wasEnabled = gnssContinuousAcquisitionEnabled;
    const bool timerWasActive = gnssContinuousAcquireTimer && gnssContinuousAcquireTimer->isActive();
    const bool running = gnssAcquisitionRunning;
    const QString source = gnssAcquisitionSource;

    gnssContinuousAcquisitionEnabled = false;
    gnssContinuousAcquisitionTickActive = false;
    gnssGpsFocusedPrn = 0;
    gnssGpsFocusedDopplerHz = 0.0;
    gnssGpsFocusedWeakCount = 0;
    gnssGpsFocusedStableCount = 0;
    gnssLastContinuousAcquisitionSequence = 0;

    if (gnssContinuousAcquireTimer) {
        gnssContinuousAcquireTimer->stop();
    }
    if (gnssDeepAcquireButton && gnssDeepAcquireButton->isChecked()) {
        QSignalBlocker blocker(gnssDeepAcquireButton);
        gnssDeepAcquireButton->setChecked(false);
    }
    if (gnssAcquisitionCancelFlag) {
        gnssAcquisitionCancelFlag->store(true, std::memory_order_relaxed);
    }
    if (running && gnssAcquireStatusLabel) {
        gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gps_ca_scan_cancelling"),
                                              QStringLiteral("GPS C/A accumulation: cancelling")));
    }

    const bool userVisibleStop = reason == QStringLiteral("ui-toggle");
    if ((userVisibleStop || fobosVerboseLoggingEnabled()) &&
        (wasEnabled || timerWasActive || running)) {
        qDebug() << "[GNSS acquisition] SDR acquisition stopped"
                 << "reason" << reason
                 << "wasEnabled" << wasEnabled
                 << "timerActive" << timerWasActive
                 << "running" << running
                 << "source" << source;
    }

    if (persistSettings) {
        savePersistentSettings();
    }
}

void YourClassName::scheduleGnssContinuousAcquisition(int delayMs) {
    if (!gnssContinuousAcquisitionEnabled || !gnssContinuousAcquireTimer) {
        return;
    }
    const int intervalMs = (std::clamp)(gnssContinuousAcquisitionIntervalMs,
                                        GNSS_CONTINUOUS_ACQUISITION_MIN_INTERVAL_MS,
                                        GNSS_CONTINUOUS_ACQUISITION_MAX_INTERVAL_MS);
    gnssContinuousAcquisitionIntervalMs = intervalMs;
    gnssContinuousAcquireTimer->start(delayMs >= 0 ? delayMs : intervalMs);
}

void YourClassName::handleGnssContinuousAcquisitionTick() {
    if (!gnssContinuousAcquisitionEnabled) {
        return;
    }
    if (runState != RadioRunState::Running || !processor || !processor->isRunning()) {
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gnss_continuous_acq_enabled"),
                                                  QStringLiteral("GNSS continuous acquisition: waiting for IQ")));
        }
        scheduleGnssContinuousAcquisition();
        return;
    }
    if (gnssAcquisitionRunning) {
        scheduleGnssContinuousAcquisition();
        return;
    }

    gnssContinuousAcquisitionTickActive = true;
    runGnssAcquisitionTest();
    gnssContinuousAcquisitionTickActive = false;

    if (!gnssAcquisitionRunning) {
        scheduleGnssContinuousAcquisition();
    }
}

void YourClassName::runGnssOfflineReplayTest() {
    if (gnssAcquisitionRunning) {
        return;
    }

    const QString path = selectedPlaybackFilePath();
    if (path.isEmpty()) {
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gnss_replay_no_file"),
                                                  QStringLiteral("GNSS replay: select a Channel IQ WAV recording.")));
        }
        return;
    }

    PlaybackManager::WavInfo info;
    QString errorMessage;
    if (!PlaybackManager::readWavInfo(path, info, &errorMessage) ||
        info.mode != PlaybackManager::Mode::ChannelIqWav ||
        info.channels != 2 ||
        info.bitsPerSample != 16 ||
        info.sampleRate <= 0) {
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gnss_replay_bad_file"),
                                                  QStringLiteral("GNSS replay: selected file is not a supported stereo IQ WAV.")));
        }
        qDebug() << "[GNSS replay] bad file" << path << errorMessage;
        return;
    }

    updateGnssSystemSelection();
    if (gnssIntegrationSpin) {
        gnssAcquisitionIntegrationMs =
            (std::clamp)(gnssIntegrationSpin->value(),
                         GNSS_ACQUISITION_MIN_INTEGRATION_MS,
                         GNSS_ACQUISITION_MAX_INTEGRATION_MS);
    }
    if (gnssChannelFilterSpin) {
        const double cutoffHz = gnssChannelFilterSpin->value() * 1000000.0;
        gnssChannelFilterCutoffHz =
            (std::clamp)(std::isfinite(cutoffHz) ? cutoffHz : 1800000.0,
                         GNSS_CHANNEL_FILTER_MIN_HZ,
                         GNSS_CHANNEL_FILTER_MAX_HZ);
    }
    if (gnssDopplerSpanSpin) {
        gnssDopplerSpanHz = (std::clamp)(gnssDopplerSpanSpin->value(), 1, 50) * 1000;
    }
    if (gnssDopplerStepSpin) {
        gnssDopplerStepHz = (std::clamp)(gnssDopplerStepSpin->value(), 250, 5000);
    }

    const GnssSystemPreset selectedPreset = gnssSystemPreset(gnssSystemId);
    const GnssSystemPreset acquisitionPreset =
        selectedPreset.id == QStringLiteral("all_l1")
            ? gnssSystemPreset(QStringLiteral("gps_l1_ca"))
            : selectedPreset;
    const QString acquisitionName = uiText(acquisitionPreset.textKey, acquisitionPreset.fallbackName);
    if (!isGnssAcquisitionImplemented(acquisitionPreset.acquisitionKind)) {
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(
                uiText(QStringLiteral("gnss_acq_not_implemented"),
                       QStringLiteral("%1 acquisition parser is not implemented yet. RF tune/scan, IQ monitor and IQ saving are ready."))
                    .arg(acquisitionName));
        }
        return;
    }

    double centerFrequency = 0.0;
    double targetFrequency = acquisitionPreset.targetHz;
    QFileInfo wavInfo(path);
    const QString sidecarPath = wavInfo.dir().filePath(wavInfo.completeBaseName() + QStringLiteral(".json"));
    QFile sidecar(sidecarPath);
    if (sidecar.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument document = QJsonDocument::fromJson(sidecar.readAll());
        if (document.isObject()) {
            const QJsonObject metadata = document.object();
            centerFrequency = metadata.value(QStringLiteral("centerFrequency")).toDouble(centerFrequency);
            const double listening = metadata.value(QStringLiteral("listeningFrequency")).toDouble(0.0);
            if (centerFrequency <= 0.0 && listening > 0.0) {
                centerFrequency = listening;
            }
            const QString sidecarSystem = metadata.value(QStringLiteral("gnssSystemId")).toString();
            if (!sidecarSystem.isEmpty() && gnssSystemCombo && gnssSystemCombo->findData(sidecarSystem) >= 0) {
                gnssSystemId = sidecarSystem;
                updateGnssSystemSelection();
            }
        }
    }
    if (centerFrequency <= 0.0 && info.hasRadioSettings) {
        const bool looksLikeChannelIq =
            info.radioSettings.sampleRate > 0.0 &&
            std::abs(static_cast<double>(info.sampleRate) - info.radioSettings.sampleRate) > 0.5;
        centerFrequency = looksLikeChannelIq && info.radioSettings.listeningFrequency > 0.0
                              ? info.radioSettings.listeningFrequency
                              : info.radioSettings.centerFrequency;
    }
    if (centerFrequency <= 0.0) {
        centerFrequency = pendingSettings.centerFrequency > 0.0
                              ? pendingSettings.centerFrequency
                              : pendingSettings.listeningFrequency;
    }

    const double sampleRate = static_cast<double>(info.sampleRate);
    if (!std::isfinite(centerFrequency) || centerFrequency <= 0.0 ||
        !std::isfinite(targetFrequency) || targetFrequency <= 0.0 ||
        std::abs(targetFrequency - centerFrequency) > sampleRate * 0.48) {
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(
                uiText(QStringLiteral("gnss_acq_target_out_of_span"),
                       QStringLiteral("%1 target %2 MHz is outside the current IQ span. Tune this system or wait for its scan center."))
                    .arg(acquisitionName,
                         QString::number(targetFrequency / 1000000.0, 'f', 6)));
        }
        qDebug() << "[GNSS replay] target out of span"
                 << "path" << path
                 << "centerHz" << centerFrequency
                 << "targetHz" << targetFrequency
                 << "sampleRate" << sampleRate;
        return;
    }

    const int requestedIntegrationMs =
        (std::clamp)(gnssAcquisitionIntegrationMs,
                     GNSS_ACQUISITION_MIN_INTEGRATION_MS,
                     GNSS_ACQUISITION_MAX_INTEGRATION_MS);
    const quint64 requestedSamples =
        static_cast<quint64>(std::ceil(sampleRate * static_cast<double>(requestedIntegrationMs) / 1000.0)) +
        8192ULL;
    const quint64 availableSamples = info.dataSize / 4ULL;
    const quint64 samplesToRead = (std::min)(availableSamples, (std::max<quint64>)(4092ULL, requestedSamples));
    if (samplesToRead < 4092ULL) {
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gnss_replay_too_short"),
                                                  QStringLiteral("GNSS replay: recording is too short for one GPS C/A millisecond.")));
        }
        return;
    }

    QFile wavFile(path);
    if (!wavFile.open(QIODevice::ReadOnly) ||
        !wavFile.seek(static_cast<qint64>(info.dataOffset))) {
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gnss_replay_read_failed"),
                                                  QStringLiteral("GNSS replay: cannot read selected WAV.")));
        }
        return;
    }
    const quint64 bytesToRead = samplesToRead * 4ULL;
    QByteArray pcm = wavFile.read(static_cast<qint64>((std::min<quint64>)(bytesToRead, 128ULL * 1024ULL * 1024ULL)));
    wavFile.close();
    const int iqSamples = pcm.size() / 4;
    if (iqSamples < 4092) {
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gnss_replay_too_short"),
                                                  QStringLiteral("GNSS replay: recording is too short for one GPS C/A millisecond.")));
        }
        return;
    }

    std::vector<float> snapshot;
    snapshot.reserve(static_cast<std::size_t>(iqSamples) * 2U);
    for (int offset = 0; offset + 3 < pcm.size(); offset += 4) {
        const qint16 iSample = readLeI16(pcm.constData() + offset);
        const qint16 qSample = readLeI16(pcm.constData() + offset + 2);
        snapshot.push_back(static_cast<float>(iSample) / 32768.0f);
        snapshot.push_back(static_cast<float>(qSample) / 32768.0f);
    }

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    gnssAcquisitionCancelFlag = cancelFlag;
    gnssAcquisitionRunning = true;
    const QString acquisitionSource = QStringLiteral("replay");
    gnssAcquisitionSource = acquisitionSource;
    if (gnssAcquireButton) {
        gnssAcquireButton->setEnabled(false);
    }
    if (gnssOfflineAcquireButton) {
        gnssOfflineAcquireButton->setEnabled(false);
    }
    if (gnssSelfTestButton) {
        gnssSelfTestButton->setEnabled(false);
    }
    if (gnssAcquireStatusLabel) {
        gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gnss_replay_running"),
                                              QStringLiteral("GNSS replay: running acquisition on selected WAV")));
    }

    qDebug() << "[GNSS replay] start"
             << "path" << path
             << "sidecar" << sidecarPath
             << "inputSamples" << iqSamples
             << "requestedIntegrationMs" << requestedIntegrationMs
             << "sampleRate" << sampleRate
             << "centerHz" << centerFrequency
             << "targetHz" << targetFrequency
             << "channelFilterCutoffHz" << gnssChannelFilterCutoffHz
             << "dopplerMinHz" << -gnssDopplerSpanHz
             << "dopplerMaxHz" << gnssDopplerSpanHz
             << "dopplerStepHz" << gnssDopplerStepHz;

    QPointer<YourClassName> self(this);
    QtConcurrent::run([self,
                       snapshot = std::move(snapshot),
                       sampleRate,
                       centerFrequency,
                       targetFrequency,
                       integrationMs = requestedIntegrationMs,
                       channelFilterCutoffHz = gnssChannelFilterCutoffHz,
                       dopplerSpanHz = gnssDopplerSpanHz,
                       dopplerStepHz = gnssDopplerStepHz,
                       acquisitionPreset,
                       acquisitionSource,
                       cancelFlag]() mutable {
        QElapsedTimer acquisitionTimer;
        acquisitionTimer.start();
        GnssAcquisitionResult result =
            acquireForPreset(acquisitionPreset,
                             snapshot,
                             sampleRate,
                             centerFrequency,
                             targetFrequency,
                             integrationMs,
                             channelFilterCutoffHz,
                             cancelFlag.get(),
                             -dopplerSpanHz,
                             dopplerSpanHz,
                             dopplerStepHz);
        result.processingElapsedMs = acquisitionTimer.elapsed();
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, result, acquisitionSource, cancelFlag]() {
            if (!self) {
                return;
            }
            self->gnssAcquisitionRunning = false;
            if (self->gnssAcquisitionCancelFlag == cancelFlag) {
                self->gnssAcquisitionCancelFlag.reset();
            }
            if (self->gnssAcquireButton) {
                self->gnssAcquireButton->setEnabled(true);
            }
            if (self->gnssOfflineAcquireButton) {
                self->gnssOfflineAcquireButton->setEnabled(true);
            }
            if (self->gnssSelfTestButton) {
                self->gnssSelfTestButton->setEnabled(true);
            }
            self->gnssAcquisitionSource = acquisitionSource;
            self->updateGnssAcquisitionStatus(result);
        }, Qt::QueuedConnection);
    });
}

void YourClassName::runGnssSyntheticSelfTest() {
    if (gnssAcquisitionRunning) {
        return;
    }

    if (gnssIntegrationSpin) {
        gnssAcquisitionIntegrationMs =
            (std::clamp)(gnssIntegrationSpin->value(),
                         GNSS_ACQUISITION_MIN_INTEGRATION_MS,
                         GNSS_ACQUISITION_MAX_INTEGRATION_MS);
    }
    if (gnssChannelFilterSpin) {
        const double cutoffHz = gnssChannelFilterSpin->value() * 1000000.0;
        gnssChannelFilterCutoffHz =
            (std::clamp)(std::isfinite(cutoffHz) ? cutoffHz : 1800000.0,
                         GNSS_CHANNEL_FILTER_MIN_HZ,
                         GNSS_CHANNEL_FILTER_MAX_HZ);
    }

    updateGnssSystemSelection();
    const GnssSystemPreset selectedPreset = gnssSystemPreset(gnssSystemId);
    const GnssSystemPreset acquisitionPreset = selectedPreset;
    if (!isGnssAcquisitionImplemented(acquisitionPreset.acquisitionKind)) {
        const QString status =
            uiText(QStringLiteral("gnss_acq_not_implemented"),
                   QStringLiteral("%1 acquisition parser is not implemented yet. RF tune/scan, IQ monitor and IQ saving are ready."))
                .arg(uiText(acquisitionPreset.textKey, acquisitionPreset.fallbackName));
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(status);
        }
        GnssAcquisitionResult skippedResult;
        skippedResult.status = status;
        skippedResult.systemName = acquisitionPreset.fallbackName;
        updateGnssAcquisitionPlot(skippedResult);
        return;
    }

    const int syntheticPrn = 7;
    const int syntheticGlonassChannel = -2;
    const bool glonassSelfTest = acquisitionPreset.acquisitionKind == GnssAcquisitionKind::GlonassL1Of;
    const int expectedRowId = glonassSelfTest
                                  ? syntheticGlonassChannel + 8
                                  : syntheticPrn;
    const QString expectedLabel = glonassSelfTest
                                      ? QStringLiteral("R%1").arg(syntheticGlonassChannel)
                                      : QStringLiteral("G%1").arg(syntheticPrn, 2, 10, QLatin1Char('0'));
    const double syntheticDopplerHz = glonassSelfTest
                                          ? 2500.0
                                          : 3500.0;
    const int integrationMs =
        (std::clamp)((std::max)(gnssAcquisitionIntegrationMs, 160),
                     GNSS_ACQUISITION_MIN_INTEGRATION_MS,
                     GNSS_ACQUISITION_MAX_INTEGRATION_MS);
    double sampleRate = pendingSettings.sampleRate;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        sampleRate = 50000000.0;
    }
    const double targetFrequency = glonassSelfTest
                                       ? 1602000000.0 +
                                             static_cast<double>(syntheticGlonassChannel) * 562500.0
                                       : GNSS_GPS_L1_HZ;
    double centerFrequency = pendingSettings.centerFrequency;
    if (!std::isfinite(centerFrequency) || centerFrequency <= 0.0 ||
        std::abs(targetFrequency - centerFrequency) > sampleRate * 0.45) {
        centerFrequency = GNSS_L1_LISTENING_SCAN_CENTER_HZ;
    }
    if (sampleRate > 8192000.0 && std::abs(targetFrequency - centerFrequency) < 8192000.0 * 0.45) {
        sampleRate = 8192000.0;
    }

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    gnssAcquisitionCancelFlag = cancelFlag;
    gnssAcquisitionRunning = true;
    const QString acquisitionSource = QStringLiteral("synthetic-iq");
    gnssAcquisitionSource = acquisitionSource;
    if (gnssAcquireButton) {
        gnssAcquireButton->setEnabled(false);
    }
    if (gnssOfflineAcquireButton) {
        gnssOfflineAcquireButton->setEnabled(false);
    }
    if (gnssSelfTestButton) {
        gnssSelfTestButton->setEnabled(false);
    }
    if (gnssAcquireStatusLabel) {
        gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gps_ca_self_test_running"),
                                              QStringLiteral("GNSS self-test: running")));
    }

    constexpr int kSelfTestDopplerSpanHz = 5000;
    constexpr int kSelfTestDopplerStepHz = 1000;
    qDebug() << "[GNSS self-test] start"
             << "system" << acquisitionPreset.id
             << "expectedId" << expectedRowId
             << "expectedLabel" << expectedLabel
             << "expectedDopplerHz" << syntheticDopplerHz
             << "inputSampleRate" << sampleRate
             << "centerHz" << centerFrequency
             << "targetHz" << targetFrequency
             << "integrationMs" << integrationMs
             << "channelFilterCutoffHz" << gnssChannelFilterCutoffHz
             << "dopplerMinHz" << -kSelfTestDopplerSpanHz
             << "dopplerMaxHz" << kSelfTestDopplerSpanHz
             << "dopplerStepHz" << kSelfTestDopplerStepHz;

    QPointer<YourClassName> self(this);
    QtConcurrent::run([self,
                       sampleRate,
                       centerFrequency,
                       targetFrequency,
                       syntheticPrn,
                       syntheticGlonassChannel,
                       expectedRowId,
                       expectedLabel,
                       syntheticDopplerHz,
                       integrationMs,
                       channelFilterCutoffHz = gnssChannelFilterCutoffHz,
                       dopplerSpanHz = kSelfTestDopplerSpanHz,
                       dopplerStepHz = kSelfTestDopplerStepHz,
                       acquisitionPreset,
                       acquisitionSource,
                       cancelFlag]() {
        QElapsedTimer timer;
        timer.start();
        std::vector<float> syntheticIq;
        if (acquisitionPreset.acquisitionKind == GnssAcquisitionKind::GlonassL1Of) {
            syntheticIq = GnssAcquisition::makeSyntheticGlonassL1OfIq(sampleRate,
                                                                      centerFrequency,
                                                                      syntheticGlonassChannel,
                                                                      syntheticDopplerHz,
                                                                      integrationMs,
                                                                      0.25);
        } else {
            syntheticIq = GnssAcquisition::makeSyntheticGpsL1CaIq(sampleRate,
                                                                  centerFrequency,
                                                                  targetFrequency,
                                                                  syntheticPrn,
                                                                  syntheticDopplerHz,
                                                                  integrationMs,
                                                                  0.25);
        }
        GnssAcquisitionResult result =
            acquireForPreset(acquisitionPreset,
                             syntheticIq,
                             sampleRate,
                             centerFrequency,
                             targetFrequency,
                             integrationMs,
                             channelFilterCutoffHz,
                             cancelFlag.get(),
                             -dopplerSpanHz,
                             dopplerSpanHz,
                             dopplerStepHz);
        result.processingElapsedMs = timer.elapsed();
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, result, expectedRowId, expectedLabel, syntheticDopplerHz, acquisitionSource, cancelFlag]() {
            if (!self) {
                return;
            }
            self->gnssAcquisitionRunning = false;
            if (self->gnssAcquisitionCancelFlag == cancelFlag) {
                self->gnssAcquisitionCancelFlag.reset();
            }
            if (self->gnssAcquireButton) {
                self->gnssAcquireButton->setEnabled(true);
            }
            if (self->gnssOfflineAcquireButton) {
                self->gnssOfflineAcquireButton->setEnabled(true);
            }
            if (self->gnssSelfTestButton) {
                self->gnssSelfTestButton->setEnabled(true);
            }
            self->gnssAcquisitionSource = acquisitionSource;
            if (!result.topCandidates.isEmpty()) {
                const GnssAcquisitionCandidate best = result.topCandidates.first();
                const double metricDb =
                    10.0 * std::log10((std::max)(best.metric,
                                                 std::numeric_limits<double>::min()));
                const double peakToSecondDb =
                    10.0 * std::log10((std::max)(best.peakToSecond,
                                                 std::numeric_limits<double>::min()));
                const QString bestLabel = best.label.trimmed().isEmpty()
                                              ? QString::number(best.prn)
                                              : best.label.trimmed();
                qDebug() << "[GNSS self-test] finished"
                         << "expectedId" << expectedRowId
                         << "expectedLabel" << expectedLabel
                         << "expectedDopplerHz" << syntheticDopplerHz
                         << "bestId" << best.prn
                         << "bestLabel" << bestLabel
                         << "bestDopplerHz" << best.dopplerHz
                         << "bestCodePhase" << best.codePhaseSamples
                         << "bestMetricDb" << metricDb
                         << "bestPeakToSecondDb" << peakToSecondDb
                         << "processingMs" << result.processingElapsedMs
                         << "idMatch" << (best.prn == expectedRowId);
            } else {
                qDebug() << "[GNSS self-test] finished"
                         << "valid" << result.valid
                         << "status" << result.status
                         << "processingMs" << result.processingElapsedMs;
            }
            self->updateGnssAcquisitionStatus(result);
        }, Qt::QueuedConnection);
    });
}

void YourClassName::runGnssPositionSelfTest() {
    constexpr double truthLat = 50.4501;
    constexpr double truthLon = 30.5234;
    constexpr double truthAltMeters = 180.0;
    constexpr double receiverClockBiasMeters = 72000.0;
    const Vec3d truthEcef = geodeticToEcef(truthLat, truthLon, truthAltMeters);

    Vec3d east;
    Vec3d north;
    Vec3d up;
    enuBasis(truthLat, truthLon, &east, &north, &up);

    struct SyntheticSatelliteDirection {
        double azimuthDeg = 0.0;
        double elevationDeg = 0.0;
        double slantMeters = 0.0;
        double noiseMeters = 0.0;
    };
    const std::array<SyntheticSatelliteDirection, 6> directions = {{
        {15.0, 62.0, 21400000.0, 0.8},
        {82.0, 48.0, 22100000.0, -0.4},
        {145.0, 54.0, 23200000.0, 0.2},
        {218.0, 38.0, 24600000.0, -0.7},
        {292.0, 44.0, 22900000.0, 0.5},
        {335.0, 27.0, 25500000.0, -0.3}
    }};

    QVector<SyntheticPseudorange> measurements;
    measurements.reserve(static_cast<int>(directions.size()));
    for (const SyntheticSatelliteDirection &direction : directions) {
        const double az = direction.azimuthDeg * GNSS_DEG_TO_RAD;
        const double el = direction.elevationDeg * GNSS_DEG_TO_RAD;
        const Vec3d los =
            east * (std::sin(az) * std::cos(el)) +
            north * (std::cos(az) * std::cos(el)) +
            up * std::sin(el);
        SyntheticPseudorange measurement;
        measurement.satellite = truthEcef + los * direction.slantMeters;
        measurement.pseudorangeMeters =
            vectorNorm(measurement.satellite - truthEcef) +
            receiverClockBiasMeters +
            direction.noiseMeters;
        measurements.append(measurement);
    }

    const double initialLat = (std::clamp)(truthLat + 3.0, -80.0, 80.0);
    double initialLon = truthLon - 4.0;
    if (initialLon < -180.0) {
        initialLon += 360.0;
    }
    const Vec3d initialEcef = geodeticToEcef(initialLat, initialLon, 0.0);
    const SyntheticPositionResult result =
        solveSyntheticPosition(measurements, truthEcef, initialEcef);

    if (!result.valid) {
        const QString status = uiText(QStringLiteral("gnss_position_self_test_failed"),
                                      QStringLiteral("GNSS position self-test failed: solver did not converge."));
        if (qthStatusLabel) {
            qthStatusLabel->setText(status);
        }
        qDebug() << "[GNSS position self-test] failed"
                 << "truthLat" << truthLat
                 << "truthLon" << truthLon
                 << "satellites" << measurements.size();
        return;
    }

    qthLatitude = result.latitude;
    qthLongitude = result.longitude;
    qthSource = QStringLiteral("manual");
    qthPositionVisible = true;
    const QString locator = qth::maidenheadLocator(qthLatitude, qthLongitude, 6);
    updateQthControls();
    openQthMapWindow();
    if (qthMapWidget) {
        const QString label =
            uiText(QStringLiteral("gnss_position_self_test_marker"),
                   QStringLiteral("GNSS synthetic"));
        qthMapWidget->setSearchMarker(qthLatitude, qthLongitude, label);
        qthMapWidget->centerOn(qthLatitude, qthLongitude);
    }
    savePersistentSettings();

    const QString status =
        uiText(QStringLiteral("gnss_position_self_test_result"),
               QStringLiteral("GNSS position self-test: %1, %2 (%3), error %4 m, clock %5 m, %6 satellites"))
            .arg(QString::number(qthLatitude, 'f', 6),
                 QString::number(qthLongitude, 'f', 6),
                 locator,
                 QString::number(result.errorMeters, 'f', 2),
                 QString::number(result.clockBiasMeters, 'f', 1),
                 QString::number(result.satellites));
    if (qthStatusLabel) {
        qthStatusLabel->setText(status);
    }
    qDebug() << "[GNSS position self-test] result"
             << "truthLat" << truthLat
             << "truthLon" << truthLon
             << "solvedLat" << result.latitude
             << "solvedLon" << result.longitude
             << "altMeters" << result.altitudeMeters
             << "errorMeters" << result.errorMeters
             << "clockBiasMeters" << result.clockBiasMeters
             << "expectedClockBiasMeters" << receiverClockBiasMeters
             << "iterations" << result.iterations
             << "satellites" << result.satellites
             << "qth" << locator;
}
