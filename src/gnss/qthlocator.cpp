#include "qthlocator.h"

#include <algorithm>
#include <cmath>

namespace qth {

namespace {

double clampLatitudeForLocator(double latitude) {
    if (!std::isfinite(latitude)) {
        return 0.0;
    }
    return (std::clamp)(latitude, -90.0, 89.999999999);
}

double normalizeLongitude(double longitude) {
    if (!std::isfinite(longitude)) {
        return 0.0;
    }
    while (longitude < -180.0) {
        longitude += 360.0;
    }
    while (longitude >= 180.0) {
        longitude -= 360.0;
    }
    return longitude;
}

int pairDivisor(int pairIndex) {
    if (pairIndex == 0) {
        return 18;
    }
    return (pairIndex % 2) == 1 ? 10 : 24;
}

bool pairUsesDigits(int pairIndex) {
    return pairIndex > 0 && (pairIndex % 2) == 1;
}

QChar locatorChar(int value, int pairIndex) {
    if (pairUsesDigits(pairIndex)) {
        return QChar(QLatin1Char('0' + value));
    }
    const char base = pairIndex == 0 ? 'A' : 'a';
    return QChar(QLatin1Char(static_cast<char>(base + value)));
}

int locatorValue(QChar ch, int pairIndex) {
    if (pairUsesDigits(pairIndex)) {
        return ch.isDigit() ? ch.digitValue() : -1;
    }
    const QChar upper = ch.toUpper();
    if (upper < QLatin1Char('A') || upper > QLatin1Char('X')) {
        return -1;
    }
    return upper.toLatin1() - 'A';
}

} // namespace

bool isValidLatitude(double latitude) {
    return std::isfinite(latitude) && latitude >= -90.0 && latitude <= 90.0;
}

bool isValidLongitude(double longitude) {
    return std::isfinite(longitude) && longitude >= -180.0 && longitude <= 180.0;
}

QString maidenheadLocator(double latitude, double longitude, int length) {
    if (length < 2) {
        length = 2;
    }
    if (length % 2 != 0) {
        --length;
    }
    length = (std::clamp)(length, 2, 10);

    double lon = normalizeLongitude(longitude) + 180.0;
    double lat = clampLatitudeForLocator(latitude) + 90.0;
    double lonSpan = 360.0;
    double latSpan = 180.0;

    QString locator;
    locator.reserve(length);
    for (int pair = 0; pair < length / 2; ++pair) {
        const int divisor = pairDivisor(pair);
        lonSpan /= divisor;
        latSpan /= divisor;

        int lonIndex = static_cast<int>(std::floor(lon / lonSpan));
        int latIndex = static_cast<int>(std::floor(lat / latSpan));
        lonIndex = (std::clamp)(lonIndex, 0, divisor - 1);
        latIndex = (std::clamp)(latIndex, 0, divisor - 1);

        locator.append(locatorChar(lonIndex, pair));
        locator.append(locatorChar(latIndex, pair));

        lon -= lonIndex * lonSpan;
        lat -= latIndex * latSpan;
    }
    return locator;
}

GeoBounds maidenheadBounds(const QString &locator) {
    const QString trimmed = locator.trimmed();
    if (trimmed.size() < 2 || trimmed.size() % 2 != 0) {
        return {};
    }

    double minLon = -180.0;
    double minLat = -90.0;
    double lonSpan = 360.0;
    double latSpan = 180.0;

    for (int pair = 0; pair < trimmed.size() / 2; ++pair) {
        const int divisor = pairDivisor(pair);
        lonSpan /= divisor;
        latSpan /= divisor;

        const int lonValue = locatorValue(trimmed.at(pair * 2), pair);
        const int latValue = locatorValue(trimmed.at(pair * 2 + 1), pair);
        if (lonValue < 0 || latValue < 0 ||
            lonValue >= divisor || latValue >= divisor) {
            return {};
        }

        minLon += lonValue * lonSpan;
        minLat += latValue * latSpan;
    }

    GeoBounds bounds;
    bounds.minLongitude = minLon;
    bounds.maxLongitude = minLon + lonSpan;
    bounds.minLatitude = minLat;
    bounds.maxLatitude = minLat + latSpan;
    bounds.valid = true;
    return bounds;
}

} // namespace qth
