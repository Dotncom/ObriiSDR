#include "main.h"
#include "multivfowidget.h"
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

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QSettings>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <limits>

extern bool secondGraph;
extern bool syncWariable;
extern float sensitivity;
extern float contrast;
extern bool colorf;

namespace {
constexpr quint32 HF_INTERFERENCE_BASELINE_MAGIC = 0x4648424Cu; // FHBL
constexpr quint32 HF_INTERFERENCE_BASELINE_VERSION = 1u;
constexpr quint32 HF_INTERFERENCE_BASELINE_MAX_POINTS = 2000000u;

QByteArray encodeHfInterferenceBaselineCurve(const std::vector<float> &frequencies,
                                             const std::vector<float> &magnitudes) {
    if (frequencies.empty() || frequencies.size() != magnitudes.size()) {
        return QByteArray();
    }

    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setVersion(QDataStream::Qt_5_12);
    out << HF_INTERFERENCE_BASELINE_MAGIC
        << HF_INTERFERENCE_BASELINE_VERSION
        << static_cast<quint32>(frequencies.size());
    for (std::size_t i = 0; i < frequencies.size(); ++i) {
        out << frequencies[i] << magnitudes[i];
    }
    return qCompress(payload, 1);
}

bool decodeHfInterferenceBaselineCurve(const QByteArray &encoded,
                                       std::vector<float> &frequencies,
                                       std::vector<float> &magnitudes) {
    frequencies.clear();
    magnitudes.clear();
    if (encoded.isEmpty()) {
        return false;
    }

    const QByteArray payload = qUncompress(encoded);
    if (payload.isEmpty()) {
        return false;
    }

    QDataStream in(payload);
    in.setByteOrder(QDataStream::LittleEndian);
    in.setVersion(QDataStream::Qt_5_12);

    quint32 magic = 0;
    quint32 version = 0;
    quint32 count = 0;
    in >> magic >> version >> count;
    if (in.status() != QDataStream::Ok ||
        magic != HF_INTERFERENCE_BASELINE_MAGIC ||
        version != HF_INTERFERENCE_BASELINE_VERSION ||
        count < 2 ||
        count > HF_INTERFERENCE_BASELINE_MAX_POINTS) {
        frequencies.clear();
        magnitudes.clear();
        return false;
    }

    frequencies.resize(count);
    magnitudes.resize(count);
    for (quint32 i = 0; i < count; ++i) {
        in >> frequencies[i] >> magnitudes[i];
    }
    if (in.status() != QDataStream::Ok) {
        frequencies.clear();
        magnitudes.clear();
        return false;
    }
    return true;
}
}

void YourClassName::loadHfInterferenceBaselineCurve() {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    const QByteArray encoded = settings.value(QStringLiteral("hfInterference/baselineCurve")).toByteArray();
    if (encoded.isEmpty()) {
        hfInterferenceBaselineFrequencies.clear();
        hfInterferenceBaselineMagnitudes.clear();
        hfInterferenceBaselineEnabled = false;
        return;
    }
    if (!decodeHfInterferenceBaselineCurve(encoded,
                                           hfInterferenceBaselineFrequencies,
                                           hfInterferenceBaselineMagnitudes)) {
        qDebug() << "[HF interference] stored baseline curve could not be loaded";
        hfInterferenceBaselineFrequencies.clear();
        hfInterferenceBaselineMagnitudes.clear();
        hfInterferenceBaselineEnabled = false;
        return;
    }
    qDebug() << "[HF interference] stored baseline loaded"
             << "bins" << hfInterferenceBaselineFrequencies.size();
}

void YourClassName::saveHfInterferenceBaselineCurve() const {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("hfInterference/baselineCurve"),
                      encodeHfInterferenceBaselineCurve(hfInterferenceBaselineFrequencies,
                                                        hfInterferenceBaselineMagnitudes));
}

