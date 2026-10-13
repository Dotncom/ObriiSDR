#ifndef MAPTILER_TERRAIN_H
#define MAPTILER_TERRAIN_H

#include "radiopropagation.h"
#include <QHash>
#include <QImage>
#include <QString>

namespace propagation {

// Terrain RGB tiles are held only for the current calculation, not saved as a DEM.
class MapTilerTerrain {
public:
    explicit MapTilerTerrain(const Parameters &parameters, bool coverage);
    bool fetch(const QString &apiKey, const std::atomic_bool *cancel,
               const std::function<void(int, int)> &progress = {});
    void insertTile(int zoom, int x, int y, const QImage &image);
    double elevation(double latitude, double longitude) const;
    QString error() const { return failure; }
    int zoomLevel() const { return zoom; }
    int tileCount() const { return requests.size(); }
    int missingCount() const { return missing; }

private:
    struct TileId { int x, y; };
    static quint64 tileKey(int zoom, int x, int y);
    QVector<TileId> requests;
    QHash<quint64, QImage> images;
    int zoom = 0;
    int missing = 0;
    QString failure;
};

}
#endif
