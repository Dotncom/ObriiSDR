#include "main.h"
#include "multivfowidget.h"
#include "dspflowpanel.h"
#include "appconstants.h"
#include "appruntimeutils.h"
#include "diagnosticlogging.h"
#include "iqbuffer.h"
#include "receiverdeviceutils.h"
#include "researchanalysisdialog.h"
#include "wirelesslabdialog.h"
#include "scanvisualutils.h"
#include "spectrumfftworker.h"
#include "spectrumdisplayreducer.h"
#include "tuningutils.h"
#include "zerospandialog.h"

#include <QDebug>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSignalBlocker>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

extern std::vector<float> fftMagnitudes;
extern std::vector<float> fftFrequencies;
extern int fftLength;
extern double currentScale;
extern bool secondGraph;
extern bool syncWariable;
extern float sensitivity;
extern float contrast;
extern bool colorf;

namespace {
constexpr int MinFftLength = 512;
constexpr int MaxCustomFftLength = 134217728;
constexpr std::array<int, 18> StandardFftLengths = {
    512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144,
    524288, 1048576, 2097152, 4194304, 8388608, 16777216,
    33554432, 67108864
};

struct SpectrumDisplayProfile {
    int fftLength = 0;
    int frames = 0;
    qint64 preReduceNs = 0;
    qint64 reduceNs = 0;
    qint64 widgetNs = 0;
    qint64 totalNs = 0;
};

void recordSpectrumDisplayProfile(int fftLength,
                                  qint64 preReduceNs,
                                  qint64 reduceNs,
                                  qint64 widgetNs,
                                  qint64 totalNs) {
    if (!fobosVerboseLoggingEnabled()) {
        return;
    }
    static SpectrumDisplayProfile profile;
    if (profile.fftLength != fftLength) {
        profile = SpectrumDisplayProfile{};
        profile.fftLength = fftLength;
    }
    ++profile.frames;
    profile.preReduceNs += preReduceNs;
    profile.reduceNs += reduceNs;
    profile.widgetNs += widgetNs;
    profile.totalNs += totalNs;
    constexpr int ProfileFrames = 30;
    if (profile.frames < ProfileFrames) {
        return;
    }
    const double scale = 1.0 / (1000000.0 * profile.frames);
    qInfo() << "[Spectrum display profile]"
            << "length" << profile.fftLength
            << "frames" << profile.frames
            << "preReduceMs" << profile.preReduceNs * scale
            << "reduceMs" << profile.reduceNs * scale
            << "widgetMs" << profile.widgetNs * scale
            << "totalMs" << profile.totalNs * scale;
    profile.frames = 0;
    profile.preReduceNs = 0;
    profile.reduceNs = 0;
    profile.widgetNs = 0;
    profile.totalNs = 0;
}
}

void YourClassName::onfftLengthEntered() {
    if (fftBinWidthModeEnabled) {
        syncFftResolutionControls();
        return;
    }
    const int newFftLength = fftComboBox->currentText().toInt();
    applyFftLengthChange(newFftLength, true);
}

int YourClassName::fftLengthForBinWidth(double sampleRate, double hzPerPoint) const {
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 ||
        !std::isfinite(hzPerPoint) || hzPerPoint <= 0.0) {
        return (std::clamp)(pendingSettings.fftLength, MinFftLength, MaxCustomFftLength);
    }

    qint64 length = qRound64(sampleRate / hzPerPoint);
    length = (std::clamp)(length,
                          static_cast<qint64>(MinFftLength),
                          static_cast<qint64>(MaxCustomFftLength));
    if ((length & 1) != 0 && length < MaxCustomFftLength) {
        ++length;
    }
    return static_cast<int>(length);
}

int YourClassName::nearestStandardFftLength(int requestedLength) const {
    const auto best = std::min_element(
        StandardFftLengths.begin(),
        StandardFftLengths.end(),
        [requestedLength](int left, int right) {
            const qint64 leftDistance = std::abs(static_cast<qint64>(left) - requestedLength);
            const qint64 rightDistance = std::abs(static_cast<qint64>(right) - requestedLength);
            if (leftDistance == rightDistance) {
                return left > right;
            }
            return leftDistance < rightDistance;
        });
    return best != StandardFftLengths.end() ? *best : 65536;
}

void YourClassName::syncFftResolutionControls() {
    const double actualBinWidth = pendingSettings.fftLength > 0 && pendingSettings.sampleRate > 0.0
                                      ? pendingSettings.sampleRate / pendingSettings.fftLength
                                      : 0.0;
    const double displayedBinWidth =
        fftBinWidthModeEnabled ? fftTargetBinWidthHz : actualBinWidth;

    if (fftBinWidthModeCheckbox) {
        QSignalBlocker blocker(fftBinWidthModeCheckbox);
        fftBinWidthModeCheckbox->setChecked(fftBinWidthModeEnabled);
    }
    if (fftBinWidthSpin) {
        QSignalBlocker blocker(fftBinWidthSpin);
        fftBinWidthSpin->setValue(displayedBinWidth);
        fftBinWidthSpin->setEnabled(fftBinWidthModeEnabled);
    }
    if (fftComboBox) {
        QSignalBlocker blocker(fftComboBox);
        fftComboBox->setEnabled(!fftBinWidthModeEnabled);
        fftComboBox->setEditText(QString::number(pendingSettings.fftLength));
    }

    const QString status = uiText(
                               QStringLiteral("fft_hz_per_point_status"),
                               QStringLiteral("Target: %1 Hz/point | actual: %2 Hz/point | FFT: %3"))
                               .arg(displayedBinWidth, 0, 'f', 3)
                               .arg(actualBinWidth, 0, 'f', 6)
                               .arg(pendingSettings.fftLength);
    const QString modeTip = uiText(
        QStringLiteral("fft_hz_per_point_tooltip"),
        QStringLiteral("Calculate an exact even FFT length from sample rate / target Hz per point. Disabling this mode selects the nearest standard FFT length."));
    if (fftBinWidthModeCheckbox) {
        fftBinWidthModeCheckbox->setToolTip(modeTip + QStringLiteral("\n") + status);
    }
    if (fftBinWidthSpin) {
        fftBinWidthSpin->setToolTip(status);
    }
    if (fftComboBox) {
        fftComboBox->setToolTip(
            uiText(QStringLiteral("fft_length_tooltip"),
                   QStringLiteral("Spectrum FFT length. Very large transforms can use several gigabytes of memory.")) +
            QStringLiteral("\n") + status);
    }
}

void YourClassName::applyFftBinWidthTarget(bool notifyRemote) {
    if (!fftBinWidthModeEnabled) {
        syncFftResolutionControls();
        return;
    }
    const int calculatedLength = fftLengthForBinWidth(pendingSettings.sampleRate,
                                                       fftTargetBinWidthHz);
    applyFftLengthChange(calculatedLength, notifyRemote);
    syncFftResolutionControls();
}

void YourClassName::onFftBinWidthModeChanged(bool checked) {
    if (checked) {
        const double actualBinWidth =
            pendingSettings.fftLength > 0 && pendingSettings.sampleRate > 0.0
                ? pendingSettings.sampleRate / pendingSettings.fftLength
                : fftTargetBinWidthHz;
        fftTargetBinWidthHz = (std::clamp)(actualBinWidth, 0.1, 1000000.0);
        fftBinWidthModeEnabled = true;
        applyFftBinWidthTarget(true);
    } else {
        fftBinWidthModeEnabled = false;
        applyFftLengthChange(nearestStandardFftLength(pendingSettings.fftLength), true);
        syncFftResolutionControls();
    }
    savePersistentSettings();
}

void YourClassName::onFftBinWidthChanged(double value) {
    fftTargetBinWidthHz = (std::clamp)(value, 0.1, 1000000.0);
    if (fftBinWidthModeEnabled) {
        applyFftBinWidthTarget(true);
        savePersistentSettings();
    } else {
        syncFftResolutionControls();
    }
}

bool YourClassName::applyFftLengthChange(int newFftLength, bool notifyRemote) {
    if (newFftLength <= 0) {
        qDebug() << "Invalid FFT length selected.";
        return false;
    }

    if (pendingSettings.fftLength == newFftLength && fftResult) {
        syncFftResolutionControls();
        return true;
    }

    qDebug() << "[FFT] applying FFT length"
             << "previous" << pendingSettings.fftLength
             << "new" << newFftLength
             << "state" << runStateName(runState)
             << "networkMode" << static_cast<int>(networkMode)
             << "processingMode" << static_cast<int>(networkProcessingMode);

    pendingSettings.fftLength = newFftLength;
    publishSettingsToGlobals();
    updateSpectrumTimerInterval();
    if (fftResult) {
        fftResult->resetHfNoiseCancelState();
    }
    if (spectrumFftWorker) {
        spectrumFftWorker->resetHfNoiseCancelState();
    }

    if (fftComboBox) {
        fftComboBox->blockSignals(true);
        fftComboBox->setEditText(QString::number(pendingSettings.fftLength));
        fftComboBox->blockSignals(false);
    }

    syncFftResolutionControls();

    if (notifyRemote && isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
    }

    return true;
}