void YourClassName::loadPersistentSettings() {
    const QString settingsPath = persistentSettingsFilePath();
    const bool settingsFileExists = QFile::exists(settingsPath);
    QSettings settings(settingsPath, QSettings::IniFormat);
    if (settings.status() != QSettings::NoError) {
        qDebug() << "[Settings] unable to read settings file" << settingsPath
                 << "status" << settings.status();
        return;
    }

    pendingSettings.deviceIndex = settings.value("receiver/deviceIndex", pendingSettings.deviceIndex).toInt();
    pendingSettings.clockSource = settings.value("receiver/clockSource", pendingSettings.clockSource).toInt();
    pendingSettings.inputMode = (std::clamp)(settings.value("receiver/inputMode", pendingSettings.inputMode).toInt(),
                                             static_cast<int>(INPUT_RF),
                                             static_cast<int>(INPUT_HF_NOISE_CANCEL));
    pendingSettings.centerFrequency = settings.value("receiver/centerFrequency", pendingSettings.centerFrequency).toDouble();
    pendingSettings.actualFrequency = settings.value("receiver/actualFrequency", pendingSettings.actualFrequency).toDouble();
    pendingSettings.listeningFrequency = settings.value("receiver/listeningFrequency", pendingSettings.listeningFrequency).toDouble();
    pendingSettings.sampleRate = (std::max)(1000.0, settings.value("receiver/sampleRate", pendingSettings.sampleRate).toDouble());
    pendingSettings.bandwidth = (std::max)(1.0, settings.value("receiver/bandwidth", pendingSettings.bandwidth).toDouble());
    pendingSettings.modulationType = settings.value("receiver/modulationType", pendingSettings.modulationType).toInt();
    pendingSettings.fftLength = (std::max)(1024, settings.value("receiver/fftLength", pendingSettings.fftLength).toInt());
    fftBinWidthModeEnabled = settings.value("receiver/fftBinWidthModeEnabled", fftBinWidthModeEnabled).toBool();
    fftTargetBinWidthHz = (std::clamp)(settings.value("receiver/fftTargetBinWidthHz", fftTargetBinWidthHz).toDouble(),
                                       0.1,
                                       1000000.0);
    if (fftBinWidthModeEnabled) {
        pendingSettings.fftLength = fftLengthForBinWidth(pendingSettings.sampleRate,
                                                          fftTargetBinWidthHz);
    }
    pendingSettings.fftWindowType = normalizedFftWindowType(
        settings.value("receiver/fftWindowType", pendingSettings.fftWindowType).toInt());
    pendingSettings.lnaGain = (std::clamp)(settings.value("receiver/lnaGain", pendingSettings.lnaGain).toInt(), 1, 3);
    pendingSettings.vgaGain = (std::clamp)(settings.value("receiver/vgaGain", pendingSettings.vgaGain).toInt(), 0, 31);
    pendingSettings.hackRfLnaGainDb =
        (std::clamp)(settings.value("receiver/hackRfLnaGainDb",
                                    pendingSettings.hackRfLnaGainDb).toInt(),
                     0,
                     40) / 8 * 8;
    pendingSettings.hackRfVgaGainDb =
        (std::clamp)(settings.value("receiver/hackRfVgaGainDb",
                                    pendingSettings.hackRfVgaGainDb).toInt(),
                     0,
                     62) / 2 * 2;
    pendingSettings.hackRfAmpEnabled =
        settings.value("receiver/hackRfAmpEnabled", pendingSettings.hackRfAmpEnabled).toBool();
    pendingSettings.hackRfBiasTeeEnabled =
        settings.value("receiver/hackRfBiasTeeEnabled", pendingSettings.hackRfBiasTeeEnabled).toBool();
    pendingSettings.hackRfAutomaticBandwidth =
        settings.value("receiver/hackRfAutomaticBandwidth",
                       pendingSettings.hackRfAutomaticBandwidth).toBool();
    pendingSettings.hackRfBandwidthHz =
        (std::clamp)(settings.value("receiver/hackRfBandwidthHz",
                                    pendingSettings.hackRfBandwidthHz).toInt(),
                     0, 28000000);
    pendingSettings.hackRfExplicitTuningEnabled =
        settings.value("receiver/hackRfExplicitTuningEnabled",
                       pendingSettings.hackRfExplicitTuningEnabled).toBool();
    pendingSettings.hackRfExplicitIfHz =
        settings.value("receiver/hackRfExplicitIfHz",
                       pendingSettings.hackRfExplicitIfHz).toDouble();
    pendingSettings.hackRfExplicitLoHz =
        settings.value("receiver/hackRfExplicitLoHz",
                       pendingSettings.hackRfExplicitLoHz).toDouble();
    pendingSettings.hackRfExplicitPath =
        (std::clamp)(settings.value("receiver/hackRfExplicitPath",
                                    pendingSettings.hackRfExplicitPath).toInt(), 0, 2);
    pendingSettings.hackRfClockOutEnabled =
        settings.value("receiver/hackRfClockOutEnabled",
                       pendingSettings.hackRfClockOutEnabled).toBool();
    pendingSettings.hackRfHardwareSyncEnabled =
        settings.value("receiver/hackRfHardwareSyncEnabled",
                       pendingSettings.hackRfHardwareSyncEnabled).toBool();
    pendingSettings.hackRfRxOverrunLimit =
        (std::max)(0, settings.value("receiver/hackRfRxOverrunLimit",
                                     pendingSettings.hackRfRxOverrunLimit).toInt());
    pendingSettings.hackRfOperaCakeEnabled =
        settings.value("receiver/hackRfOperaCakeEnabled",
                       pendingSettings.hackRfOperaCakeEnabled).toBool();
    pendingSettings.hackRfOperaCakeAddress =
        (std::clamp)(settings.value("receiver/hackRfOperaCakeAddress",
                                    pendingSettings.hackRfOperaCakeAddress).toInt(), 0, 7);
    pendingSettings.hackRfOperaCakeMode =
        (std::clamp)(settings.value("receiver/hackRfOperaCakeMode",
                                    pendingSettings.hackRfOperaCakeMode).toInt(), 0, 2);
    pendingSettings.hackRfOperaCakePortA =
        (std::clamp)(settings.value("receiver/hackRfOperaCakePortA",
                                    pendingSettings.hackRfOperaCakePortA).toInt(), 0, 7);
    pendingSettings.hackRfOperaCakePortB =
        (std::clamp)(settings.value("receiver/hackRfOperaCakePortB",
                                    pendingSettings.hackRfOperaCakePortB).toInt(), 0, 7);
    pendingSettings.hackRfOperaCakeRangesJson =
        settings.value("receiver/hackRfOperaCakeRangesJson",
                       pendingSettings.hackRfOperaCakeRangesJson).toString();
    pendingSettings.hackRfOperaCakeDwellsJson =
        settings.value("receiver/hackRfOperaCakeDwellsJson",
                       pendingSettings.hackRfOperaCakeDwellsJson).toString();
    pendingSettings.hackRfSweepRanges =
        settings.value("receiver/hackRfSweepRanges",
                       pendingSettings.hackRfSweepRanges).toString();
    pendingSettings.hackRfSweepStepHz =
        (std::clamp)(settings.value("receiver/hackRfSweepStepHz",
                                    pendingSettings.hackRfSweepStepHz).toInt(),
                     1000, 20000000);
    pendingSettings.hackRfSweepBytesPerTune =
        (std::clamp)(settings.value("receiver/hackRfSweepBytesPerTune",
                                    pendingSettings.hackRfSweepBytesPerTune).toInt(),
                     16384, 16384 * 64);
    pendingSettings.hackRfSweepInterleaved =
        settings.value("receiver/hackRfSweepInterleaved",
                       pendingSettings.hackRfSweepInterleaved).toBool();
    pendingSettings.rtlAgc = settings.value("receiver/rtlAgc", pendingSettings.rtlAgc).toBool();
    pendingSettings.rtlTunerGainTenthsDb =
        (std::clamp)(settings.value("receiver/rtlTunerGainTenthsDb",
                                    pendingSettings.rtlTunerGainTenthsDb).toInt(),
                     0,
                     496);
    pendingSettings.audioDeviceId = (std::max)(0, settings.value("receiver/audioDeviceId", pendingSettings.audioDeviceId).toInt());
    pendingSettings.audioLowPassHz = clampAudioLowPassHz(settings.value("audio/lowPassHz", pendingSettings.audioLowPassHz).toDouble());
    pendingSettings.audioHighPassHz = clampAudioHighPassHz(settings.value("audio/highPassHz", pendingSettings.audioHighPassHz).toDouble());
    pendingSettings.audioFilterChainJson =
        settings.value("audio/filterChainJson", pendingSettings.audioFilterChainJson).toString();
    pendingSettings.simplifiedAudioChannelizer =
        settings.value("audio/simplifiedChannelizer",
                       pendingSettings.simplifiedAudioChannelizer).toBool();
    if (multiVfoWidget) {
        multiVfoWidget->setConfigurationJson(
            settings.value(QStringLiteral("multiVfo/configuration"), QStringLiteral("{\"version\":1,\"enabled\":false,\"channels\":[]}"))
                .toString());
    }
    pendingSettings.hfNoiseCancelDepth = clampHfNoiseCancelDepth(settings.value("hfNoiseCancel/depth", pendingSettings.hfNoiseCancelDepth).toDouble());
    pendingSettings.hfNoiseCancelRefGainDb =
        clampHfNoiseCancelRefGainDb(settings.value("hfNoiseCancel/refGainDb", pendingSettings.hfNoiseCancelRefGainDb).toDouble());
    pendingSettings.hfNoiseCancelRefDelayNs =
        clampHfNoiseCancelRefDelayNs(settings.value("hfNoiseCancel/refDelayNs", pendingSettings.hfNoiseCancelRefDelayNs).toDouble());
    pendingSettings.hfNoiseCancelRefTiltDb =
        clampHfNoiseCancelRefTiltDb(settings.value("hfNoiseCancel/refTiltDb", pendingSettings.hfNoiseCancelRefTiltDb).toDouble());
    pendingSettings.hfNoiseCancelFreeze = settings.value("hfNoiseCancel/freeze", pendingSettings.hfNoiseCancelFreeze).toBool();
    pendingSettings.hfAudioBlankerEnabled =
        settings.value("hfNoiseCancel/audioBlankerEnabled", pendingSettings.hfAudioBlankerEnabled).toBool();
    pendingSettings.hfAudioBlankerThreshold =
        (std::clamp)(settings.value("hfNoiseCancel/audioBlankerThreshold", pendingSettings.hfAudioBlankerThreshold).toDouble(),
                     2.0,
                     20.0);
    hfInterferenceBaselineEnabled = settings.value("hfInterference/baselineEnabled", hfInterferenceBaselineEnabled).toBool();
    hfInterferenceRawOverlayEnabled =
        settings.value("hfInterference/rawOverlayEnabled", hfInterferenceRawOverlayEnabled).toBool();
    hfInterferenceBaselineDepth =
        (std::clamp)(settings.value("hfInterference/baselineDepth", hfInterferenceBaselineDepth).toDouble(), 0.0, 2.0);
    hfInterferenceBaselineSmoothBins =
        (std::clamp)(settings.value("hfInterference/baselineSmoothBins", hfInterferenceBaselineSmoothBins).toInt(), 1, 301) | 1;
    loadHfInterferenceBaselineCurve();
    pendingSettings.audioEnabled = settings.value("receiver/audioEnabled", pendingSettings.audioEnabled).toBool();
    pendingSettings.syncEnabled = false;
    pendingSettings.gpoValue = static_cast<std::uint8_t>((std::clamp)(settings.value("receiver/gpoValue", static_cast<int>(pendingSettings.gpoValue)).toInt(), 0, 255));
    agileScanEnabled = settings.value("agileScan/enabled", agileScanEnabled).toBool();
    agileScanAutoStepSampleRate =
        settings.value("agileScan/autoStepSampleRate", agileScanAutoStepSampleRate).toBool();
    agileScanRangesMhz = settings.value("agileScan/rangesMhz", agileScanRangesMhz).toString().trimmed();
    agileScanStepMhz = (std::clamp)(settings.value("agileScan/stepMhz", agileScanStepMhz).toDouble(),
                                    AGILE_SCAN_MIN_STEP_MHZ,
                                    AGILE_SCAN_MAX_STEP_MHZ);
    applyAgileScanAutoStep(false);
    scanVisualMode =
        normalizedScanVisualMode(settings.value("scan/visualMode", scanVisualMode).toInt());
    standardScanEnabled = settings.value("standardScan/enabled", standardScanEnabled).toBool();
    scanListeningLockEnabled = settings.value("standardScan/listenLock", scanListeningLockEnabled).toBool();
    standardScanCentersMhz = settings.value("standardScan/centersMhz", standardScanCentersMhz).toString().trimmed();
    standardScanDwellMs = (std::clamp)(settings.value("standardScan/dwellMs", standardScanDwellMs).toInt(),
                                       STANDARD_SCAN_MIN_DWELL_MS,
                                       STANDARD_SCAN_MAX_DWELL_MS);
    standardScanSettleMs = (std::clamp)(settings.value("standardScan/settleMs", standardScanSettleMs).toInt(),
                                        STANDARD_SCAN_MIN_SETTLE_MS,
                                        STANDARD_SCAN_MAX_SETTLE_MS);
    standardScanRangeStartMhz = settings.value("standardScan/rangeStartMhz", standardScanRangeStartMhz).toString().trimmed();
    standardScanRangeEndMhz = settings.value("standardScan/rangeEndMhz", standardScanRangeEndMhz).toString().trimmed();
    listeningScanEnabled = settings.value("listeningScan/enabled", listeningScanEnabled).toBool();
    listeningScanTargetsMhz = settings.value("listeningScan/targetsMhz", listeningScanTargetsMhz).toString().trimmed();
    listeningScanDwellMs = (std::clamp)(settings.value("listeningScan/dwellMs", listeningScanDwellMs).toInt(),
                                        LISTENING_SCAN_MIN_DWELL_MS,
                                        LISTENING_SCAN_MAX_DWELL_MS);
    listeningScanSettleMs = (std::clamp)(settings.value("listeningScan/settleMs", listeningScanSettleMs).toInt(),
                                         LISTENING_SCAN_MIN_SETTLE_MS,
                                         LISTENING_SCAN_MAX_SETTLE_MS);
    qthLatitude = (std::clamp)(settings.value("gpsQth/latitude", qthLatitude).toDouble(), -90.0, 90.0);
    qthLongitude = (std::clamp)(settings.value("gpsQth/longitude", qthLongitude).toDouble(), -180.0, 180.0);
    qthPositionVisible = settings.value("gpsQth/positionVisible", qthPositionVisible).toBool();
    qthSource = settings.value("gpsQth/source", qthSource).toString().trimmed();
    if (qthSource.isEmpty()) {
        qthSource = QStringLiteral("manual");
    }
    if (qthSource == QStringLiteral("nmea")) {
        qthPositionVisible = false;
    }
    gnssSerialPortName =
        settings.value("gpsQth/nmeaSerialPort", gnssSerialPortName).toString().trimmed();
    gnssSerialBaud =
        (std::clamp)(settings.value("gpsQth/nmeaSerialBaud", gnssSerialBaud).toInt(),
                     1200,
                     921600);
    gnssPositionPolicy =
        normalizedGnssPositionPolicy(settings.value("gpsQth/positionPolicy", gnssPositionPolicy).toString());
    gnssUbxAutoEnable =
        settings.value("gpsQth/ubxAutoEnable", gnssUbxAutoEnable).toBool();
    gnssTimeZoneOffsetMinutes =
        (std::clamp)(settings.value("gpsQth/timeZoneOffsetMinutes", gnssTimeZoneOffsetMinutes).toInt(),
                     -12 * 60,
                     100000);
    gnssSatelliteTableVisible =
        settings.value("gpsQth/satelliteTableVisible", gnssSatelliteTableVisible).toBool();
    qthTileDirectory = settings.value("gpsQth/tileDirectory", qthTileDirectory).toString().trimmed();
    qthMapLayer = (std::clamp)(settings.value("gpsQth/mapLayer", qthMapLayer).toInt(), 0, 2);
    qthMapZoom = (std::clamp)(settings.value("gpsQth/mapZoom", qthMapZoom).toInt(), 0, 19);
    qthOnlineProviderId =
        settings.value("gpsQth/onlineProviderId", qthOnlineProviderId).toString().trimmed();
    if (qthOnlineProviderId.isEmpty()) {
        qthOnlineProviderId = QStringLiteral("custom");
    }
    qthOnlineTileUrlTemplate =
        settings.value("gpsQth/onlineTileUrlTemplate", qthOnlineTileUrlTemplate).toString().trimmed();
    qthOnlineAttribution =
        settings.value("gpsQth/onlineAttribution", qthOnlineAttribution).toString().trimmed();
    qthOnlineApiKey =
        settings.value("gpsQth/onlineApiKey", qthOnlineApiKey).toString().trimmed();
    qthOnlineNoDiskCache =
        settings.value("gpsQth/onlineNoDiskCache", qthOnlineNoDiskCache).toBool();
    gnssSystemId =
        gnssSystemPreset(settings.value("gpsQth/gnssSystemId", gnssSystemId).toString().trimmed()).id;
    gnssMonitorEnabled =
        settings.value("gpsQth/gnssMonitorEnabled", gnssMonitorEnabled).toBool();
    gnssUseGps = settings.value("gpsQth/gnssUseGps", gnssUseGps).toBool();
    gnssUseGlonass = settings.value("gpsQth/gnssUseGlonass", gnssUseGlonass).toBool();
    gnssUseGalileo = settings.value("gpsQth/gnssUseGalileo", gnssUseGalileo).toBool();
    gnssUseBeidou = settings.value("gpsQth/gnssUseBeidou", gnssUseBeidou).toBool();
    gnssUseQzss = settings.value("gpsQth/gnssUseQzss", gnssUseQzss).toBool();
    gnssUseSbas = settings.value("gpsQth/gnssUseSbas", gnssUseSbas).toBool();
    gnssUseOther = settings.value("gpsQth/gnssUseOther", gnssUseOther).toBool();
    gnssDisabledSatelliteKeys.clear();
    const QStringList disabledSatelliteKeys =
        settings.value("gpsQth/gnssDisabledSatellites").toStringList();
    for (const QString &key : disabledSatelliteKeys) {
        const QString normalized = key.trimmed();
        if (!normalized.isEmpty()) {
            gnssDisabledSatelliteKeys.insert(normalized);
        }
    }
    gnssAcquisitionIntegrationMs =
        (std::clamp)(settings.value("gpsQth/gnssAcquisitionIntegrationMs",
                                    gnssAcquisitionIntegrationMs).toInt(),
                     GNSS_ACQUISITION_MIN_INTEGRATION_MS,
                     GNSS_ACQUISITION_MAX_INTEGRATION_MS);
    gnssChannelFilterCutoffHz =
        (std::clamp)(settings.value("gpsQth/gnssChannelFilterCutoffHz",
                                    gnssChannelFilterCutoffHz).toDouble(),
                     GNSS_CHANNEL_FILTER_MIN_HZ,
                     GNSS_CHANNEL_FILTER_MAX_HZ);
    gnssDopplerSpanHz =
        (std::clamp)(settings.value("gpsQth/gnssDopplerSpanHz", gnssDopplerSpanHz).toInt(),
                     1000,
                     50000);
    gnssDopplerStepHz =
        (std::clamp)(settings.value("gpsQth/gnssDopplerStepHz", gnssDopplerStepHz).toInt(),
                     250,
                     5000);
    gnssContinuousAcquisitionEnabled =
        settings.value("gpsQth/gnssContinuousAcquisitionEnabled",
                       gnssContinuousAcquisitionEnabled).toBool();
    gnssContinuousAcquisitionIntervalMs =
        (std::clamp)(settings.value("gpsQth/gnssContinuousAcquisitionIntervalMs",
                                    gnssContinuousAcquisitionIntervalMs).toInt(),
                     GNSS_CONTINUOUS_ACQUISITION_MIN_INTERVAL_MS,
                     GNSS_CONTINUOUS_ACQUISITION_MAX_INTERVAL_MS);
    qthGridPrecision = settings.value("gpsQth/gridPrecision", qthGridPrecision).toInt();
    if (qthGridPrecision <= 2) {
        qthGridPrecision = 2;
    } else if (qthGridPrecision <= 4) {
        qthGridPrecision = 4;
    } else {
        qthGridPrecision = 6;
    }
    qthMapOverlayMode = (std::clamp)(settings.value("gpsQth/mapOverlayMode", qthMapOverlayMode).toInt(), 0, 3);
    qthUserMarkers.clear();
    QVector<int> qthMarkerNumbers;
    const int qthMarkerCount = settings.beginReadArray(QStringLiteral("gpsQth/markers"));
    for (int i = 0; i < qthMarkerCount; ++i) {
        settings.setArrayIndex(i);
        qth::UserMarker marker;
        marker.number = settings.value(QStringLiteral("number")).toInt();
        marker.name = settings.value(QStringLiteral("name")).toString().trimmed().left(80);
        marker.description = settings.value(QStringLiteral("description")).toString().trimmed().left(512);
        marker.latitude = settings.value(QStringLiteral("latitude")).toDouble();
        marker.longitude = settings.value(QStringLiteral("longitude")).toDouble();
        if (marker.number <= 0 ||
            qthMarkerNumbers.contains(marker.number) ||
            !qth::isValidLatitude(marker.latitude) ||
            !qth::isValidLongitude(marker.longitude)) {
            continue;
        }
        if (marker.name.isEmpty()) {
            marker.name = qth::maidenheadLocator(marker.latitude, marker.longitude, 6);
        }
        qthMarkerNumbers.append(marker.number);
        qthUserMarkers.append(marker);
    }
    settings.endArray();
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
    scanMeasurementEnabled =
        settings.value("spectrumMeasurement/enabled",
                       settings.value("agileScan/measurementEnabled", scanMeasurementEnabled)).toBool();
    waterfallAreaMeasurementEnabled =
        settings.value("spectrumMeasurement/waterfallAreaMeasurement",
                       waterfallAreaMeasurementEnabled).toBool();
    scanMeasurementBinMhz = (std::clamp)(settings.value("spectrumMeasurement/binMhz",
                                                        settings.value("agileScan/measurementBinMhz", scanMeasurementBinMhz)).toDouble(),
                                         SCAN_MEASUREMENT_MIN_BIN_MHZ,
                                         SCAN_MEASUREMENT_MAX_BIN_MHZ);
    scanMeasurementUpdateIntervalMs =
        (std::clamp)(settings.value("spectrumMeasurement/updateIntervalMs",
                                    scanMeasurementUpdateIntervalMs).toInt(),
                     SCAN_MEASUREMENT_MIN_UPDATE_MS,
                     SCAN_MEASUREMENT_MAX_UPDATE_MS);
    spectrumScienceMaxHoldEnabled = settings.value("spectrumScience/maxHold", spectrumScienceMaxHoldEnabled).toBool();
    if (settings.contains("spectrumScience/markers"))
        spectrumScienceAnalyzer.restoreMarkerConfiguration(QJsonDocument::fromJson(settings.value("spectrumScience/markers").toByteArray()).array());
    spectrumScienceMinHoldEnabled = settings.value("spectrumScience/minHold", spectrumScienceMinHoldEnabled).toBool();
    spectrumScienceAverageEnabled = settings.value("spectrumScience/average", spectrumScienceAverageEnabled).toBool();
    spectrumScienceAverageSeconds = (std::clamp)(settings.value("spectrumScience/averageSeconds",
                                                                spectrumScienceAverageSeconds).toDouble(),
                                                  0.05,
                                                  60.0);
    spectrumDetectorMode = normalizedSpectrumDetectorMode(
        settings.value("spectrumScience/detectorMode", spectrumDetectorMode).toInt());
    spectrumDetectorFrames = (std::clamp)(
        settings.value("spectrumScience/detectorFrames", spectrumDetectorFrames).toInt(), 1, 256);
    spectrumVbwHz = (std::clamp)(
        settings.value("spectrumScience/vbwHz", spectrumVbwHz).toDouble(), 0.0, 10000.0);
    spectrumFftOverlapPercent = settings.value("spectrumScience/fftOverlapPercent",
                                               spectrumFftOverlapPercent).toInt();
    if (spectrumFftOverlapPercent != 25 && spectrumFftOverlapPercent != 50 &&
        spectrumFftOverlapPercent != 75) spectrumFftOverlapPercent = 0;
    spectrumAverageFrameCount = (std::clamp)(
        settings.value("spectrumScience/averageFrameCount", spectrumAverageFrameCount).toInt(), 0, 10000);
    spectrumPercentile50Enabled = settings.value("spectrumScience/p50", spectrumPercentile50Enabled).toBool();
    spectrumPercentile90Enabled = settings.value("spectrumScience/p90", spectrumPercentile90Enabled).toBool();
    spectrumPercentile99Enabled = settings.value("spectrumScience/p99", spectrumPercentile99Enabled).toBool();
    spectrumAmplitudeUnit = (std::clamp)(settings.value("spectrumScience/amplitudeUnit",
                                                        spectrumAmplitudeUnit).toInt(), 0, 3);
    dmrHunterSettings.enabled = settings.value("dmrHunter/enabled", dmrHunterSettings.enabled).toBool();
    dmrHunterSettings.minWidthKhz =
        settings.value("dmrHunter/minWidthKhz", dmrHunterSettings.minWidthKhz).toDouble();
    dmrHunterSettings.maxWidthKhz =
        settings.value("dmrHunter/maxWidthKhz", dmrHunterSettings.maxWidthKhz).toDouble();
    dmrHunterSettings.thresholdDb =
        settings.value("dmrHunter/thresholdDb", dmrHunterSettings.thresholdDb).toDouble();
    dmrHunterSettings = DmrHunterDetector::normalizedSettings(dmrHunterSettings);
    fpvHunterSettings.enabled = settings.value("fpvHunter/enabled", fpvHunterSettings.enabled).toBool();
    fpvHunterSettings.minWidthMhz =
        settings.value("fpvHunter/minWidthMhz", fpvHunterSettings.minWidthMhz).toDouble();
    fpvHunterSettings.maxWidthMhz =
        settings.value("fpvHunter/maxWidthMhz", fpvHunterSettings.maxWidthMhz).toDouble();
    fpvHunterSettings.thresholdDb =
        settings.value("fpvHunter/thresholdDb", fpvHunterSettings.thresholdDb).toDouble();
    fpvHunterSettings = FpvHunterDetector::normalizedSettings(fpvHunterSettings);
    fpvHunterFollowEnabled = settings.value("fpvHunter/followEnabled", fpvHunterFollowEnabled).toBool();
    digitalVideoHunterSettings.enabled =
        settings.value("digitalVideoHunter/enabled", digitalVideoHunterSettings.enabled).toBool();
    digitalVideoHunterSettings.minWidthMhz =
        settings.value("digitalVideoHunter/minWidthMhz", digitalVideoHunterSettings.minWidthMhz).toDouble();
    digitalVideoHunterSettings.maxWidthMhz =
        settings.value("digitalVideoHunter/maxWidthMhz", digitalVideoHunterSettings.maxWidthMhz).toDouble();
    digitalVideoHunterSettings.thresholdDb =
        settings.value("digitalVideoHunter/thresholdDb", digitalVideoHunterSettings.thresholdDb).toDouble();
    digitalVideoHunterSettings = DigitalVideoHunterDetector::normalizedSettings(digitalVideoHunterSettings);
    agileScanPresets.clear();
    const int scanPresetCount = settings.beginReadArray("agileScan/presets");
    for (int i = 0; i < scanPresetCount; ++i) {
        settings.setArrayIndex(i);
        const QString name = settings.value("name").toString().trimmed();
        const QString ranges = settings.value("rangesMhz").toString().trimmed();
        const double step = (std::clamp)(settings.value("stepMhz", agileScanStepMhz).toDouble(),
                                         AGILE_SCAN_MIN_STEP_MHZ,
                                         AGILE_SCAN_MAX_STEP_MHZ);
        if (!name.isEmpty() && !ranges.isEmpty()) {
            agileScanPresets[name] = agileScanPresetSpec(ranges, step);
        }
    }
    settings.endArray();
    agileScanPresetOrder = settings.value("agileScan/presetOrder").toStringList();
    if (agileScanPresets.isEmpty()) {
        agileScanPresets[QStringLiteral("Narrow DMR example")] =
            agileScanPresetSpec(QStringLiteral("430-432"), 0.0125);
        agileScanPresets[QStringLiteral("VHF DMR 160-174 coarse")] =
            agileScanPresetSpec(QStringLiteral("160-174"), 0.1);
        agileScanPresets[QStringLiteral("UHF DMR 400-470 coarse")] =
            agileScanPresetSpec(QStringLiteral("400-470"), 0.5);
        agileScanPresets[QStringLiteral("FPV 1.2/2.4 sparse")] =
            agileScanPresetSpec(QStringLiteral("1080-1360\\2300-2500"), 5.0);
    }
    if (!agileScanPresets.contains(QStringLiteral("REB broad check 300/600/5800"))) {
        agileScanPresets[QStringLiteral("REB broad check 300/600/5800")] =
            agileScanPresetSpec(QStringLiteral("300-400\\600-1200\\5650-5950"), 5.0);
    }
    if (!agileScanPresets.contains(QStringLiteral("REB 300-400 1MHz"))) {
        agileScanPresets[QStringLiteral("REB 300-400 1MHz")] =
            agileScanPresetSpec(QStringLiteral("300-400"), 1.0);
    }
    if (!agileScanPresets.contains(QStringLiteral("REB 600-1200 5MHz"))) {
        agileScanPresets[QStringLiteral("REB 600-1200 5MHz")] =
            agileScanPresetSpec(QStringLiteral("600-1200"), 5.0);
    }
    if (!agileScanPresets.contains(QStringLiteral("REB 5.8GHz 5MHz"))) {
        agileScanPresets[QStringLiteral("REB 5.8GHz 5MHz")] =
            agileScanPresetSpec(QStringLiteral("5650-5950"), 5.0);
    }
    if (!agileScanPresets.contains(QStringLiteral("Digital video sparse"))) {
        agileScanPresets[QStringLiteral("Digital video sparse")] =
            agileScanPresetSpec(QStringLiteral("1080-1360\\2300-2500\\3200-3500\\4900-5925"), 10.0);
    } else {
        const QString legacyDigitalVideoRanges = QStringLiteral("1080-1360\\2300-2500\\3200-3500\\4900-5925");
        const QString legacySpec = agileScanPresets.value(QStringLiteral("Digital video sparse"));
        if (agileScanPresetRanges(legacySpec) == legacyDigitalVideoRanges &&
            agileScanPresetStepMhz(legacySpec, 10.0) < 10.0) {
            QString parseError;
            parseAgileScanFrequenciesMhz(legacyDigitalVideoRanges,
                                         agileScanPresetStepMhz(legacySpec, 5.0),
                                         &parseError);
            if (parseError.contains(QStringLiteral("Too many scan points"), Qt::CaseInsensitive)) {
                agileScanPresets[QStringLiteral("Digital video sparse")] =
                    agileScanPresetSpec(legacyDigitalVideoRanges, 10.0);
            }
        }
    }
    if (!agileScanPresets.contains(QStringLiteral("Cellular LTE/3G downlinks sparse"))) {
        agileScanPresets[QStringLiteral("Cellular LTE/3G downlinks sparse")] =
            agileScanPresetSpec(QStringLiteral("758-821\\925-960\\1805-1880\\2110-2170\\2620-2690"), 10.0);
    }
    if (!agileScanPresets.contains(QStringLiteral("UHF Satcom 240-270 250kHz"))) {
        agileScanPresets[QStringLiteral("UHF Satcom 240-270 250kHz")] =
            agileScanPresetSpec(QStringLiteral("240-270"), 0.25);
    }
    if (!agileScanPresets.contains(QStringLiteral("FPV 1.1-1.3 common"))) {
        agileScanPresets[QStringLiteral("FPV 1.1-1.3 common")] =
            agileScanPresetSpec(QStringLiteral("1080,1120,1160,1200,1240,1258,1280,1320,1360"), 1.0);
    }
    if (!agileScanPresets.contains(QStringLiteral("FPV 3.3GHz sparse"))) {
        agileScanPresets[QStringLiteral("FPV 3.3GHz sparse")] =
            agileScanPresetSpec(QStringLiteral("3200-3500"), 50.0);
    }
    if (!agileScanPresets.contains(QStringLiteral("GNSS L1 1559-1610 50MHz"))) {
        agileScanPresets[QStringLiteral("GNSS L1 1559-1610 50MHz")] =
            agileScanPresetSpec(QStringLiteral("1559-1610"), GNSS_USEFUL_STANDARD_SPAN_HZ / 1000000.0);
    }
    agileScanPresetOrder =
        normalizedPresetOrder(agileScanPresetOrder,
                              agileScanPresets,
                              defaultAgileScanPresetOrder());

    standardScanPresets.clear();
    const int standardScanPresetCount = settings.beginReadArray("standardScan/presets");
    for (int i = 0; i < standardScanPresetCount; ++i) {
        settings.setArrayIndex(i);
        const QString name = settings.value("name").toString().trimmed();
        const QString centers = settings.value("centersMhz").toString().trimmed();
        const int dwellMs = (std::clamp)(settings.value("dwellMs", standardScanDwellMs).toInt(),
                                         STANDARD_SCAN_MIN_DWELL_MS,
                                         STANDARD_SCAN_MAX_DWELL_MS);
        const int settleMs = (std::clamp)(settings.value("settleMs", standardScanSettleMs).toInt(),
                                          STANDARD_SCAN_MIN_SETTLE_MS,
                                          STANDARD_SCAN_MAX_SETTLE_MS);
        QString parseError;
        parseStandardScanCentersMhz(centers, pendingSettings.sampleRate, AGILE_SCAN_MIN_POINTS, &parseError, nullptr);
        if (!name.isEmpty() && parseError.isEmpty()) {
            standardScanPresets[name] = standardScanPresetSpec(centers, dwellMs, settleMs);
        }
    }
    settings.endArray();
    standardScanPresetOrder = settings.value("standardScan/presetOrder").toStringList();
    if (standardScanPresets.isEmpty()) {
        standardScanPresets[QStringLiteral("RF 100-300 by 50MHz")] =
            standardScanPresetSpec(QStringLiteral("100, 150, 200, 250, 300"), 120, 40);
        standardScanPresets[QStringLiteral("UHF broad 400-700 by 50MHz")] =
            standardScanPresetSpec(QStringLiteral("400, 450, 500, 550, 600, 650, 700"), 120, 40);
    }
    if (!standardScanPresets.contains(QStringLiteral("Cellular LTE/3G downlinks"))) {
        standardScanPresets[QStringLiteral("Cellular LTE/3G downlinks")] =
            standardScanPresetSpec(QStringLiteral("780.5, 942.5, 1842.5, 2140, 2655"), 180, 80);
    }
    if (!standardScanPresets.contains(QStringLiteral("GNSS L1 two-center 50MHz useful"))) {
        standardScanPresets[QStringLiteral("GNSS L1 two-center 50MHz useful")] =
            standardScanPresetSpec(QStringLiteral("1575.420000, 1625.420000"), 300, 80);
    }
    standardScanPresetOrder =
        normalizedPresetOrder(standardScanPresetOrder,
                              standardScanPresets,
                              defaultStandardScanPresetOrder());

    listeningScanPresets.clear();
    const int listeningScanPresetCount = settings.beginReadArray("listeningScan/presets");
    for (int i = 0; i < listeningScanPresetCount; ++i) {
        settings.setArrayIndex(i);
        const QString name = settings.value("name").toString().trimmed();
        const QString targets = settings.value("targetsMhz").toString().trimmed();
        const int dwellMs = (std::clamp)(settings.value("dwellMs", listeningScanDwellMs).toInt(),
                                         LISTENING_SCAN_MIN_DWELL_MS,
                                         LISTENING_SCAN_MAX_DWELL_MS);
        const int settleMs = (std::clamp)(settings.value("settleMs", listeningScanSettleMs).toInt(),
                                          LISTENING_SCAN_MIN_SETTLE_MS,
                                          LISTENING_SCAN_MAX_SETTLE_MS);
        QString parseError;
        parseListeningScanTargetsMhz(targets, 0.0, RF_EXPERIMENTAL_MAX_FREQUENCY, 1, &parseError);
        if (!name.isEmpty() && parseError.isEmpty()) {
            listeningScanPresets[name] = listeningScanPresetSpec(targets, dwellMs, settleMs);
        }
    }
    settings.endArray();
    listeningScanPresetOrder = settings.value("listeningScan/presetOrder").toStringList();
    if (listeningScanPresets.isEmpty()) {
        listeningScanPresets[QStringLiteral("GNSS L1 main signals")] =
            listeningScanPresetSpec(QStringLiteral("1561.098, 1575.420, 1602.000"), 3000, 100);
        listeningScanPresets[QStringLiteral("FT8 HF common")] =
            listeningScanPresetSpec(QStringLiteral("1.840, 3.573, 7.074, 10.136, 14.074, 18.100, 21.074, 24.915, 28.074, 50.313"), 5000, 100);
    }
    if (!listeningScanPresets.contains(QStringLiteral("Cellular LTE/3G anchors"))) {
        listeningScanPresets[QStringLiteral("Cellular LTE/3G anchors")] =
            listeningScanPresetSpec(QStringLiteral("780.5, 806, 942.5, 1842.5, 2140, 2655"), 3000, 100);
    }
    if (!listeningScanPresets.contains(QStringLiteral("UHF Satcom survey"))) {
        listeningScanPresets[QStringLiteral("UHF Satcom survey")] =
            listeningScanPresetSpec(QStringLiteral("243, 250, 255, 260, 265, 270"), 3000, 100);
    }
    if (!listeningScanPresets.contains(QStringLiteral("GLONASS L1OF channels"))) {
        listeningScanPresets[QStringLiteral("GLONASS L1OF channels")] =
            listeningScanPresetSpec(QStringLiteral("1598.0625, 1598.625, 1599.1875, 1599.750, 1600.3125, 1600.875, 1601.4375, 1602.000, 1602.5625, 1603.125, 1603.6875, 1604.250, 1604.8125, 1605.375"), 2500, 100);
    }
    if (!listeningScanPresets.contains(QStringLiteral("GNSS L1 dense"))) {
        listeningScanPresets[QStringLiteral("GNSS L1 dense")] =
            listeningScanPresetSpec(QStringLiteral("1561.098, 1575.420, 1598.0625, 1598.625, 1599.1875, 1599.750, 1600.3125, 1600.875, 1601.4375, 1602.000, 1602.5625, 1603.125, 1603.6875, 1604.250, 1604.8125, 1605.375"), 2500, 100);
    }
    listeningScanPresetOrder =
        normalizedPresetOrder(listeningScanPresetOrder,
                              listeningScanPresets,
                              defaultListeningScanPresetOrder());

    auto readFrequencyPresetArray = [&settings](const char *path, QMap<QString, double> &target) {
        target.clear();
        const int count = settings.beginReadArray(QString::fromLatin1(path));
        for (int i = 0; i < count; ++i) {
            settings.setArrayIndex(i);
            const QString name = settings.value("name").toString().trimmed();
            bool ok = false;
            const double value = settings.value("valueHz").toDouble(&ok);
            if (!name.isEmpty() && ok && std::isfinite(value)) {
                target[name] = value;
            }
        }
        settings.endArray();
    };
    readFrequencyPresetArray("frequencyPresets/center", centerFrequencyPresets);
    readFrequencyPresetArray("frequencyPresets/listening", listeningFrequencyPresets);
    readFrequencyPresetArray("frequencyPresets/bandwidth", bandwidthValuePresets);
    centerFrequencyPresetOrder = settings.value("frequencyPresets/centerOrder").toStringList();
    listeningFrequencyPresetOrder = settings.value("frequencyPresets/listeningOrder").toStringList();
    bandwidthPresetOrder = settings.value("frequencyPresets/bandwidthOrder").toStringList();
    ensureDefaultFrequencyPresets();
    centerFrequencyUnitIndex = (std::clamp)(settings.value("frequencyControls/centerUnitIndex", centerFrequencyUnitIndex).toInt(), 0, 3);
    listeningFrequencyUnitIndex = (std::clamp)(settings.value("frequencyControls/listeningUnitIndex", listeningFrequencyUnitIndex).toInt(), 0, 3);
    bandwidthUnitIndex = (std::clamp)(settings.value("frequencyControls/bandwidthUnitIndex", bandwidthUnitIndex).toInt(), 0, 3);
    centerFrequencyStepName = settings.value("frequencyControls/centerStepName", centerFrequencyStepName).toString();
    listeningFrequencyStepName = settings.value("frequencyControls/listeningStepName", listeningFrequencyStepName).toString();
    bandwidthStepName = settings.value("frequencyControls/bandwidthStepName", bandwidthStepName).toString();
    centerFrequencyPresetName = settings.value("frequencyControls/centerPresetName", centerFrequencyPresetName).toString();
    listeningFrequencyPresetName = settings.value("frequencyControls/listeningPresetName", listeningFrequencyPresetName).toString();
    bandwidthPresetName = settings.value("frequencyControls/bandwidthPresetName", bandwidthPresetName).toString();
    frequencyControlUiStateRestorePending = true;

    bandMarkers.clear();
    bandMarkersCustomized = settings.contains(QStringLiteral("bandMarkers/size"));
    const int bandMarkerCount = settings.beginReadArray(QStringLiteral("bandMarkers"));
    for (int i = 0; i < bandMarkerCount; ++i) {
        settings.setArrayIndex(i);
        GraphBandMarker marker;
        marker.startHz = settings.value(QStringLiteral("startHz")).toDouble();
        marker.endHz = settings.value(QStringLiteral("endHz")).toDouble();
        marker.label = settings.value(QStringLiteral("label")).toString().trimmed();
        marker.amateur = settings.value(QStringLiteral("amateur")).toBool();
        if (!marker.label.isEmpty() &&
            std::isfinite(marker.startHz) &&
            std::isfinite(marker.endHz) &&
            marker.endHz > marker.startHz) {
            bandMarkers.append(marker);
        }
    }
    settings.endArray();
    if (bandMarkers.isEmpty()) {
        bandMarkersCustomized = false;
    }
    if (!bandMarkersCustomized) {
        ensureDefaultBandMarkers();
    }

    currentScale = (std::clamp)(settings.value("display/scalePercent", currentScale).toDouble(),
                                MIN_SCALE_PERCENT,
                                MAX_SCALE_PERCENT);
    additionalScaleDivisor = (std::clamp)(
        settings.value("display/additionalScaleDivisor", additionalScaleDivisor).toInt(),
        1,
        ADDITIONAL_SCALE_DIVISOR_MAX);
    contrast = static_cast<float>((std::clamp)(settings.value("display/contrast", static_cast<double>(contrast)).toDouble(), 1.0, 20.0));
    sensitivity = static_cast<float>((std::clamp)(settings.value("display/sensitivity", static_cast<double>(sensitivity)).toDouble(), 1.0, 30.0));
    displayLevelMin = static_cast<float>((std::clamp)(settings.value("display/levelMin", static_cast<double>(displayLevelMin)).toDouble(), -160.0, 20.0));
    displayLevelMax = static_cast<float>((std::clamp)(settings.value("display/levelMax", static_cast<double>(displayLevelMax)).toDouble(), -160.0, 20.0));
    if (displayLevelMin >= displayLevelMax) {
        displayLevelMin = (std::max)(-160.0f, displayLevelMax - MIN_LEVEL_GAP);
    }
    secondGraph = settings.value("display/secondGraph", secondGraph).toBool();
    colorf = settings.value("display/color", colorf).toBool();
    showGeneralBandMarkers = settings.value("display/generalBandMarkers", showGeneralBandMarkers).toBool();
    showAmateurBandMarkers = settings.value("display/amateurBandMarkers", showAmateurBandMarkers).toBool();
    compactBandMarkers = settings.value("display/compactBandMarkers", compactBandMarkers).toBool();
    spurSuppressionEnabled = settings.value("display/spurSuppressionEnabled", spurSuppressionEnabled).toBool();
    diagnosticVerboseLogging =
        settings.value("diagnostics/verboseLogging",
                       fobosVerboseLoggingDefaultEnabled()).toBool();
    extendedRecordingMetadataEnabled =
        settings.value("recording/extendedMetadata", extendedRecordingMetadataEnabled).toBool();
    setFobosVerboseLoggingEnabled(diagnosticVerboseLogging);
    qDebug() << "[Log] Verbose diagnostic logging"
             << (diagnosticVerboseLogging ? "enabled" : "disabled")
             << "source" << (settings.contains("diagnostics/verboseLogging") ? "settings" : "default");
    spurMaskEntries.clear();
    const int spurMaskCount = settings.beginReadArray(QStringLiteral("display/spurMask"));
    for (int i = 0; i < spurMaskCount; ++i) {
        settings.setArrayIndex(i);
        SpurMaskEntry entry;
        entry.offsetHz = settings.value(QStringLiteral("offsetHz")).toDouble();
        entry.widthHz = settings.value(QStringLiteral("widthHz"), SPUR_MIN_MASK_WIDTH_HZ).toDouble();
        entry.prominenceDb = static_cast<float>(settings.value(QStringLiteral("prominenceDb"), 0.0).toDouble());
        entry.hits = settings.value(QStringLiteral("hits"), 0).toInt();
        if (std::isfinite(entry.offsetHz) &&
            std::isfinite(entry.widthHz) &&
            entry.widthHz > 0.0) {
            spurMaskEntries.append(entry);
        }
    }
    settings.endArray();
    if (spurSuppressionCheckbox) {
        QSignalBlocker blocker(spurSuppressionCheckbox);
        spurSuppressionCheckbox->setChecked(spurSuppressionEnabled);
    }
    updateSpurSuppressionStatus();
    volumePercent = (std::clamp)(settings.value("audio/volumePercent", volumePercent).toInt(), 0, 200);
    uiLanguage = normalizedUiLanguage(settings.value("ui/language", uiLanguage).toString());
    fineTuneControlMode = (std::clamp)(settings.value("ui/fineTuneControlMode", fineTuneControlMode).toInt(),
                                       FINE_TUNE_MODE_SCALE,
                                       FINE_TUNE_MODE_DIAL);
    spectrumUpdateIntervalMs =
        (std::clamp)(settings.value("ui/spectrumUpdateIntervalMs", spectrumUpdateIntervalMs).toInt(),
                     SPECTRUM_UPDATE_AUTO_MS,
                     SPECTRUM_UPDATE_MAX_MS);
    if (spectrumUpdateIntervalMs > 0 && spectrumUpdateIntervalMs < SPECTRUM_UPDATE_MIN_MS) {
        spectrumUpdateIntervalMs = SPECTRUM_UPDATE_MIN_MS;
    }
    fftBackendPreference = normalizedFftBackendPreference(
        settings.value("ui/fftBackendPreference", fftBackendPreference).toInt());
    frequencyCalibrationOffsetHz =
        (std::clamp)(settings.value("calibration/frequencyOffsetHz",
                                    frequencyCalibrationOffsetHz).toDouble(),
                     -10000000.0,
                     10000000.0);
    amplitudeCalibrationOffsetDb =
        (std::clamp)(settings.value("calibration/amplitudeOffsetDb",
                                    amplitudeCalibrationOffsetDb).toDouble(),
                     -200.0,
                     200.0);
    calibrationTableEnabled =
        settings.value("calibration/tableEnabled", calibrationTableEnabled).toBool();
    QVector<ReceiverCalibrationPoint> calibrationPoints;
    const int calibrationPointCount = settings.beginReadArray(QStringLiteral("calibration/table"));
    calibrationPoints.reserve(calibrationPointCount);
    for (int i = 0; i < calibrationPointCount; ++i) {
        settings.setArrayIndex(i);
        ReceiverCalibrationPoint point;
        point.frequencyHz = settings.value(QStringLiteral("frequencyHz")).toDouble();
        point.frequencyOffsetHz = settings.value(QStringLiteral("frequencyOffsetHz")).toDouble();
        point.amplitudeOffsetDb = settings.value(QStringLiteral("amplitudeOffsetDb")).toDouble();
        point.uncertaintyDb = settings.value(QStringLiteral("uncertaintyDb"), 0.0).toDouble();
        point.note = settings.value(QStringLiteral("note")).toString();
        point.conditions = ReceiverCalibrationContext::fromJson(QJsonDocument::fromJson(
            settings.value(QStringLiteral("conditions")).toByteArray()).object());
        calibrationPoints.append(point);
    }
    settings.endArray();
    receiverCalibrationTable.setPoints(calibrationPoints);
    waterfallRowsPerFrame =
        (std::clamp)(settings.value("ui/waterfallRowsPerFrame",
                                    WATERFALL_ROWS_PER_FRAME_DEFAULT).toInt(),
                     WATERFALL_ROWS_PER_FRAME_MIN,
                     WATERFALL_ROWS_PER_FRAME_MAX);
    if (waterfallWidget) {
        waterfallWidget->setRowsPerFrame(waterfallRowsPerFrame);
    }
    waterfallDisplayMode =
        (std::clamp)(settings.value("ui/waterfallDisplayMode", waterfallDisplayMode).toInt(), 0, 2);
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
    waterfall3DResolutionDivisor =
        settings.value("ui/waterfall3DResolutionDivisor", waterfall3DResolutionDivisor).toInt();
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
    if (waterfallWidget) {
        waterfallWidget->set3DResolutionDivisor(waterfall3DResolutionDivisor);
    }
    waterfall3DSurfaceStyle = (std::clamp)(
        settings.value("ui/waterfall3DSurfaceStyle", waterfall3DSurfaceStyle).toInt(), 0, 1);
    waterfall3DSmoothing = (std::clamp)(
        settings.value("ui/waterfall3DSmoothing", waterfall3DSmoothing).toInt(), 0, 2);
    waterfall3DLighting = (std::clamp)(
        settings.value("ui/waterfall3DLighting", waterfall3DLighting).toInt(), 0, 2);
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
        waterfallWidget->set3DSurfaceStyle(waterfall3DSurfaceStyle);
        waterfallWidget->set3DSurfaceSmoothing(waterfall3DSmoothing);
        waterfallWidget->set3DSurfaceLighting(waterfall3DLighting);
    }
    waterfall3DHistoryRows =
        (std::clamp)(settings.value("ui/waterfall3DHistoryRows", waterfall3DHistoryRows).toInt(),
                     16,
                     2048);
    if (waterfall3DHistoryRowsSpin) {
        QSignalBlocker blocker(waterfall3DHistoryRowsSpin);
        waterfall3DHistoryRowsSpin->setValue(waterfall3DHistoryRows);
    }
    if (waterfallWidget) {
        waterfallWidget->set3DHistoryRows(waterfall3DHistoryRows);
    }
    waterfall3DSliceScrollStep =
        (std::clamp)(settings.value("ui/waterfall3DSliceScrollStep",
                                    waterfall3DSliceScrollStep).toInt(),
                     1,
                     256);
    waterfall3DSliceWidth =
        (std::clamp)(settings.value("ui/waterfall3DSliceWidth",
                                    waterfall3DSliceWidth).toInt(),
                     1,
                     4096);
    waterfall3DSpectrumSliceScrollStep =
        (std::clamp)(settings.value("ui/waterfall3DSpectrumSliceScrollStep",
                                    waterfall3DSpectrumSliceScrollStep).toInt(),
                     1,
                     2048);
    waterfall3DSpectrumSliceRows =
        (std::clamp)(settings.value("ui/waterfall3DSpectrumSliceRows",
                                    waterfall3DSpectrumSliceRows).toInt(),
                     1,
                     2048);
    waterfall3DSpectrumSliceCapture =
        settings.value("ui/waterfall3DSpectrumSliceCapture",
                       waterfall3DSpectrumSliceCapture).toBool();
    waterfall3DSpectrumSliceCaptureFixed =
        settings.value("ui/waterfall3DSpectrumSliceCaptureFixed",
                       waterfall3DSpectrumSliceCaptureFixed).toBool();
    waterfall3DFixedPlane =
        settings.value("ui/waterfall3DFixedPlane", waterfall3DFixedPlane).toBool();
    waterfall3DMonochrome =
        settings.value("ui/waterfall3DMonochrome", waterfall3DMonochrome).toBool();
    waterfall3DAlternativeView =
        settings.value("ui/waterfall3DAlternativeView", waterfall3DAlternativeView).toBool();
    waterfall3DVncSliceInput =
        settings.value("ui/waterfall3DVncSliceInput",
                       waterfall3DVncSliceInput).toBool();
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
        waterfallWidget->set3DModifierFreeSliceInput(waterfall3DVncSliceInput);
    }
    const bool hasWorkspaceModeSetting = settings.contains("ui/waterfall3DAlternativeView");
    const bool legacyAlternativeInterface =
        settings.value("ui/alternativeInterfaceMode", alternativeInterfaceMode).toBool();
    if (!hasWorkspaceModeSetting && legacyAlternativeInterface) {
        waterfall3DAlternativeView = true;
        alternativeInterfaceMode = false;
    } else {
        alternativeInterfaceMode = legacyAlternativeInterface;
    }
    alternativeSpectrumGradientFill =
        settings.value("ui/alternativeSpectrumGradientFill",
                       alternativeSpectrumGradientFill).toBool();
    alternativeSpectrumGradientOpacity =
        (std::clamp)(settings.value("ui/alternativeSpectrumGradientOpacity",
                                    alternativeSpectrumGradientOpacity).toInt(),
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
    experimentalGpuWaterfall = settings.value("ui/experimentalGpuWaterfall", experimentalGpuWaterfall).toBool();
    spectrumDisplayReductionMode = (std::clamp)(
        settings.value("ui/spectrumDisplayReductionMode", spectrumDisplayReductionMode).toInt(),
        0,
        4);
    if (waterfallWidget) {
        waterfallWidget->setRenderBackend(experimentalGpuWaterfall
                                              ? MyWaterfallWidget::RenderBackend::GpuPrepared
                                              : MyWaterfallWidget::RenderBackend::CpuTexture);
    }
    showSpectrumFps = settings.value("ui/showSpectrumFps", showSpectrumFps).toBool();
    showWaterfallFps = settings.value("ui/showWaterfallFps", showWaterfallFps).toBool();
    showAudioQueueDropRows = settings.value("diagnostics/showAudioQueueDropRows", false).toBool();
    showSpectrumPeakMeter = settings.value("ui/showSpectrumPeakMeter", showSpectrumPeakMeter).toBool();
    spectrumPeakMeterStyle = std::clamp(settings.value("ui/spectrumPeakMeterStyle", spectrumPeakMeterStyle).toInt(), 0, 1);
    showExtendedSpectrumInfo = settings.value("ui/showExtendedSpectrumInfo", showExtendedSpectrumInfo).toBool();
    if (graphWidget) {
        graphWidget->setFpsOverlayEnabled(showSpectrumFps);
        graphWidget->setAnalogPeakMeterEnabled(showSpectrumPeakMeter);
        graphWidget->setAnalogPeakMeterStyle(spectrumPeakMeterStyle);
    }
    if (waterfallWidget) {
        waterfallWidget->setFpsOverlayEnabled(showWaterfallFps);
    }
    if (graphWidget) {
        graphWidget->setExtendedInfoOverlayEnabled(showExtendedSpectrumInfo);
    }
    if (waterfallWidget) {
        waterfallWidget->setExtendedInfoOverlayEnabled(showExtendedSpectrumInfo);
    }
    agileLiveRetuneCommandIntervalMs =
        (std::clamp)(settings.value("ui/agileLiveRetuneIntervalMs",
                                    AGILE_LIVE_RETUNE_DEFAULT_COMMAND_INTERVAL_MS).toInt(),
                     AGILE_LIVE_RETUNE_MIN_COMMAND_INTERVAL_MS,
                     AGILE_LIVE_RETUNE_MAX_COMMAND_INTERVAL_MS);
    fineTuneScaleHoldMode = settings.value("ui/fineTuneScaleHoldMode", fineTuneScaleHoldMode).toBool();
    spectrumFrameBufferEnabled =
        settings.value("spectrumFrames/bufferEnabled", spectrumFrameBufferEnabled).toBool();
    spectrumFramePrebufferSeconds =
        (std::clamp)(settings.value("spectrumFrames/prebufferSeconds",
                                    spectrumFramePrebufferSeconds).toInt(),
                     1,
                     120);
    spectrumEventCaptureMode =
        (std::clamp)(settings.value("spectrumFrames/eventCaptureMode",
                                    spectrumEventCaptureMode).toInt(),
                     0,
                     2);
    if (spectrumFrameBinsCombo) {
        const int requestedBins =
            (std::clamp)(settings.value("spectrumFrames/bins",
                                        spectrumFrameBinsCombo->currentData().toInt()).toInt(),
                         256,
                         2097152);
        int bestIndex = 0;
        int bestDistance = std::numeric_limits<int>::max();
        for (int i = 0; i < spectrumFrameBinsCombo->count(); ++i) {
            const int bins = spectrumFrameBinsCombo->itemData(i).toInt();
            const int distance = std::abs(bins - requestedBins);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = i;
            }
        }
        QSignalBlocker blocker(spectrumFrameBinsCombo);
        spectrumFrameBinsCombo->setCurrentIndex(bestIndex);
    }

    networkMode = NetworkMode::Disabled;
    const int processingModeValue = (std::clamp)(settings.value("network/processingMode", static_cast<int>(networkProcessingMode)).toInt(),
                                                 static_cast<int>(NetworkProcessingMode::ServerSide),
                                                 static_cast<int>(NetworkProcessingMode::FullIqClientSide));
    networkProcessingMode = static_cast<NetworkProcessingMode>(processingModeValue);
    networkServerAddress = settings.value("network/serverAddress", networkServerAddress).toString();
    networkBindAddress = settings.value("network/bindAddress", networkBindAddress).toString();
    networkControlPort = static_cast<quint16>((std::clamp)(settings.value("network/controlPort", static_cast<int>(networkControlPort)).toInt(), 1, 65535));
    serverDisableLocalVisualAudio = settings.value("network/serverDisableLocalVisualAudio", serverDisableLocalVisualAudio).toBool();
    networkFullResolutionSpectrumFrames =
        settings.value("network/fullResolutionSpectrumFrames", networkFullResolutionSpectrumFrames).toBool();
    audioRelayTransmitEnabled = settings.value("audioRelay/transmitEnabled", audioRelayTransmitEnabled).toBool();
    audioRelayHost = settings.value("audioRelay/host", audioRelayHost).toString();
    audioRelayPort = static_cast<quint16>((std::clamp)(settings.value("audioRelay/port", static_cast<int>(audioRelayPort)).toInt(), 1, 65535));
    audioRelayReceiveEnabled = settings.value("audioRelay/receiveEnabled", audioRelayReceiveEnabled).toBool();
    audioRelayListenPort = static_cast<quint16>((std::clamp)(settings.value("audioRelay/listenPort", static_cast<int>(audioRelayListenPort)).toInt(), 1, 65535));
    audioHttpStreamEnabled = settings.value("audioHttpStream/enabled", audioHttpStreamEnabled).toBool();
    audioHttpStreamPort = static_cast<quint16>((std::clamp)(settings.value("audioHttpStream/port", static_cast<int>(audioHttpStreamPort)).toInt(), 1, 65535));
    digitalDecodeEnabled = settings.value("digital/decodeEnabled", digitalDecodeEnabled).toBool();
    pendingSettings.cwDecoderToneHz =
        (std::clamp)(settings.value("digital/cwToneHz", pendingSettings.cwDecoderToneHz).toDouble(),
                     100.0,
                     3000.0);
    pendingSettings.cwDecoderWpm =
        (std::clamp)(settings.value("digital/cwInitialWpm", pendingSettings.cwDecoderWpm).toInt(),
                     5,
                     60);
    pendingSettings.cwDecoderAdaptiveSpeed =
        settings.value("digital/cwAdaptiveSpeed", pendingSettings.cwDecoderAdaptiveSpeed).toBool();
    pendingSettings.cwDecoderAlphabet =
        (std::clamp)(settings.value("digital/cwAlphabet", pendingSettings.cwDecoderAlphabet).toInt(),
                     0,
                     2);
    pendingSettings.cwDecoderSelectivity =
        (std::clamp)(settings.value("digital/cwSelectivity",
                                    pendingSettings.cwDecoderSelectivity).toInt(),
                     1,
                     10);
    if (cwDecoderToneSpin) cwDecoderToneSpin->setValue(qRound(pendingSettings.cwDecoderToneHz));
    if (cwDecoderWpmSpin) cwDecoderWpmSpin->setValue(pendingSettings.cwDecoderWpm);
    if (cwDecoderAdaptiveCheckbox) {
        cwDecoderAdaptiveCheckbox->setChecked(pendingSettings.cwDecoderAdaptiveSpeed);
    }
    if (cwDecoderAlphabetCombo) {
        const int index = cwDecoderAlphabetCombo->findData(pendingSettings.cwDecoderAlphabet);
        if (index >= 0) cwDecoderAlphabetCombo->setCurrentIndex(index);
    }
    if (cwDecoderSelectivitySpin) {
        cwDecoderSelectivitySpin->setValue(pendingSettings.cwDecoderSelectivity);
    }
    if (cwDecoderControlsWidget) {
        cwDecoderControlsWidget->setVisible(pendingSettings.modulationType == MOD_CW);
    }
    pendingSettings.pskDecoderToneHz = std::clamp(settings.value("digital/pskToneHz",1000).toDouble(),100.0,3500.0);
    pendingSettings.pskDecoderSquelchPercent = std::clamp(settings.value("digital/pskSquelch",50).toInt(),1,100);
    pendingSettings.pskDecoderAfc = settings.value("digital/pskAfc",true).toBool();
    pendingSettings.pskDecoderEncoding = std::clamp(settings.value("digital/pskEncoding",0).toInt(),0,1);
    if (pskDecoderToneSpin) { QSignalBlocker b(pskDecoderToneSpin); pskDecoderToneSpin->setValue(pendingSettings.pskDecoderToneHz); }
    if (pskDecoderSquelchSpin) { QSignalBlocker b(pskDecoderSquelchSpin); pskDecoderSquelchSpin->setValue(pendingSettings.pskDecoderSquelchPercent); }
    if (pskDecoderAfcCheckbox) { QSignalBlocker b(pskDecoderAfcCheckbox); pskDecoderAfcCheckbox->setChecked(pendingSettings.pskDecoderAfc); }
    if (pskDecoderEncodingCombo) { QSignalBlocker b(pskDecoderEncodingCombo); pskDecoderEncodingCombo->setCurrentIndex(pendingSettings.pskDecoderEncoding); }
    if (pskDecoderControlsWidget) pskDecoderControlsWidget->setVisible(pendingSettings.modulationType == MOD_PSK);
    auto setComboToData = [](QComboBox *combo, const QVariant &data) {
        if (!combo) {
            return;
        }
        const int index = combo->findData(data);
        if (index >= 0) {
            combo->setCurrentIndex(index);
        }
    };
    if (dmrLabCaptureCheckbox) {
        dmrLabCaptureCheckbox->setChecked(
            settings.value("digital/dmrLockEnabled",
                           settings.value("digital/dmrLabCaptureEnabled", false)).toBool());
    }
    setComboToData(dmrLabColorCodeCombo, settings.value("digital/dmrLabColorCode", -1));
    setComboToData(dmrLabSlotCombo, settings.value("digital/dmrLabTimeslot", 0));
    setComboToData(dmrLabCallTypeCombo, settings.value("digital/dmrLabCallType", QStringLiteral("unknown")));
    pendingSettings.dmrBasebandSampleRate =
        normalizedDmrBasebandSampleRate(settings.value("digital/dmrBasebandSampleRate",
                                                       pendingSettings.dmrBasebandSampleRate).toInt());
    setComboToData(dmrBasebandRateCombo, pendingSettings.dmrBasebandSampleRate);
    pendingSettings.dmrChannelSampleRate =
        settings.value("digital/dmrChannelSampleRate",
                       pendingSettings.dmrChannelSampleRate).toInt();
    if (pendingSettings.dmrChannelSampleRate != 0 &&
        pendingSettings.dmrChannelSampleRate != 192000 &&
        pendingSettings.dmrChannelSampleRate != 384000 &&
        pendingSettings.dmrChannelSampleRate != 768000 &&
        pendingSettings.dmrChannelSampleRate != 1536000) {
        pendingSettings.dmrChannelSampleRate = 0;
    }
    setComboToData(dmrChannelRateCombo, pendingSettings.dmrChannelSampleRate);
    pendingSettings.dmrAmbeLayout =
        normalizedDmrAmbeLayout(settings.value("digital/dmrAmbeLayout",
                                               pendingSettings.dmrAmbeLayout).toInt());
    setComboToData(dmrAmbeLayoutCombo, pendingSettings.dmrAmbeLayout);
    pendingSettings.dmrPrivacyMode =
        normalizedDmrPrivacyMode(settings.value("digital/dmrPrivacyMode",
                                                pendingSettings.dmrPrivacyMode).toInt());
    pendingSettings.dmrPrivacyKeyId =
        (std::clamp)(settings.value("digital/dmrPrivacyKeyId",
                                    pendingSettings.dmrPrivacyKeyId).toInt(),
                     0,
                     255);
    pendingSettings.dmrPrivacyKeyHex =
        normalizedDmrPrivacyKeyHex(settings.value("digital/dmrPrivacyKeyHex",
                                                 pendingSettings.dmrPrivacyKeyHex).toString());
    pendingSettings.dmrPrivacyVariant =
        settings.value("digital/dmrPrivacyVariant", pendingSettings.dmrPrivacyVariant)
            .toString()
            .trimmed()
            .toLower();
    if (pendingSettings.dmrPrivacyVariant.isEmpty()) {
        pendingSettings.dmrPrivacyVariant = QStringLiteral("dmra");
    }
    pendingSettings.dmrPrivacyLayout =
        settings.value("digital/dmrPrivacyLayout", pendingSettings.dmrPrivacyLayout)
            .toString()
            .trimmed()
            .toLower();
    if (pendingSettings.dmrPrivacyLayout.isEmpty()) {
        pendingSettings.dmrPrivacyLayout = QStringLiteral("normal");
    }
    pendingSettings.dmrPrivacyFrameOffset =
        (std::clamp)(settings.value("digital/dmrPrivacyFrameOffset",
                                    pendingSettings.dmrPrivacyFrameOffset).toInt(),
                     0,
                     17);

    dmrPrivacyKeys.clear();
    const int privacyKeyCount = settings.beginReadArray(QStringLiteral("digital/dmrPrivacyKeys"));
    for (int i = 0; i < privacyKeyCount; ++i) {
        settings.setArrayIndex(i);
        DmrPrivacyKeyEntry entry;
        entry.active = settings.value(QStringLiteral("active"), false).toBool();
        entry.mode = normalizedDmrPrivacyMode(settings.value(QStringLiteral("mode"), DMR_PRIVACY_NONE).toInt());
        entry.keyId = (std::clamp)(settings.value(QStringLiteral("keyId"), 0).toInt(), 0, 255);
        entry.keyHex = normalizedDmrPrivacyKeyHex(settings.value(QStringLiteral("keyHex")).toString());
        entry.note = settings.value(QStringLiteral("note")).toString().trimmed();
        if (entry.mode == DMR_PRIVACY_NONE && entry.keyHex.isEmpty() && entry.note.isEmpty()) {
            continue;
        }
        const int existingIndex =
            findDmrPrivacyKeyIndexByModeAndId(dmrPrivacyKeys, entry.mode, entry.keyId);
        if (existingIndex >= 0) {
            dmrPrivacyKeys[existingIndex] = entry;
        } else {
            dmrPrivacyKeys.append(entry);
        }
    }
    settings.endArray();
    if (dmrPrivacyKeys.isEmpty() &&
        (pendingSettings.dmrPrivacyMode != DMR_PRIVACY_NONE ||
         !pendingSettings.dmrPrivacyKeyHex.isEmpty())) {
        DmrPrivacyKeyEntry entry;
        entry.active = true;
        entry.mode = pendingSettings.dmrPrivacyMode;
        entry.keyId = pendingSettings.dmrPrivacyKeyId;
        entry.keyHex = pendingSettings.dmrPrivacyKeyHex;
        dmrPrivacyKeys.append(entry);
    }
    const int activeKeyIndex =
        findDmrPrivacyKeyIndexByModeAndId(dmrPrivacyKeys,
                                          pendingSettings.dmrPrivacyMode,
                                          pendingSettings.dmrPrivacyKeyId);
    if (activeKeyIndex >= 0) {
        const DmrPrivacyKeyEntry entry = dmrPrivacyKeys.at(activeKeyIndex);
        pendingSettings.dmrPrivacyMode = normalizedDmrPrivacyMode(entry.mode);
        pendingSettings.dmrPrivacyKeyHex = normalizedDmrPrivacyKeyHex(entry.keyHex);
    }

    setComboToData(dmrPrivacyModeCombo, pendingSettings.dmrPrivacyMode);
    refreshDmrPrivacyKeyIdCombo();
    applyDmrPrivacyKeySelection();
    setComboToData(dmrPrivacyFrameOffsetCombo, pendingSettings.dmrPrivacyFrameOffset);
    setComboToData(dmrPrivacyDropCombo, pendingSettings.dmrPrivacyVariant);
    setComboToData(dmrPrivacyBitLayoutCombo, pendingSettings.dmrPrivacyLayout);
    pendingSettings.dmrManualTimingEnabled =
        settings.value("digital/dmrManualTimingEnabled",
                       pendingSettings.dmrManualTimingEnabled).toBool();
    pendingSettings.dmrManualTimingOffset =
        (std::clamp)(settings.value("digital/dmrManualTimingOffset",
                                    pendingSettings.dmrManualTimingOffset).toInt(),
                     -80,
                     80);
    pendingSettings.dmrSlicerRatio =
        (std::clamp)(settings.value("digital/dmrSlicerRatio",
                                    pendingSettings.dmrSlicerRatio).toDouble(),
                     0.45,
                     0.80);
    pendingSettings.dmrAdaptiveSlicer =
        settings.value("digital/dmrAdaptiveSlicer",
                       pendingSettings.dmrAdaptiveSlicer).toBool();
    if (dmrManualTimingCheckbox) {
        dmrManualTimingCheckbox->setChecked(pendingSettings.dmrManualTimingEnabled);
    }
    if (dmrTimingOffsetSpin) {
        dmrTimingOffsetSpin->setValue(pendingSettings.dmrManualTimingOffset);
    }
    if (dmrSlicerRatioSpin) {
        dmrSlicerRatioSpin->setValue(pendingSettings.dmrSlicerRatio);
    }
    if (dmrAdaptiveSlicerCheckbox) {
        dmrAdaptiveSlicerCheckbox->setChecked(pendingSettings.dmrAdaptiveSlicer);
    }
    if (dmrLabSourceIdEdit) {
        dmrLabSourceIdEdit->setText(settings.value("digital/dmrLabSourceId").toString());
    }
    if (dmrLabTargetIdEdit) {
        dmrLabTargetIdEdit->setText(settings.value("digital/dmrLabTargetId").toString());
    }
    if (dmrLabRadioEdit) {
        dmrLabRadioEdit->setText(settings.value("digital/dmrLabRadio").toString());
    }
    if (dmrLabNotesEdit) {
        dmrLabNotesEdit->setText(settings.value("digital/dmrLabNotes").toString());
    }
    {
        const QSignalBlocker backendBlocker(dmrBackendCombo);
        const QSignalBlocker autoBlocker(dsdNeoAutoStartCheckbox);
        const QSignalBlocker programBlocker(dsdNeoProgramEdit);
        const QSignalBlocker inputPortBlocker(dsdNeoInputPortSpin);
        const QSignalBlocker outputPortBlocker(dsdNeoUdpOutputPortSpin);
        if (dmrBackendCombo) {
            int backend = settings.value("digital/dmrBackend", DMR_BACKEND_FOBOS_MBELIB).toInt();
            if (settings.value("digital/dsdNeoBridgeEnabled", false).toBool()) {
                backend = DMR_BACKEND_DSD_NEO;
            }
            const int backendIndex = dmrBackendCombo->findData(backend);
            dmrBackendCombo->setCurrentIndex(backendIndex >= 0 ? backendIndex : 0);
        }
        if (dsdNeoAutoStartCheckbox) {
            dsdNeoAutoStartCheckbox->setChecked(settings.value("digital/dsdNeoAutoStart", false).toBool());
        }
        if (dsdNeoProgramEdit) {
            const QString savedProgram =
                settings.value("digital/dsdNeoProgram", defaultDsdNeoProgramPath()).toString();
            dsdNeoProgramEdit->setText(isLegacyDsdNeoProgramName(savedProgram)
                                           ? defaultDsdNeoProgramPath()
                                           : savedProgram);
        }
        if (dsdNeoInputPortSpin) {
            dsdNeoInputPortSpin->setValue((std::clamp)(settings.value("digital/dsdNeoInputPort", 7355).toInt(),
                                                       1024,
                                                       65535));
        }
        if (dsdNeoUdpOutputPortSpin) {
            dsdNeoUdpOutputPortSpin->setValue((std::clamp)(settings.value("digital/dsdNeoUdpOutputPort", 23456).toInt(),
                                                           1024,
                                                           65535));
        }
    }
    updateDsdNeoBridgeSettings();
    updateGopherTrunkBridgeSettings();
    videoDecodeEnabled = settings.value("video/decodeEnabled", videoDecodeEnabled).toBool();
    pendingSettings.sstvDemodulationMode = normalizedSstvDemodulationMode(
        settings.value("video/sstvDemodulationMode",
                       pendingSettings.sstvDemodulationMode).toInt());
    if (sstvDemodulationCombo) {
        const QSignalBlocker blocker(sstvDemodulationCombo);
        const int index = sstvDemodulationCombo->findData(
            pendingSettings.sstvDemodulationMode);
        sstvDemodulationCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (videoDemodCombo) {
        const int demodMode = settings.value("video/demodMode", VideoProcessor::FmVideo).toInt();
        const int demodIndex = videoDemodCombo->findData(demodMode);
        if (demodIndex >= 0) {
            videoDemodCombo->setCurrentIndex(demodIndex);
        }
    }
    if (videoStandardCombo) {
        const int standardIndex = (std::clamp)(settings.value("video/standardIndex", 0).toInt(),
                                               0,
                                               (std::max)(0, videoStandardCombo->count() - 1));
        videoStandardCombo->setCurrentIndex(standardIndex);
    }
    if (videoInvertCheckbox) {
        videoInvertCheckbox->setChecked(settings.value("video/invert", false).toBool());
    }
    if (videoHSyncCheckbox) {
        videoHSyncCheckbox->setChecked(settings.value("video/hSync", true).toBool());
    }
    if (videoVSyncCheckbox) {
        videoVSyncCheckbox->setChecked(settings.value("video/vSync", true).toBool());
    }

    normalizeTuning(pendingSettings);
    const bool dmrCenterOffset = offsetDmrCenterFromListening(pendingSettings);
    updateFrequencyPresetControls();
    if (dmrCenterOffset) {
        qDebug() << "[Settings] DMR center offset from listening frequency while loading"
                 << "center" << pendingSettings.centerFrequency
                 << "listening" << pendingSettings.listeningFrequency;
    }
    qDebug() << (settingsFileExists ? "[Settings] loaded" : "[Settings] using defaults; settings file will be created on clean exit")
             << settingsPath
             << "sampleRate" << pendingSettings.sampleRate
             << "center" << pendingSettings.centerFrequency
             << "listening" << pendingSettings.listeningFrequency;
}

