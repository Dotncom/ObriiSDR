#include "appruntimeutils.h"

#include "appconstants.h"

#include <cmath>

bool spectrumFftSettingsMatch(const RadioSettings &a, const RadioSettings &b) {
    auto closeEnough = [](double lhs, double rhs, double tolerance) {
        return std::abs(lhs - rhs) <= tolerance;
    };

    return a.inputMode == b.inputMode &&
           a.fftLength == b.fftLength &&
           normalizedFftWindowType(a.fftWindowType) == normalizedFftWindowType(b.fftWindowType) &&
           closeEnough(a.sampleRate, b.sampleRate, 0.5) &&
           closeEnough(a.centerFrequency, b.centerFrequency, 0.5) &&
           closeEnough(a.actualFrequency, b.actualFrequency, 0.5) &&
           closeEnough(a.hfNoiseCancelDepth, b.hfNoiseCancelDepth, 0.0001) &&
           closeEnough(a.hfNoiseCancelRefGainDb, b.hfNoiseCancelRefGainDb, 0.0001) &&
           closeEnough(a.hfNoiseCancelRefDelayNs, b.hfNoiseCancelRefDelayNs, 0.0001) &&
           closeEnough(a.hfNoiseCancelRefTiltDb, b.hfNoiseCancelRefTiltDb, 0.0001) &&
           a.hfNoiseCancelFreeze == b.hfNoiseCancelFreeze;
}

qint64 agileRfLiveSettleMs(double sampleRate, bool sampleRateChange) {
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        return sampleRateChange ? AGILE_RF_MID_RATE_SAMPLE_SETTLE_MS
                                : AGILE_RF_MID_RATE_RETUNE_SETTLE_MS;
    }
    if (sampleRate <= 12500000.5) {
        return sampleRateChange ? AGILE_RF_LOW_RATE_SAMPLE_SETTLE_MS
                                : AGILE_RF_LOW_RATE_RETUNE_SETTLE_MS;
    }
    if (sampleRate <= 20000000.5) {
        return sampleRateChange ? AGILE_RF_MID_RATE_SAMPLE_SETTLE_MS
                                : AGILE_RF_MID_RATE_RETUNE_SETTLE_MS;
    }
    return sampleRateChange ? AGILE_RF_HIGH_RATE_SAMPLE_SETTLE_MS
                            : AGILE_RF_HIGH_RATE_RETUNE_SETTLE_MS;
}

double agileRfAutoBandwidthRatio(double sampleRate) {
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        return AGILE_RF_MID_RATE_AUTO_BANDWIDTH_RATIO;
    }
    if (sampleRate <= 12500000.5) {
        return AGILE_RF_LOW_RATE_AUTO_BANDWIDTH_RATIO;
    }
    if (sampleRate <= 20000000.5) {
        return AGILE_RF_MID_RATE_AUTO_BANDWIDTH_RATIO;
    }
    return AGILE_RF_HIGH_RATE_AUTO_BANDWIDTH_RATIO;
}

const char *runStateName(RadioRunState state) {
    switch (state) {
    case RadioRunState::Idle:
        return "Idle";
    case RadioRunState::Starting:
        return "Starting";
    case RadioRunState::Running:
        return "Running";
    case RadioRunState::Stopping:
        return "Stopping";
    }
    return "Unknown";
}

bool firmwareLooksAgile(const QString &firmwareVersion) {
    bool ok = false;
    const int major = firmwareVersion.trimmed().section('.', 0, 0).toInt(&ok);
    return ok && major >= 3;
}

bool frequencyListChanged(const QVector<double> &a, const QVector<double> &b, double toleranceHz) {
    if (a.size() != b.size()) {
        return true;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (std::abs(a.at(i) - b.at(i)) > toleranceHz) {
            return true;
        }
    }
    return false;
}

bool jsonValuesEquivalent(const QJsonValue &left, const QJsonValue &right) {
    if (left.type() != right.type()) {
        if (left.isDouble() && right.isDouble()) {
            return std::abs(left.toDouble() - right.toDouble()) <= 0.5;
        }
        return false;
    }
    if (left.isDouble()) {
        return std::abs(left.toDouble() - right.toDouble()) <= 0.5;
    }
    return left == right;
}
