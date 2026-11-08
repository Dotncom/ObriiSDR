#include "gnssqthhelpers.h"

#include "qthlocator.h"

#include <QByteArray>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kWgs84A = 6378137.0;
constexpr double kWgs84F = 1.0 / 298.257223563;
constexpr double kWgs84E2 = kWgs84F * (2.0 - kWgs84F);
constexpr double kDegToRad = 3.1415926535897932384626433832795 / 180.0;
constexpr double kRadToDeg = 180.0 / 3.1415926535897932384626433832795;

} // namespace
Vec3d operator+(const Vec3d &a, const Vec3d &b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3d operator-(const Vec3d &a, const Vec3d &b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3d operator*(const Vec3d &v, double scale) {
    return {v.x * scale, v.y * scale, v.z * scale};
}

double dot(const Vec3d &a, const Vec3d &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double norm(const Vec3d &v) {
    return std::sqrt(dot(v, v));
}

double vectorNorm(const Vec3d &v) {
    return norm(v);
}

Vec3d geodeticToEcef(double latitudeDeg, double longitudeDeg, double altitudeMeters) {
    const double lat = latitudeDeg * kDegToRad;
    const double lon = longitudeDeg * kDegToRad;
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double sinLon = std::sin(lon);
    const double cosLon = std::cos(lon);
    const double n = kWgs84A / std::sqrt(1.0 - kWgs84E2 * sinLat * sinLat);
    return {
        (n + altitudeMeters) * cosLat * cosLon,
        (n + altitudeMeters) * cosLat * sinLon,
        (n * (1.0 - kWgs84E2) + altitudeMeters) * sinLat
    };
}

void ecefToGeodetic(const Vec3d &ecef, double *latitudeDeg, double *longitudeDeg, double *altitudeMeters) {
    const double b = kWgs84A * (1.0 - kWgs84F);
    const double ep2 = (kWgs84A * kWgs84A - b * b) / (b * b);
    const double p = std::sqrt(ecef.x * ecef.x + ecef.y * ecef.y);
    const double theta = std::atan2(ecef.z * kWgs84A, p * b);
    const double sinTheta = std::sin(theta);
    const double cosTheta = std::cos(theta);
    const double lat = std::atan2(ecef.z + ep2 * b * sinTheta * sinTheta * sinTheta,
                                  p - kWgs84E2 * kWgs84A * cosTheta * cosTheta * cosTheta);
    const double lon = std::atan2(ecef.y, ecef.x);
    const double sinLat = std::sin(lat);
    const double n = kWgs84A / std::sqrt(1.0 - kWgs84E2 * sinLat * sinLat);
    const double alt = p / std::cos(lat) - n;
    if (latitudeDeg) {
        *latitudeDeg = lat * kRadToDeg;
    }
    if (longitudeDeg) {
        *longitudeDeg = lon * kRadToDeg;
    }
    if (altitudeMeters) {
        *altitudeMeters = alt;
    }
}

void enuBasis(double latitudeDeg, double longitudeDeg, Vec3d *east, Vec3d *north, Vec3d *up) {
    const double lat = latitudeDeg * kDegToRad;
    const double lon = longitudeDeg * kDegToRad;
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double sinLon = std::sin(lon);
    const double cosLon = std::cos(lon);
    if (east) {
        *east = {-sinLon, cosLon, 0.0};
    }
    if (north) {
        *north = {-sinLat * cosLon, -sinLat * sinLon, cosLat};
    }
    if (up) {
        *up = {cosLat * cosLon, cosLat * sinLon, sinLat};
    }
}

bool solve4x4(double a[4][4], double b[4], double out[4]) {
    double m[4][5] = {};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            m[row][col] = a[row][col];
        }
        m[row][4] = b[row];
    }

    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 4; ++row) {
            if (std::abs(m[row][col]) > std::abs(m[pivot][col])) {
                pivot = row;
            }
        }
        if (std::abs(m[pivot][col]) < 1.0e-9) {
            return false;
        }
        if (pivot != col) {
            for (int k = col; k < 5; ++k) {
                std::swap(m[col][k], m[pivot][k]);
            }
        }
        const double divisor = m[col][col];
        for (int k = col; k < 5; ++k) {
            m[col][k] /= divisor;
        }
        for (int row = 0; row < 4; ++row) {
            if (row == col) {
                continue;
            }
            const double factor = m[row][col];
            for (int k = col; k < 5; ++k) {
                m[row][k] -= factor * m[col][k];
            }
        }
    }

    for (int i = 0; i < 4; ++i) {
        out[i] = m[i][4];
    }
    return true;
}

