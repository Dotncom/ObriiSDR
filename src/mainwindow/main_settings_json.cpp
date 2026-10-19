#include "main.h"
#include "appconstants.h"
#include "appsettingsutils.h"
#include "diagnosticlogging.h"
#include "dmrbackendpaths.h"
#include "dmrprivacyutils.h"
#include "gnssqthhelpers.h"
#include "gnssserialutils.h"
#include "modulationutils.h"
#include "presethelpers.h"
#include "receiverdeviceutils.h"
#include "scanvisualutils.h"
#include "tuningutils.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>

extern bool secondGraph;
extern bool syncWariable;
extern float sensitivity;
extern float contrast;
extern bool colorf;
QJsonObject YourClassName::settingsToJson() const {
    QJsonObject settings;
    settings["deviceIndex"] = pendingSettings.deviceIndex;
    settings["clockSource"] = pendingSettings.clockSource;
    settings["inputMode"] = pendingSettings.inputMode;
    settings["centerFrequency"] = pendingSettings.centerFrequency;
    settings["actualFrequency"] = pendingSettings.actualFrequency;
    settings["listeningFrequency"] = pendingSettings.listeningFrequency;
    settings["sampleRate"] = pendingSettings.sampleRate;
    settings["bandwidth"] = pendingSettings.bandwidth;
    settings["modulationType"] = pendingSettings.modulationType;
    settings["cwDecoderSelectivity"] = pendingSettings.cwDecoderSelectivity;
    settings["pskDecoderToneHz"] = pendingSettings.pskDecoderToneHz;
    settings["pskDecoderSquelchPercent"] = pendingSettings.pskDecoderSquelchPercent;
    settings["pskDecoderAfc"] = pendingSettings.pskDecoderAfc;
    settings["pskDecoderEncoding"] = pendingSettings.pskDecoderEncoding;
    settings["sstvDemodulationMode"] =
        normalizedSstvDemodulationMode(pendingSettings.sstvDemodulationMode);
    settings["fftLength"] = pendingSettings.fftLength;
    settings["fftBinWidthModeEnabled"] = fftBinWidthModeEnabled;
    settings["fftTargetBinWidthHz"] = fftTargetBinWidthHz;
    settings["fftWindowType"] = normalizedFftWindowType(pendingSettings.fftWindowType);
    settings["lnaGain"] = pendingSettings.lnaGain;
    settings["vgaGain"] = pendingSettings.vgaGain;
    settings["hackRfLnaGainDb"] = pendingSettings.hackRfLnaGainDb;
    settings["hackRfVgaGainDb"] = pendingSettings.hackRfVgaGainDb;
    settings["hackRfAmpEnabled"] = pendingSettings.hackRfAmpEnabled;
    settings["hackRfBiasTeeEnabled"] = pendingSettings.hackRfBiasTeeEnabled;
    settings["hackRfAutomaticBandwidth"] = pendingSettings.hackRfAutomaticBandwidth;
    settings["hackRfBandwidthHz"] = pendingSettings.hackRfBandwidthHz;
    settings["hackRfExplicitTuningEnabled"] = pendingSettings.hackRfExplicitTuningEnabled;
    settings["hackRfExplicitIfHz"] = pendingSettings.hackRfExplicitIfHz;
    settings["hackRfExplicitLoHz"] = pendingSettings.hackRfExplicitLoHz;
    settings["hackRfExplicitPath"] = pendingSettings.hackRfExplicitPath;
    settings["hackRfClockOutEnabled"] = pendingSettings.hackRfClockOutEnabled;
    settings["hackRfHardwareSyncEnabled"] = pendingSettings.hackRfHardwareSyncEnabled;
    settings["hackRfRxOverrunLimit"] = pendingSettings.hackRfRxOverrunLimit;
    settings["hackRfOperaCakeEnabled"] = pendingSettings.hackRfOperaCakeEnabled;
    settings["hackRfOperaCakeAddress"] = pendingSettings.hackRfOperaCakeAddress;
    settings["hackRfOperaCakeMode"] = pendingSettings.hackRfOperaCakeMode;
    settings["hackRfOperaCakePortA"] = pendingSettings.hackRfOperaCakePortA;
    settings["hackRfOperaCakePortB"] = pendingSettings.hackRfOperaCakePortB;
    settings["hackRfOperaCakeRangesJson"] = pendingSettings.hackRfOperaCakeRangesJson;
    settings["hackRfOperaCakeDwellsJson"] = pendingSettings.hackRfOperaCakeDwellsJson;
    settings["hackRfSweepRanges"] = pendingSettings.hackRfSweepRanges;
    settings["hackRfSweepStepHz"] = pendingSettings.hackRfSweepStepHz;
    settings["hackRfSweepBytesPerTune"] = pendingSettings.hackRfSweepBytesPerTune;
    settings["hackRfSweepInterleaved"] = pendingSettings.hackRfSweepInterleaved;
    settings["rtlAgc"] = pendingSettings.rtlAgc;
    settings["rtlTunerGainTenthsDb"] = pendingSettings.rtlTunerGainTenthsDb;
    settings["audioLowPassHz"] = pendingSettings.audioLowPassHz;
    settings["audioHighPassHz"] = pendingSettings.audioHighPassHz;
    settings["audioFilterChainJson"] = pendingSettings.audioFilterChainJson;
    settings["simplifiedAudioChannelizer"] = pendingSettings.simplifiedAudioChannelizer;
    settings["hfNoiseCancelDepth"] = pendingSettings.hfNoiseCancelDepth;
    settings["hfNoiseCancelRefGainDb"] = pendingSettings.hfNoiseCancelRefGainDb;
    settings["hfNoiseCancelRefDelayNs"] = pendingSettings.hfNoiseCancelRefDelayNs;
    settings["hfNoiseCancelRefTiltDb"] = pendingSettings.hfNoiseCancelRefTiltDb;
    settings["hfNoiseCancelFreeze"] = pendingSettings.hfNoiseCancelFreeze;
    settings["hfAudioBlankerEnabled"] = pendingSettings.hfAudioBlankerEnabled;
    settings["hfAudioBlankerThreshold"] = pendingSettings.hfAudioBlankerThreshold;
    settings["audioEnabled"] = pendingSettings.audioEnabled;
    settings["syncEnabled"] = false;
    settings["gpoValue"] = static_cast<int>(pendingSettings.gpoValue);
    settings["dmrBasebandSampleRate"] = normalizedDmrBasebandSampleRate(pendingSettings.dmrBasebandSampleRate);
    settings["dmrAmbeLayout"] = normalizedDmrAmbeLayout(pendingSettings.dmrAmbeLayout);
    settings["dmrManualTimingEnabled"] = pendingSettings.dmrManualTimingEnabled;
    settings["dmrManualTimingOffset"] = pendingSettings.dmrManualTimingOffset;
    settings["dmrSlicerRatio"] = pendingSettings.dmrSlicerRatio;
    settings["dmrAdaptiveSlicer"] = pendingSettings.dmrAdaptiveSlicer;
    settings["dmrPrivacyMode"] = pendingSettings.dmrPrivacyMode;
    settings["dmrPrivacyKeyId"] = pendingSettings.dmrPrivacyKeyId;
    settings["dmrPrivacyKeyHex"] = pendingSettings.dmrPrivacyKeyHex;
    settings["dmrPrivacyVariant"] = pendingSettings.dmrPrivacyVariant;
    settings["dmrPrivacyLayout"] = pendingSettings.dmrPrivacyLayout;
    settings["dmrPrivacyFrameOffset"] = pendingSettings.dmrPrivacyFrameOffset;
    settings["scalePercent"] = currentScale;
    settings["additionalScaleDivisor"] = additionalScaleDivisor;
    settings["agileScanEnabled"] = agileScanEnabled;
    settings["agileScanAutoStepSampleRate"] = agileScanAutoStepSampleRate;
    settings["scanVisualMode"] = normalizedScanVisualMode(scanVisualMode);
    settings["agileScanRangesMhz"] = agileScanRangesMhz;
    settings["agileScanStepMhz"] = agileScanStepMhz;
    settings["scanListeningLockEnabled"] = scanListeningLockEnabled;
    settings["standardScanEnabled"] = standardScanEnabled;
    settings["standardScanCentersMhz"] = standardScanCentersMhz;
    settings["standardScanDwellMs"] = standardScanDwellMs;
    settings["standardScanSettleMs"] = standardScanSettleMs;
    settings["standardScanRangeStartMhz"] = standardScanRangeStartMhz;
    settings["standardScanRangeEndMhz"] = standardScanRangeEndMhz;
    settings["listeningScanEnabled"] = listeningScanEnabled;
    settings["listeningScanTargetsMhz"] = listeningScanTargetsMhz;
    settings["listeningScanDwellMs"] = listeningScanDwellMs;
    settings["listeningScanSettleMs"] = listeningScanSettleMs;
    settings["scanMeasurementEnabled"] = scanMeasurementEnabled;
    settings["waterfallAreaMeasurementEnabled"] = waterfallAreaMeasurementEnabled;
    settings["scanMeasurementBinMhz"] = scanMeasurementBinMhz;
    settings["scanMeasurementUpdateIntervalMs"] = scanMeasurementUpdateIntervalMs;
    settings["spectrumScienceMaxHold"] = spectrumScienceMaxHoldEnabled;
    settings["spectrumScienceMarkers"] = spectrumScienceAnalyzer.markerConfiguration();
    settings["spectrumScienceMinHold"] = spectrumScienceMinHoldEnabled;
    settings["spectrumScienceAverage"] = spectrumScienceAverageEnabled;
    settings["spectrumScienceAverageSeconds"] = spectrumScienceAverageSeconds;
    settings["spectrumDetectorMode"] = spectrumDetectorMode;
    settings["spectrumDetectorFrames"] = spectrumDetectorFrames;
    settings["spectrumVbwHz"] = spectrumVbwHz;
    settings["spectrumFftOverlapPercent"] = spectrumFftOverlapPercent;
    settings["spectrumAverageFrameCount"] = spectrumAverageFrameCount;
    settings["spectrumPercentile50"] = spectrumPercentile50Enabled;
    settings["spectrumPercentile90"] = spectrumPercentile90Enabled;
    settings["spectrumPercentile99"] = spectrumPercentile99Enabled;
    settings["spectrumAmplitudeUnit"] = spectrumAmplitudeUnit;
    settings["extendedRecordingMetadata"] = extendedRecordingMetadataEnabled;
    settings["qthLatitude"] = qthLatitude;
    settings["qthLongitude"] = qthLongitude;
    settings["qthPositionVisible"] = qthPositionVisible;
    settings["qthSource"] = qthSource;
    settings["gnssSerialPortName"] = gnssSerialPortName;
    settings["gnssSerialBaud"] = gnssSerialBaud;
    settings["gnssPositionPolicy"] = normalizedGnssPositionPolicy(gnssPositionPolicy);
    settings["gnssUbxAutoEnable"] = gnssUbxAutoEnable;
    settings["gnssTimeZoneOffsetMinutes"] = gnssTimeZoneOffsetMinutes;
    settings["gnssSatelliteTableVisible"] = gnssSatelliteTableVisible;
    settings["qthTileDirectory"] = qthTileDirectory;
    settings["qthMapLayer"] = qthMapLayer;
    settings["qthMapZoom"] = qthMapZoom;
    settings["qthGridPrecision"] = qthGridPrecision;
    settings["qthMapOverlayMode"] = qthMapOverlayMode;
    settings["qthOnlineProviderId"] = qthOnlineProviderId;
    settings["qthOnlineTileUrlTemplate"] = qthOnlineTileUrlTemplate;
    settings["qthOnlineAttribution"] = qthOnlineAttribution;
    settings["qthOnlineApiKey"] = qthOnlineApiKey;
    settings["qthOnlineNoDiskCache"] = qthOnlineNoDiskCache;
    settings["gnssSystemId"] = gnssSystemId;
    settings["gnssMonitorEnabled"] = gnssMonitorEnabled;
    settings["gnssUseGps"] = gnssUseGps;
    settings["gnssUseGlonass"] = gnssUseGlonass;
    settings["gnssUseGalileo"] = gnssUseGalileo;
    settings["gnssUseBeidou"] = gnssUseBeidou;
    settings["gnssUseQzss"] = gnssUseQzss;
    settings["gnssUseSbas"] = gnssUseSbas;
    settings["gnssUseOther"] = gnssUseOther;
    QJsonArray gnssDisabledSatellites;
    QStringList disabledSatelliteKeys = gnssDisabledSatelliteKeys.values();
    disabledSatelliteKeys.sort();
    for (const QString &key : disabledSatelliteKeys) {
        if (!key.trimmed().isEmpty()) {
            gnssDisabledSatellites.append(key);
        }
    }
    settings["gnssDisabledSatellites"] = gnssDisabledSatellites;
    settings["gnssAcquisitionIntegrationMs"] = gnssAcquisitionIntegrationMs;
    settings["gnssChannelFilterCutoffHz"] = gnssChannelFilterCutoffHz;
    settings["gnssDopplerSpanHz"] = gnssDopplerSpanHz;
    settings["gnssDopplerStepHz"] = gnssDopplerStepHz;
    settings["gnssContinuousAcquisitionEnabled"] = gnssContinuousAcquisitionEnabled;
    settings["gnssContinuousAcquisitionIntervalMs"] = gnssContinuousAcquisitionIntervalMs;
    QJsonArray qthMarkers;
    for (const qth::UserMarker &marker : qthUserMarkers) {
        if (!qth::isValidLatitude(marker.latitude) || !qth::isValidLongitude(marker.longitude)) {
            continue;
        }
        QJsonObject object;
        object["number"] = marker.number;
        object["name"] = marker.name.trimmed();
        object["description"] = marker.description.trimmed();
        object["latitude"] = marker.latitude;
        object["longitude"] = marker.longitude;
        qthMarkers.append(object);
    }
    settings["qthMarkers"] = qthMarkers;
    settings["spectrumUpdateIntervalMs"] = spectrumUpdateIntervalMs;
    settings["fftBackendPreference"] = normalizedFftBackendPreference(fftBackendPreference);
    settings["frequencyCalibrationOffsetHz"] = frequencyCalibrationOffsetHz;
    settings["amplitudeCalibrationOffsetDb"] = amplitudeCalibrationOffsetDb;
    settings["calibrationTableEnabled"] = calibrationTableEnabled;
    QJsonArray calibrationPointsJson;
    for (const ReceiverCalibrationPoint &point : receiverCalibrationTable.points()) {
        QJsonObject object;
        object["frequencyHz"] = point.frequencyHz;
        object["frequencyOffsetHz"] = point.frequencyOffsetHz;
        object["amplitudeOffsetDb"] = point.amplitudeOffsetDb;
        object["uncertaintyDb"] = point.uncertaintyDb;
        object["note"] = point.note;
        object["conditions"] = point.conditions.toJson();
        calibrationPointsJson.append(object);
    }
    settings["calibrationTable"] = calibrationPointsJson;
    settings["waterfallRowsPerFrame"] = waterfallRowsPerFrame;
    settings["waterfallDisplayMode"] = waterfallDisplayMode;
    settings["waterfall3DResolutionDivisor"] = waterfall3DResolutionDivisor;
    settings["waterfall3DSurfaceStyle"] = waterfall3DSurfaceStyle;
    settings["waterfall3DSmoothing"] = waterfall3DSmoothing;
    settings["waterfall3DLighting"] = waterfall3DLighting;
    settings["waterfall3DHistoryRows"] = waterfall3DHistoryRows;
    settings["waterfall3DSliceScrollStep"] = waterfall3DSliceScrollStep;
    settings["waterfall3DSliceWidth"] = waterfall3DSliceWidth;
    settings["waterfall3DSpectrumSliceScrollStep"] = waterfall3DSpectrumSliceScrollStep;
    settings["waterfall3DSpectrumSliceRows"] = waterfall3DSpectrumSliceRows;
    settings["waterfall3DSpectrumSliceCapture"] = waterfall3DSpectrumSliceCapture;
    settings["waterfall3DSpectrumSliceCaptureFixed"] = waterfall3DSpectrumSliceCaptureFixed;
    settings["waterfall3DFixedPlane"] = waterfall3DFixedPlane;
    settings["waterfall3DMonochrome"] = waterfall3DMonochrome;
    settings["waterfall3DHeightGradient"] = waterfall3DHeightGradient;
    settings["waterfall3DMonochromeColor"] =
        waterfall3DMonochromeColor.name(QColor::HexRgb);
    settings["waterfall3DMonochromeGradientDepth"] =
        waterfall3DMonochromeGradientDepth;
    settings["waterfall3DAlternativeView"] = waterfall3DAlternativeView;
    settings["waterfall3DVncSliceInput"] = waterfall3DVncSliceInput;
    settings["alternativeInterfaceMode"] = alternativeInterfaceMode;
    settings["alternativeSpectrumGradientFill"] = alternativeSpectrumGradientFill;
    settings["alternativeSpectrumGradientOpacity"] = alternativeSpectrumGradientOpacity;
    settings["experimentalGpuWaterfall"] = experimentalGpuWaterfall;
    settings["spectrumDisplayReductionMode"] = spectrumDisplayReductionMode;
    settings["spurSuppressionEnabled"] = spurSuppressionEnabled;
    QJsonArray spurMask;
    for (const SpurMaskEntry &entry : spurMaskEntries) {
        if (!std::isfinite(entry.offsetHz) ||
            !std::isfinite(entry.widthHz) ||
            entry.widthHz <= 0.0) {
            continue;
        }
        QJsonObject object;
        object["offsetHz"] = entry.offsetHz;
        object["widthHz"] = entry.widthHz;
        object["prominenceDb"] = entry.prominenceDb;
        object["hits"] = entry.hits;
        spurMask.append(object);
    }
    settings["spurMask"] = spurMask;
    return settings;
}

