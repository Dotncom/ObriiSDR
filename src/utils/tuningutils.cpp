#include "tuningutils.h"

#include "appconstants.h"
#include "receiverdeviceutils.h"

#include <algorithm>
#include <array>
#include <cmath>

double directMaxFrequency(double sampleRate) {
    return (std::max)(DIRECT_MIN_FREQUENCY, sampleRate / 2.0 - 1.0);
}

double directMinFrequencyForMode(int inputMode, double sampleRate) {
    return inputMode == INPUT_HF_COMBINED ? -directMaxFrequency(sampleRate) : DIRECT_MIN_FREQUENCY;
}

double rfMinimumCenterFrequency(const RadioSettings &settings) {
    if (settings.deviceIndex == VIRTUAL_SIGNAL_DEVICE_INDEX) return DIRECT_MIN_FREQUENCY;
    if (isUhdNativeComboValue(settings.deviceIndex))
        return (std::max)(70000000.0, settings.sampleRate * 0.5);
    return isHackRfNativeComboValue(settings.deviceIndex)
               ? (std::max)(HACKRF_MIN_FREQUENCY, settings.sampleRate * 0.5)
               : RF_MIN_CENTER_FREQUENCY;
}

double rfMaximumCenterFrequency(const RadioSettings &settings) {
    if (isUhdNativeComboValue(settings.deviceIndex))
        return (std::max)(rfMinimumCenterFrequency(settings),
                          6000000000.0 - settings.sampleRate * 0.5);
    return isHackRfNativeComboValue(settings.deviceIndex)
               ? (std::max)(rfMinimumCenterFrequency(settings),
                            HACKRF_MAX_FREQUENCY - settings.sampleRate * 0.5)
               : RF_EXPERIMENTAL_MAX_FREQUENCY;
}

double rfMinimumListeningFrequency(const RadioSettings &settings) {
    if (settings.deviceIndex == VIRTUAL_SIGNAL_DEVICE_INDEX) return DIRECT_MIN_FREQUENCY;
    if (isUhdNativeComboValue(settings.deviceIndex)) return 70000000.0;
    return isHackRfNativeComboValue(settings.deviceIndex)
               ? DIRECT_MIN_FREQUENCY
               : RF_MIN_LISTENING_FREQUENCY;
}

double rfMaximumFrequency(const RadioSettings &settings) {
    if (isUhdNativeComboValue(settings.deviceIndex)) return 6000000000.0;
    return isHackRfNativeComboValue(settings.deviceIndex)
               ? HACKRF_MAX_FREQUENCY
               : RF_EXPERIMENTAL_MAX_FREQUENCY;
}

QPair<double, double> listeningScanVisibleSpanHz(const RadioSettings &settings) {
    if (settings.inputMode == INPUT_RF) {
        const double halfRate = (std::max)(1.0, settings.sampleRate) * 0.5;
        const double low = (std::max)(rfMinimumListeningFrequency(settings),
                                      settings.centerFrequency - halfRate);
        const double high = (std::clamp)(settings.centerFrequency + halfRate,
                                         low,
                                         rfMaximumFrequency(settings));
        return qMakePair(low, high);
    }

    const double low = directMinFrequencyForMode(settings.inputMode, settings.sampleRate);
    const double high = (std::max)(low, directMaxFrequency(settings.sampleRate));
    return qMakePair(low, high);
}

double autoTuneRoundingStepHz(double frequencyHz, double visibleSpanHz) {
    if (!std::isfinite(frequencyHz)) {
        return 1000.0;
    }

    const double absFrequency = std::abs(frequencyHz);
    double minimumStep = 100.0;
    double maximumStep = 5000.0;

    if (absFrequency >= 3000000000.0) {
        minimumStep = 100000.0;
        maximumStep = 5000000.0;
    } else if (absFrequency >= 1000000000.0) {
        minimumStep = 10000.0;
        maximumStep = 1000000.0;
    } else if (absFrequency >= 300000000.0) {
        minimumStep = 5000.0;
        maximumStep = 12500.0;
    } else if (absFrequency >= 30000000.0) {
        minimumStep = 1000.0;
        maximumStep = 12500.0;
    } else if (absFrequency >= 3000000.0) {
        minimumStep = 100.0;
        maximumStep = 5000.0;
    }

    const double spanStep = std::isfinite(visibleSpanHz) && visibleSpanHz > 0.0
                                ? visibleSpanHz / 2000.0
                                : minimumStep;
    const double targetStep = (std::clamp)(spanStep, minimumStep, maximumStep);
    constexpr std::array<double, 19> niceSteps = {
        10.0,
        50.0,
        100.0,
        500.0,
        1000.0,
        2500.0,
        5000.0,
        6250.0,
        10000.0,
        12500.0,
        25000.0,
        50000.0,
        100000.0,
        250000.0,
        500000.0,
        1000000.0,
        2500000.0,
        5000000.0,
        10000000.0,
    };

    for (const double step : niceSteps) {
        if (step >= targetStep && step >= minimumStep && step <= maximumStep) {
            return step;
        }
    }
    return maximumStep;
}