void YourClassName::populateSampleRates() {
    if (!sampleBox) {
        return;
    }

    QSignalBlocker sampleBoxBlocker(sampleBox);
    if (isHackRfNativeSelected()) {
        sampleBox->clear();
        const QVector<double> hackRfRates = {
            2000000.0,
            4000000.0,
            8000000.0,
            10000000.0,
            12500000.0,
            16000000.0,
            20000000.0
        };
        for (const double rate : hackRfRates) {
            sampleBox->addItem(formatSampleRate(rate), rate);
        }
        if (sampleBox->findData(pendingSettings.sampleRate) < 0) {
            pendingSettings.sampleRate = 10000000.0;
        }
        const int index = sampleBox->findData(pendingSettings.sampleRate);
        if (index >= 0) {
            sampleBox->setCurrentIndex(index);
        }
        qDebug() << "[HackRF] using native sample-rate list";
        return;
    }
    if (isSoapySdrSelected()) {
        sampleBox->clear();
        const QVector<double> soapyRates = {
            1000000.0,
            1024000.0,
            1536000.0,
            2048000.0,
            2400000.0,
            3200000.0,
            8000000.0,
            10000000.0,
            20000000.0,
            50000000.0
        };
        for (const double rate : soapyRates) {
            sampleBox->addItem(formatSampleRate(rate), rate);
        }
        if (sampleBox->findData(pendingSettings.sampleRate) < 0) {
            pendingSettings.sampleRate = 2048000.0;
        }
        const int index = sampleBox->findData(pendingSettings.sampleRate);
        if (index >= 0) {
            sampleBox->setCurrentIndex(index);
        }
        qDebug() << "[SoapySDR] using generic Soapy sample-rate list";
        return;
    }
    if (isRtlBackendSelected()) {
        sampleBox->clear();
        const QVector<double> rtlRates = {
            1024000.0,
            1536000.0,
            2048000.0,
            2400000.0,
            2560000.0,
            3200000.0
        };
        for (const double rate : rtlRates) {
            sampleBox->addItem(formatSampleRate(rate), rate);
        }
        if (sampleBox->findData(pendingSettings.sampleRate) < 0) {
            pendingSettings.sampleRate = RTL_TCP_SAFE_SAMPLE_RATE;
        }
        const int index = sampleBox->findData(pendingSettings.sampleRate);
        if (index >= 0) {
            sampleBox->setCurrentIndex(index);
        }
        qDebug() << "[RTL-TCP] using RTL-SDR sample-rate list";
        return;
    }

    auto addDefaultSampleRates = [this]() {
        if (sampleBox->count() > 0) {
            return;
        }
        const QVector<double> defaultRates = {
            8000000.0, 10000000.0, 12500000.0, 16000000.0, 20000000.0,
            25000000.0, 32000000.0, 40000000.0, 50000000.0, 80000000.0
        };
        for (const double rate : defaultRates) {
            sampleBox->addItem(formatSampleRate(rate), rate);
        }
        const int defaultIndex = sampleBox->findData(pendingSettings.sampleRate);
        if (defaultIndex >= 0) {
            sampleBox->setCurrentIndex(defaultIndex);
        }
        qDebug() << "[FobosDevices] using fallback sample-rate list; no local receiver is required for client control";
    };

    if (isNetworkClientMode()) {
        addDefaultSampleRates();
        return;
    }

    void *sampleRateDevice = activeFobosDevice();
    FobosApiKind sampleRateApiKind = activeFobosApiKind;
    bool openedForSampleRates = false;

    int ret = FOBOS_ERR_OK;
    if (!sampleRateDevice) {
        const FobosDeviceInfo selectedInfo = selectedFobosDeviceInfo();
        sampleRateApiKind = selectedInfo.apiKind;
        if (selectedInfo.apiKind == FobosApiKind::Agile) {
            fobos_sdr_dev_t *openedDevice = nullptr;
            ret = openFobosAgileDeviceSafely(&openedDevice, static_cast<uint32_t>(selectedInfo.nativeIndex));
            sampleRateDevice = openedDevice;
        } else {
            fobos_dev_t *openedDevice = nullptr;
            ret = openFobosDeviceSafely(&openedDevice, static_cast<uint32_t>(selectedInfo.nativeIndex));
            sampleRateDevice = openedDevice;
        }
        openedForSampleRates = (ret == FOBOS_ERR_OK && sampleRateDevice);
    }
    if (ret != FOBOS_ERR_OK) {
        qDebug() << "[FobosDevices] sample-rate list unavailable; using fallback, open result:" << ret;
        addDefaultSampleRates();
        return;
    }
    if (!sampleRateDevice) {
        qDebug() << "Device is not initialized.";
        addDefaultSampleRates();
        return;
    }
    double sampleRates[100];
    unsigned int count = 100;
    int result = sampleRateApiKind == FobosApiKind::Agile
                     ? getFobosAgileSampleRatesSafely(static_cast<fobos_sdr_dev_t*>(sampleRateDevice), sampleRates, &count)
                     : getFobosSampleRatesSafely(static_cast<fobos_dev_t*>(sampleRateDevice), sampleRates, &count);
    if (result != FOBOS_ERR_OK) {
        qDebug() << "Failed to get sample rates, error code:" << result;
        if (openedForSampleRates) {
            if (sampleRateApiKind == FobosApiKind::Agile) {
                closeFobosAgileDeviceSafely(static_cast<fobos_sdr_dev_t*>(sampleRateDevice));
            } else {
                closeFobosDeviceSafely(static_cast<fobos_dev_t*>(sampleRateDevice));
            }
        }
        addDefaultSampleRates();
        return;
    }
    sampleBox->clear();
    for (unsigned int i = 0; i < count; ++i) {
        QString formattedRate = formatSampleRate(sampleRates[i]);
        sampleBox->addItem(formattedRate, sampleRates[i]);
    }
    int selectedIndex = sampleBox->findData(pendingSettings.sampleRate);
    if (selectedIndex < 0 || isKnownRtlSampleRate(pendingSettings.sampleRate)) {
        int defaultIndex = sampleBox->findData(FOBOS_DEFAULT_SAMPLE_RATE);
        if (defaultIndex < 0 && sampleBox->count() > 0) {
            double bestDelta = std::numeric_limits<double>::max();
            for (int i = 0; i < sampleBox->count(); ++i) {
                const double rate = sampleBox->itemData(i).toDouble();
                const double delta = std::abs(rate - FOBOS_DEFAULT_SAMPLE_RATE);
                if (delta < bestDelta) {
                    bestDelta = delta;
                    defaultIndex = i;
                }
            }
        }
        if (defaultIndex >= 0) {
            pendingSettings.sampleRate = sampleBox->itemData(defaultIndex).toDouble();
            selectedIndex = defaultIndex;
        }
    }
    if (selectedIndex >= 0) {
        sampleBox->setCurrentIndex(selectedIndex);
    }
    if (openedForSampleRates) {
        if (sampleRateApiKind == FobosApiKind::Agile) {
            closeFobosAgileDeviceSafely(static_cast<fobos_sdr_dev_t*>(sampleRateDevice));
        } else {
            closeFobosDeviceSafely(static_cast<fobos_dev_t*>(sampleRateDevice));
        }
    }
}

void YourClassName::learnHfInterferenceBaseline() {
    const std::vector<float> &sourceFrequencies =
        !hfInterferenceLastFrequencies.empty() ? hfInterferenceLastFrequencies : spectrumFrequencyScratch;
    const std::vector<float> &sourceMagnitudes =
        !hfInterferenceLastMagnitudes.empty() ? hfInterferenceLastMagnitudes : spectrumMagnitudeScratch;
    if (sourceFrequencies.size() != sourceMagnitudes.size() ||
        sourceFrequencies.size() < 2) {
        qDebug() << "[HF interference] baseline learn skipped: no visible spectrum frame";
        updateHfNoiseCancelControls();
        return;
    }

    struct BaselinePoint {
        float frequency = 0.0f;
        float magnitude = 0.0f;
    };
    std::vector<BaselinePoint> points;
    points.reserve(sourceFrequencies.size());
    for (std::size_t i = 0; i < sourceFrequencies.size(); ++i) {
        const float frequency = sourceFrequencies[i];
        const float magnitude = sourceMagnitudes[i];
        if (std::isfinite(frequency) && std::isfinite(magnitude)) {
            points.push_back({frequency, magnitude});
        }
    }
    if (points.size() < 2) {
        qDebug() << "[HF interference] baseline learn skipped: spectrum frame has no finite bins";
        updateHfNoiseCancelControls();
        return;
    }

    std::sort(points.begin(), points.end(), [](const BaselinePoint &a, const BaselinePoint &b) {
        return a.frequency < b.frequency;
    });

    const int smoothBins = (std::max)(1, hfInterferenceBaselineSmoothBins | 1);
    const int radius = smoothBins / 2;
    std::vector<double> prefix(points.size() + 1, 0.0);
    for (std::size_t i = 0; i < points.size(); ++i) {
        prefix[i + 1] = prefix[i] + points[i].magnitude;
    }

    hfInterferenceBaselineFrequencies.resize(points.size());
    hfInterferenceBaselineMagnitudes.resize(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::size_t begin = static_cast<std::size_t>((std::max)(0, static_cast<int>(i) - radius));
        const std::size_t end = (std::min)(points.size(), i + static_cast<std::size_t>(radius) + 1);
        const double count = static_cast<double>((std::max)(std::size_t(1), end - begin));
        hfInterferenceBaselineFrequencies[i] = points[i].frequency;
        hfInterferenceBaselineMagnitudes[i] = static_cast<float>((prefix[end] - prefix[begin]) / count);
    }
    hfInterferenceBaselineEnabled = true;
    if (hfInterferenceBaselineCheckbox) {
        QSignalBlocker blocker(hfInterferenceBaselineCheckbox);
        hfInterferenceBaselineCheckbox->setChecked(true);
    }
    updateHfNoiseCancelControls();
    savePersistentSettings();
    qDebug() << "[HF interference] baseline learned"
             << "bins" << hfInterferenceBaselineFrequencies.size()
             << "smoothBins" << smoothBins
             << "depth" << hfInterferenceBaselineDepth;
}