void YourClassName::applySettingsFromJson(const QJsonObject &settingsJson, bool normalizeAfterApply) {
    auto readInt = [&settingsJson](const char *key, int currentValue) {
        return settingsJson.contains(key) ? settingsJson.value(key).toInt(currentValue) : currentValue;
    };
    auto readDouble = [&settingsJson](const char *key, double currentValue) {
        return settingsJson.contains(key) ? settingsJson.value(key).toDouble(currentValue) : currentValue;
    };
    auto readBool = [&settingsJson](const char *key, bool currentValue) {
        return settingsJson.contains(key) ? settingsJson.value(key).toBool(currentValue) : currentValue;
    };
    auto readString = [&settingsJson](const char *key, const QString &currentValue) {
        return settingsJson.contains(key) ? settingsJson.value(key).toString(currentValue) : currentValue;
    };

    pendingSettings.deviceIndex = readInt("deviceIndex", pendingSettings.deviceIndex);
    pendingSettings.clockSource = readInt("clockSource", pendingSettings.clockSource);
    pendingSettings.inputMode = (std::clamp)(readInt("inputMode", pendingSettings.inputMode),
                                             static_cast<int>(INPUT_RF),
                                             static_cast<int>(INPUT_HF_NOISE_CANCEL));
    pendingSettings.centerFrequency = readDouble("centerFrequency", pendingSettings.centerFrequency);
    pendingSettings.actualFrequency = readDouble("actualFrequency", pendingSettings.actualFrequency);
    pendingSettings.listeningFrequency = readDouble("listeningFrequency", pendingSettings.listeningFrequency);
    pendingSettings.sampleRate = readDouble("sampleRate", pendingSettings.sampleRate);
    pendingSettings.bandwidth = readDouble("bandwidth", pendingSettings.bandwidth);
    pendingSettings.modulationType = readInt("modulationType", pendingSettings.modulationType);
    pendingSettings.cwDecoderSelectivity = (std::clamp)(
        readInt("cwDecoderSelectivity", pendingSettings.cwDecoderSelectivity), 1, 10);
    pendingSettings.pskDecoderToneHz = std::clamp(readDouble("pskDecoderToneHz", pendingSettings.pskDecoderToneHz),100.0,3500.0);
    pendingSettings.pskDecoderSquelchPercent = std::clamp(readInt("pskDecoderSquelchPercent", pendingSettings.pskDecoderSquelchPercent),1,100);
    pendingSettings.pskDecoderAfc = readBool("pskDecoderAfc", pendingSettings.pskDecoderAfc);
    pendingSettings.pskDecoderEncoding = std::clamp(readInt("pskDecoderEncoding", pendingSettings.pskDecoderEncoding),0,1);
    if (pskDecoderToneSpin) { QSignalBlocker b(pskDecoderToneSpin); pskDecoderToneSpin->setValue(pendingSettings.pskDecoderToneHz); }
    if (pskDecoderSquelchSpin) { QSignalBlocker b(pskDecoderSquelchSpin); pskDecoderSquelchSpin->setValue(pendingSettings.pskDecoderSquelchPercent); }
    if (pskDecoderAfcCheckbox) { QSignalBlocker b(pskDecoderAfcCheckbox); pskDecoderAfcCheckbox->setChecked(pendingSettings.pskDecoderAfc); }
    if (pskDecoderEncodingCombo) { QSignalBlocker b(pskDecoderEncodingCombo); pskDecoderEncodingCombo->setCurrentIndex(pendingSettings.pskDecoderEncoding); }
    if (cwDecoderSelectivitySpin) {
        cwDecoderSelectivitySpin->setValue(pendingSettings.cwDecoderSelectivity);
    }
    pendingSettings.sstvDemodulationMode = normalizedSstvDemodulationMode(
        readInt("sstvDemodulationMode", pendingSettings.sstvDemodulationMode));
    if (sstvDemodulationCombo) {
        const QSignalBlocker blocker(sstvDemodulationCombo);
        const int index = sstvDemodulationCombo->findData(
            pendingSettings.sstvDemodulationMode);
        sstvDemodulationCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    pendingSettings.fftLength = readInt("fftLength", pendingSettings.fftLength);
    fftBinWidthModeEnabled = readBool("fftBinWidthModeEnabled", fftBinWidthModeEnabled);
    fftTargetBinWidthHz = (std::clamp)(readDouble("fftTargetBinWidthHz", fftTargetBinWidthHz),
                                       0.1,
                                       1000000.0);
    if (fftBinWidthModeEnabled) {
        pendingSettings.fftLength = fftLengthForBinWidth(pendingSettings.sampleRate,
                                                          fftTargetBinWidthHz);
    }
    pendingSettings.fftWindowType = normalizedFftWindowType(
        readInt("fftWindowType", pendingSettings.fftWindowType));
    pendingSettings.lnaGain = readInt("lnaGain", pendingSettings.lnaGain);
    pendingSettings.vgaGain = readInt("vgaGain", pendingSettings.vgaGain);
    pendingSettings.hackRfLnaGainDb =
        (std::clamp)(readInt("hackRfLnaGainDb", pendingSettings.hackRfLnaGainDb), 0, 40) / 8 * 8;
    pendingSettings.hackRfVgaGainDb =
        (std::clamp)(readInt("hackRfVgaGainDb", pendingSettings.hackRfVgaGainDb), 0, 62) / 2 * 2;
    pendingSettings.hackRfAmpEnabled = readBool("hackRfAmpEnabled", pendingSettings.hackRfAmpEnabled);
    pendingSettings.hackRfBiasTeeEnabled = readBool("hackRfBiasTeeEnabled", pendingSettings.hackRfBiasTeeEnabled);
    pendingSettings.hackRfAutomaticBandwidth =
        readBool("hackRfAutomaticBandwidth", pendingSettings.hackRfAutomaticBandwidth);
    pendingSettings.hackRfBandwidthHz =
        (std::clamp)(readInt("hackRfBandwidthHz", pendingSettings.hackRfBandwidthHz), 0, 28000000);
    pendingSettings.hackRfExplicitTuningEnabled =
        readBool("hackRfExplicitTuningEnabled", pendingSettings.hackRfExplicitTuningEnabled);
    pendingSettings.hackRfExplicitIfHz =
        readDouble("hackRfExplicitIfHz", pendingSettings.hackRfExplicitIfHz);
    pendingSettings.hackRfExplicitLoHz =
        readDouble("hackRfExplicitLoHz", pendingSettings.hackRfExplicitLoHz);
    pendingSettings.hackRfExplicitPath =
        (std::clamp)(readInt("hackRfExplicitPath", pendingSettings.hackRfExplicitPath), 0, 2);
    pendingSettings.hackRfClockOutEnabled =
        readBool("hackRfClockOutEnabled", pendingSettings.hackRfClockOutEnabled);
    pendingSettings.hackRfHardwareSyncEnabled =
        readBool("hackRfHardwareSyncEnabled", pendingSettings.hackRfHardwareSyncEnabled);
    pendingSettings.hackRfRxOverrunLimit =
        (std::max)(0, readInt("hackRfRxOverrunLimit", pendingSettings.hackRfRxOverrunLimit));
    pendingSettings.hackRfOperaCakeEnabled =
        readBool("hackRfOperaCakeEnabled", pendingSettings.hackRfOperaCakeEnabled);
    pendingSettings.hackRfOperaCakeAddress =
        (std::clamp)(readInt("hackRfOperaCakeAddress", pendingSettings.hackRfOperaCakeAddress), 0, 7);
    pendingSettings.hackRfOperaCakeMode =
        (std::clamp)(readInt("hackRfOperaCakeMode", pendingSettings.hackRfOperaCakeMode), 0, 2);
    pendingSettings.hackRfOperaCakePortA =
        (std::clamp)(readInt("hackRfOperaCakePortA", pendingSettings.hackRfOperaCakePortA), 0, 7);
    pendingSettings.hackRfOperaCakePortB =
        (std::clamp)(readInt("hackRfOperaCakePortB", pendingSettings.hackRfOperaCakePortB), 0, 7);
    pendingSettings.hackRfOperaCakeRangesJson =
        readString("hackRfOperaCakeRangesJson", pendingSettings.hackRfOperaCakeRangesJson);
    pendingSettings.hackRfOperaCakeDwellsJson =
        readString("hackRfOperaCakeDwellsJson", pendingSettings.hackRfOperaCakeDwellsJson);
    pendingSettings.hackRfSweepRanges =
        readString("hackRfSweepRanges", pendingSettings.hackRfSweepRanges);
    pendingSettings.hackRfSweepStepHz =
        (std::clamp)(readInt("hackRfSweepStepHz", pendingSettings.hackRfSweepStepHz), 1000, 20000000);
    pendingSettings.hackRfSweepBytesPerTune =
        (std::clamp)(readInt("hackRfSweepBytesPerTune", pendingSettings.hackRfSweepBytesPerTune),
                     16384, 16384 * 64);
    pendingSettings.hackRfSweepInterleaved =
        readBool("hackRfSweepInterleaved", pendingSettings.hackRfSweepInterleaved);
    pendingSettings.rtlAgc = readBool("rtlAgc", pendingSettings.rtlAgc);
    pendingSettings.rtlTunerGainTenthsDb =
        (std::clamp)(readInt("rtlTunerGainTenthsDb", pendingSettings.rtlTunerGainTenthsDb), 0, 496);
    pendingSettings.audioLowPassHz = clampAudioLowPassHz(readDouble("audioLowPassHz", pendingSettings.audioLowPassHz));
    pendingSettings.audioHighPassHz = clampAudioHighPassHz(readDouble("audioHighPassHz", pendingSettings.audioHighPassHz));
    pendingSettings.audioFilterChainJson = readString("audioFilterChainJson",
                                                      pendingSettings.audioFilterChainJson);
    pendingSettings.simplifiedAudioChannelizer =
        readBool("simplifiedAudioChannelizer",
                 pendingSettings.simplifiedAudioChannelizer);
    pendingSettings.hfNoiseCancelDepth = clampHfNoiseCancelDepth(readDouble("hfNoiseCancelDepth", pendingSettings.hfNoiseCancelDepth));
    pendingSettings.hfNoiseCancelRefGainDb =
        clampHfNoiseCancelRefGainDb(readDouble("hfNoiseCancelRefGainDb", pendingSettings.hfNoiseCancelRefGainDb));
    pendingSettings.hfNoiseCancelRefDelayNs =
        clampHfNoiseCancelRefDelayNs(readDouble("hfNoiseCancelRefDelayNs", pendingSettings.hfNoiseCancelRefDelayNs));
    pendingSettings.hfNoiseCancelRefTiltDb =
        clampHfNoiseCancelRefTiltDb(readDouble("hfNoiseCancelRefTiltDb", pendingSettings.hfNoiseCancelRefTiltDb));
    pendingSettings.hfNoiseCancelFreeze = readBool("hfNoiseCancelFreeze", pendingSettings.hfNoiseCancelFreeze);
    pendingSettings.hfAudioBlankerEnabled =
        readBool("hfAudioBlankerEnabled", pendingSettings.hfAudioBlankerEnabled);
    pendingSettings.hfAudioBlankerThreshold =
        (std::clamp)(readDouble("hfAudioBlankerThreshold", pendingSettings.hfAudioBlankerThreshold),
                     2.0,
                     20.0);
    pendingSettings.audioEnabled = readBool("audioEnabled", pendingSettings.audioEnabled);
    pendingSettings.syncEnabled = false;
    pendingSettings.gpoValue = static_cast<std::uint8_t>(readInt("gpoValue", pendingSettings.gpoValue));
    pendingSettings.dmrBasebandSampleRate =
        normalizedDmrBasebandSampleRate(readInt("dmrBasebandSampleRate",
                                                pendingSettings.dmrBasebandSampleRate));
    pendingSettings.dmrAmbeLayout =
        normalizedDmrAmbeLayout(readInt("dmrAmbeLayout", pendingSettings.dmrAmbeLayout));
    pendingSettings.dmrManualTimingEnabled =
        readBool("dmrManualTimingEnabled", pendingSettings.dmrManualTimingEnabled);
    pendingSettings.dmrManualTimingOffset =
        (std::clamp)(readInt("dmrManualTimingOffset", pendingSettings.dmrManualTimingOffset),
                     -80,
                     80);
    pendingSettings.dmrSlicerRatio =
        (std::clamp)(readDouble("dmrSlicerRatio", pendingSettings.dmrSlicerRatio),
                     0.45,
                     0.80);
    pendingSettings.dmrAdaptiveSlicer =
        readBool("dmrAdaptiveSlicer", pendingSettings.dmrAdaptiveSlicer);
    pendingSettings.dmrPrivacyMode =
        normalizedDmrPrivacyMode(readInt("dmrPrivacyMode", pendingSettings.dmrPrivacyMode));
    pendingSettings.dmrPrivacyKeyId =
        (std::clamp)(readInt("dmrPrivacyKeyId", pendingSettings.dmrPrivacyKeyId), 0, 255);
    pendingSettings.dmrPrivacyKeyHex =
        normalizedDmrPrivacyKeyHex(settingsJson.value(QStringLiteral("dmrPrivacyKeyHex"))
                                       .toString(pendingSettings.dmrPrivacyKeyHex));
    pendingSettings.dmrPrivacyVariant =
        settingsJson.value(QStringLiteral("dmrPrivacyVariant"))
            .toString(pendingSettings.dmrPrivacyVariant)
            .trimmed()
            .toLower();
    if (pendingSettings.dmrPrivacyVariant.isEmpty()) {
        pendingSettings.dmrPrivacyVariant = QStringLiteral("dmra");
    }
    pendingSettings.dmrPrivacyLayout =
        settingsJson.value(QStringLiteral("dmrPrivacyLayout"))
            .toString(pendingSettings.dmrPrivacyLayout)
            .trimmed()
            .toLower();
    if (pendingSettings.dmrPrivacyLayout.isEmpty()) {
        pendingSettings.dmrPrivacyLayout = QStringLiteral("normal");
    }
    pendingSettings.dmrPrivacyFrameOffset =
        (std::clamp)(readInt("dmrPrivacyFrameOffset", pendingSettings.dmrPrivacyFrameOffset), 0, 17);
    currentScale = readDouble("scalePercent", currentScale);
    additionalScaleDivisor = (std::clamp)(
        readInt("additionalScaleDivisor", additionalScaleDivisor),
        1,
        ADDITIONAL_SCALE_DIVISOR_MAX);
    agileScanEnabled = readBool("agileScanEnabled", agileScanEnabled);
    agileScanAutoStepSampleRate =
        readBool("agileScanAutoStepSampleRate", agileScanAutoStepSampleRate);
    agileScanRangesMhz = settingsJson.value("agileScanRangesMhz").toString(agileScanRangesMhz).trimmed();
    agileScanStepMhz = (std::clamp)(readDouble("agileScanStepMhz", agileScanStepMhz),
                                    AGILE_SCAN_MIN_STEP_MHZ,
                                    AGILE_SCAN_MAX_STEP_MHZ);
    applyAgileScanAutoStep(false);
    scanVisualMode = normalizedScanVisualMode(readInt("scanVisualMode", scanVisualMode));
    scanListeningLockEnabled = readBool("scanListeningLockEnabled", scanListeningLockEnabled);
    standardScanEnabled = readBool("standardScanEnabled", standardScanEnabled);
    standardScanCentersMhz =
        settingsJson.value("standardScanCentersMhz").toString(standardScanCentersMhz).trimmed();
    standardScanDwellMs = (std::clamp)(readInt("standardScanDwellMs", standardScanDwellMs),
                                       STANDARD_SCAN_MIN_DWELL_MS,
                                       STANDARD_SCAN_MAX_DWELL_MS);
    standardScanSettleMs = (std::clamp)(readInt("standardScanSettleMs", standardScanSettleMs),
                                        STANDARD_SCAN_MIN_SETTLE_MS,
                                        STANDARD_SCAN_MAX_SETTLE_MS);
    standardScanRangeStartMhz =
        settingsJson.value("standardScanRangeStartMhz").toString(standardScanRangeStartMhz).trimmed();
    standardScanRangeEndMhz =
        settingsJson.value("standardScanRangeEndMhz").toString(standardScanRangeEndMhz).trimmed();
    listeningScanEnabled = readBool("listeningScanEnabled", listeningScanEnabled);
    listeningScanTargetsMhz =
        settingsJson.value("listeningScanTargetsMhz").toString(listeningScanTargetsMhz).trimmed();
    listeningScanDwellMs = (std::clamp)(readInt("listeningScanDwellMs", listeningScanDwellMs),
                                        LISTENING_SCAN_MIN_DWELL_MS,
                                        LISTENING_SCAN_MAX_DWELL_MS);
    listeningScanSettleMs = (std::clamp)(readInt("listeningScanSettleMs", listeningScanSettleMs),
                                         LISTENING_SCAN_MIN_SETTLE_MS,
                                         LISTENING_SCAN_MAX_SETTLE_MS);
    const bool previousScanMeasurementEnabled = scanMeasurementEnabled;
    const double previousScanMeasurementBinMhz = scanMeasurementBinMhz;
    scanMeasurementEnabled = readBool("scanMeasurementEnabled", scanMeasurementEnabled);
    waterfallAreaMeasurementEnabled =
        readBool("waterfallAreaMeasurementEnabled", waterfallAreaMeasurementEnabled);
    if (waterfallAreaMeasurementCheckbox) {
        QSignalBlocker blocker(waterfallAreaMeasurementCheckbox);
        waterfallAreaMeasurementCheckbox->setChecked(waterfallAreaMeasurementEnabled);
    }
    if (waterfallWidget) {
        waterfallWidget->setAreaMeasurementEnabled(waterfallAreaMeasurementEnabled);
    }
    scanMeasurementBinMhz = (std::clamp)(readDouble("scanMeasurementBinMhz", scanMeasurementBinMhz),
                                         SCAN_MEASUREMENT_MIN_BIN_MHZ,
                                         SCAN_MEASUREMENT_MAX_BIN_MHZ);
    scanMeasurementUpdateIntervalMs =
        (std::clamp)(readInt("scanMeasurementUpdateIntervalMs", scanMeasurementUpdateIntervalMs),
                     SCAN_MEASUREMENT_MIN_UPDATE_MS,
                     SCAN_MEASUREMENT_MAX_UPDATE_MS);
    spectrumScienceMaxHoldEnabled = readBool("spectrumScienceMaxHold", spectrumScienceMaxHoldEnabled);
    if (settingsJson.contains("spectrumScienceMarkers"))
        spectrumScienceAnalyzer.restoreMarkerConfiguration(settingsJson.value("spectrumScienceMarkers").toArray());
    spectrumScienceMinHoldEnabled = readBool("spectrumScienceMinHold", spectrumScienceMinHoldEnabled);
    spectrumScienceAverageEnabled = readBool("spectrumScienceAverage", spectrumScienceAverageEnabled);
    spectrumScienceAverageSeconds = (std::clamp)(readDouble("spectrumScienceAverageSeconds",
                                                            spectrumScienceAverageSeconds),
                                                  0.05,
                                                  60.0);
    spectrumDetectorMode = normalizedSpectrumDetectorMode(readInt("spectrumDetectorMode", spectrumDetectorMode));
    spectrumDetectorFrames = (std::clamp)(readInt("spectrumDetectorFrames", spectrumDetectorFrames), 1, 256);
    spectrumVbwHz = (std::clamp)(readDouble("spectrumVbwHz", spectrumVbwHz), 0.0, 10000.0);
    spectrumFftOverlapPercent = readInt("spectrumFftOverlapPercent", spectrumFftOverlapPercent);
    if (spectrumFftOverlapPercent != 25 && spectrumFftOverlapPercent != 50 &&
        spectrumFftOverlapPercent != 75) spectrumFftOverlapPercent = 0;
    spectrumAverageFrameCount = (std::clamp)(readInt("spectrumAverageFrameCount", spectrumAverageFrameCount), 0, 10000);
    spectrumPercentile50Enabled = readBool("spectrumPercentile50", spectrumPercentile50Enabled);
    spectrumPercentile90Enabled = readBool("spectrumPercentile90", spectrumPercentile90Enabled);
    spectrumPercentile99Enabled = readBool("spectrumPercentile99", spectrumPercentile99Enabled);
    spectrumAmplitudeUnit = (std::clamp)(readInt("spectrumAmplitudeUnit", spectrumAmplitudeUnit), 0, 3);
    extendedRecordingMetadataEnabled =
        readBool("extendedRecordingMetadata", extendedRecordingMetadataEnabled);
    if (previousScanMeasurementEnabled != scanMeasurementEnabled ||
        std::abs(previousScanMeasurementBinMhz - scanMeasurementBinMhz) > 0.000001) {
        clearScanMeasurement();
    }
    qthLatitude = (std::clamp)(readDouble("qthLatitude", qthLatitude), -90.0, 90.0);
    qthLongitude = (std::clamp)(readDouble("qthLongitude", qthLongitude), -180.0, 180.0);
    qthPositionVisible = settingsJson.value("qthPositionVisible").toBool(qthPositionVisible);
    qthSource = settingsJson.value("qthSource").toString(qthSource).trimmed();
    if (qthSource.isEmpty()) {
        qthSource = QStringLiteral("manual");
    }
    if (qthSource == QStringLiteral("nmea")) {
        qthPositionVisible = false;
    }
    gnssSerialPortName = settingsJson.value("gnssSerialPortName").toString(gnssSerialPortName).trimmed();
    gnssSerialBaud = (std::clamp)(readInt("gnssSerialBaud", gnssSerialBaud), 1200, 921600);
    gnssPositionPolicy =
        normalizedGnssPositionPolicy(settingsJson.value("gnssPositionPolicy").toString(gnssPositionPolicy));
    gnssUbxAutoEnable = readBool("gnssUbxAutoEnable", gnssUbxAutoEnable);
    gnssTimeZoneOffsetMinutes = (std::clamp)(readInt("gnssTimeZoneOffsetMinutes", gnssTimeZoneOffsetMinutes),
                                             -12 * 60,
                                             100000);
    gnssSatelliteTableVisible = readBool("gnssSatelliteTableVisible", gnssSatelliteTableVisible);
    qthTileDirectory = settingsJson.value("qthTileDirectory").toString(qthTileDirectory).trimmed();
    qthMapLayer = (std::clamp)(readInt("qthMapLayer", qthMapLayer), 0, 2);
    qthMapZoom = (std::clamp)(readInt("qthMapZoom", qthMapZoom), 0, 19);
    qthOnlineProviderId =
        settingsJson.value("qthOnlineProviderId").toString(qthOnlineProviderId).trimmed();
    if (qthOnlineProviderId.isEmpty()) {
        qthOnlineProviderId = QStringLiteral("custom");
    }
    qthOnlineTileUrlTemplate =
        settingsJson.value("qthOnlineTileUrlTemplate").toString(qthOnlineTileUrlTemplate).trimmed();
    qthOnlineAttribution =
        settingsJson.value("qthOnlineAttribution").toString(qthOnlineAttribution).trimmed();
    qthOnlineApiKey =
        settingsJson.value("qthOnlineApiKey").toString(qthOnlineApiKey).trimmed();
    qthOnlineNoDiskCache = readBool("qthOnlineNoDiskCache", qthOnlineNoDiskCache);
    gnssSystemId =
        gnssSystemPreset(settingsJson.value("gnssSystemId").toString(gnssSystemId).trimmed()).id;
    gnssMonitorEnabled = readBool("gnssMonitorEnabled", gnssMonitorEnabled);
    gnssUseGps = readBool("gnssUseGps", gnssUseGps);
    gnssUseGlonass = readBool("gnssUseGlonass", gnssUseGlonass);
    gnssUseGalileo = readBool("gnssUseGalileo", gnssUseGalileo);
    gnssUseBeidou = readBool("gnssUseBeidou", gnssUseBeidou);
    gnssUseQzss = readBool("gnssUseQzss", gnssUseQzss);
    gnssUseSbas = readBool("gnssUseSbas", gnssUseSbas);
    gnssUseOther = readBool("gnssUseOther", gnssUseOther);
    if (settingsJson.contains(QStringLiteral("gnssDisabledSatellites"))) {
        gnssDisabledSatelliteKeys.clear();
        const QJsonArray disabledSatellites = settingsJson.value(QStringLiteral("gnssDisabledSatellites")).toArray();
        for (const QJsonValue &value : disabledSatellites) {
            const QString key = value.toString().trimmed();
            if (!key.isEmpty()) {
                gnssDisabledSatelliteKeys.insert(key);
            }
        }
        for (auto it = gnssNmeaSatelliteEnabled.begin(); it != gnssNmeaSatelliteEnabled.end(); ++it) {
            it.value() = !gnssDisabledSatelliteKeys.contains(it.key());
        }
    }
    gnssAcquisitionIntegrationMs =
        (std::clamp)(readInt("gnssAcquisitionIntegrationMs", gnssAcquisitionIntegrationMs),
                     GNSS_ACQUISITION_MIN_INTEGRATION_MS,
                     GNSS_ACQUISITION_MAX_INTEGRATION_MS);
    gnssChannelFilterCutoffHz =
        (std::clamp)(readDouble("gnssChannelFilterCutoffHz", gnssChannelFilterCutoffHz),
                     GNSS_CHANNEL_FILTER_MIN_HZ,
                     GNSS_CHANNEL_FILTER_MAX_HZ);
    gnssDopplerSpanHz =
        (std::clamp)(readInt("gnssDopplerSpanHz", gnssDopplerSpanHz), 1000, 50000);
    gnssDopplerStepHz =
        (std::clamp)(readInt("gnssDopplerStepHz", gnssDopplerStepHz), 250, 5000);
    gnssContinuousAcquisitionEnabled =
        readBool("gnssContinuousAcquisitionEnabled", gnssContinuousAcquisitionEnabled);
    gnssContinuousAcquisitionIntervalMs =
        (std::clamp)(readInt("gnssContinuousAcquisitionIntervalMs",
                             gnssContinuousAcquisitionIntervalMs),
                     GNSS_CONTINUOUS_ACQUISITION_MIN_INTERVAL_MS,
                     GNSS_CONTINUOUS_ACQUISITION_MAX_INTERVAL_MS);
    qthGridPrecision = readInt("qthGridPrecision", qthGridPrecision);
    if (qthGridPrecision <= 2) {
        qthGridPrecision = 2;
    } else if (qthGridPrecision <= 4) {
        qthGridPrecision = 4;
    } else {
        qthGridPrecision = 6;
    }
    qthMapOverlayMode = (std::clamp)(readInt("qthMapOverlayMode", qthMapOverlayMode), 0, 3);
    if (settingsJson.contains(QStringLiteral("qthMarkers"))) {
        QVector<qth::UserMarker> nextMarkers;
        QVector<int> usedNumbers;
        const QJsonArray markers = settingsJson.value(QStringLiteral("qthMarkers")).toArray();
        for (const QJsonValue &value : markers) {
            const QJsonObject object = value.toObject();
            qth::UserMarker marker;
            marker.number = object.value(QStringLiteral("number")).toInt(0);
            marker.name = object.value(QStringLiteral("name")).toString().trimmed().left(80);
            marker.description = object.value(QStringLiteral("description")).toString().trimmed().left(512);
            marker.latitude = object.value(QStringLiteral("latitude")).toDouble(std::numeric_limits<double>::quiet_NaN());
            marker.longitude = object.value(QStringLiteral("longitude")).toDouble(std::numeric_limits<double>::quiet_NaN());
            if (marker.number <= 0 ||
                usedNumbers.contains(marker.number) ||
                !qth::isValidLatitude(marker.latitude) ||
                !qth::isValidLongitude(marker.longitude)) {
                continue;
            }
            if (marker.name.isEmpty()) {
                marker.name = qth::maidenheadLocator(marker.latitude, marker.longitude, 6);
            }
            usedNumbers.append(marker.number);
            nextMarkers.append(marker);
        }
        qthUserMarkers = nextMarkers;
    }
    spectrumUpdateIntervalMs = (std::clamp)(readInt("spectrumUpdateIntervalMs", spectrumUpdateIntervalMs),
                                            SPECTRUM_UPDATE_AUTO_MS,
                                            SPECTRUM_UPDATE_MAX_MS);
    if (spectrumUpdateIntervalMs > 0 && spectrumUpdateIntervalMs < SPECTRUM_UPDATE_MIN_MS) {
        spectrumUpdateIntervalMs = SPECTRUM_UPDATE_MIN_MS;
    }
    fftBackendPreference = normalizedFftBackendPreference(
        readInt("fftBackendPreference", fftBackendPreference));
    frequencyCalibrationOffsetHz =
        (std::clamp)(readDouble("frequencyCalibrationOffsetHz", frequencyCalibrationOffsetHz),
                     -10000000.0,
                     10000000.0);
    amplitudeCalibrationOffsetDb =
        (std::clamp)(readDouble("amplitudeCalibrationOffsetDb", amplitudeCalibrationOffsetDb),
                     -200.0,
                     200.0);
    calibrationTableEnabled = readBool("calibrationTableEnabled", calibrationTableEnabled);
    if (settingsJson.contains(QStringLiteral("calibrationTable"))) {
        QVector<ReceiverCalibrationPoint> calibrationPoints;
        const QJsonArray array = settingsJson.value(QStringLiteral("calibrationTable")).toArray();
        calibrationPoints.reserve(array.size());
        for (const QJsonValue &value : array) {
            const QJsonObject object = value.toObject();
            ReceiverCalibrationPoint point;
            point.frequencyHz = object.value(QStringLiteral("frequencyHz")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            point.frequencyOffsetHz = object.value(QStringLiteral("frequencyOffsetHz")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            point.amplitudeOffsetDb = object.value(QStringLiteral("amplitudeOffsetDb")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            point.uncertaintyDb = object.value(QStringLiteral("uncertaintyDb")).toDouble(0.0);
            point.note = object.value(QStringLiteral("note")).toString();
            point.conditions = ReceiverCalibrationContext::fromJson(object.value("conditions").toObject());
            calibrationPoints.append(point);
        }
        receiverCalibrationTable.setPoints(calibrationPoints);
    }
    if (processor) {
        processor->setFrequencyCalibrationOffset(
            effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency));
    }
    waterfallRowsPerFrame = (std::clamp)(readInt("waterfallRowsPerFrame", waterfallRowsPerFrame),
                                         WATERFALL_ROWS_PER_FRAME_MIN,
                                         WATERFALL_ROWS_PER_FRAME_MAX);
    if (waterfallWidget) {
        waterfallWidget->setRowsPerFrame(waterfallRowsPerFrame);
    }
    waterfallDisplayMode = (std::clamp)(readInt("waterfallDisplayMode", waterfallDisplayMode), 0, 2);
    if (waterfallDisplayModeCombo) {
        QSignalBlocker blocker(waterfallDisplayModeCombo);
        const int index = waterfallDisplayModeCombo->findData(waterfallDisplayMode);
        if (index >= 0) {
            waterfallDisplayModeCombo->setCurrentIndex(index);
        }
    }
    if (waterfallWidget) {
        waterfallWidget->setDisplayMode(
            static_cast<MyWaterfallWidget::DisplayMode>(waterfallDisplayMode));
    }
    waterfall3DResolutionDivisor = readInt("waterfall3DResolutionDivisor", waterfall3DResolutionDivisor);
    if (!QVector<int>{1, 2, 4, 8, 16, 32, 64}.contains(waterfall3DResolutionDivisor)) {
        waterfall3DResolutionDivisor = 4;
    }
    if (waterfall3DResolutionCombo) {
        QSignalBlocker blocker(waterfall3DResolutionCombo);
        const int index = waterfall3DResolutionCombo->findData(waterfall3DResolutionDivisor);
        if (index >= 0) {
            waterfall3DResolutionCombo->setCurrentIndex(index);
        }
    }
    waterfall3DSurfaceStyle = (std::clamp)(
        readInt("waterfall3DSurfaceStyle", waterfall3DSurfaceStyle), 0, 1);
    waterfall3DSmoothing = (std::clamp)(
        readInt("waterfall3DSmoothing", waterfall3DSmoothing), 0, 2);
    waterfall3DLighting = (std::clamp)(
        readInt("waterfall3DLighting", waterfall3DLighting), 0, 2);
    const auto restore3DCombo = [](QComboBox *combo, int value) {
        if (!combo) return;
        const QSignalBlocker blocker(combo);
        const int index = combo->findData(value);
        if (index >= 0) combo->setCurrentIndex(index);
    };
    restore3DCombo(waterfall3DSurfaceStyleCombo, waterfall3DSurfaceStyle);
    restore3DCombo(waterfall3DSmoothingCombo, waterfall3DSmoothing);
    restore3DCombo(waterfall3DLightingCombo, waterfall3DLighting);
    if (waterfallWidget) {
        waterfallWidget->set3DResolutionDivisor(waterfall3DResolutionDivisor);
        waterfallWidget->set3DSurfaceStyle(waterfall3DSurfaceStyle);
        waterfallWidget->set3DSurfaceSmoothing(waterfall3DSmoothing);
        waterfallWidget->set3DSurfaceLighting(waterfall3DLighting);
    }
    waterfall3DHistoryRows =
        (std::clamp)(readInt("waterfall3DHistoryRows", waterfall3DHistoryRows), 16, 2048);
    if (waterfall3DHistoryRowsSpin) {
        QSignalBlocker blocker(waterfall3DHistoryRowsSpin);
        waterfall3DHistoryRowsSpin->setValue(waterfall3DHistoryRows);
    }
    if (waterfallWidget) {
        waterfallWidget->set3DHistoryRows(waterfall3DHistoryRows);
    }
    waterfall3DSliceScrollStep =
        (std::clamp)(readInt("waterfall3DSliceScrollStep", waterfall3DSliceScrollStep), 1, 256);
    waterfall3DSliceWidth =
        (std::clamp)(readInt("waterfall3DSliceWidth", waterfall3DSliceWidth), 1, 4096);
    waterfall3DSpectrumSliceScrollStep =
        (std::clamp)(readInt("waterfall3DSpectrumSliceScrollStep",
                             waterfall3DSpectrumSliceScrollStep),
                     1,
                     2048);
    waterfall3DSpectrumSliceRows =
        (std::clamp)(readInt("waterfall3DSpectrumSliceRows", waterfall3DSpectrumSliceRows),
                     1,
                     2048);
    waterfall3DSpectrumSliceCapture =
        readBool("waterfall3DSpectrumSliceCapture", waterfall3DSpectrumSliceCapture);
    waterfall3DSpectrumSliceCaptureFixed =
        readBool("waterfall3DSpectrumSliceCaptureFixed", waterfall3DSpectrumSliceCaptureFixed);
    waterfall3DFixedPlane = readBool("waterfall3DFixedPlane", waterfall3DFixedPlane);
    waterfall3DMonochrome = readBool("waterfall3DMonochrome", waterfall3DMonochrome);
    waterfall3DHeightGradient =
        readBool("waterfall3DHeightGradient", waterfall3DHeightGradient);
    const QColor importedMonochromeColor(
        readString("waterfall3DMonochromeColor",
                   waterfall3DMonochromeColor.name(QColor::HexRgb)));
    if (importedMonochromeColor.isValid()) {
        waterfall3DMonochromeColor = importedMonochromeColor;
    }
    waterfall3DMonochromeGradientDepth = (std::clamp)(
        readInt("waterfall3DMonochromeGradientDepth",
                waterfall3DMonochromeGradientDepth),
        0,
        100);
    waterfall3DAlternativeView = readBool("waterfall3DAlternativeView", waterfall3DAlternativeView);
    waterfall3DVncSliceInput =
        readBool("waterfall3DVncSliceInput", waterfall3DVncSliceInput);
    if (waterfall3DSliceStepSpin) {
        QSignalBlocker blocker(waterfall3DSliceStepSpin);
        waterfall3DSliceStepSpin->setValue(waterfall3DSliceScrollStep);
    }
    if (waterfall3DSliceWidthSpin) {
        QSignalBlocker blocker(waterfall3DSliceWidthSpin);
        waterfall3DSliceWidthSpin->setValue(waterfall3DSliceWidth);
    }
    if (waterfall3DSpectrumSliceStepSpin) {
        QSignalBlocker blocker(waterfall3DSpectrumSliceStepSpin);
        waterfall3DSpectrumSliceStepSpin->setValue(waterfall3DSpectrumSliceScrollStep);
    }
    if (waterfall3DSpectrumSliceRowsSpin) {
        QSignalBlocker blocker(waterfall3DSpectrumSliceRowsSpin);
        waterfall3DSpectrumSliceRowsSpin->setValue(waterfall3DSpectrumSliceRows);
    }
    if (waterfall3DSpectrumSliceCaptureCheckbox) {
        QSignalBlocker blocker(waterfall3DSpectrumSliceCaptureCheckbox);
        waterfall3DSpectrumSliceCaptureCheckbox->setChecked(waterfall3DSpectrumSliceCapture);
    }
    if (waterfall3DSpectrumSliceCaptureFixedCheckbox) {
        QSignalBlocker blocker(waterfall3DSpectrumSliceCaptureFixedCheckbox);
        waterfall3DSpectrumSliceCaptureFixedCheckbox->setChecked(waterfall3DSpectrumSliceCaptureFixed);
        waterfall3DSpectrumSliceCaptureFixedCheckbox->setEnabled(waterfall3DSpectrumSliceCapture);
    }
    if (waterfall3DFixedPlaneCheckbox) {
        QSignalBlocker blocker(waterfall3DFixedPlaneCheckbox);
        waterfall3DFixedPlaneCheckbox->setChecked(waterfall3DFixedPlane);
    }
    if (waterfall3DMonochromeCheckbox) {
        QSignalBlocker blocker(waterfall3DMonochromeCheckbox);
        waterfall3DMonochromeCheckbox->setChecked(waterfall3DMonochrome);
    }
    if (waterfall3DHeightGradientCheckbox) {
        QSignalBlocker blocker(waterfall3DHeightGradientCheckbox);
        waterfall3DHeightGradientCheckbox->setChecked(waterfall3DHeightGradient);
    }
    if (waterfall3DMonochromeColorButton) {
        const QColor textColor = waterfall3DMonochromeColor.lightnessF() > 0.58
                                     ? Qt::black : Qt::white;
        waterfall3DMonochromeColorButton->setText(
            waterfall3DMonochromeColor.name(QColor::HexRgb).toUpper());
        waterfall3DMonochromeColorButton->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: %1; color: %2; border: 1px solid #707780; padding: 3px 8px; }")
            .arg(waterfall3DMonochromeColor.name(QColor::HexRgb),
                 textColor.name(QColor::HexRgb)));
    }
    if (waterfall3DMonochromeGradientDepthSlider) {
        QSignalBlocker blocker(waterfall3DMonochromeGradientDepthSlider);
        waterfall3DMonochromeGradientDepthSlider->setValue(
            waterfall3DMonochromeGradientDepth);
    }
    if (waterfall3DMonochromeGradientDepthValueLabel) {
        waterfall3DMonochromeGradientDepthValueLabel->setText(
            QStringLiteral("%1%").arg(waterfall3DMonochromeGradientDepth));
    }
    if (waterfall3DAlternativeViewCheckbox) {
        QSignalBlocker blocker(waterfall3DAlternativeViewCheckbox);
        waterfall3DAlternativeViewCheckbox->setChecked(waterfall3DAlternativeView);
    }
    if (waterfall3DVncSliceInputCheckbox) {
        QSignalBlocker blocker(waterfall3DVncSliceInputCheckbox);
        waterfall3DVncSliceInputCheckbox->setChecked(waterfall3DVncSliceInput);
    }
    if (waterfallWidget) {
        waterfallWidget->set3DSliceScrollStep(waterfall3DSliceScrollStep);
        waterfallWidget->set3DSliceWidth(waterfall3DSliceWidth);
        waterfallWidget->set3DSpectrumSliceScrollStep(waterfall3DSpectrumSliceScrollStep);
        waterfallWidget->set3DSpectrumSliceWidth(waterfall3DSpectrumSliceRows);
        waterfallWidget->set3DSpectrumSliceCapture(waterfall3DSpectrumSliceCapture);
        waterfallWidget->set3DSpectrumSliceCaptureFixed(waterfall3DSpectrumSliceCaptureFixed);
        waterfallWidget->set3DFixedPlane(waterfall3DFixedPlane);
        waterfallWidget->set3DMonochrome(waterfall3DMonochrome);
        waterfallWidget->set3DHeightGradient(waterfall3DHeightGradient);
        waterfallWidget->set3DMonochromeColor(waterfall3DMonochromeColor);
        waterfallWidget->set3DMonochromeGradientDepth(
            waterfall3DMonochromeGradientDepth);
        waterfallWidget->set3DModifierFreeSliceInput(waterfall3DVncSliceInput);
    }
    if (!settingsJson.contains("waterfall3DAlternativeView") &&
        settingsJson.value("alternativeInterfaceMode").toBool(false)) {
        waterfall3DAlternativeView = true;
        alternativeInterfaceMode = false;
    } else {
        alternativeInterfaceMode = readBool("alternativeInterfaceMode", alternativeInterfaceMode);
    }
    alternativeSpectrumGradientFill =
        readBool("alternativeSpectrumGradientFill", alternativeSpectrumGradientFill);
    alternativeSpectrumGradientOpacity =
        (std::clamp)(readInt("alternativeSpectrumGradientOpacity",
                             alternativeSpectrumGradientOpacity),
                     0,
                     100);
    if (alternativeSpectrumGradientCheckbox) {
        QSignalBlocker blocker(alternativeSpectrumGradientCheckbox);
        alternativeSpectrumGradientCheckbox->setChecked(alternativeSpectrumGradientFill);
    }
    if (alternativeSpectrumGradientOpacitySlider) {
        QSignalBlocker blocker(alternativeSpectrumGradientOpacitySlider);
        alternativeSpectrumGradientOpacitySlider->setValue(alternativeSpectrumGradientOpacity);
    }
    if (alternativeSpectrumGradientOpacityValueLabel) {
        alternativeSpectrumGradientOpacityValueLabel->setText(
            QStringLiteral("%1%").arg(alternativeSpectrumGradientOpacity));
    }
    if (waterfallWidget) {
        waterfallWidget->setAlternativeSpectrumGradientFill(alternativeSpectrumGradientFill);
        waterfallWidget->setAlternativeSpectrumGradientOpacity(alternativeSpectrumGradientOpacity);
    }
    applyAlternativeInterfaceMode();
    experimentalGpuWaterfall = readBool("experimentalGpuWaterfall", experimentalGpuWaterfall);
    spectrumDisplayReductionMode = (std::clamp)(
        readInt("spectrumDisplayReductionMode", spectrumDisplayReductionMode), 0, 4);
    if (waterfallWidget) {
        waterfallWidget->setRenderBackend(experimentalGpuWaterfall
                                              ? MyWaterfallWidget::RenderBackend::GpuPrepared
                                              : MyWaterfallWidget::RenderBackend::CpuTexture);
    }
    {
        bool adjusted = false;
        QString standardScanError;
        const QVector<double> normalized =
            parseStandardScanCentersMhz(standardScanCentersMhz,
                                        pendingSettings.sampleRate,
                                        0,
                                        &standardScanError,
                                        &adjusted);
        if (adjusted && standardScanError.isEmpty() && !normalized.isEmpty()) {
            standardScanCentersMhz = formatMhzList(normalized);
        }
    }
    spurSuppressionEnabled = readBool("spurSuppressionEnabled", spurSuppressionEnabled);
    if (settingsJson.contains(QStringLiteral("spurMask"))) {
        QVector<SpurMaskEntry> nextMask;
        const QJsonArray array = settingsJson.value(QStringLiteral("spurMask")).toArray();
        for (const QJsonValue &value : array) {
            const QJsonObject object = value.toObject();
            SpurMaskEntry entry;
            entry.offsetHz = object.value(QStringLiteral("offsetHz")).toDouble(std::numeric_limits<double>::quiet_NaN());
            entry.widthHz = object.value(QStringLiteral("widthHz")).toDouble(SPUR_MIN_MASK_WIDTH_HZ);
            entry.prominenceDb = static_cast<float>(object.value(QStringLiteral("prominenceDb")).toDouble(0.0));
            entry.hits = object.value(QStringLiteral("hits")).toInt(0);
            if (std::isfinite(entry.offsetHz) &&
                std::isfinite(entry.widthHz) &&
                entry.widthHz > 0.0) {
                nextMask.append(entry);
            }
        }
        spurMaskEntries = nextMask;
    }
    if (spurSuppressionCheckbox) {
        QSignalBlocker blocker(spurSuppressionCheckbox);
        spurSuppressionCheckbox->setChecked(spurSuppressionEnabled);
    }
    updateSpurSuppressionStatus();
    if (normalizeAfterApply) {
        normalizeTuning(pendingSettings);
    }
}