SyntheticPositionResult solveSyntheticPosition(const QVector<SyntheticPseudorange> &measurements,
                                               const Vec3d &truthEcef,
                                               const Vec3d &initialEcef) {
    SyntheticPositionResult result;
    result.satellites = measurements.size();
    if (measurements.size() < 4) {
        return result;
    }

    Vec3d receiver = initialEcef;
    double clockBias = 0.0;
    for (int iteration = 0; iteration < 12; ++iteration) {
        double normal[4][4] = {};
        double rhs[4] = {};

        for (const SyntheticPseudorange &measurement : measurements) {
            const Vec3d delta = receiver - measurement.satellite;
            const double range = (std::max)(1.0, norm(delta));
            const double predicted = range + clockBias;
            const double residual = measurement.pseudorangeMeters - predicted;
            const double h[4] = {
                delta.x / range,
                delta.y / range,
                delta.z / range,
                1.0
            };
            for (int row = 0; row < 4; ++row) {
                rhs[row] += h[row] * residual;
                for (int col = 0; col < 4; ++col) {
                    normal[row][col] += h[row] * h[col];
                }
            }
        }

        double update[4] = {};
        if (!solve4x4(normal, rhs, update)) {
            return result;
        }
        receiver.x += update[0];
        receiver.y += update[1];
        receiver.z += update[2];
        clockBias += update[3];
        result.iterations = iteration + 1;
        if (std::sqrt(update[0] * update[0] + update[1] * update[1] + update[2] * update[2]) < 0.001 &&
            std::abs(update[3]) < 0.001) {
            break;
        }
    }

    ecefToGeodetic(receiver, &result.latitude, &result.longitude, &result.altitudeMeters);
    result.clockBiasMeters = clockBias;
    result.errorMeters = norm(receiver - truthEcef);
    result.valid = qth::isValidLatitude(result.latitude) &&
                   qth::isValidLongitude(result.longitude) &&
                   std::isfinite(result.errorMeters);
    return result;
}

bool nmeaChecksumMatches(const QString &body, const QString &checksumText) {
    if (checksumText.size() < 2) {
        return false;
    }
    bool ok = false;
    const int expected = checksumText.left(2).toInt(&ok, 16);
    if (!ok) {
        return false;
    }
    quint8 actual = 0;
    const QByteArray bytes = body.toLatin1();
    for (char byte : bytes) {
        actual ^= static_cast<quint8>(byte);
    }
    return actual == static_cast<quint8>(expected);
}

bool parseNmeaCoordinate(const QString &value,
                         const QString &hemisphere,
                         bool latitude,
                         double *coordinate) {
    if (!coordinate) {
        return false;
    }
    const QString text = value.trimmed();
    const QString hemi = hemisphere.trimmed().toUpper();
    const int degreeDigits = latitude ? 2 : 3;
    if (text.size() <= degreeDigits || hemi.size() != 1) {
        return false;
    }
    bool degreesOk = false;
    bool minutesOk = false;
    const int degrees = text.left(degreeDigits).toInt(&degreesOk);
    const double minutes = text.mid(degreeDigits).toDouble(&minutesOk);
    if (!degreesOk || !minutesOk || !std::isfinite(minutes) || minutes < 0.0 || minutes >= 60.0) {
        return false;
    }
    double result = static_cast<double>(degrees) + minutes / 60.0;
    if (hemi == QStringLiteral("S") || hemi == QStringLiteral("W")) {
        result = -result;
    } else if (hemi != QStringLiteral("N") && hemi != QStringLiteral("E")) {
        return false;
    }
    if (latitude) {
        if (result < -90.0 || result > 90.0) {
            return false;
        }
    } else if (result < -180.0 || result > 180.0) {
        return false;
    }
    *coordinate = result;
    return true;
}

