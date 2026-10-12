#include "main.h"

#include "appconstants.h"
#include "gnssqthhelpers.h"
#include "presethelpers.h"

#include <algorithm>
#include <cmath>
void YourClassName::saveAgileScanPreset() {
    if (!agileScanPresetCombo) {
        return;
    }
    QString name = agileScanPresetCombo->currentText().trimmed();
    if (name.isEmpty()) {
        name = QStringLiteral("Preset %1").arg(agileScanPresets.size() + 1);
    }
    refreshSettingsFromUi();
    agileScanPresets[name] = agileScanPresetSpec(agileScanRangesMhz, agileScanStepMhz);
    if (!agileScanPresetOrder.contains(name)) {
        agileScanPresetOrder.append(name);
    }
    agileScanPresetCombo->setEditText(name);
    updateAgileScanControls();
    savePersistentSettings();
}

void YourClassName::deleteAgileScanPreset() {
    if (!agileScanPresetCombo) {
        return;
    }
    const QString name = agileScanPresetCombo->currentText().trimmed();
    if (!name.isEmpty()) {
        agileScanPresets.remove(name);
        agileScanPresetOrder.removeAll(name);
    }
    updateAgileScanControls();
    savePersistentSettings();
}

void YourClassName::ensureDefaultFrequencyPresets() {
    if (centerFrequencyPresets.isEmpty()) {
        centerFrequencyPresets[QStringLiteral("FM broadcast 100 MHz")] = 100000000.0;
        centerFrequencyPresets[QStringLiteral("Airband 125 MHz")] = 125000000.0;
        centerFrequencyPresets[QStringLiteral("VHF 145 MHz")] = 145000000.0;
        centerFrequencyPresets[QStringLiteral("UHF 433 MHz")] = 433000000.0;
        centerFrequencyPresets[QStringLiteral("GSM/LTE 900 MHz")] = 900000000.0;
        centerFrequencyPresets[QStringLiteral("FPV 1.1 GHz")] = 1120000000.0;
        centerFrequencyPresets[QStringLiteral("FPV 1.2 GHz")] = 1200000000.0;
        centerFrequencyPresets[QStringLiteral("FPV 1.3 GHz")] = 1280000000.0;
        centerFrequencyPresets[QStringLiteral("FPV 2.4 GHz")] = 2400000000.0;
        centerFrequencyPresets[QStringLiteral("FPV 3.3 GHz")] = 3350000000.0;
        centerFrequencyPresets[QStringLiteral("Experimental 7.0 GHz")] = 7000000000.0;
        centerFrequencyPresets[QStringLiteral("Experimental 7.5 GHz")] = 7500000000.0;
    }
    if (listeningFrequencyPresets.isEmpty()) {
        listeningFrequencyPresets[QStringLiteral("HF center 0 Hz")] = 0.0;
        listeningFrequencyPresets[QStringLiteral("HF 500 kHz")] = 500000.0;
        listeningFrequencyPresets[QStringLiteral("HF 1.25 MHz")] = 1250000.0;
        listeningFrequencyPresets[QStringLiteral("80 m 3.65 MHz")] = 3650000.0;
        listeningFrequencyPresets[QStringLiteral("40 m 7.05 MHz")] = 7050000.0;
        listeningFrequencyPresets[QStringLiteral("20 m FT8 14.074 MHz")] = 14074000.0;
        listeningFrequencyPresets[QStringLiteral("VHF 145 MHz")] = 145000000.0;
        listeningFrequencyPresets[QStringLiteral("UHF 433 MHz")] = 433000000.0;
    }
    auto addMissingFrequencyPreset = [this](const QString &name, double valueHz) {
        if (!centerFrequencyPresets.contains(name)) {
            centerFrequencyPresets[name] = valueHz;
        }
        if (!listeningFrequencyPresets.contains(name)) {
            listeningFrequencyPresets[name] = valueHz;
        }
    };
    const QVector<QPair<const char *, double>> wirelessPresets = {
        {"Wi-Fi 2.4 ch1 2412 MHz", 2412.0},
        {"Wi-Fi 2.4 ch6 2437 MHz", 2437.0},
        {"Wi-Fi 2.4 ch11 2462 MHz", 2462.0},
        {"Wi-Fi 2.4 ch13 2472 MHz", 2472.0},
        {"Wi-Fi 5 ch36 5180 MHz", 5180.0},
        {"Wi-Fi 5 ch100 5500 MHz", 5500.0},
        {"Wi-Fi 5 ch149 5745 MHz", 5745.0},
        {"Wi-Fi 6 ch1 5955 MHz", 5955.0},
        {"BLE advertising 37 2402 MHz", 2402.0},
        {"BLE advertising 38 2426 MHz", 2426.0},
        {"BLE advertising 39 2480 MHz", 2480.0},
        {"Bluetooth 2.4 band center 2441 MHz", 2441.0},
        {"GSM/LTE B8 downlink 942.5 MHz", 942.5},
        {"NR n78 band center 3550 MHz", 3550.0}
    };
    for (const auto &preset : wirelessPresets) {
        addMissingFrequencyPreset(QString::fromLatin1(preset.first), preset.second * 1000000.0);
    }
    addMissingFrequencyPreset(QStringLiteral("LTE 700 downlink 780.5 MHz"), 780500000.0);
    addMissingFrequencyPreset(QStringLiteral("LTE 800 downlink 806 MHz"), 806000000.0);
    addMissingFrequencyPreset(QStringLiteral("UMTS/LTE 1800 downlink 1842.5 MHz"), 1842500000.0);
    addMissingFrequencyPreset(QStringLiteral("UMTS/LTE 2100 downlink 2140 MHz"), 2140000000.0);
    addMissingFrequencyPreset(QStringLiteral("LTE 2600 downlink 2655 MHz"), 2655000000.0);
    addMissingFrequencyPreset(QStringLiteral("UHF Satcom 255 MHz"), 255000000.0);
    addMissingFrequencyPreset(QStringLiteral("FPV 1.1 GHz"), 1120000000.0);
    addMissingFrequencyPreset(QStringLiteral("FPV 1.3 GHz"), 1280000000.0);
    addMissingFrequencyPreset(QStringLiteral("FPV 3.3 GHz"), 3350000000.0);
    const QVector<double> fpvVideoPresetMhz = {
        1080.0, 1120.0, 1160.0, 1200.0,
        1240.0, 1258.0, 1280.0, 1320.0,
        1360.0, 1440.0, 1450.0, 1600.0, 1620.0,
        3200.0, 3250.0, 3300.0, 3350.0,
        3400.0, 3450.0, 3500.0,
        4990.0, 5010.0, 5360.0, 5460.0,
        5640.0, 5660.0, 5680.0, 5880.0,
        5890.0, 5910.0
    };
    for (double mhz : fpvVideoPresetMhz) {
        addMissingFrequencyPreset(QStringLiteral("FPV video %1 MHz").arg(mhz, 0, 'f', 0),
                                  mhz * 1000000.0);
    }
    addMissingFrequencyPreset(QStringLiteral("GNSS L1 compact center 1583 MHz"), GNSS_L1_LISTENING_SCAN_CENTER_HZ);
    addMissingFrequencyPreset(QStringLiteral("GNSS L1 band center 1584.5 MHz"), GNSS_L1_BAND_CENTER_HZ);
    addMissingFrequencyPreset(QStringLiteral("GPS/Galileo L1 1575.42 MHz"), GNSS_GPS_L1_HZ);
    addMissingFrequencyPreset(QStringLiteral("BeiDou B1I 1561.098 MHz"), GNSS_BEIDOU_B1I_HZ);
    addMissingFrequencyPreset(QStringLiteral("GLONASS L1 center 1602 MHz"), GNSS_GLONASS_L1_CENTER_HZ);
    if (bandwidthValuePresets.isEmpty()) {
        bandwidthValuePresets[QStringLiteral("CW 500 Hz")] = 500.0;
        bandwidthValuePresets[QStringLiteral("SSB 2.7 kHz")] = 2700.0;
        bandwidthValuePresets[QStringLiteral("FT8 3 kHz")] = 3000.0;
        bandwidthValuePresets[QStringLiteral("AM 6 kHz")] = 6000.0;
        bandwidthValuePresets[QStringLiteral("AM 10 kHz")] = 10000.0;
        bandwidthValuePresets[QStringLiteral("NFM 12.5 kHz")] = 12500.0;
        bandwidthValuePresets[QStringLiteral("DMR 12.5 kHz")] = 12500.0;
        bandwidthValuePresets[QStringLiteral("WFM 200 kHz")] = 200000.0;
        bandwidthValuePresets[QStringLiteral("SSTV 3 kHz")] = 3000.0;
        bandwidthValuePresets[QStringLiteral("NOAA APT 40 kHz")] = 40000.0;
        bandwidthValuePresets[QStringLiteral("WEFAX 3 kHz")] = 3000.0;
        bandwidthValuePresets[QStringLiteral("LRPT 140 kHz")] = 140000.0;
        bandwidthValuePresets[QStringLiteral("ATV 3 MHz")] = 3000000.0;
        bandwidthValuePresets[QStringLiteral("ATV 5 MHz")] = 5000000.0;
        bandwidthValuePresets[QStringLiteral("FPV 8 MHz")] = 8000000.0;
        bandwidthValuePresets[QStringLiteral("FPV 10 MHz")] = 10000000.0;
    }
    auto addMissingBandwidthPreset = [this](const QString &name, double valueHz) {
        if (!bandwidthValuePresets.contains(name)) {
            bandwidthValuePresets[name] = valueHz;
        }
    };
    addMissingBandwidthPreset(QStringLiteral("ATV 3 MHz"), 3000000.0);
    addMissingBandwidthPreset(QStringLiteral("ATV 5 MHz"), 5000000.0);
    addMissingBandwidthPreset(QStringLiteral("FPV 8 MHz"), 8000000.0);
    addMissingBandwidthPreset(QStringLiteral("FPV 10 MHz"), 10000000.0);
    addMissingBandwidthPreset(QStringLiteral("UHF Satcom NFM 25 kHz"), 25000.0);
    addMissingBandwidthPreset(QStringLiteral("GNSS C/A 2.046 MHz"), GNSS_RAW_BANDWIDTH_HZ);
    addMissingBandwidthPreset(QStringLiteral("GNSS raw 4.092 MHz"), 4092000.0);
    addMissingBandwidthPreset(QStringLiteral("GLONASS L1OF 9 MHz"), 9000000.0);
    addMissingBandwidthPreset(QStringLiteral("GNSS L1 survey 50 MHz"), GNSS_USEFUL_STANDARD_SPAN_HZ);

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
}

