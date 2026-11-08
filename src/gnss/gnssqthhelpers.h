#ifndef GNSSQTHHELPERS_H
#define GNSSQTHHELPERS_H

#include <QColor>
#include <QString>
#include <QVector>

#include <cstddef>

inline constexpr double GNSS_L1_BAND_START_HZ = 1559000000.0;
inline constexpr double GNSS_L1_BAND_END_HZ = 1610000000.0;
inline constexpr double GNSS_L1_BAND_CENTER_HZ = (GNSS_L1_BAND_START_HZ + GNSS_L1_BAND_END_HZ) * 0.5;
inline constexpr double GNSS_L1_LISTENING_SCAN_CENTER_HZ = 1583000000.0;
inline constexpr double GNSS_GPS_L1_HZ = 1575420000.0;
inline constexpr double GNSS_BEIDOU_B1I_HZ = 1561098000.0;
inline constexpr double GNSS_GLONASS_L1_CENTER_HZ = 1602000000.0;
inline constexpr double GNSS_RAW_BANDWIDTH_HZ = 2046000.0;
inline constexpr double GNSS_USEFUL_STANDARD_SPAN_HZ = 50000000.0;
inline constexpr int GNSS_ACQUISITION_INTEGRATION_MS = 24;
inline constexpr int GNSS_ACQUISITION_MIN_INTEGRATION_MS = 1;
inline constexpr int GNSS_ACQUISITION_MAX_INTEGRATION_MS = 1200;
inline constexpr int GNSS_DEEP_ACQUISITION_MS = 600;
inline constexpr int GNSS_CONTINUOUS_ACQUISITION_FIRST_DELAY_MS = 100;
inline constexpr int GNSS_CONTINUOUS_ACQUISITION_MIN_INTERVAL_MS = 500;
inline constexpr int GNSS_CONTINUOUS_ACQUISITION_MAX_INTERVAL_MS = 30000;
inline constexpr int GNSS_CONTINUOUS_ACQUISITION_MAX_INTEGRATION_MS = 24;
inline constexpr std::size_t GNSS_QUICK_SNAPSHOT_MAX_FLOATS = 4 * 1024 * 1024;
inline constexpr double GNSS_CHANNEL_FILTER_MIN_HZ = 300000.0;
inline constexpr double GNSS_CHANNEL_FILTER_MAX_HZ = 1840000.0;
inline constexpr double GNSS_DEG_TO_RAD = 3.1415926535897932384626433832795 / 180.0;

enum class GnssAcquisitionKind {
    None,
    GpsL1Ca,
    GlonassL1Of,
    GpsGlonassL1,
};

struct GnssSystemPreset {
    QString id;
    QString textKey;
    QString fallbackName;
    double centerHz = 0.0;
    double targetHz = 0.0;
    double bandwidthHz = 0.0;
    QString standardScanCentersMhz;
    QString agileScanRangesMhz;
    int standardDwellMs = 3000;
    int standardSettleMs = 120;
    GnssAcquisitionKind acquisitionKind = GnssAcquisitionKind::None;
};

struct ParsedNmeaPosition {
    double latitude = 0.0;
    double longitude = 0.0;
    QString talker;
    QString sentence;
    QString utc;
};

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct SyntheticPseudorange {
    Vec3d satellite;
    double pseudorangeMeters = 0.0;
};

struct SyntheticPositionResult {
    bool valid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeMeters = 0.0;
    double clockBiasMeters = 0.0;
    double errorMeters = 0.0;
    int iterations = 0;
    int satellites = 0;
};

struct QthOnlineProviderPreset {
    QString id;
    QString textKey;
    QString fallbackName;
    QString urlTemplate;
    QString attribution;
    bool requiresKey = false;
    bool noDiskCache = false;
    int maxZoom = 19;
};

Vec3d operator+(const Vec3d &a, const Vec3d &b);
Vec3d operator-(const Vec3d &a, const Vec3d &b);
Vec3d operator*(const Vec3d &v, double scale);
double vectorNorm(const Vec3d &v);

Vec3d geodeticToEcef(double latitudeDeg, double longitudeDeg, double altitudeMeters);
void ecefToGeodetic(const Vec3d &ecef, double *latitudeDeg, double *longitudeDeg, double *altitudeMeters);
void enuBasis(double latitudeDeg, double longitudeDeg, Vec3d *east, Vec3d *north, Vec3d *up);

SyntheticPositionResult solveSyntheticPosition(const QVector<SyntheticPseudorange> &measurements,
                                               const Vec3d &truthEcef,
                                               const Vec3d &initialEcef);

bool parseNmeaPositionSentence(const QString &line, ParsedNmeaPosition *position);

QVector<GnssSystemPreset> gnssSystemPresets();
GnssSystemPreset gnssSystemPreset(const QString &id);

QVector<QthOnlineProviderPreset> qthOnlineProviderPresets();
QthOnlineProviderPreset qthOnlineProviderPreset(const QString &id);
bool qthOnlineTemplateNeedsKey(const QString &urlTemplate);

QColor gnssHeatColor(double value, double minValue, double maxValue);

#endif // GNSSQTHHELPERS_H