bool parseNmeaPositionSentence(const QString &line, ParsedNmeaPosition *position) {
    if (!position) {
        return false;
    }
    QString sentence = line.trimmed();
    if (sentence.isEmpty()) {
        return false;
    }
    const int start = sentence.indexOf(QLatin1Char('$'));
    if (start >= 0) {
        sentence = sentence.mid(start + 1);
    } else if (sentence.startsWith(QLatin1Char('!'))) {
        sentence.remove(0, 1);
    }
    const int checksumPos = sentence.indexOf(QLatin1Char('*'));
    const QString body = checksumPos >= 0 ? sentence.left(checksumPos) : sentence;
    if (checksumPos >= 0) {
        const QString checksumText = sentence.mid(checksumPos + 1).trimmed();
        if (!nmeaChecksumMatches(body, checksumText)) {
            return false;
        }
    }

    const QStringList fields = body.split(QLatin1Char(','));
    if (fields.isEmpty()) {
        return false;
    }
    const QString type = fields.at(0).trimmed().toUpper();
    if (type.size() < 5) {
        return false;
    }

    double lat = 0.0;
    double lon = 0.0;
    if (type.endsWith(QStringLiteral("GGA"))) {
        if (fields.size() < 7) {
            return false;
        }
        bool fixOk = false;
        const int fixQuality = fields.at(6).trimmed().toInt(&fixOk);
        if (!fixOk || fixQuality <= 0) {
            return false;
        }
        if (!parseNmeaCoordinate(fields.at(2), fields.at(3), true, &lat) ||
            !parseNmeaCoordinate(fields.at(4), fields.at(5), false, &lon)) {
            return false;
        }
        position->utc = fields.at(1).trimmed();
    } else if (type.endsWith(QStringLiteral("RMC"))) {
        if (fields.size() < 7 || fields.at(2).trimmed().toUpper() != QStringLiteral("A")) {
            return false;
        }
        if (!parseNmeaCoordinate(fields.at(3), fields.at(4), true, &lat) ||
            !parseNmeaCoordinate(fields.at(5), fields.at(6), false, &lon)) {
            return false;
        }
        position->utc = fields.at(1).trimmed();
    } else {
        return false;
    }

    position->latitude = lat;
    position->longitude = lon;
    position->talker = type.left(type.size() - 3);
    position->sentence = type.right(3);
    return true;
}

QVector<GnssSystemPreset> gnssSystemPresets() {
    return {
        {
            QStringLiteral("all_l1"),
            QStringLiteral("gnss_system_all_l1"),
            QStringLiteral("All L1 systems"),
            GNSS_L1_LISTENING_SCAN_CENTER_HZ,
            GNSS_GPS_L1_HZ,
            GNSS_USEFUL_STANDARD_SPAN_HZ,
            QStringLiteral("1583.000000"),
            QStringLiteral("1559-1610"),
            3000,
            120,
            GnssAcquisitionKind::GpsGlonassL1
        },
        {
            QStringLiteral("gps_l1_ca"),
            QStringLiteral("gnss_system_gps_l1_ca"),
            QStringLiteral("GPS L1 C/A"),
            GNSS_GPS_L1_HZ,
            GNSS_GPS_L1_HZ,
            GNSS_RAW_BANDWIDTH_HZ,
            QStringLiteral("1575.420000"),
            QStringLiteral("1574.397-1576.443"),
            3000,
            80,
            GnssAcquisitionKind::GpsL1Ca
        },
        {
            QStringLiteral("galileo_e1"),
            QStringLiteral("gnss_system_galileo_e1"),
            QStringLiteral("Galileo E1"),
            GNSS_GPS_L1_HZ,
            GNSS_GPS_L1_HZ,
            4092000.0,
            QStringLiteral("1575.420000"),
            QStringLiteral("1571.328-1579.512"),
            3000,
            80,
            GnssAcquisitionKind::None
        },
        {
            QStringLiteral("beidou_b1i"),
            QStringLiteral("gnss_system_beidou_b1i"),
            QStringLiteral("BeiDou B1I"),
            GNSS_BEIDOU_B1I_HZ,
            GNSS_BEIDOU_B1I_HZ,
            GNSS_RAW_BANDWIDTH_HZ,
            QStringLiteral("1561.098000"),
            QStringLiteral("1560.075-1562.121"),
            3000,
            80,
            GnssAcquisitionKind::None
        },
        {
            QStringLiteral("glonass_l1of"),
            QStringLiteral("gnss_system_glonass_l1of"),
            QStringLiteral("GLONASS L1OF"),
            GNSS_GLONASS_L1_CENTER_HZ,
            GNSS_GLONASS_L1_CENTER_HZ,
            9000000.0,
            QStringLiteral("1602.000000"),
            QStringLiteral("1598-1606"),
            3000,
            80,
            GnssAcquisitionKind::GlonassL1Of
        }
    };
}