void YourClassName::ensureDefaultBandMarkers() {
    if (bandMarkersCustomized || !bandMarkers.isEmpty()) {
        return;
    }

    auto addMhz = [this](const char *label, double startMhz, double endMhz, bool amateur) {
        GraphBandMarker marker;
        marker.startHz = startMhz * 1000000.0;
        marker.endHz = endMhz * 1000000.0;
        marker.label = QString::fromLatin1(label);
        marker.amateur = amateur;
        bandMarkers.append(marker);
    };

    addMhz("MW BC", 0.5265, 1.705, false);
    addMhz("SW 49m", 5.9, 6.2, false);
    addMhz("SW 41m", 7.2, 7.45, false);
    addMhz("SW 31m", 9.4, 9.9, false);
    addMhz("SW 25m", 11.6, 12.1, false);
    addMhz("SW 19m", 15.1, 15.8, false);
    addMhz("CB", 26.965, 27.405, false);
    addMhz("FM BC", 87.5, 108.0, false);
    addMhz("Air", 118.0, 137.0, false);
    addMhz("WX Sat", 137.0, 138.0, false);
    addMhz("Marine", 156.0, 162.05, false);
    addMhz("UHF Satcom", 240.0, 270.0, false);
    addMhz("TETRA", 380.0, 430.0, false);
    addMhz("PMR446", 446.0, 446.2, false);
    addMhz("LTE 700 DL", 758.0, 803.0, false);
    addMhz("LTE 800 DL", 791.0, 821.0, false);
    addMhz("GSM/LTE 900 DL", 925.0, 960.0, false);
    addMhz("ADS-B", 1089.5, 1090.5, false);
    addMhz("L-band Sat", 1525.0, 1660.5, false);
    addMhz("GNSS L1", 1559.0, 1610.0, false);
    addMhz("UMTS/LTE 1800 DL", 1805.0, 1880.0, false);
    addMhz("UMTS/LTE 2100 DL", 2110.0, 2170.0, false);
    addMhz("ISM 2.4", 2400.0, 2483.5, false);
    addMhz("LTE 2600 DL", 2620.0, 2690.0, false);
    addMhz("FPV 5.8", 5650.0, 5925.0, false);

    addMhz("2200m", 0.1357, 0.1378, true);
    addMhz("630m", 0.472, 0.479, true);
    addMhz("160m", 1.81, 2.0, true);
    addMhz("80m", 3.5, 3.8, true);
    addMhz("60m", 5.3515, 5.3665, true);
    addMhz("40m", 7.0, 7.2, true);
    addMhz("30m", 10.1, 10.15, true);
    addMhz("20m", 14.0, 14.35, true);
    addMhz("17m", 18.068, 18.168, true);
    addMhz("15m", 21.0, 21.45, true);
    addMhz("12m", 24.89, 24.99, true);
    addMhz("10m", 28.0, 29.7, true);
    addMhz("6m", 50.0, 52.0, true);
    addMhz("4m", 70.0, 70.5, true);
    addMhz("2m", 144.0, 146.0, true);
    addMhz("70cm", 430.0, 440.0, true);
    addMhz("23cm", 1240.0, 1300.0, true);
    addMhz("13cm", 2300.0, 2450.0, true);
    addMhz("6cm", 5650.0, 5850.0, true);
}

