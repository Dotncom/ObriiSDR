#ifndef PRESETHELPERS_H
#define PRESETHELPERS_H

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>

inline constexpr double PRESET_RF_MIN_CENTER_FREQUENCY = 50000000.0;
inline constexpr double PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY = 7750000000.0;
inline constexpr int PRESET_STANDARD_SCAN_MIN_SETTLE_MS = 0;
inline constexpr int PRESET_STANDARD_SCAN_MAX_SETTLE_MS = 1000;
inline constexpr int PRESET_STANDARD_SCAN_MIN_DWELL_MS = 20;
inline constexpr int PRESET_STANDARD_SCAN_MAX_DWELL_MS = 5000;
inline constexpr int PRESET_LISTENING_SCAN_MIN_DWELL_MS = 50;
inline constexpr int PRESET_LISTENING_SCAN_MAX_DWELL_MS = 600000;
inline constexpr int PRESET_LISTENING_SCAN_MIN_SETTLE_MS = 0;
inline constexpr int PRESET_LISTENING_SCAN_MAX_SETTLE_MS = 10000;
inline constexpr int PRESET_AGILE_SCAN_MIN_POINTS = 2;
inline constexpr int PRESET_AGILE_SCAN_MAX_POINTS = 256;
inline constexpr double PRESET_AGILE_SCAN_MIN_STEP_MHZ = 0.001;
inline constexpr double PRESET_AGILE_SCAN_MAX_STEP_MHZ = 1000.0;
inline constexpr double PRESET_BANDWIDTH_MAX_HZ = 80000000.0;

QString agileScanPresetSpec(const QString &rangesMhz, double stepMhz);
QString standardScanPresetSpec(const QString &centersMhz, int dwellMs, int settleMs);
QString listeningScanPresetSpec(const QString &targetsMhz, int dwellMs, int settleMs);

QString agileScanPresetRanges(const QString &spec, const QString &fallback = QString());
QString standardScanPresetCenters(const QString &spec, const QString &fallback = QString());
QString listeningScanPresetTargets(const QString &spec, const QString &fallback = QString());

double agileScanPresetStepMhz(const QString &spec, double fallback);
int standardScanPresetDwellMs(const QString &spec, int fallback);
int standardScanPresetSettleMs(const QString &spec, int fallback);
int listeningScanPresetDwellMs(const QString &spec, int fallback);
int listeningScanPresetSettleMs(const QString &spec, int fallback);

template <typename T>
QStringList normalizedPresetOrder(const QStringList &requestedOrder,
                                  const QMap<QString, T> &presets,
                                  const QStringList &defaultOrder = QStringList()) {
    QStringList normalized;
    auto appendIfValid = [&normalized, &presets](const QString &rawName) {
        const QString name = rawName.trimmed();
        if (!name.isEmpty() &&
            presets.contains(name) &&
            !normalized.contains(name)) {
            normalized.append(name);
        }
    };

    const QStringList seedOrder = requestedOrder.isEmpty() ? defaultOrder : requestedOrder;
    for (const QString &name : seedOrder) {
        appendIfValid(name);
    }

    for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
        appendIfValid(it.key());
    }
    return normalized;
}

QStringList defaultCenterFrequencyPresetOrder();
QStringList defaultListeningFrequencyPresetOrder();
QStringList defaultBandwidthPresetOrder();
QStringList defaultAgileScanPresetOrder();
QStringList defaultStandardScanPresetOrder();
QStringList defaultListeningScanPresetOrder();

const QVector<double> &fpv6GhzVideoFrequenciesMhz();
QString fpv6GhzFrequencyPresetName(double frequencyMhz);
QString fpv6GhzScanTargetsMhz(double maximumMhz);

QVector<double> parseAgileScanFrequenciesMhz(const QString &rangesMhz,
                                             double stepMhz,
                                             QString *error);
QString formatMhzList(const QVector<double> &frequenciesHz);
QVector<double> parseStandardScanCentersMhz(const QString &centersMhz,
                                            double sampleRateHz,
                                            int minimumPoints,
                                            QString *error,
                                            bool *adjusted);
QVector<double> parseListeningScanTargetsMhz(const QString &targetsMhz,
                                             double minimumHz,
                                             double maximumHz,
                                             int minimumPoints,
                                             QString *error);

#endif // PRESETHELPERS_H