double roundAutoTuneFrequencyHz(double frequencyHz, double visibleSpanHz) {
    if (!std::isfinite(frequencyHz)) {
        return frequencyHz;
    }
    const double stepHz = autoTuneRoundingStepHz(frequencyHz, visibleSpanHz);
    if (!std::isfinite(stepHz) || stepHz <= 0.0) {
        return frequencyHz;
    }
    return std::round(frequencyHz / stepHz) * stepHz;
}

void normalizeTuning(RadioSettings &settings, bool preserveCenter) {
    if (settings.sampleRate <= 0.0) {
        return;
    }

    if (settings.inputMode == INPUT_RF) {
        const double halfRate = settings.sampleRate / 2.0;
        const double minimumCenter = rfMinimumCenterFrequency(settings);
        const double maximumCenter = rfMaximumCenterFrequency(settings);
        const double minimumListening = rfMinimumListeningFrequency(settings);
        const double maximumFrequency = rfMaximumFrequency(settings);
        settings.centerFrequency = (std::clamp)(settings.centerFrequency,
                                                minimumCenter,
                                                maximumCenter);
        settings.listeningFrequency = (std::clamp)(settings.listeningFrequency,
                                                   minimumListening,
                                                   maximumFrequency);

        if (!preserveCenter && settings.listeningFrequency < settings.centerFrequency - halfRate) {
            settings.centerFrequency = (std::clamp)(settings.listeningFrequency + halfRate,
                                                    minimumCenter,
                                                    maximumCenter);
        } else if (!preserveCenter && settings.listeningFrequency > settings.centerFrequency + halfRate) {
            settings.centerFrequency = (std::clamp)(settings.listeningFrequency - halfRate,
                                                    minimumCenter,
                                                    maximumCenter);
        }

        const double low = (std::max)(minimumListening,
                                      settings.centerFrequency - halfRate);
        const double high = (std::clamp)(settings.centerFrequency + halfRate,
                                         low,
                                         maximumFrequency);
        settings.listeningFrequency = (std::clamp)(settings.listeningFrequency, low, high);
    } else {
        settings.centerFrequency = 0.0;
        settings.actualFrequency = 0.0;
        const double directMin = directMinFrequencyForMode(settings.inputMode, settings.sampleRate);
        const double directMax = directMaxFrequency(settings.sampleRate);
        settings.listeningFrequency = (std::clamp)(settings.listeningFrequency,
                                                   directMin,
                                                   directMax);
    }
}

bool shouldOffsetDmrCenterFromListening(const RadioSettings &settings) {
    return settings.inputMode == INPUT_RF &&
           settings.modulationType == MOD_DMR &&
           std::isfinite(settings.centerFrequency) &&
           std::isfinite(settings.listeningFrequency) &&
           std::abs(settings.centerFrequency - settings.listeningFrequency) <
               DMR_CENTER_MIN_OFFSET_HZ;
}

bool offsetDmrCenterFromListening(RadioSettings &settings) {
    if (!shouldOffsetDmrCenterFromListening(settings)) {
        return false;
    }

    const double halfRate = settings.sampleRate > 0.0
                                ? settings.sampleRate * 0.5
                                : DMR_CENTER_MIN_OFFSET_HZ * 2.0;
    const double safeOffset =
        (std::min)(DMR_CENTER_MIN_OFFSET_HZ,
                   (std::max)(1000.0, halfRate * 0.25));
    double newCenter = settings.listeningFrequency - safeOffset;
    const double minimumCenter = rfMinimumCenterFrequency(settings);
    const double maximumCenter = rfMaximumCenterFrequency(settings);
    if (newCenter < minimumCenter) {
        newCenter = settings.listeningFrequency + safeOffset;
    }
    settings.centerFrequency = (std::clamp)(newCenter,
                                            minimumCenter,
                                            maximumCenter);
    settings.actualFrequency = settings.centerFrequency;
    normalizeTuning(settings, true);
    return true;
}

int scalePercentToSliderValue(double scalePercent) {
    const double clamped = (std::clamp)(scalePercent, MIN_SCALE_PERCENT, MAX_SCALE_PERCENT);
    return static_cast<int>(std::lround(clamped * SCALE_SLIDER_FACTOR));
}