QVector<QPair<QString, double>> YourClassName::presetMapToVector(const QMap<QString, double> &presets,
                                                                 const QStringList &order) const {
    QVector<QPair<QString, double>> values;
    values.reserve(presets.size());
    const QStringList normalizedOrder = normalizedPresetOrder(order, presets);
    for (const QString &name : normalizedOrder) {
        auto it = presets.constFind(name);
        if (it != presets.constEnd() &&
            !it.key().trimmed().isEmpty() &&
            std::isfinite(it.value())) {
            values.append(qMakePair(it.key(), it.value()));
        }
    }
    return values;
}

void YourClassName::updateFrequencyPresetControls() {
    ensureDefaultFrequencyPresets();
    if (frequencyControl) {
        frequencyControl->setValuePresets(presetMapToVector(centerFrequencyPresets,
                                                            centerFrequencyPresetOrder));
    }
    if (listeningFrequencyControl) {
        listeningFrequencyControl->setValuePresets(presetMapToVector(listeningFrequencyPresets,
                                                                     listeningFrequencyPresetOrder));
    }
    if (bandwidthControl) {
        bandwidthControl->setValuePresets(presetMapToVector(bandwidthValuePresets,
                                                            bandwidthPresetOrder));
    }
    updateAgileScanControls();
}

void YourClassName::updateGraphBandMarkers() {
    if (!graphWidget) {
        return;
    }
    graphWidget->setBandMarkers(bandMarkers);
    graphWidget->setBandMarkersEnabled(showGeneralBandMarkers, showAmateurBandMarkers);
    graphWidget->setBandMarkersCompact(compactBandMarkers);
    if (waterfallWidget) {
        waterfallWidget->setBandMarkers(bandMarkers);
        waterfallWidget->setBandMarkersEnabled(showGeneralBandMarkers, showAmateurBandMarkers);
        waterfallWidget->setBandMarkersCompact(compactBandMarkers);
    }
}
