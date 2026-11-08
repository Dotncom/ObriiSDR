#ifndef QTHLOCATOR_H
#define QTHLOCATOR_H

#include <QString>

namespace qth {

struct GeoPosition {
    double latitude = 0.0;
    double longitude = 0.0;
};

struct GeoBounds {
    double minLatitude = 0.0;
    double maxLatitude = 0.0;
    double minLongitude = 0.0;
    double maxLongitude = 0.0;
    bool valid = false;
};

struct UserMarker {
    int number = 0;
    QString name;
    QString description;
    double latitude = 0.0;
    double longitude = 0.0;
};

bool isValidLatitude(double latitude);
bool isValidLongitude(double longitude);
QString maidenheadLocator(double latitude, double longitude, int length = 6);
GeoBounds maidenheadBounds(const QString &locator);

} // namespace qth

#endif // QTHLOCATOR_H