void YourClassName::clearHfInterferenceBaseline() {
    hfInterferenceBaselineFrequencies.clear();
    hfInterferenceBaselineMagnitudes.clear();
    hfInterferenceVisualMagnitudes.clear();
    hfInterferenceBaselineEnabled = false;
    if (hfInterferenceBaselineCheckbox) {
        QSignalBlocker blocker(hfInterferenceBaselineCheckbox);
        hfInterferenceBaselineCheckbox->setChecked(false);
    }
    updateHfNoiseCancelControls();
    savePersistentSettings();
    qDebug() << "[HF interference] baseline cleared";
}

void YourClassName::resetHfInterferenceDefaults() {
    hfInterferenceBaselineFrequencies.clear();
    hfInterferenceBaselineMagnitudes.clear();
    hfInterferenceVisualMagnitudes.clear();
    hfInterferenceBaselineEnabled = false;
    hfInterferenceRawOverlayEnabled = false;
    hfInterferenceBaselineDepth = 1.0;
    hfInterferenceBaselineSmoothBins = 121;

    pendingSettings.hfNoiseCancelDepth = 1.0;
    pendingSettings.hfNoiseCancelRefGainDb = 0.0;
    pendingSettings.hfNoiseCancelRefDelayNs = 0.0;
    pendingSettings.hfNoiseCancelRefTiltDb = 0.0;
    pendingSettings.hfNoiseCancelFreeze = false;
    pendingSettings.hfAudioBlankerEnabled = false;
    pendingSettings.hfAudioBlankerThreshold = 8.0;

    if (hfInterferenceBaselineCheckbox) {
        QSignalBlocker blocker(hfInterferenceBaselineCheckbox);
        hfInterferenceBaselineCheckbox->setChecked(false);
    }
    if (hfInterferenceRawOverlayCheckbox) {
        QSignalBlocker blocker(hfInterferenceRawOverlayCheckbox);
        hfInterferenceRawOverlayCheckbox->setChecked(false);
    }
    if (hfInterferenceBaselineDepthSlider) {
        QSignalBlocker blocker(hfInterferenceBaselineDepthSlider);
        hfInterferenceBaselineDepthSlider->setValue(static_cast<int>(std::lround(hfInterferenceBaselineDepth * 100.0)));
    }
    if (hfInterferenceBaselineSmoothSlider) {
        QSignalBlocker blocker(hfInterferenceBaselineSmoothSlider);
        hfInterferenceBaselineSmoothSlider->setValue(hfInterferenceBaselineSmoothBins);
    }
    if (hfNoiseCancelDepthSlider) {
        QSignalBlocker blocker(hfNoiseCancelDepthSlider);
        hfNoiseCancelDepthSlider->setValue(hfNoiseCancelDepthToSliderValue(pendingSettings.hfNoiseCancelDepth));
    }
    if (hfNoiseCancelRefGainSlider) {
        QSignalBlocker blocker(hfNoiseCancelRefGainSlider);
        hfNoiseCancelRefGainSlider->setValue(hfNoiseCancelRefGainToSliderValue(pendingSettings.hfNoiseCancelRefGainDb));
    }
    if (hfNoiseCancelRefDelaySlider) {
        QSignalBlocker blocker(hfNoiseCancelRefDelaySlider);
        hfNoiseCancelRefDelaySlider->setValue(hfNoiseCancelRefDelayToSliderValue(pendingSettings.hfNoiseCancelRefDelayNs));
    }
    if (hfNoiseCancelRefTiltSlider) {
        QSignalBlocker blocker(hfNoiseCancelRefTiltSlider);
        hfNoiseCancelRefTiltSlider->setValue(hfNoiseCancelRefTiltToSliderValue(pendingSettings.hfNoiseCancelRefTiltDb));
    }
    if (hfNoiseCancelFreezeCheckbox) {
        QSignalBlocker blocker(hfNoiseCancelFreezeCheckbox);
        hfNoiseCancelFreezeCheckbox->setChecked(false);
    }
    if (hfAudioBlankerCheckbox) {
        QSignalBlocker blocker(hfAudioBlankerCheckbox);
        hfAudioBlankerCheckbox->setChecked(false);
    }
    if (hfAudioBlankerThresholdSlider) {
        QSignalBlocker blocker(hfAudioBlankerThresholdSlider);
        hfAudioBlankerThresholdSlider->setValue(static_cast<int>(std::lround(pendingSettings.hfAudioBlankerThreshold * 10.0)));
    }

    if (fftResult) {
        fftResult->resetHfNoiseCancelState();
    }
    if (spectrumFftWorker) {
        spectrumFftWorker->resetHfNoiseCancelState();
    }
    if (audioProcessor) {
        audioProcessor->resetHfNoiseCancelState();
        audioProcessor->configure(audioProcessorSettings());
    }

    updateHfNoiseCancelControls();
    savePersistentSettings();
    qDebug() << "[HF interference] defaults restored";
}

bool YourClassName::buildHfInterferenceBaselineVisual(const std::vector<float> &frequencies,
                                                      const std::vector<float> &magnitudes,
                                                      std::vector<float> &outputMagnitudes) {
    if (!hfInterferenceBaselineEnabled ||
        hfInterferenceBaselineFrequencies.size() < 2 ||
        hfInterferenceBaselineFrequencies.size() != hfInterferenceBaselineMagnitudes.size() ||
        frequencies.size() != magnitudes.size() ||
        frequencies.empty()) {
        return false;
    }

    outputMagnitudes.resize(magnitudes.size());
    const float floorDb = displayLevelMin;
    const float ceilingDb = displayLevelMax;
    const float depth = static_cast<float>((std::clamp)(hfInterferenceBaselineDepth, 0.0, 2.0));
    const auto baselineBegin = hfInterferenceBaselineFrequencies.begin();
    const auto baselineEnd = hfInterferenceBaselineFrequencies.end();
    const bool ascendingFrequencies = frequencies.size() < 2 || frequencies.back() >= frequencies.front();
    std::size_t rollingBaselineIndex = 1;
    for (std::size_t i = 0; i < magnitudes.size(); ++i) {
        const float frequency = frequencies[i];
        const float magnitude = magnitudes[i];
        if (!std::isfinite(frequency) || !std::isfinite(magnitude)) {
            outputMagnitudes[i] = floorDb;
            continue;
        }

        float baseline = hfInterferenceBaselineMagnitudes.front();
        if (ascendingFrequencies) {
            while (rollingBaselineIndex < hfInterferenceBaselineFrequencies.size() &&
                   hfInterferenceBaselineFrequencies[rollingBaselineIndex] < frequency) {
                ++rollingBaselineIndex;
            }
            if (rollingBaselineIndex == 0 || frequency <= hfInterferenceBaselineFrequencies.front()) {
                baseline = hfInterferenceBaselineMagnitudes.front();
            } else if (rollingBaselineIndex >= hfInterferenceBaselineFrequencies.size()) {
                baseline = hfInterferenceBaselineMagnitudes.back();
            } else {
                const std::size_t upperIndex = rollingBaselineIndex;
                const std::size_t lowerIndex = upperIndex - 1;
                const float f0 = hfInterferenceBaselineFrequencies[lowerIndex];
                const float f1 = hfInterferenceBaselineFrequencies[upperIndex];
                const float m0 = hfInterferenceBaselineMagnitudes[lowerIndex];
                const float m1 = hfInterferenceBaselineMagnitudes[upperIndex];
                const float span = f1 - f0;
                const float ratio = span > 0.0f ? (frequency - f0) / span : 0.0f;
                baseline = m0 + (m1 - m0) * (std::clamp)(ratio, 0.0f, 1.0f);
            }
        } else {
            const auto it = std::lower_bound(baselineBegin, baselineEnd, frequency);
            if (it == baselineBegin) {
                baseline = hfInterferenceBaselineMagnitudes.front();
            } else if (it == baselineEnd) {
                baseline = hfInterferenceBaselineMagnitudes.back();
            } else {
                const std::size_t upperIndex = static_cast<std::size_t>(std::distance(baselineBegin, it));
                const std::size_t lowerIndex = upperIndex - 1;
                const float f0 = hfInterferenceBaselineFrequencies[lowerIndex];
                const float f1 = hfInterferenceBaselineFrequencies[upperIndex];
                const float m0 = hfInterferenceBaselineMagnitudes[lowerIndex];
                const float m1 = hfInterferenceBaselineMagnitudes[upperIndex];
                const float span = f1 - f0;
                const float ratio = span > 0.0f ? (frequency - f0) / span : 0.0f;
                baseline = m0 + (m1 - m0) * (std::clamp)(ratio, 0.0f, 1.0f);
            }
        }

        const float excess = magnitude - baseline;
        outputMagnitudes[i] = excess <= 0.0f
                                  ? floorDb
                                  : (std::clamp)(floorDb + excess * depth, floorDb, ceilingDb);
    }
    return true;
}