void YourClassName::flushPendingPersistentSettingsSave() {
    if (!persistentSettingsSaveDeferred) {
        return;
    }
    persistentSettingsSaveDeferred = false;
    savePersistentSettings();
}

void YourClassName::savePersistentSettings() {
    if (!persistentSettingsReady) {
        return;
    }

    if (closeShutdownInProgress &&
        persistentSettingsLastSaveTimer.isValid() &&
        persistentSettingsLastSaveTimer.elapsed() >= 0 &&
        persistentSettingsLastSaveTimer.elapsed() < PERSISTENT_SETTINGS_MIN_SAVE_INTERVAL_MS) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[Settings] close save skipped; recent settings sync already completed"
                     << "elapsedMs" << persistentSettingsLastSaveTimer.elapsed();
        }
        persistentSettingsSaveDeferred = false;
        if (persistentSettingsSaveTimer) {
            persistentSettingsSaveTimer->stop();
        }
        return;
    }

    const bool canDefer =
        !persistentSettingsSaveInProgress &&
        (runState == RadioRunState::Running || runState == RadioRunState::Stopping);
    if (canDefer && persistentSettingsLastSaveTimer.isValid()) {
        const qint64 elapsedMs = persistentSettingsLastSaveTimer.elapsed();
        if (elapsedMs >= 0 && elapsedMs < PERSISTENT_SETTINGS_MIN_SAVE_INTERVAL_MS) {
            persistentSettingsSaveDeferred = true;
            if (persistentSettingsSaveTimer) {
                const int delayMs = static_cast<int>(
                    (std::clamp)(PERSISTENT_SETTINGS_MIN_SAVE_INTERVAL_MS - elapsedMs,
                                 qint64(1),
                                 qint64(PERSISTENT_SETTINGS_MIN_SAVE_INTERVAL_MS)));
                if (!persistentSettingsSaveTimer->isActive() ||
                    persistentSettingsSaveTimer->remainingTime() > delayMs) {
                    persistentSettingsSaveTimer->start(delayMs);
                }
            }
            return;
        }
    }

    if (persistentSettingsSaveTimer) {
        persistentSettingsSaveTimer->stop();
    }
    persistentSettingsSaveDeferred = false;
    persistentSettingsSaveInProgress = true;

    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    RadioSettings settingsToSave = pendingSettings;
    const bool savedDmrCenterOffset = offsetDmrCenterFromListening(settingsToSave);

    settings.setValue("receiver/deviceIndex", settingsToSave.deviceIndex);
    settings.setValue("receiver/clockSource", settingsToSave.clockSource);
    settings.setValue("receiver/inputMode", settingsToSave.inputMode);
    settings.setValue("receiver/centerFrequency", settingsToSave.centerFrequency);
    settings.setValue("receiver/actualFrequency", settingsToSave.actualFrequency);
    settings.setValue("receiver/listeningFrequency", settingsToSave.listeningFrequency);
    settings.setValue("receiver/sampleRate", settingsToSave.sampleRate);
    settings.setValue("receiver/bandwidth", settingsToSave.bandwidth);
    settings.setValue("receiver/modulationType", settingsToSave.modulationType);
    settings.setValue("receiver/fftLength", settingsToSave.fftLength);
    settings.setValue("receiver/fftBinWidthModeEnabled", fftBinWidthModeEnabled);
    settings.setValue("receiver/fftTargetBinWidthHz", fftTargetBinWidthHz);
    settings.setValue("receiver/fftWindowType", normalizedFftWindowType(settingsToSave.fftWindowType));
    settings.setValue("receiver/lnaGain", settingsToSave.lnaGain);
    settings.setValue("receiver/vgaGain", settingsToSave.vgaGain);
    settings.setValue("receiver/hackRfLnaGainDb", settingsToSave.hackRfLnaGainDb);
    settings.setValue("receiver/hackRfVgaGainDb", settingsToSave.hackRfVgaGainDb);
    settings.setValue("receiver/hackRfAmpEnabled", settingsToSave.hackRfAmpEnabled);
    settings.setValue("receiver/hackRfBiasTeeEnabled", settingsToSave.hackRfBiasTeeEnabled);
    settings.setValue("receiver/hackRfAutomaticBandwidth", settingsToSave.hackRfAutomaticBandwidth);
    settings.setValue("receiver/hackRfBandwidthHz", settingsToSave.hackRfBandwidthHz);
    settings.setValue("receiver/hackRfExplicitTuningEnabled", settingsToSave.hackRfExplicitTuningEnabled);
    settings.setValue("receiver/hackRfExplicitIfHz", settingsToSave.hackRfExplicitIfHz);
    settings.setValue("receiver/hackRfExplicitLoHz", settingsToSave.hackRfExplicitLoHz);
    settings.setValue("receiver/hackRfExplicitPath", settingsToSave.hackRfExplicitPath);
    settings.setValue("receiver/hackRfClockOutEnabled", settingsToSave.hackRfClockOutEnabled);
    settings.setValue("receiver/hackRfHardwareSyncEnabled", settingsToSave.hackRfHardwareSyncEnabled);
    settings.setValue("receiver/hackRfRxOverrunLimit", settingsToSave.hackRfRxOverrunLimit);
    settings.setValue("receiver/hackRfOperaCakeEnabled", settingsToSave.hackRfOperaCakeEnabled);
    settings.setValue("receiver/hackRfOperaCakeAddress", settingsToSave.hackRfOperaCakeAddress);
    settings.setValue("receiver/hackRfOperaCakeMode", settingsToSave.hackRfOperaCakeMode);
    settings.setValue("receiver/hackRfOperaCakePortA", settingsToSave.hackRfOperaCakePortA);
    settings.setValue("receiver/hackRfOperaCakePortB", settingsToSave.hackRfOperaCakePortB);
    settings.setValue("receiver/hackRfOperaCakeRangesJson", settingsToSave.hackRfOperaCakeRangesJson);
    settings.setValue("receiver/hackRfOperaCakeDwellsJson", settingsToSave.hackRfOperaCakeDwellsJson);
    settings.setValue("receiver/hackRfSweepRanges", settingsToSave.hackRfSweepRanges);
    settings.setValue("receiver/hackRfSweepStepHz", settingsToSave.hackRfSweepStepHz);
    settings.setValue("receiver/hackRfSweepBytesPerTune", settingsToSave.hackRfSweepBytesPerTune);
    settings.setValue("receiver/hackRfSweepInterleaved", settingsToSave.hackRfSweepInterleaved);
    settings.setValue("receiver/rtlAgc", settingsToSave.rtlAgc);
    settings.setValue("receiver/rtlTunerGainTenthsDb", settingsToSave.rtlTunerGainTenthsDb);
    settings.setValue("receiver/audioDeviceId", settingsToSave.audioDeviceId);
    settings.setValue("receiver/audioEnabled", settingsToSave.audioEnabled);
    if (multiVfoWidget) {
        settings.setValue(QStringLiteral("multiVfo/configuration"), multiVfoWidget->configurationJson());
    }
    settings.setValue("receiver/gpoValue", static_cast<int>(settingsToSave.gpoValue));
    if (savedDmrCenterOffset) {
        qDebug() << "[Settings] DMR center offset from listening frequency while saving"
                 << "center" << settingsToSave.centerFrequency
                 << "listening" << settingsToSave.listeningFrequency;
    }
    settings.setValue("agileScan/enabled", agileScanEnabled);
    settings.setValue("agileScan/autoStepSampleRate", agileScanAutoStepSampleRate);
    settings.setValue("agileScan/rangesMhz", agileScanRangesMhz);
    settings.setValue("agileScan/stepMhz", agileScanStepMhz);
    settings.setValue("scan/visualMode", normalizedScanVisualMode(scanVisualMode));
    settings.setValue("standardScan/enabled", standardScanEnabled);
    settings.setValue("standardScan/listenLock", scanListeningLockEnabled);
    settings.setValue("standardScan/centersMhz", standardScanCentersMhz);
    settings.setValue("standardScan/dwellMs", standardScanDwellMs);
    settings.setValue("standardScan/settleMs", standardScanSettleMs);
    settings.setValue("standardScan/rangeStartMhz", standardScanRangeStartMhz);
    settings.setValue("standardScan/rangeEndMhz", standardScanRangeEndMhz);
    settings.setValue("listeningScan/enabled", listeningScanEnabled);
    settings.setValue("listeningScan/targetsMhz", listeningScanTargetsMhz);
    settings.setValue("listeningScan/dwellMs", listeningScanDwellMs);
    settings.setValue("listeningScan/settleMs", listeningScanSettleMs);
    settings.setValue("gpsQth/latitude", qthLatitude);
    settings.setValue("gpsQth/longitude", qthLongitude);
    settings.setValue("gpsQth/positionVisible", qthPositionVisible);
    settings.setValue("gpsQth/source", qthSource);
    settings.setValue("gpsQth/nmeaSerialPort", gnssSerialPortName);
    settings.setValue("gpsQth/nmeaSerialBaud", gnssSerialBaud);
    settings.setValue("gpsQth/positionPolicy", normalizedGnssPositionPolicy(gnssPositionPolicy));
    settings.setValue("gpsQth/ubxAutoEnable", gnssUbxAutoEnable);
    settings.setValue("gpsQth/timeZoneOffsetMinutes", gnssTimeZoneOffsetMinutes);
    settings.setValue("gpsQth/satelliteTableVisible", gnssSatelliteTableVisible);
    settings.setValue("gpsQth/tileDirectory", qthTileDirectory);
    settings.setValue("gpsQth/mapLayer", qthMapLayer);
    settings.setValue("gpsQth/mapZoom", qthMapZoom);
    settings.setValue("gpsQth/gridPrecision", qthGridPrecision);
    settings.setValue("gpsQth/mapOverlayMode", qthMapOverlayMode);
    settings.setValue("gpsQth/onlineProviderId", qthOnlineProviderId);
    settings.setValue("gpsQth/onlineTileUrlTemplate", qthOnlineTileUrlTemplate);
    settings.setValue("gpsQth/onlineAttribution", qthOnlineAttribution);
    settings.setValue("gpsQth/onlineApiKey", qthOnlineApiKey);
    settings.setValue("gpsQth/onlineNoDiskCache", qthOnlineNoDiskCache);
    settings.setValue("gpsQth/gnssSystemId", gnssSystemId);
    settings.setValue("gpsQth/gnssMonitorEnabled", gnssMonitorEnabled);
    settings.setValue("gpsQth/gnssUseGps", gnssUseGps);
    settings.setValue("gpsQth/gnssUseGlonass", gnssUseGlonass);
    settings.setValue("gpsQth/gnssUseGalileo", gnssUseGalileo);
    settings.setValue("gpsQth/gnssUseBeidou", gnssUseBeidou);
    settings.setValue("gpsQth/gnssUseQzss", gnssUseQzss);
    settings.setValue("gpsQth/gnssUseSbas", gnssUseSbas);
    settings.setValue("gpsQth/gnssUseOther", gnssUseOther);
    QStringList disabledSatelliteKeys = gnssDisabledSatelliteKeys.values();
    disabledSatelliteKeys.sort();
    settings.setValue("gpsQth/gnssDisabledSatellites", disabledSatelliteKeys);
    settings.setValue("gpsQth/gnssAcquisitionIntegrationMs", gnssAcquisitionIntegrationMs);
    settings.setValue("gpsQth/gnssChannelFilterCutoffHz", gnssChannelFilterCutoffHz);
    settings.setValue("gpsQth/gnssDopplerSpanHz", gnssDopplerSpanHz);
    settings.setValue("gpsQth/gnssDopplerStepHz", gnssDopplerStepHz);
    settings.setValue("gpsQth/gnssContinuousAcquisitionEnabled", gnssContinuousAcquisitionEnabled);
    settings.setValue("gpsQth/gnssContinuousAcquisitionIntervalMs", gnssContinuousAcquisitionIntervalMs);
    settings.beginWriteArray(QStringLiteral("gpsQth/markers"));
    int qthMarkerIndex = 0;
    for (const qth::UserMarker &marker : qthUserMarkers) {
        if (marker.number <= 0 ||
            !qth::isValidLatitude(marker.latitude) ||
            !qth::isValidLongitude(marker.longitude)) {
            continue;
        }
        settings.setArrayIndex(qthMarkerIndex++);
        settings.setValue(QStringLiteral("number"), marker.number);
        settings.setValue(QStringLiteral("name"), marker.name.trimmed());
        settings.setValue(QStringLiteral("description"), marker.description.trimmed());
        settings.setValue(QStringLiteral("latitude"), marker.latitude);
        settings.setValue(QStringLiteral("longitude"), marker.longitude);
    }
    settings.endArray();
    settings.setValue("spectrumMeasurement/enabled", scanMeasurementEnabled);
    settings.setValue("spectrumMeasurement/waterfallAreaMeasurement",
                      waterfallAreaMeasurementEnabled);
    settings.setValue("spectrumMeasurement/binMhz", scanMeasurementBinMhz);
    settings.setValue("spectrumMeasurement/updateIntervalMs", scanMeasurementUpdateIntervalMs);
    settings.setValue("spectrumScience/maxHold", spectrumScienceMaxHoldEnabled);
    settings.setValue("spectrumScience/markers", QJsonDocument(spectrumScienceAnalyzer.markerConfiguration()).toJson(QJsonDocument::Compact));
    settings.setValue("spectrumScience/minHold", spectrumScienceMinHoldEnabled);
    settings.setValue("spectrumScience/average", spectrumScienceAverageEnabled);
    settings.setValue("spectrumScience/averageSeconds", spectrumScienceAverageSeconds);
    settings.setValue("spectrumScience/detectorMode", spectrumDetectorMode);
    settings.setValue("spectrumScience/detectorFrames", spectrumDetectorFrames);
    settings.setValue("spectrumScience/vbwHz", spectrumVbwHz);
    settings.setValue("spectrumScience/fftOverlapPercent", spectrumFftOverlapPercent);
    settings.setValue("spectrumScience/averageFrameCount", spectrumAverageFrameCount);
    settings.setValue("spectrumScience/p50", spectrumPercentile50Enabled);
    settings.setValue("spectrumScience/p90", spectrumPercentile90Enabled);
    settings.setValue("spectrumScience/p99", spectrumPercentile99Enabled);
    settings.setValue("spectrumScience/amplitudeUnit", spectrumAmplitudeUnit);
    settings.setValue("dmrHunter/enabled", dmrHunterSettings.enabled);
    settings.setValue("dmrHunter/minWidthKhz", dmrHunterSettings.minWidthKhz);
    settings.setValue("dmrHunter/maxWidthKhz", dmrHunterSettings.maxWidthKhz);
    settings.setValue("dmrHunter/thresholdDb", dmrHunterSettings.thresholdDb);
    settings.setValue("fpvHunter/enabled", fpvHunterSettings.enabled);
    settings.setValue("fpvHunter/minWidthMhz", fpvHunterSettings.minWidthMhz);
    settings.setValue("fpvHunter/maxWidthMhz", fpvHunterSettings.maxWidthMhz);
    settings.setValue("fpvHunter/thresholdDb", fpvHunterSettings.thresholdDb);
    settings.setValue("fpvHunter/followEnabled", fpvHunterFollowEnabled);
    settings.setValue("digitalVideoHunter/enabled", digitalVideoHunterSettings.enabled);
    settings.setValue("digitalVideoHunter/minWidthMhz", digitalVideoHunterSettings.minWidthMhz);
    settings.setValue("digitalVideoHunter/maxWidthMhz", digitalVideoHunterSettings.maxWidthMhz);
    settings.setValue("digitalVideoHunter/thresholdDb", digitalVideoHunterSettings.thresholdDb);
    agileScanPresetOrder =
        normalizedPresetOrder(agileScanPresetOrder,
                              agileScanPresets,
                              defaultAgileScanPresetOrder());
    settings.setValue("agileScan/presetOrder", agileScanPresetOrder);
    settings.beginWriteArray("agileScan/presets");
    int scanPresetIndex = 0;
    for (const QString &name : std::as_const(agileScanPresetOrder)) {
        auto it = agileScanPresets.constFind(name);
        if (it == agileScanPresets.constEnd()) {
            continue;
        }
        settings.setArrayIndex(scanPresetIndex++);
        settings.setValue("name", name);
        settings.setValue("rangesMhz", agileScanPresetRanges(it.value()));
        settings.setValue("stepMhz", agileScanPresetStepMhz(it.value(), agileScanStepMhz));
    }
    settings.endArray();
    standardScanPresetOrder =
        normalizedPresetOrder(standardScanPresetOrder,
                              standardScanPresets,
                              defaultStandardScanPresetOrder());
    settings.setValue("standardScan/presetOrder", standardScanPresetOrder);
    settings.beginWriteArray("standardScan/presets");
    int standardScanPresetIndex = 0;
    for (const QString &name : std::as_const(standardScanPresetOrder)) {
        auto it = standardScanPresets.constFind(name);
        if (it == standardScanPresets.constEnd()) {
            continue;
        }
        settings.setArrayIndex(standardScanPresetIndex++);
        settings.setValue("name", name);
        settings.setValue("centersMhz", standardScanPresetCenters(it.value()));
        settings.setValue("dwellMs", standardScanPresetDwellMs(it.value(), standardScanDwellMs));
        settings.setValue("settleMs", standardScanPresetSettleMs(it.value(), standardScanSettleMs));
    }
    settings.endArray();
    listeningScanPresetOrder =
        normalizedPresetOrder(listeningScanPresetOrder,
                              listeningScanPresets,
                              defaultListeningScanPresetOrder());
    settings.setValue("listeningScan/presetOrder", listeningScanPresetOrder);
    settings.beginWriteArray("listeningScan/presets");
    int listeningScanPresetIndex = 0;
    for (const QString &name : std::as_const(listeningScanPresetOrder)) {
        auto it = listeningScanPresets.constFind(name);
        if (it == listeningScanPresets.constEnd()) {
            continue;
        }
        settings.setArrayIndex(listeningScanPresetIndex++);
        settings.setValue("name", name);
        settings.setValue("targetsMhz", listeningScanPresetTargets(it.value()));
        settings.setValue("dwellMs", listeningScanPresetDwellMs(it.value(), listeningScanDwellMs));
        settings.setValue("settleMs", listeningScanPresetSettleMs(it.value(), listeningScanSettleMs));
    }
    settings.endArray();

    centerFrequencyPresetOrder =
        normalizedPresetOrder(centerFrequencyPresetOrder,
                              centerFrequencyPresets,
                              defaultCenterFrequencyPresetOrder());
    listeningFrequencyPresetOrder =
        normalizedPresetOrder(listeningFrequencyPresetOrder,
                              listeningFrequencyPresets,
                              defaultListeningFrequencyPresetOrder());
    bandwidthPresetOrder =
        normalizedPresetOrder(bandwidthPresetOrder,
                              bandwidthValuePresets,
                              defaultBandwidthPresetOrder());
    settings.setValue("frequencyPresets/centerOrder", centerFrequencyPresetOrder);
    settings.setValue("frequencyPresets/listeningOrder", listeningFrequencyPresetOrder);
    settings.setValue("frequencyPresets/bandwidthOrder", bandwidthPresetOrder);

    auto writeFrequencyPresetArray = [&settings](const char *path,
                                                 const QMap<QString, double> &presets,
                                                 const QStringList &order) {
        settings.beginWriteArray(QString::fromLatin1(path));
        int index = 0;
        const QStringList normalizedOrder = normalizedPresetOrder(order, presets);
        for (const QString &name : normalizedOrder) {
            auto it = presets.constFind(name);
            if (it == presets.constEnd() ||
                it.key().trimmed().isEmpty() ||
                !std::isfinite(it.value())) {
                continue;
            }
            settings.setArrayIndex(index++);
            settings.setValue("name", name);
            settings.setValue("valueHz", it.value());
        }
        settings.endArray();
    };
    writeFrequencyPresetArray("frequencyPresets/center", centerFrequencyPresets, centerFrequencyPresetOrder);
    writeFrequencyPresetArray("frequencyPresets/listening", listeningFrequencyPresets, listeningFrequencyPresetOrder);
    writeFrequencyPresetArray("frequencyPresets/bandwidth", bandwidthValuePresets, bandwidthPresetOrder);
    if (frequencyControl) {
        settings.setValue("frequencyControls/centerUnitIndex", frequencyControl->selectedUnitIndex());
        settings.setValue("frequencyControls/centerStepName", frequencyControl->selectedStepName());
        settings.setValue("frequencyControls/centerPresetName", frequencyControl->selectedValuePresetName());
    }
    if (listeningFrequencyControl) {
        settings.setValue("frequencyControls/listeningUnitIndex", listeningFrequencyControl->selectedUnitIndex());
        settings.setValue("frequencyControls/listeningStepName", listeningFrequencyControl->selectedStepName());
        settings.setValue("frequencyControls/listeningPresetName", listeningFrequencyControl->selectedValuePresetName());
    }
    if (bandwidthControl) {
        settings.setValue("frequencyControls/bandwidthUnitIndex", bandwidthControl->selectedUnitIndex());
        settings.setValue("frequencyControls/bandwidthStepName", bandwidthControl->selectedStepName());
        settings.setValue("frequencyControls/bandwidthPresetName", bandwidthControl->selectedValuePresetName());
    }

    settings.beginWriteArray(QStringLiteral("bandMarkers"));
    int bandMarkerIndex = 0;
    for (const GraphBandMarker &marker : std::as_const(bandMarkers)) {
        if (marker.label.trimmed().isEmpty() ||
            !std::isfinite(marker.startHz) ||
            !std::isfinite(marker.endHz) ||
            marker.endHz <= marker.startHz) {
            continue;
        }
        settings.setArrayIndex(bandMarkerIndex++);
        settings.setValue(QStringLiteral("label"), marker.label.trimmed());
        settings.setValue(QStringLiteral("startHz"), marker.startHz);
        settings.setValue(QStringLiteral("endHz"), marker.endHz);
        settings.setValue(QStringLiteral("amateur"), marker.amateur);
    }
    settings.endArray();

    settings.setValue("display/scalePercent", currentScale);
    settings.setValue("display/additionalScaleDivisor", additionalScaleDivisor);
    settings.setValue("display/contrast", contrast);
    settings.setValue("display/sensitivity", sensitivity);
    settings.setValue("display/levelMin", displayLevelMin);
    settings.setValue("display/levelMax", displayLevelMax);
    settings.setValue("display/secondGraph", secondGraph);
    settings.setValue("display/color", colorf);
    settings.setValue("display/generalBandMarkers", showGeneralBandMarkers);
    settings.setValue("display/amateurBandMarkers", showAmateurBandMarkers);
    settings.setValue("display/compactBandMarkers", compactBandMarkers);
    settings.setValue("display/spurSuppressionEnabled", spurSuppressionEnabled);
    settings.setValue("diagnostics/verboseLogging", diagnosticVerboseLogging);
    settings.setValue("recording/extendedMetadata", extendedRecordingMetadataEnabled);
    settings.beginWriteArray(QStringLiteral("display/spurMask"));
    int spurMaskIndex = 0;
    for (const SpurMaskEntry &entry : std::as_const(spurMaskEntries)) {
        if (!std::isfinite(entry.offsetHz) ||
            !std::isfinite(entry.widthHz) ||
            entry.widthHz <= 0.0) {
            continue;
        }
        settings.setArrayIndex(spurMaskIndex++);
        settings.setValue(QStringLiteral("offsetHz"), entry.offsetHz);
        settings.setValue(QStringLiteral("widthHz"), entry.widthHz);
        settings.setValue(QStringLiteral("prominenceDb"), entry.prominenceDb);
        settings.setValue(QStringLiteral("hits"), entry.hits);
    }
    settings.endArray();
    settings.setValue("audio/volumePercent", volumePercent);
    settings.setValue("ui/language", uiLanguage);
    settings.setValue("ui/fineTuneControlMode", fineTuneControlMode);
    settings.setValue("ui/spectrumUpdateIntervalMs", spectrumUpdateIntervalMs);
    settings.setValue("ui/fftBackendPreference",
                      normalizedFftBackendPreference(fftBackendPreference));
    settings.setValue("calibration/frequencyOffsetHz", frequencyCalibrationOffsetHz);
    settings.setValue("calibration/amplitudeOffsetDb", amplitudeCalibrationOffsetDb);
    settings.setValue("calibration/tableEnabled", calibrationTableEnabled);
    settings.beginWriteArray(QStringLiteral("calibration/table"));
    int calibrationPointIndex = 0;
    for (const ReceiverCalibrationPoint &point : receiverCalibrationTable.points()) {
        settings.setArrayIndex(calibrationPointIndex++);
        settings.setValue(QStringLiteral("frequencyHz"), point.frequencyHz);
        settings.setValue(QStringLiteral("frequencyOffsetHz"), point.frequencyOffsetHz);
        settings.setValue(QStringLiteral("amplitudeOffsetDb"), point.amplitudeOffsetDb);
        settings.setValue(QStringLiteral("uncertaintyDb"), point.uncertaintyDb);
        settings.setValue(QStringLiteral("note"), point.note);
        settings.setValue(QStringLiteral("conditions"), QJsonDocument(point.conditions.toJson()).toJson(QJsonDocument::Compact));
    }
    settings.endArray();
    settings.setValue("ui/waterfallRowsPerFrame", waterfallRowsPerFrame);
    settings.setValue("ui/waterfallDisplayMode", waterfallDisplayMode);
    settings.setValue("ui/waterfall3DResolutionDivisor", waterfall3DResolutionDivisor);
    settings.setValue("ui/waterfall3DSurfaceStyle", waterfall3DSurfaceStyle);
    settings.setValue("ui/waterfall3DSmoothing", waterfall3DSmoothing);
    settings.setValue("ui/waterfall3DLighting", waterfall3DLighting);
    settings.setValue("ui/waterfall3DHistoryRows", waterfall3DHistoryRows);
    settings.setValue("ui/waterfall3DSliceScrollStep", waterfall3DSliceScrollStep);
    settings.setValue("ui/waterfall3DSliceWidth", waterfall3DSliceWidth);
    settings.setValue("ui/waterfall3DSpectrumSliceScrollStep", waterfall3DSpectrumSliceScrollStep);
    settings.setValue("ui/waterfall3DSpectrumSliceRows", waterfall3DSpectrumSliceRows);
    settings.setValue("ui/waterfall3DSpectrumSliceCapture", waterfall3DSpectrumSliceCapture);
    settings.setValue("ui/waterfall3DSpectrumSliceCaptureFixed", waterfall3DSpectrumSliceCaptureFixed);
    settings.setValue("ui/waterfall3DFixedPlane", waterfall3DFixedPlane);
    settings.setValue("ui/waterfall3DMonochrome", waterfall3DMonochrome);
    settings.setValue("ui/waterfall3DAlternativeView", waterfall3DAlternativeView);
    settings.setValue("ui/waterfall3DVncSliceInput", waterfall3DVncSliceInput);
    settings.setValue("ui/alternativeInterfaceMode", alternativeInterfaceMode);
    settings.setValue("ui/alternativeSpectrumGradientFill", alternativeSpectrumGradientFill);
    settings.setValue("ui/alternativeSpectrumGradientOpacity", alternativeSpectrumGradientOpacity);
    settings.setValue("ui/experimentalGpuWaterfall", experimentalGpuWaterfall);
    settings.setValue("ui/spectrumDisplayReductionMode", spectrumDisplayReductionMode);
    settings.setValue("ui/showSpectrumFps", showSpectrumFps);
    settings.setValue("ui/showWaterfallFps", showWaterfallFps);
    settings.setValue("diagnostics/showAudioQueueDropRows", showAudioQueueDropRows);
    settings.setValue("ui/showSpectrumPeakMeter", showSpectrumPeakMeter);
    settings.setValue("ui/spectrumPeakMeterStyle", spectrumPeakMeterStyle);
    settings.setValue("ui/showExtendedSpectrumInfo", showExtendedSpectrumInfo);
    settings.setValue("ui/agileLiveRetuneIntervalMs", agileLiveRetuneCommandIntervalMs);
    settings.setValue("ui/fineTuneScaleHoldMode", fineTuneScaleHoldMode);
    settings.setValue("spectrumFrames/bufferEnabled", spectrumFrameBufferEnabled);
    settings.setValue("spectrumFrames/prebufferSeconds", spectrumFramePrebufferSeconds);
    settings.setValue("spectrumFrames/eventCaptureMode", spectrumEventCaptureMode);
    if (spectrumFrameBinsCombo) {
        settings.setValue("spectrumFrames/bins", spectrumFrameBinsCombo->currentData().toInt());
    }
    settings.setValue("audio/lowPassHz", pendingSettings.audioLowPassHz);
    settings.setValue("audio/highPassHz", pendingSettings.audioHighPassHz);
    settings.setValue("audio/filterChainJson", pendingSettings.audioFilterChainJson);
    settings.setValue("audio/simplifiedChannelizer",
                      pendingSettings.simplifiedAudioChannelizer);
    settings.setValue("hfNoiseCancel/depth", pendingSettings.hfNoiseCancelDepth);
    settings.setValue("hfNoiseCancel/refGainDb", pendingSettings.hfNoiseCancelRefGainDb);
    settings.setValue("hfNoiseCancel/refDelayNs", pendingSettings.hfNoiseCancelRefDelayNs);
    settings.setValue("hfNoiseCancel/refTiltDb", pendingSettings.hfNoiseCancelRefTiltDb);
    settings.setValue("hfNoiseCancel/freeze", pendingSettings.hfNoiseCancelFreeze);
    settings.setValue("hfNoiseCancel/audioBlankerEnabled", pendingSettings.hfAudioBlankerEnabled);
    settings.setValue("hfNoiseCancel/audioBlankerThreshold", pendingSettings.hfAudioBlankerThreshold);
    settings.setValue("hfInterference/baselineEnabled", hfInterferenceBaselineEnabled);
    settings.setValue("hfInterference/rawOverlayEnabled", hfInterferenceRawOverlayEnabled);
    settings.setValue("hfInterference/baselineDepth", hfInterferenceBaselineDepth);
    settings.setValue("hfInterference/baselineSmoothBins", hfInterferenceBaselineSmoothBins);
    settings.setValue("hfInterference/baselineCurve",
                      encodeHfInterferenceBaselineCurve(hfInterferenceBaselineFrequencies,
                                                        hfInterferenceBaselineMagnitudes));

    settings.setValue("network/serverAddress", networkServerAddress);
    settings.setValue("network/bindAddress", networkBindAddress);
    settings.setValue("network/controlPort", static_cast<int>(networkControlPort));
    settings.setValue("network/processingMode", static_cast<int>(networkProcessingMode));
    settings.setValue("network/serverDisableLocalVisualAudio", serverDisableLocalVisualAudio);
    settings.setValue("network/fullResolutionSpectrumFrames", networkFullResolutionSpectrumFrames);
    settings.setValue("audioRelay/transmitEnabled", audioRelayTransmitEnabled);
    settings.setValue("audioRelay/host", audioRelayHost);
    settings.setValue("audioRelay/port", static_cast<int>(audioRelayPort));
    settings.setValue("audioRelay/receiveEnabled", audioRelayReceiveEnabled);
    settings.setValue("audioRelay/listenPort", static_cast<int>(audioRelayListenPort));
    settings.setValue("audioHttpStream/enabled", audioHttpStreamEnabled);
    settings.setValue("audioHttpStream/port", static_cast<int>(audioHttpStreamPort));
    settings.setValue("digital/decodeEnabled", digitalDecodeEnabled);
    settings.setValue("digital/cwToneHz",
                      cwDecoderToneSpin ? cwDecoderToneSpin->value()
                                        : pendingSettings.cwDecoderToneHz);
    settings.setValue("digital/pskToneHz", pendingSettings.pskDecoderToneHz);
    settings.setValue("digital/pskSquelch", pendingSettings.pskDecoderSquelchPercent);
    settings.setValue("digital/pskAfc", pendingSettings.pskDecoderAfc);
    settings.setValue("digital/pskEncoding", pendingSettings.pskDecoderEncoding);
    settings.setValue("digital/cwInitialWpm",
                      cwDecoderWpmSpin ? cwDecoderWpmSpin->value()
                                       : pendingSettings.cwDecoderWpm);
    settings.setValue("digital/cwAdaptiveSpeed",
                      !cwDecoderAdaptiveCheckbox || cwDecoderAdaptiveCheckbox->isChecked());
    settings.setValue("digital/cwAlphabet",
                      cwDecoderAlphabetCombo ? cwDecoderAlphabetCombo->currentData().toInt()
                                             : pendingSettings.cwDecoderAlphabet);
    settings.setValue("digital/cwSelectivity",
                      cwDecoderSelectivitySpin ? cwDecoderSelectivitySpin->value()
                                               : pendingSettings.cwDecoderSelectivity);
    settings.setValue("digital/dmrLockEnabled", dmrLabCaptureCheckbox && dmrLabCaptureCheckbox->isChecked());
    settings.setValue("digital/dmrLabColorCode", dmrLabColorCodeCombo ? dmrLabColorCodeCombo->currentData().toInt() : -1);
    settings.setValue("digital/dmrLabTimeslot", dmrLabSlotCombo ? dmrLabSlotCombo->currentData().toInt() : 0);
    settings.setValue("digital/dmrLabCallType", dmrLabCallTypeCombo ? dmrLabCallTypeCombo->currentData().toString() : QStringLiteral("unknown"));
    settings.setValue("digital/dmrBasebandSampleRate",
                      dmrBasebandRateCombo
                          ? normalizedDmrBasebandSampleRate(dmrBasebandRateCombo->currentData().toInt())
                          : normalizedDmrBasebandSampleRate(pendingSettings.dmrBasebandSampleRate));
    settings.setValue("digital/dmrChannelSampleRate",
                      dmrChannelRateCombo
                          ? dmrChannelRateCombo->currentData().toInt()
                          : pendingSettings.dmrChannelSampleRate);
    settings.setValue("digital/dmrAmbeLayout",
                      dmrAmbeLayoutCombo
                          ? normalizedDmrAmbeLayout(dmrAmbeLayoutCombo->currentData().toInt())
                          : normalizedDmrAmbeLayout(pendingSettings.dmrAmbeLayout));
    settings.setValue("digital/dmrPrivacyMode",
                      dmrPrivacyModeCombo
                          ? normalizedDmrPrivacyMode(dmrPrivacyModeCombo->currentData().toInt())
                          : normalizedDmrPrivacyMode(pendingSettings.dmrPrivacyMode));
    settings.setValue("digital/dmrPrivacyKeyId",
                      pendingSettings.dmrPrivacyKeyId);
    settings.setValue("digital/dmrPrivacyKeyHex",
                      normalizedDmrPrivacyKeyHex(pendingSettings.dmrPrivacyKeyHex));
    settings.remove("digital/dmrPrivacyForwardToBackends");
    settings.setValue("digital/dmrPrivacyVariant",
                      dmrPrivacyDropCombo
                          ? dmrPrivacyDropCombo->currentData().toString()
                          : pendingSettings.dmrPrivacyVariant);
    settings.setValue("digital/dmrPrivacyLayout",
                      dmrPrivacyBitLayoutCombo
                          ? dmrPrivacyBitLayoutCombo->currentData().toString()
                          : pendingSettings.dmrPrivacyLayout);
    settings.setValue("digital/dmrPrivacyFrameOffset",
                      dmrPrivacyFrameOffsetCombo
                          ? (std::clamp)(dmrPrivacyFrameOffsetCombo->currentData().toInt(), 0, 17)
                          : (std::clamp)(pendingSettings.dmrPrivacyFrameOffset, 0, 17));
    settings.beginWriteArray(QStringLiteral("digital/dmrPrivacyKeys"));
    int dmrPrivacyKeyIndex = 0;
    for (const DmrPrivacyKeyEntry &entry : std::as_const(dmrPrivacyKeys)) {
        const int mode = normalizedDmrPrivacyMode(entry.mode);
        const QString keyHex = normalizedDmrPrivacyKeyHex(entry.keyHex);
        if ((mode != DMR_PRIVACY_ARC4 && mode != DMR_PRIVACY_AES256 &&
             mode != DMR_PRIVACY_HYTERA_BASIC) ||
            (keyHex.isEmpty() && entry.note.trimmed().isEmpty())) {
            continue;
        }
        settings.setArrayIndex(dmrPrivacyKeyIndex++);
        settings.setValue(QStringLiteral("active"), entry.active);
        settings.setValue(QStringLiteral("mode"), mode);
        settings.setValue(QStringLiteral("keyId"), (std::clamp)(entry.keyId, 0, 255));
        settings.setValue(QStringLiteral("keyHex"), keyHex);
        settings.setValue(QStringLiteral("note"), entry.note.trimmed());
    }
    settings.endArray();
    settings.setValue("digital/dmrManualTimingEnabled",
                      dmrManualTimingCheckbox && dmrManualTimingCheckbox->isChecked());
    settings.setValue("digital/dmrManualTimingOffset",
                      dmrTimingOffsetSpin ? dmrTimingOffsetSpin->value() : pendingSettings.dmrManualTimingOffset);
    settings.setValue("digital/dmrSlicerRatio",
                      dmrSlicerRatioSpin ? dmrSlicerRatioSpin->value() : pendingSettings.dmrSlicerRatio);
    settings.setValue("digital/dmrAdaptiveSlicer",
                      !dmrAdaptiveSlicerCheckbox || dmrAdaptiveSlicerCheckbox->isChecked());
    settings.setValue("digital/dmrLabSourceId", dmrLabSourceIdEdit ? dmrLabSourceIdEdit->text().trimmed() : QString());
    settings.setValue("digital/dmrLabTargetId", dmrLabTargetIdEdit ? dmrLabTargetIdEdit->text().trimmed() : QString());
    settings.setValue("digital/dmrLabRadio", dmrLabRadioEdit ? dmrLabRadioEdit->text().trimmed() : QString());
    settings.setValue("digital/dmrLabNotes", dmrLabNotesEdit ? dmrLabNotesEdit->text().trimmed() : QString());
    settings.setValue("digital/dmrBackend", selectedDmrBackend());
    settings.setValue("digital/dsdNeoBridgeEnabled", selectedDmrBackend() == DMR_BACKEND_DSD_NEO);
    settings.setValue("digital/dsdNeoAutoStart", dsdNeoAutoStartCheckbox && dsdNeoAutoStartCheckbox->isChecked());
    settings.setValue("digital/dsdNeoProgram", dsdNeoProgramEdit ? dsdNeoProgramEdit->text().trimmed() : defaultDsdNeoProgramPath());
    settings.setValue("digital/dsdNeoInputPort", dsdNeoInputPortSpin ? dsdNeoInputPortSpin->value() : 7355);
    settings.setValue("digital/dsdNeoUdpOutputPort", dsdNeoUdpOutputPortSpin ? dsdNeoUdpOutputPortSpin->value() : 23456);
    settings.setValue("video/decodeEnabled", videoDecodeEnabled);
    settings.setValue("video/sstvDemodulationMode",
                      sstvDemodulationCombo
                          ? normalizedSstvDemodulationMode(
                                sstvDemodulationCombo->currentData().toInt())
                          : normalizedSstvDemodulationMode(
                                pendingSettings.sstvDemodulationMode));
    settings.setValue("video/demodMode", videoDemodCombo ? videoDemodCombo->currentData().toInt() : VideoProcessor::FmVideo);
    settings.setValue("video/standardIndex", videoStandardCombo ? videoStandardCombo->currentIndex() : 0);
    settings.setValue("video/invert", videoInvertCheckbox && videoInvertCheckbox->isChecked());
    settings.setValue("video/hSync", !videoHSyncCheckbox || videoHSyncCheckbox->isChecked());
    settings.setValue("video/vSync", !videoVSyncCheckbox || videoVSyncCheckbox->isChecked());
    settings.sync();

    if (settings.status() == QSettings::NoError) {
        if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[Settings] saved" << persistentSettingsFilePath();
        }
    } else {
        qDebug() << "[Settings] save failed" << persistentSettingsFilePath()
                 << "status" << settings.status();
    }
    persistentSettingsSaveInProgress = false;
    persistentSettingsLastSaveTimer.restart();
}