double sliderValueToScalePercent(int sliderValue) {
    const int clamped = (std::clamp)(sliderValue,
                                     scalePercentToSliderValue(MIN_SCALE_PERCENT),
                                     scalePercentToSliderValue(MAX_SCALE_PERCENT));
    return clamped / static_cast<double>(SCALE_SLIDER_FACTOR);
}

QString formatScalePercent(double scalePercent) {
    if (std::abs(scalePercent - std::round(scalePercent)) < 0.05) {
        return QString::number(scalePercent, 'f', 0);
    }
    return QString::number(scalePercent, 'f', 1);
}

QString scaleLabelText(double scalePercent) {
    return QString("Scale: %1").arg(formatScalePercent(scalePercent));
}

float sliderValueToLevel(int sliderValue) {
    const int clamped = (std::clamp)(sliderValue, MIN_LEVEL_SLIDER_VALUE, MAX_LEVEL_SLIDER_VALUE);
    return clamped / static_cast<float>(LEVEL_SLIDER_FACTOR);
}

int levelToSliderValue(float level) {
    const int value = static_cast<int>(std::lround(level * LEVEL_SLIDER_FACTOR));
    return (std::clamp)(value, MIN_LEVEL_SLIDER_VALUE, MAX_LEVEL_SLIDER_VALUE);
}

QString levelLabelText(const QString &name, float level) {
    return QString("%1: %2").arg(name, QString::number(level, 'f', 1));
}

double clampAudioLowPassHz(double hz) {
    if (!std::isfinite(hz) || hz <= 0.0) {
        return 0.0;
    }
    return (std::clamp)(hz, static_cast<double>(AUDIO_LOW_PASS_SLIDER_STEP_HZ),
                        static_cast<double>(AUDIO_LOW_PASS_SLIDER_STEP_HZ * AUDIO_LOW_PASS_SLIDER_MAX));
}

double clampAudioHighPassHz(double hz) {
    if (!std::isfinite(hz) || hz <= 0.0) {
        return 0.0;
    }
    return (std::clamp)(hz, static_cast<double>(AUDIO_HIGH_PASS_SLIDER_STEP_HZ),
                        static_cast<double>(AUDIO_HIGH_PASS_SLIDER_STEP_HZ * AUDIO_HIGH_PASS_SLIDER_MAX));
}

double audioLowPassSliderValueToHz(int value) {
    return value <= 0 ? 0.0 : clampAudioLowPassHz(value * AUDIO_LOW_PASS_SLIDER_STEP_HZ);
}

int audioLowPassHzToSliderValue(double hz) {
    if (!std::isfinite(hz) || hz <= 0.0) {
        return 0;
    }
    return (std::clamp)(static_cast<int>(std::lround(hz / AUDIO_LOW_PASS_SLIDER_STEP_HZ)),
                        1,
                        AUDIO_LOW_PASS_SLIDER_MAX);
}

double audioHighPassSliderValueToHz(int value) {
    return value <= 0 ? 0.0 : clampAudioHighPassHz(value * AUDIO_HIGH_PASS_SLIDER_STEP_HZ);
}

int audioHighPassHzToSliderValue(double hz) {
    if (!std::isfinite(hz) || hz <= 0.0) {
        return 0;
    }
    return (std::clamp)(static_cast<int>(std::lround(hz / AUDIO_HIGH_PASS_SLIDER_STEP_HZ)),
                        1,
                        AUDIO_HIGH_PASS_SLIDER_MAX);
}

QString audioFilterFrequencyText(double hz) {
    if (!std::isfinite(hz) || hz <= 0.0) {
        return QString();
    }
    if (hz >= 1000.0) {
        const int decimals = hz >= 10000.0 ? 0 : 1;
        return QString("%1 kHz").arg(hz / 1000.0, 0, 'f', decimals);
    }
    return QString("%1 Hz").arg(static_cast<int>(std::lround(hz)));
}

QString formatSampleRate(double sampleRate) {
    if (sampleRate >= 1e9) {
        return QString::number(sampleRate / 1e9, 'f', 2) + QStringLiteral(" GHz");
    }
    if (sampleRate >= 1e6) {
        return QString::number(sampleRate / 1e6, 'f', 2) + QStringLiteral(" MHz");
    }
    if (sampleRate >= 1e3) {
        return QString::number(sampleRate / 1e3, 'f', 2) + QStringLiteral(" kHz");
    }
    return QString::number(sampleRate, 'f', 2) + QStringLiteral(" Hz");
}