GnssSystemPreset gnssSystemPreset(const QString &id) {
    const QVector<GnssSystemPreset> presets = gnssSystemPresets();
    for (const GnssSystemPreset &preset : presets) {
        if (preset.id == id) {
            return preset;
        }
    }
    for (const GnssSystemPreset &preset : presets) {
        if (preset.id == QStringLiteral("gps_l1_ca")) {
            return preset;
        }
    }
    return presets.constFirst();
}

QVector<QthOnlineProviderPreset> qthOnlineProviderPresets() {
    return {
        {
            QStringLiteral("osm"),
            QStringLiteral("qth_provider_osm"),
            QStringLiteral("OpenStreetMap"),
            QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
            QString::fromUtf8("\xC2\xA9 OpenStreetMap contributors"),
            false,
            false,
            19
        },
        {
            QStringLiteral("maptiler_satellite"),
            QStringLiteral("qth_provider_maptiler_satellite"),
            QStringLiteral("MapTiler Satellite"),
            QStringLiteral("https://api.maptiler.com/maps/satellite/256/{z}/{x}/{y}.jpg?key={key}"),
            QStringLiteral("MapTiler Satellite"),
            true,
            true,
            19
        },
        {
            QStringLiteral("maptiler_hybrid"),
            QStringLiteral("qth_provider_maptiler_hybrid"),
            QStringLiteral("MapTiler Hybrid"),
            QStringLiteral("https://api.maptiler.com/maps/hybrid/256/{z}/{x}/{y}.jpg?key={key}"),
            QStringLiteral("MapTiler Hybrid"),
            true,
            true,
            19
        },
        {
            QStringLiteral("mapbox_satellite"),
            QStringLiteral("qth_provider_mapbox_satellite"),
            QStringLiteral("Mapbox Satellite"),
            QStringLiteral("https://api.mapbox.com/v4/mapbox.satellite/{z}/{x}/{y}.jpg90?access_token={key}"),
            QStringLiteral("Mapbox Satellite"),
            true,
            true,
            19
        },
        {
            QStringLiteral("nasa_gibs_truecolor"),
            QStringLiteral("qth_provider_nasa_gibs_truecolor"),
            QStringLiteral("NASA GIBS True Color"),
            QStringLiteral("https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/MODIS_Terra_CorrectedReflectance_TrueColor/default/default/GoogleMapsCompatible_Level9/{z}/{row}/{col}.jpg"),
            QStringLiteral("NASA GIBS / ESDIS"),
            false,
            true,
            9
        },
        {
            QStringLiteral("custom"),
            QStringLiteral("qth_provider_custom"),
            QStringLiteral("Custom XYZ"),
            QString(),
            QString(),
            false,
            false,
            19
        }
    };
}

QthOnlineProviderPreset qthOnlineProviderPreset(const QString &id) {
    const QVector<QthOnlineProviderPreset> presets = qthOnlineProviderPresets();
    for (const QthOnlineProviderPreset &preset : presets) {
        if (preset.id == id) {
            return preset;
        }
    }
    return presets.constLast();
}

bool qthOnlineTemplateNeedsKey(const QString &urlTemplate) {
    return urlTemplate.contains(QStringLiteral("{key}"), Qt::CaseInsensitive) ||
           urlTemplate.contains(QStringLiteral("{token}"), Qt::CaseInsensitive);
}

QColor gnssHeatColor(double value, double minValue, double maxValue) {
    if (!std::isfinite(value)) {
        value = minValue;
    }
    const double span = (std::max)(0.001, maxValue - minValue);
    const double t = (std::clamp)((value - minValue) / span, 0.0, 1.0);
    if (t < 0.5) {
        const double u = t * 2.0;
        return QColor(static_cast<int>(30 + u * 35),
                      static_cast<int>(55 + u * 130),
                      static_cast<int>(110 + u * 70));
    }
    const double u = (t - 0.5) * 2.0;
    return QColor(static_cast<int>(65 + u * 190),
                  static_cast<int>(185 + u * 55),
                  static_cast<int>(180 - u * 155));
}