void YourClassName::updateSpectrum() {
    if (!fftResult) {
        return;
    }

    const bool asynchronousFftMode =
        !agileScanRunning &&
        !standardScanRunning &&
        !networkSpectrumFrameMetadataValid;
    if (asynchronousFftMode &&
        spectrumFftWorker &&
        spectrumFftWorker->workOutstanding() &&
        !spectrumFftWorker->resultReady()) {
        return;
    }

    const bool verboseLogging = fobosVerboseLoggingEnabled();
    if (!verboseLogging) {
        spectrumDebugFramesRemaining = 0;
        spectrumTuningDebugFramesRemaining = 0;
    }
    const bool traceFrame = verboseLogging && spectrumDebugFramesRemaining > 0;
    QElapsedTimer traceTimer;
    if (traceFrame) {
        traceTimer.start();
        qDebug() << "[Spectrum] update begin"
                 << "framesLeft" << spectrumDebugFramesRemaining
                 << "sampleRate" << pendingSettings.sampleRate
                 << "fftLength" << pendingSettings.fftLength
                 << "iqSnapshotFloats" << IqBuffer::size()
                 << "queuedBlocks" << IqBuffer::queuedBlocks();
    }

    auto finishTrace = [&](const char *stage,
                           const std::vector<float> &frequencies,
                           const std::vector<float> &magnitudes) {
        if (!traceFrame) {
            return;
        }
        qDebug() << "[Spectrum] update" << stage
                 << "elapsedMs" << traceTimer.elapsed()
                 << "freqCount" << frequencies.size()
                 << "magCount" << magnitudes.size()
                 << "iqSnapshotFloats" << IqBuffer::size()
                 << "queuedBlocks" << IqBuffer::queuedBlocks();
        --spectrumDebugFramesRemaining;
    };

    if (liveRetuneSettleTimer.isValid()) {
        const qint64 elapsedMs = liveRetuneSettleTimer.elapsed();
        const qint64 settleMs = liveRetuneSettleDurationMs > 0 ? liveRetuneSettleDurationMs : LIVE_RETUNE_SETTLE_MS;
        IqBuffer::clear();
        if (elapsedMs < settleMs) {
            if (traceFrame) {
                qDebug() << "[Spectrum] update retune_settle"
                         << "elapsedMs" << elapsedMs
                         << "settleMs" << settleMs
                         << "queuedBlocks" << IqBuffer::queuedBlocks();
                --spectrumDebugFramesRemaining;
            }
            return;
        }
        liveRetuneSettleTimer.invalidate();
        if (traceFrame) {
            qDebug() << "[Spectrum] update retune_settle_done"
                     << "elapsedMs" << elapsedMs
                     << "settleMs" << settleMs
                     << "queuedBlocks" << IqBuffer::queuedBlocks();
            --spectrumDebugFramesRemaining;
        }
        return;
    }

    std::vector<float> &spectrumFrequencies = spectrumFrequencyScratch;
    std::vector<float> &spectrumMagnitudes = spectrumMagnitudeScratch;
    std::vector<float> &referenceMagnitudes = spectrumReferenceScratch;
    std::vector<SpectrumFftHistoryRow> waterfallHistoryRows;
    bool haveSpectrum = false;
    RadioSettings spectrumSettings = spectrumProcessingSettings();

    int scanIndexBeforeFft = -1;
    int scanIndexAfterFft = -1;
    double scanCenterBeforeFft = std::numeric_limits<double>::quiet_NaN();
    double scanCenterAfterFft = std::numeric_limits<double>::quiet_NaN();
    IqBuffer::BlockMetadata fftBlockMetadata;
    QString scanVisualSource = QStringLiteral("none");
    if (agileScanRunning &&
        activeFobosApiKind == FobosApiKind::Agile &&
        agileDevice &&
        !activeAgileScanFrequencies.isEmpty()) {
        scanVisualSource = QStringLiteral("agile");
    } else if (standardScanRunning && !activeStandardScanFrequencies.isEmpty()) {
        scanVisualSource = QStringLiteral("standard");
        scanIndexBeforeFft = (std::clamp)(standardScanIndex,
                                          0,
                                          activeStandardScanFrequencies.size() - 1);
        scanCenterBeforeFft = activeStandardScanFrequencies.at(scanIndexBeforeFft);
    }

    double scanCenterFrequency = scanCenterBeforeFft;
    if (!std::isfinite(scanCenterFrequency) && scanVisualSource != QStringLiteral("agile")) {
        scanCenterFrequency = currentAgileScanCenterFrequencyHz();
        if (!std::isfinite(scanCenterFrequency)) {
            scanCenterFrequency = currentStandardScanCenterFrequencyHz();
        }
    }
    if (std::isfinite(scanCenterFrequency) && spectrumSettings.inputMode == INPUT_RF) {
        spectrumSettings.centerFrequency = scanCenterFrequency;
        spectrumSettings.actualFrequency = scanCenterFrequency;
    }
    processGnssIqSnapshot(spectrumSettings);
    double fullMinFrequency = minFrequency;
    double fullMaxFrequency = maxFrequency;
    if (spectrumSettings.inputMode == INPUT_RF && spectrumSettings.sampleRate > 0.0) {
        fullMinFrequency = spectrumSettings.centerFrequency - spectrumSettings.sampleRate * 0.5;
        fullMaxFrequency = spectrumSettings.centerFrequency + spectrumSettings.sampleRate * 0.5;
    } else if (spectrumSettings.inputMode == INPUT_HF_COMBINED && spectrumSettings.sampleRate > 0.0) {
        fullMinFrequency = -spectrumSettings.sampleRate * 0.5;
        fullMaxFrequency = spectrumSettings.sampleRate * 0.5;
    } else if (spectrumSettings.sampleRate > 0.0) {
        fullMinFrequency = 0.0;
        fullMaxFrequency = spectrumSettings.sampleRate * 0.5;
    }
    if (!std::isfinite(fullMinFrequency) ||
        !std::isfinite(fullMaxFrequency) ||
        fullMaxFrequency <= fullMinFrequency) {
        fullMinFrequency = minFrequency;
        fullMaxFrequency = maxFrequency;
    }

    const double fullSpan = (std::max)(1.0, fullMaxFrequency - fullMinFrequency);
    double visibleSpan = fullSpan * (effectiveScalePercent() / 100.0);
    if (!std::isfinite(visibleSpan) || visibleSpan <= 0.0) {
        visibleSpan = fullSpan;
    }
    visibleSpan = (std::clamp)(visibleSpan, 1.0, fullSpan);

    double visibleCenter = spectrumDisplayCenterHz;
    if (!std::isfinite(visibleCenter) ||
        visibleCenter < fullMinFrequency ||
        visibleCenter > fullMaxFrequency) {
        visibleCenter = std::isfinite(scanCenterFrequency)
                            ? scanCenterFrequency
                            : (fullMinFrequency + fullMaxFrequency) * 0.5;
    }
    visibleCenter = (std::clamp)(visibleCenter, fullMinFrequency, fullMaxFrequency);
    spectrumDisplayCenterHz = visibleCenter;

    double frameMinFrequency = visibleCenter - visibleSpan * 0.5;
    frameMinFrequency = (std::clamp)(frameMinFrequency,
                                     fullMinFrequency,
                                     fullMaxFrequency - visibleSpan);
    double frameMaxFrequency = frameMinFrequency + visibleSpan;

    const bool spectrumWorkerAllowed =
        scanVisualSource == QStringLiteral("none") &&
        !networkSpectrumFrameMetadataValid;
    const bool batchWaterfallRows =
        waterfallWidget && waterfallWidget->isVisible() && !isMinimized() &&
        spectrumSettings.fftLength < 8388608 &&
        !spectrumSettings.simplifiedAudioChannelizer;

    if (spectrumWorkerAllowed) {
        if (!spectrumFftWorker) {
            spectrumFftWorker = std::make_unique<SpectrumFftWorker>();
            spectrumFftWorker->request(spectrumSettings,
                                       fftBackendPreference,
                                       updateTimer ? updateTimer->interval() : 0,
                                       spectrumFftOverlapPercent,
                                       batchWaterfallRows);
            finishTrace("fft_worker_start", spectrumFrequencies, spectrumMagnitudes);
            return;
        }

        SpectrumFftFrame frame;
        if (!spectrumFftWorker->takeLatest(frame)) {
            spectrumFftWorker->request(spectrumSettings,
                                       fftBackendPreference,
                                       updateTimer ? updateTimer->interval() : 0,
                                       spectrumFftOverlapPercent,
                                       batchWaterfallRows);
            finishTrace("fft_worker_pending", spectrumFrequencies, spectrumMagnitudes);
            return;
        }

        if (frame.badAlloc) {
            qCritical() << "[Spectrum] worker bad_alloc" << frame.error
                        << "sampleRate" << pendingSettings.sampleRate
                        << "fftLength" << pendingSettings.fftLength;
            updateTimer->stop();
            finishTrace("fft_worker_bad_alloc", spectrumFrequencies, spectrumMagnitudes);
            return;
        }

        if (!frame.error.isEmpty()) {
            qCritical() << "[Spectrum] worker exception" << frame.error;
            spectrumFftWorker->request(spectrumSettings,
                                       fftBackendPreference,
                                       updateTimer ? updateTimer->interval() : 0,
                                       spectrumFftOverlapPercent,
                                       batchWaterfallRows);
            finishTrace("fft_worker_exception", spectrumFrequencies, spectrumMagnitudes);
            return;
        }

        if (!frame.valid || !spectrumFftSettingsMatch(frame.settings, spectrumSettings)) {
            spectrumFftWorker->request(spectrumSettings,
                                       fftBackendPreference,
                                       updateTimer ? updateTimer->interval() : 0,
                                       spectrumFftOverlapPercent,
                                       batchWaterfallRows);
            finishTrace(frame.valid ? "fft_worker_stale" : "fft_worker_no_data",
                        spectrumFrequencies,
                        spectrumMagnitudes);
            return;
        }

        spectrumFrequencies = std::move(frame.frequencies);
        spectrumMagnitudes = std::move(frame.magnitudes);
        referenceMagnitudes = std::move(frame.referenceMagnitudes);
        waterfallHistoryRows = std::move(frame.historyRows);
        fftBlockMetadata = frame.metadata;
        haveSpectrum = true;
        spectrumFftWorker->request(spectrumSettings,
                                   fftBackendPreference,
                                   updateTimer ? updateTimer->interval() : 0,
                                   spectrumFftOverlapPercent,
                                   batchWaterfallRows);
    } else {
        try {
            fftResult->setBackendPreference(fftBackendPreference);
            haveSpectrum = fftResult->storeFFTResults(spectrumSettings,
                                                      spectrumFrequencies,
                                                      spectrumMagnitudes,
                                                      &referenceMagnitudes,
                                                      &fftBlockMetadata);
        } catch (const std::bad_alloc &error) {
            qCritical() << "[Spectrum] bad_alloc" << error.what()
                        << "sampleRate" << pendingSettings.sampleRate
                        << "fftLength" << pendingSettings.fftLength;
            updateTimer->stop();
            finishTrace("bad_alloc", spectrumFrequencies, spectrumMagnitudes);
            return;
        } catch (const std::exception &error) {
            qCritical() << "[Spectrum] exception" << error.what();
            finishTrace("exception", spectrumFrequencies, spectrumMagnitudes);
            return;
        } catch (...) {
            qCritical() << "[Spectrum] unknown exception";
            finishTrace("unknown_exception", spectrumFrequencies, spectrumMagnitudes);
            return;
        }
    }

    if (!haveSpectrum || spectrumFrequencies.empty() || spectrumMagnitudes.empty()) {
        finishTrace("no_data", spectrumFrequencies, spectrumMagnitudes);
        return;
    }

    // The waterfall can consume several unique FFT rows between two screen
    // repaints. Updating every auxiliary widget for each row serializes the
    // producer with the GUI and makes sub-10 ms waterfall intervals pointless.
    constexpr qint64 AUXILIARY_UI_INTERVAL_MS = 8;
    bool refreshAuxiliaryUi = spectrumUpdateIntervalMs <= SPECTRUM_UPDATE_AUTO_MS ||
                              spectrumUpdateIntervalMs >= AUXILIARY_UI_INTERVAL_MS;
    if (!spectrumAuxiliaryUiTimer.isValid()) {
        spectrumAuxiliaryUiTimer.start();
        refreshAuxiliaryUi = true;
    } else if (spectrumAuxiliaryUiTimer.elapsed() >= AUXILIARY_UI_INTERVAL_MS) {
        spectrumAuxiliaryUiTimer.restart();
        refreshAuxiliaryUi = true;
    }
    const bool refreshAnalysis = spectrumScienceAnalysisRequired();

    // Gate successive FFT snapshots by the exact amount of newly published IQ.
    // This makes 0/25/50/75% overlap deterministic without copying or queuing
    // another full FFT block in the UI thread.
    if (fftBlockMetadata.totalFloatCount > 0) {
        const std::uint64_t frameEnd = fftBlockMetadata.totalFloatCount;
        const std::uint64_t fftFloats = static_cast<std::uint64_t>(
            (std::max)(1, spectrumSettings.fftLength)) * 2ULL;
        const std::uint64_t hopFloats = (std::max)(2ULL,
            fftFloats * static_cast<std::uint64_t>(100 - spectrumFftOverlapPercent) / 100ULL);
        const bool sameGeometry = lastSpectrumFftEpoch == fftBlockMetadata.epoch &&
                                  lastSpectrumFftLength == spectrumSettings.fftLength;
        if (sameGeometry && frameEnd <= lastSpectrumFftEndFloats) {
            finishTrace("fft_duplicate_snapshot", spectrumFrequencies, spectrumMagnitudes);
            return;
        }
        if (sameGeometry && frameEnd - lastSpectrumFftEndFloats < hopFloats) {
            finishTrace("fft_overlap_wait", spectrumFrequencies, spectrumMagnitudes);
            return;
        }
        lastSpectrumFftEpoch = fftBlockMetadata.epoch;
        lastSpectrumFftEndFloats = frameEnd;
        lastSpectrumFftLength = spectrumSettings.fftLength;
    }
    ++spectrumCalibrationFrameSerial;

    const bool profileDisplayFrame = verboseLogging &&
                                     spectrumSettings.fftLength >= 524288;
    QElapsedTimer displayProfileTimer;
    if (profileDisplayFrame) {
        displayProfileTimer.start();
    }

    if (calibrationTableEnabled) {
        receiverCalibrationTable.applyAmplitudeCorrection(spectrumFrequencies,
                                                           spectrumMagnitudes,
                                                           &referenceMagnitudes,
                                                           amplitudeCalibrationOffsetDb);
    } else if (std::isfinite(amplitudeCalibrationOffsetDb) &&
               std::abs(amplitudeCalibrationOffsetDb) > 0.000001) {
        const float offsetDb = static_cast<float>(amplitudeCalibrationOffsetDb);
        for (float &magnitude : spectrumMagnitudes) {
            if (std::isfinite(magnitude)) magnitude += offsetDb;
        }
        for (float &magnitude : referenceMagnitudes) {
            if (std::isfinite(magnitude)) magnitude += offsetDb;
        }
    }

    if (scanVisualSource == QStringLiteral("agile") &&
        fftBlockMetadata.valid &&
        fftBlockMetadata.scanIndex >= 0 &&
        fftBlockMetadata.scanIndex < activeAgileScanFrequencies.size()) {
        scanIndexBeforeFft = fftBlockMetadata.scanIndex;
        scanIndexAfterFft = fftBlockMetadata.scanIndex;
        scanCenterBeforeFft = fftBlockMetadata.centerFrequencyHz;
        scanCenterAfterFft = fftBlockMetadata.centerFrequencyHz;
        scanCenterFrequency = fftBlockMetadata.centerFrequencyHz;
        if (std::isfinite(scanCenterFrequency) && spectrumSettings.inputMode == INPUT_RF) {
            spectrumSettings.centerFrequency = scanCenterFrequency;
            spectrumSettings.actualFrequency = scanCenterFrequency;
        }
    } else if (scanVisualSource == QStringLiteral("standard") &&
               !activeStandardScanFrequencies.isEmpty()) {
        scanIndexAfterFft = (std::clamp)(standardScanIndex,
                                         0,
                                         activeStandardScanFrequencies.size() - 1);
        scanCenterAfterFft = activeStandardScanFrequencies.at(scanIndexAfterFft);
    }

    updateSpurCalibration(spectrumFrequencies, spectrumMagnitudes, spectrumSettings.centerFrequency);
    applySpurSuppression(spectrumFrequencies, spectrumMagnitudes, spectrumSettings.centerFrequency);
    updateGnssSpurWatch(spectrumFrequencies, spectrumMagnitudes, spectrumSettings.centerFrequency);
    updateScanMeasurement(spectrumFrequencies, spectrumMagnitudes);

    const std::vector<float> *displayFrequenciesPtr = &spectrumFrequencies;
    const std::vector<float> *displayMagnitudesPtr = &spectrumMagnitudes;
    const std::vector<float> *displayReferenceMagnitudesPtr = &referenceMagnitudes;
    const std::vector<float> *displayMeasurementFrequenciesPtr = &spectrumFrequencies;
    const std::vector<float> *dmrHunterFrequenciesPtr = &spectrumFrequencies;
    const std::vector<float> *dmrHunterMagnitudesPtr = &spectrumMagnitudes;
    const std::vector<float> *fpvHunterFrequenciesPtr = &spectrumFrequencies;
    const std::vector<float> *fpvHunterMagnitudesPtr = &spectrumMagnitudes;
    const std::vector<float> *digitalVideoHunterFrequenciesPtr = &spectrumFrequencies;
    const std::vector<float> *digitalVideoHunterMagnitudesPtr = &spectrumMagnitudes;
    double displayCenterFrequency = spectrumSettings.centerFrequency;
    double displayMinFrequency = frameMinFrequency;
    double displayMaxFrequency = frameMaxFrequency;
    int displayFftLength = static_cast<int>(spectrumFrequencies.size());
    QVector<ScanVisualSegment> displayScanSegments;
    bool displayScanSegmentMarkers = false;
    bool updateWaterfallFrame = true;
    bool scanVisualFrameValid = false;
    bool scanVisualWindowed = false;
    int scanVisualSelectedIndex = -1;
    double scanVisualSelectedCenter = std::numeric_limits<double>::quiet_NaN();
    double scanVisualFrameMin = std::numeric_limits<double>::quiet_NaN();
    double scanVisualFrameMax = std::numeric_limits<double>::quiet_NaN();
    ScanVisualFrame scanFrame;
    ScanVisualFrame visibleScanFrame;
    const bool agileScanVisualActive =
        agileScanRunning &&
        activeFobosApiKind == FobosApiKind::Agile &&
        spectrumSettings.inputMode == INPUT_RF &&
        activeAgileScanFrequencies.size() > 1;
    const bool standardScanVisualActive =
        standardScanRunning &&
        spectrumSettings.inputMode == INPUT_RF &&
        activeStandardScanFrequencies.size() > 1;
    const bool scanVisualActive = agileScanVisualActive || standardScanVisualActive;
    const QVector<double> &scanVisualFrequencies =
        standardScanVisualActive ? activeStandardScanFrequencies : activeAgileScanFrequencies;
    if (scanVisualActive) {
        scanVisualSelectedIndex =
            nearestScanFrequencyIndex(scanVisualFrequencies, spectrumSettings.centerFrequency);
        if (scanVisualSelectedIndex >= 0 && scanVisualSelectedIndex < scanVisualFrequencies.size()) {
            scanVisualSelectedCenter = scanVisualFrequencies.at(scanVisualSelectedIndex);
        }
        int scanVisualBins = 4096;
        if (graphWidget && graphWidget->width() > 0) {
            scanVisualBins = (std::max)(scanVisualBins, graphWidget->width() * 2);
        }
        if (waterfallWidget && waterfallWidget->width() > 0) {
            scanVisualBins = (std::max)(scanVisualBins, waterfallWidget->width() * 2);
        }
        if (scanVisualAssembler.configure(scanVisualFrequencies,
                                          spectrumSettings.sampleRate,
                                          scanVisualBins,
                                          scanVisualModeFromInt(scanVisualMode))) {
            scanFrame = scanVisualAssembler.update(spectrumSettings.centerFrequency,
                                                   spectrumFrequencies,
                                                   spectrumMagnitudes,
                                                   referenceMagnitudes);
            if (scanFrame.valid) {
                scanVisualFrameValid = true;
                scanVisualFrameMin = scanFrame.minFrequency;
                scanVisualFrameMax = scanFrame.maxFrequency;
                const double scanFullSpanHz = scanFrame.maxFrequency - scanFrame.minFrequency;
                const double scanVisibleSpanHz =
                    std::isfinite(scanFullSpanHz) && scanFullSpanHz > 0.0
                        ? scanFullSpanHz * (effectiveScalePercent() / 100.0)
                        : scanFullSpanHz;
                const double scanVisibleCenterHz =
                    displayFrequencyForScanActual(spectrumDisplayCenterHz,
                                                  scanFrame.segments,
                                                  scanFrame.centerFrequency);
                const ScanVisualFrame *displayScanFrame = &scanFrame;
                if (std::isfinite(scanFullSpanHz) &&
                    scanFullSpanHz > 0.0 &&
                    scanVisibleSpanHz < scanFullSpanHz * 0.999) {
                    visibleScanFrame =
                        windowedScanVisualFrame(scanFrame, scanVisibleCenterHz, scanVisibleSpanHz);
                    displayScanFrame = &visibleScanFrame;
                    scanVisualWindowed = true;
                }

                displayFrequenciesPtr = &displayScanFrame->frequencies;
                displayMagnitudesPtr = &displayScanFrame->magnitudes;
                displayReferenceMagnitudesPtr = &displayScanFrame->referenceMagnitudes;
                displayMeasurementFrequenciesPtr = &displayScanFrame->actualFrequencies;
                displayCenterFrequency = displayScanFrame->centerFrequency;
                displayMinFrequency = displayScanFrame->minFrequency;
                displayMaxFrequency = displayScanFrame->maxFrequency;
                displayFftLength = displayScanFrame->fftLength;
                displayScanSegments = displayScanFrame->segments;
                displayScanSegmentMarkers = displayScanFrame->showSegmentMarkers;
                updateWaterfallFrame = displayScanFrame->fresh;
                if (displayScanFrame->actualFrequencies.size() == displayScanFrame->magnitudes.size()) {
                    dmrHunterFrequenciesPtr = &displayScanFrame->actualFrequencies;
                    dmrHunterMagnitudesPtr = &displayScanFrame->magnitudes;
                    fpvHunterFrequenciesPtr = &displayScanFrame->actualFrequencies;
                    fpvHunterMagnitudesPtr = &displayScanFrame->magnitudes;
                    digitalVideoHunterFrequenciesPtr = &displayScanFrame->actualFrequencies;
                    digitalVideoHunterMagnitudesPtr = &displayScanFrame->magnitudes;
                }
            }
        }
    } else {
        scanVisualAssembler.reset();
    }

    if (!scanVisualActive || !verboseLogging) {
        scanVisualDebugFramesRemaining = 0;
        scanVisualDebugSequence = 0;
    } else {
        if (scanVisualDebugFramesRemaining <= 0 && scanVisualDebugSequence == 0) {
            scanVisualDebugFramesRemaining = 48;
            qDebug() << "[ScanVisualDebug] capture begin"
                     << "source" << scanVisualSource
                     << "mode" << normalizedScanVisualMode(scanVisualMode)
                     << "points" << scanVisualFrequencies.size()
                     << "sampleRate" << spectrumSettings.sampleRate
                     << "fftLength" << pendingSettings.fftLength
                     << "rowsPerFrame" << waterfallRowsPerFrame
                     << "updateMs" << updateTimer->interval();
        }
        if (scanVisualDebugFramesRemaining > 0) {
            const IqBuffer::Stats iqStats = IqBuffer::stats();
            const double inputFirstHz =
                spectrumFrequencies.empty()
                    ? std::numeric_limits<double>::quiet_NaN()
                    : static_cast<double>(spectrumFrequencies.front());
            const double inputLastHz =
                spectrumFrequencies.empty()
                    ? std::numeric_limits<double>::quiet_NaN()
                    : static_cast<double>(spectrumFrequencies.back());
            const double displayFirstHz =
                displayFrequenciesPtr->empty()
                    ? std::numeric_limits<double>::quiet_NaN()
                    : static_cast<double>(displayFrequenciesPtr->front());
            const double displayLastHz =
                displayFrequenciesPtr->empty()
                    ? std::numeric_limits<double>::quiet_NaN()
                    : static_cast<double>(displayFrequenciesPtr->back());
            qDebug() << "[ScanVisualDebug] row"
                     << "seq" << static_cast<qulonglong>(++scanVisualDebugSequence)
                     << "source" << scanVisualSource
                     << "idxBefore0" << scanIndexBeforeFft
                     << "idxAfter0" << scanIndexAfterFft
                     << "idxBefore1" << (scanIndexBeforeFft >= 0 ? scanIndexBeforeFft + 1 : -1)
                     << "idxAfter1" << (scanIndexAfterFft >= 0 ? scanIndexAfterFft + 1 : -1)
                     << "centerBeforeMHz" << (scanCenterBeforeFft / 1000000.0)
                     << "centerAfterMHz" << (scanCenterAfterFft / 1000000.0)
                     << "centerUsedMHz" << (spectrumSettings.centerFrequency / 1000000.0)
                     << "metaValid" << fftBlockMetadata.valid
                     << "metaTuning" << fftBlockMetadata.tuning
                     << "metaIndex0" << fftBlockMetadata.scanIndex
                     << "metaIndex1" << (fftBlockMetadata.scanIndex >= 0 ? fftBlockMetadata.scanIndex + 1 : -1)
                     << "metaCenterMHz" << (fftBlockMetadata.centerFrequencyHz / 1000000.0)
                     << "metaSeq" << static_cast<qulonglong>(fftBlockMetadata.sequence)
                     << "selected0" << scanVisualSelectedIndex
                     << "selected1" << (scanVisualSelectedIndex >= 0 ? scanVisualSelectedIndex + 1 : -1)
                     << "selectedMHz" << (scanVisualSelectedCenter / 1000000.0)
                     << "frameValid" << scanVisualFrameValid
                     << "fresh" << updateWaterfallFrame
                     << "windowed" << scanVisualWindowed
                     << "inputFirstMHz" << (inputFirstHz / 1000000.0)
                     << "inputLastMHz" << (inputLastHz / 1000000.0)
                     << "displayFirstMHz" << (displayFirstHz / 1000000.0)
                     << "displayLastMHz" << (displayLastHz / 1000000.0)
                     << "frameMinMHz" << (scanVisualFrameMin / 1000000.0)
                     << "frameMaxMHz" << (scanVisualFrameMax / 1000000.0)
                     << "iqSeq" << static_cast<qulonglong>(iqStats.sequence)
                     << "iqSnapshotFloats" << iqStats.snapshotSize
                     << "queuedBlocks" << iqStats.queuedBlocks;
            --scanVisualDebugFramesRemaining;
            if (scanVisualDebugFramesRemaining == 0) {
                qDebug() << "[ScanVisualDebug] capture end"
                         << "rows" << static_cast<qulonglong>(scanVisualDebugSequence);
            }
        }
    }
    const std::vector<float> &displayFrequencies = *displayFrequenciesPtr;
    const std::vector<float> &displayMagnitudes = *displayMagnitudesPtr;
    const std::vector<float> &displayReferenceMagnitudes = *displayReferenceMagnitudesPtr;
    const std::vector<float> &displayMeasurementFrequencies = *displayMeasurementFrequenciesPtr;
    const std::vector<float> &dmrHunterFrequencies = *dmrHunterFrequenciesPtr;
    const std::vector<float> &dmrHunterMagnitudes = *dmrHunterMagnitudesPtr;
    const std::vector<float> &fpvHunterFrequencies = *fpvHunterFrequenciesPtr;
    const std::vector<float> &fpvHunterMagnitudes = *fpvHunterMagnitudesPtr;
    const std::vector<float> &digitalVideoHunterFrequencies = *digitalVideoHunterFrequenciesPtr;
    const std::vector<float> &digitalVideoHunterMagnitudes = *digitalVideoHunterMagnitudesPtr;
    if (dmrHunterSettings.enabled) {
        updateDmrHunter(dmrHunterFrequencies, dmrHunterMagnitudes);
    }
    if (fpvHunterSettings.enabled) {
        updateFpvHunter(fpvHunterFrequencies, fpvHunterMagnitudes);
    }
    if (digitalVideoHunterSettings.enabled) {
        updateDigitalVideoHunter(digitalVideoHunterFrequencies, digitalVideoHunterMagnitudes);
    }

    const std::vector<float> *visualMagnitudesPtr = &displayMagnitudes;
    std::vector<float> baselineRawOverlay;
    if (isDirectInputMode(spectrumSettings.inputMode) &&
        buildHfInterferenceBaselineVisual(displayFrequencies,
                                          displayMagnitudes,
                                          hfInterferenceVisualMagnitudes)) {
        visualMagnitudesPtr = &hfInterferenceVisualMagnitudes;
        if (hfInterferenceRawOverlayEnabled &&
            displayMagnitudes.size() == hfInterferenceVisualMagnitudes.size()) {
            baselineRawOverlay = displayMagnitudes;
        }
    }
    const std::vector<float> &visualMagnitudes = *visualMagnitudesPtr;
    if ((spectrumFrameBufferEnabled || spectrumFrameRecorder.isRecording()) &&
        updateWaterfallFrame &&
        !displayFrequencies.empty() &&
        displayFrequencies.size() == visualMagnitudes.size()) {
        const int bins = spectrumFrameBinsCombo ? spectrumFrameBinsCombo->currentData().toInt() : 4096;
        SpectrumFrameRecord frame =
            SpectrumFrameRecorder::makeFrame(displayFrequencies,
                                             visualMagnitudes,
                                             displayCenterFrequency,
                                             displayMinFrequency,
                                             displayMaxFrequency,
                                             displayFftLength,
                                             bins,
                                             QDateTime::currentMSecsSinceEpoch());
        if (!frame.magnitudes.empty()) {
            if (spectrumFrameBufferEnabled) {
                spectrumFramePrebuffer.push_back(frame);
                const qint64 keepMs = static_cast<qint64>((std::max)(1, spectrumFramePrebufferSeconds)) * 1000;
                while (!spectrumFramePrebuffer.empty() &&
                       frame.utcMs - spectrumFramePrebuffer.front().utcMs > keepMs) {
                    spectrumFramePrebuffer.pop_front();
                }
            }
            if (spectrumFrameRecorder.isRecording()) {
                if (!spectrumFrameRecorder.appendFrameRecord(frame)) {
                    stopSpectrumFrameRecording();
                    updateSpectrumFrameRecordingStatus(QStringLiteral("Spectrum recording failed: write error"));
                } else if ((spectrumFrameRecorder.frameCount() % 100) == 0) {
                    updateSpectrumFrameRecordingStatus(
                        QStringLiteral("Spectrum recording: %1 frames").arg(spectrumFrameRecorder.frameCount()));
                }
            } else if (spectrumFrameBufferEnabled && (spectrumFramePrebuffer.size() % 100) == 0) {
                updateSpectrumFrameRecordingStatus(
                    QStringLiteral("Spectrum frames: buffering %1 frames").arg(spectrumFramePrebuffer.size()));
            }
        }
    }

    if (spectrumTuningDebugFramesRemaining > 0 &&
        displayFrequencies.size() == displayMagnitudes.size() &&
        !displayFrequencies.empty()) {
        --spectrumTuningDebugFramesRemaining;

        std::array<int, 3> peakIndices = {-1, -1, -1};
        std::array<float, 3> peakLevels = {
            -std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity()};
        int listeningIndex = -1;
        double listeningDelta = std::numeric_limits<double>::max();
        for (int i = 0; i < static_cast<int>(displayFrequencies.size()); ++i) {
            const double frequency = displayFrequencies[static_cast<std::size_t>(i)];
            const float level = displayMagnitudes[static_cast<std::size_t>(i)];
            if (!std::isfinite(frequency) || !std::isfinite(level)) {
                continue;
            }

            const double delta = std::abs(frequency - pendingSettings.listeningFrequency);
            if (delta < listeningDelta) {
                listeningDelta = delta;
                listeningIndex = i;
            }

            for (int slot = 0; slot < 3; ++slot) {
                if (level <= peakLevels[static_cast<std::size_t>(slot)]) {
                    continue;
                }
                for (int move = 2; move > slot; --move) {
                    peakLevels[static_cast<std::size_t>(move)] = peakLevels[static_cast<std::size_t>(move - 1)];
                    peakIndices[static_cast<std::size_t>(move)] = peakIndices[static_cast<std::size_t>(move - 1)];
                }
                peakLevels[static_cast<std::size_t>(slot)] = level;
                peakIndices[static_cast<std::size_t>(slot)] = i;
                break;
            }
        }

        QStringList peakSummary;
        for (int slot = 0; slot < 3; ++slot) {
            const int index = peakIndices[static_cast<std::size_t>(slot)];
            if (index < 0) {
                continue;
            }
            const double frequency = displayFrequencies[static_cast<std::size_t>(index)];
            const double offset = frequency - displayCenterFrequency;
            peakSummary << QStringLiteral("%1MHz/%2kHz/%3dB")
                               .arg(frequency / 1000000.0, 0, 'f', 6)
                               .arg(offset / 1000.0, 0, 'f', 1)
                               .arg(peakLevels[static_cast<std::size_t>(slot)], 0, 'f', 1);
        }

        const float listeningLevel =
            listeningIndex >= 0
                ? displayMagnitudes[static_cast<std::size_t>(listeningIndex)]
                : std::numeric_limits<float>::quiet_NaN();
        qDebug() << "[SpectrumTune]"
                 << "center" << displayCenterFrequency
                 << "listening" << pendingSettings.listeningFrequency
                 << "actual" << pendingSettings.actualFrequency
                 << "range" << displayMinFrequency << displayMaxFrequency
                 << "sampleRate" << spectrumSettings.sampleRate
                 << "listenOffset" << (pendingSettings.listeningFrequency - displayCenterFrequency)
                 << "listeningBinDelta" << listeningDelta
                 << "listeningLevel" << listeningLevel
                 << "peaks" << peakSummary.join(QStringLiteral(", "));
    }

    if (traceFrame) {
        qDebug() << "[Spectrum] before graph"
                 << "elapsedMs" << traceTimer.elapsed()
                 << "freqCount" << displayFrequencies.size()
                 << "magCount" << displayMagnitudes.size()
                 << "scanVisual" << scanVisualActive;
    }
    const bool suppressLocalVisual =
        networkMode == NetworkMode::Server &&
        serverDisableLocalVisualAudio &&
        networkController &&
        networkController->isControlReady();
    if (!suppressLocalVisual) {
        const bool presentMainSpectrum = graphWidget && graphWidget->isVisible() && !isMinimized();
        const bool presentMainWaterfall = waterfallWidget && waterfallWidget->isVisible() && !isMinimized();
        if (scaleWidget && scaleWidget->isVisible() && !isMinimized()) {
            double scaleListening = pendingSettings.listeningFrequency;
            if (!displayScanSegments.isEmpty()) {
                if (!scanListeningLockEnabled &&
                    !actualFrequencyInsideScanSegments(scaleListening, displayScanSegments)) {
                    scaleListening =
                        fallbackActualFrequencyForScanSegments(displayScanSegments, displayCenterFrequency);
                }
            }
            scaleWidget->setScanSegments(displayScanSegments);
            scaleWidget->setScanSegmentMarkersVisible(displayScanSegmentMarkers);
            scaleWidget->setTuning(scaleListening,
                                   displayCenterFrequency,
                                   pendingSettings.bandwidth,
                                   pendingSettings.modulationType);
            scaleWidget->setRange(displayMinFrequency, displayMaxFrequency);
        }
        const std::vector<float> &measurementFrequencies =
            displayMeasurementFrequencies.size() == displayMagnitudes.size()
                ? displayMeasurementFrequencies
                : displayFrequencies;
        const std::vector<float> measurementOverlay =
            scanMeasurementOverlay(measurementFrequencies, static_cast<int>(displayMagnitudes.size()));
        const std::vector<float> *displayOverlaySource = nullptr;
        if (!baselineRawOverlay.empty()) {
            displayOverlaySource = &baselineRawOverlay;
        } else if (!measurementOverlay.empty()) {
            displayOverlaySource = &measurementOverlay;
        } else if (pendingSettings.inputMode == INPUT_HF_NOISE_CANCEL &&
                   !displayReferenceMagnitudes.empty()) {
            displayOverlaySource = &displayReferenceMagnitudes;
        }

        const int displayTargetBins = (std::max)({512,
                                                  presentMainSpectrum ? graphWidget->width() : 0,
                                                  presentMainWaterfall ? waterfallWidget->width() : 0,
                                                  dspFlowPanel ? dspFlowPanel->workspaceSpectrumTargetBins() : 0});
        static SpectrumDisplayFrame preparedDisplayFrame;
        const qint64 preReduceNs = profileDisplayFrame
                                       ? displayProfileTimer.nsecsElapsed()
                                       : 0;
        prepareSpectrumDisplayFrame(displayFrequencies,
                                    visualMagnitudes,
                                    displayOverlaySource,
                                    displayFftLength,
                                    displayMinFrequency,
                                    displayMaxFrequency,
                                    displayTargetBins,
                                    displayLevelMin,
                                    static_cast<SpectrumDisplayReduction>(spectrumDisplayReductionMode),
                                    preparedDisplayFrame);
        if (dspFlowPanel && dspFlowPanel->shouldUpdateWorkspaceSpectrum()) {
            dspFlowPanel->updateWorkspaceSpectrum(preparedDisplayFrame.frequencies,
                                                  preparedDisplayFrame.levels,
                                                  displayCenterFrequency,
                                                  pendingSettings.listeningFrequency,
                                                  spectrumSettings.sampleRate,
                                                  pendingSettings.bandwidth,
                                                  pendingSettings.modulationType);
        }
        if (multiVfoWidget) {
            multiVfoWidget->setReceiverContext(displayCenterFrequency,
                                               spectrumSettings.sampleRate,
                                               pendingSettings.listeningFrequency,
                                               pendingSettings.bandwidth,
                                               pendingSettings.modulationType);
            multiVfoWidget->updateSpectrum(displayFrequencies,
                                           visualMagnitudes);
            if (dspFlowPanel && dspFlowPanel->shouldUpdateMultiVfoSpectrum()) {
                dspFlowPanel->updateMultiVfoSpectrum(multiVfoWidget->configurationJson(),
                                                     displayFrequencies,
                                                     visualMagnitudes);
            }
        }
        if (wirelessLabDialog && wirelessLabDialog->isVisible() && displayScanSegments.isEmpty())
            wirelessLabDialog->appendSpectrum(preparedDisplayFrame.frequencies,
                                               preparedDisplayFrame.levels, spectrumSettings.sampleRate);
        if (refreshAnalysis) {
            const std::vector<float> scienceFrequencies =
                !displayScanSegments.isEmpty()
                    ? actualFrequenciesFromScanSegments(preparedDisplayFrame.frequencies, displayScanSegments)
                    : std::vector<float>();
            updateSpectrumScience(scienceFrequencies.size() == preparedDisplayFrame.frequencies.size()
                                      ? scienceFrequencies
                                      : preparedDisplayFrame.frequencies,
                                  preparedDisplayFrame.levels);
            if (displayScanSegments.isEmpty()) {
                feedZeroSpanFrame(displayFrequencies, visualMagnitudes, true);
            } else {
                feedZeroSpanFrame(scienceFrequencies.size() == preparedDisplayFrame.frequencies.size()
                                      ? scienceFrequencies
                                      : preparedDisplayFrame.frequencies,
                                  preparedDisplayFrame.levels,
                                  false);
            }
        }
        const qint64 afterReduceNs = profileDisplayFrame
                                         ? displayProfileTimer.nsecsElapsed()
                                         : 0;

        if (refreshAuxiliaryUi && presentMainSpectrum) {
            graphWidget->setLevelRange(displayLevelMin, displayLevelMax);
            graphWidget->setSpectrumMetadata(displayCenterFrequency,
                                             pendingSettings.listeningFrequency,
                                             spectrumSettings.sampleRate,
                                             spectrumSettings.fftLength,
                                             spectrumSettings.fftWindowType);
            graphWidget->setScanSegments(displayScanSegments);
            graphWidget->setScanSegmentMarkersVisible(displayScanSegmentMarkers);
            graphWidget->setData(preparedDisplayFrame.frequencies,
                                 preparedDisplayFrame.levels,
                                 displayMinFrequency,
                                 displayMaxFrequency,
                                 static_cast<int>(preparedDisplayFrame.levels.size()),
                                 colorf,
                                 true);
            graphWidget->setOverlayData(preparedDisplayFrame.overlayLevels,
                                        !preparedDisplayFrame.overlayLevels.empty(),
                                        true);
        }
        if (traceFrame) {
            qDebug() << "[Spectrum] before waterfall" << "elapsedMs" << traceTimer.elapsed();
        }
        if (updateWaterfallFrame && presentMainWaterfall) {
            waterfallWidget->setSpectrumMetadata(displayCenterFrequency,
                                                 pendingSettings.listeningFrequency,
                                                 spectrumSettings.sampleRate,
                                                 spectrumSettings.fftLength,
                                                 spectrumSettings.fftWindowType);
            const IqBuffer::DiscontinuityStats discontinuity = IqBuffer::discontinuityStats();
            if (discontinuity.epoch != lastIqDiscontinuityEpoch) {
                lastIqDiscontinuityEpoch = discontinuity.epoch;
                lastIqDroppedQueuedBlocks = discontinuity.droppedQueuedBlocks;
                lastIqSkippedSnapshotBlocks = discontinuity.skippedSnapshotBlocks;
            } else {
                // Both kinds of diagnostic row are opt-in. Audio overflow alone
                // does not discard the independent FFT snapshot.
                if (showAudioQueueDropRows) {
                    if (discontinuity.droppedQueuedBlocks > lastIqDroppedQueuedBlocks) {
                        waterfallWidget->queueDiscontinuityRow(
                            MyWaterfallWidget::DiscontinuityKind::ProcessingQueueDrop);
                    } else if (discontinuity.skippedSnapshotBlocks > lastIqSkippedSnapshotBlocks) {
                        waterfallWidget->queueDiscontinuityRow(
                            MyWaterfallWidget::DiscontinuityKind::VisualSnapshotSkip);
                    }
                }
                lastIqDroppedQueuedBlocks = discontinuity.droppedQueuedBlocks;
                lastIqSkippedSnapshotBlocks = discontinuity.skippedSnapshotBlocks;
            }
            // FFT production is independent from the OpenGL repaint cadence.
            // Feed every completed intermediate row into the widget's existing
            // bounded texture queue, then use the newest row for the graph and
            // the normal analysis path above.
            SpectrumDisplayFrame historyDisplayFrame;
            std::vector<float> historyBaselineVisual;
            for (SpectrumFftHistoryRow &historyRow : waterfallHistoryRows) {
                if (historyRow.magnitudes.size() != spectrumFrequencies.size() ||
                    (historyRow.metadata.epoch != 0 &&
                     fftBlockMetadata.epoch != 0 &&
                     historyRow.metadata.epoch != fftBlockMetadata.epoch)) {
                    continue;
                }

                if (calibrationTableEnabled) {
                    receiverCalibrationTable.applyAmplitudeCorrection(
                        spectrumFrequencies,
                        historyRow.magnitudes,
                        &historyRow.referenceMagnitudes,
                        amplitudeCalibrationOffsetDb);
                } else if (std::isfinite(amplitudeCalibrationOffsetDb) &&
                           std::abs(amplitudeCalibrationOffsetDb) > 0.000001) {
                    const float offsetDb = static_cast<float>(amplitudeCalibrationOffsetDb);
                    for (float &magnitude : historyRow.magnitudes) {
                        if (std::isfinite(magnitude)) magnitude += offsetDb;
                    }
                }

                const std::vector<float> *historyVisual = &historyRow.magnitudes;
                if (isDirectInputMode(spectrumSettings.inputMode) &&
                    buildHfInterferenceBaselineVisual(spectrumFrequencies,
                                                      historyRow.magnitudes,
                                                      historyBaselineVisual)) {
                    historyVisual = &historyBaselineVisual;
                }
                prepareSpectrumDisplayFrame(spectrumFrequencies,
                                            *historyVisual,
                                            nullptr,
                                            displayFftLength,
                                            displayMinFrequency,
                                            displayMaxFrequency,
                                            displayTargetBins,
                                            displayLevelMin,
                                            static_cast<SpectrumDisplayReduction>(spectrumDisplayReductionMode),
                                            historyDisplayFrame);
                if (historyDisplayFrame.levels.empty()) {
                    continue;
                }
                waterfallWidget->setData(historyDisplayFrame.frequencies,
                                         historyDisplayFrame.levels,
                                         displayMinFrequency,
                                         displayMaxFrequency,
                                         static_cast<int>(historyDisplayFrame.levels.size()),
                                         secondGraph,
                                         colorf,
                                         contrast,
                                         sensitivity,
                                         displayLevelMin,
                                         displayLevelMax,
                                         true);
            }
            waterfallWidget->setData(preparedDisplayFrame.frequencies,
                                     preparedDisplayFrame.levels,
                                     displayMinFrequency,
                                     displayMaxFrequency,
                                     static_cast<int>(preparedDisplayFrame.levels.size()),
                                     secondGraph,
                                     colorf,
                                     contrast,
                                     sensitivity,
                                     displayLevelMin,
                                     displayLevelMax,
                                     true);
        }
        if (presentMainWaterfall) {
            waterfallWidget->setScanSegments(displayScanSegments);
            waterfallWidget->setScanSegmentMarkersVisible(displayScanSegmentMarkers);
        }
        if (profileDisplayFrame) {
            const qint64 afterWidgetsNs = displayProfileTimer.nsecsElapsed();
            recordSpectrumDisplayProfile(spectrumSettings.fftLength,
                                         preReduceNs,
                                         afterReduceNs - preReduceNs,
                                         afterWidgetsNs - afterReduceNs,
                                         afterWidgetsNs);
        }
    } else if (traceFrame) {
        qDebug() << "[Spectrum] local server visual update skipped" << "elapsedMs" << traceTimer.elapsed();
    }
    sendNetworkSpectrumFrame(displayFrequencies,
                             visualMagnitudes,
                             displayReferenceMagnitudes,
                             displayCenterFrequency,
                             displayMinFrequency,
                             displayMaxFrequency,
                             displayScanSegments,
                             updateWaterfallFrame,
                             displayScanSegmentMarkers);
    finishTrace("end", displayFrequencies, displayMagnitudes);
    advanceStandardScanIfNeeded();
}