double clampHfNoiseCancelDepth(double depth) {
    if (!std::isfinite(depth)) {
        return 1.0;
    }
    return (std::clamp)(depth,
                        HF_NOISE_CANCEL_DEPTH_MIN / 100.0,
                        HF_NOISE_CANCEL_DEPTH_MAX / 100.0);
}

int hfNoiseCancelDepthToSliderValue(double depth) {
    return (std::clamp)(static_cast<int>(std::lround(clampHfNoiseCancelDepth(depth) * 100.0)),
                        HF_NOISE_CANCEL_DEPTH_MIN,
                        HF_NOISE_CANCEL_DEPTH_MAX);
}

double hfNoiseCancelSliderValueToDepth(int value) {
    return (std::clamp)(value, HF_NOISE_CANCEL_DEPTH_MIN, HF_NOISE_CANCEL_DEPTH_MAX) / 100.0;
}

QString hfNoiseCancelDepthLabelText(double depth) {
    return QString("HF cancel: %1%").arg(hfNoiseCancelDepthToSliderValue(depth));
}

double clampHfNoiseCancelRefGainDb(double gainDb) {
    if (!std::isfinite(gainDb)) {
        return 0.0;
    }
    return (std::clamp)(gainDb,
                        HF_NOISE_CANCEL_REF_GAIN_MIN / 10.0,
                        HF_NOISE_CANCEL_REF_GAIN_MAX / 10.0);
}

int hfNoiseCancelRefGainToSliderValue(double gainDb) {
    return (std::clamp)(static_cast<int>(std::lround(clampHfNoiseCancelRefGainDb(gainDb) * 10.0)),
                        HF_NOISE_CANCEL_REF_GAIN_MIN,
                        HF_NOISE_CANCEL_REF_GAIN_MAX);
}

double hfNoiseCancelSliderValueToRefGainDb(int value) {
    return (std::clamp)(value, HF_NOISE_CANCEL_REF_GAIN_MIN, HF_NOISE_CANCEL_REF_GAIN_MAX) / 10.0;
}

QString hfNoiseCancelRefGainLabelText(double gainDb) {
    return QString("Ref gain: %1 dB").arg(clampHfNoiseCancelRefGainDb(gainDb), 0, 'f', 1);
}

double clampHfNoiseCancelRefDelayNs(double delayNs) {
    if (!std::isfinite(delayNs)) {
        return 0.0;
    }
    return (std::clamp)(delayNs,
                        static_cast<double>(HF_NOISE_CANCEL_REF_DELAY_MIN_NS),
                        static_cast<double>(HF_NOISE_CANCEL_REF_DELAY_MAX_NS));
}

int hfNoiseCancelRefDelayToSliderValue(double delayNs) {
    return (std::clamp)(static_cast<int>(std::lround(clampHfNoiseCancelRefDelayNs(delayNs))),
                        HF_NOISE_CANCEL_REF_DELAY_MIN_NS,
                        HF_NOISE_CANCEL_REF_DELAY_MAX_NS);
}

double hfNoiseCancelSliderValueToRefDelayNs(int value) {
    return (std::clamp)(value,
                        HF_NOISE_CANCEL_REF_DELAY_MIN_NS,
                        HF_NOISE_CANCEL_REF_DELAY_MAX_NS);
}

QString hfNoiseCancelRefDelayLabelText(double delayNs) {
    return QString("Ref delay: %1 ns").arg(static_cast<int>(std::lround(clampHfNoiseCancelRefDelayNs(delayNs))));
}

double clampHfNoiseCancelRefTiltDb(double tiltDb) {
    if (!std::isfinite(tiltDb)) {
        return 0.0;
    }
    return (std::clamp)(tiltDb,
                        HF_NOISE_CANCEL_REF_TILT_MIN / 10.0,
                        HF_NOISE_CANCEL_REF_TILT_MAX / 10.0);
}

int hfNoiseCancelRefTiltToSliderValue(double tiltDb) {
    return (std::clamp)(static_cast<int>(std::lround(clampHfNoiseCancelRefTiltDb(tiltDb) * 10.0)),
                        HF_NOISE_CANCEL_REF_TILT_MIN,
                        HF_NOISE_CANCEL_REF_TILT_MAX);
}

double hfNoiseCancelSliderValueToRefTiltDb(int value) {
    return (std::clamp)(value, HF_NOISE_CANCEL_REF_TILT_MIN, HF_NOISE_CANCEL_REF_TILT_MAX) / 10.0;
}

QString hfNoiseCancelRefTiltLabelText(double tiltDb) {
    return QString("Ref tilt: %1 dB").arg(clampHfNoiseCancelRefTiltDb(tiltDb), 0, 'f', 1);
}
