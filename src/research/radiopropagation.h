#ifndef RADIOPROPAGATION_H
#define RADIOPROPAGATION_H

#include "qthlocator.h"
#include <QColor>
#include <QImage>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>
#include <limits>

namespace propagation {
constexpr double Unknown = std::numeric_limits<double>::quiet_NaN();
enum class Model { KnifeEdge, Itm, FreeSpace };
struct Parameters {
    qth::GeoPosition tx, rx;
    double frequencyMHz = 145.0;
    double powerW = 5.0;
    double txHeightM = 10.0, rxHeightM = 1.5;
    double txGainDbi = 0.0, rxGainDbi = 0.0;
    double txLossDb = 0.0, rxLossDb = 0.0, additionalLossDb = 0.0;
    double sensitivityDbm = -110.0, fadeMarginDb = 10.0;
    double radiusKm = 10.0, profileStepM = 90.0, earthK = 4.0 / 3.0;
    double refractivity = 301.0, permittivity = 15.0, conductivity = 0.005;
    double confidencePercent = 50.0, reliabilityPercent = 50.0;
    int climate = 5, polarization = 1, gridSize = 64;
    Model model = Model::KnifeEdge;
    bool flatTerrain = false;
    QString terrainDirectory;
};
struct ProfilePoint {
    double distanceM = 0.0, terrainM = Unknown, adjustedTerrainM = Unknown;
    double rayM = Unknown, fresnelM = Unknown;
};
struct PathResult {
    bool valid = false, completeTerrain = false, cancelled = false, shortPathFallback = false;
    double distanceM = 0.0, freeSpaceDb = Unknown, diffractionDb = Unknown;
    double pathLossDb = Unknown, receivedDbm = Unknown, marginDb = Unknown;
    double minimumClearanceM = Unknown, minimumFresnelRatio = Unknown;
    long itmWarnings = 0;
    int itmError = 0;
    QString error;
    QVector<ProfilePoint> points;
};
struct CoverageCell {
    double receivedDbm = Unknown, marginDb = Unknown, terrainM = Unknown;
};
struct CoverageResult {
    Parameters parameters;
    qth::GeoBounds bounds;
    QVector<CoverageCell> cells;
    PathResult path;
    QStringList missingTiles;
    QString error;
    int validCells = 0, unknownCells = 0, warningCells = 0, shortPathCells = 0;
    bool cancelled = false;
};
class SrtmTerrain {
public:
    explicit SrtmTerrain(const QString &directory);
    ~SrtmTerrain();
    double elevation(double latitude, double longitude);
    QStringList unavailableTiles() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
using Elevation = std::function<double(double, double)>;
double distanceM(const qth::GeoPosition &a, const qth::GeoPosition &b);
qth::GeoPosition destination(const qth::GeoPosition &a, double bearingRadians, double distanceM);
double freeSpaceLossDb(double distanceM, double frequencyMHz);
double knifeEdgeLossDb(double v);
QString validate(const Parameters &p);
PathResult calculatePath(const Parameters &p, const Elevation &terrain,
                         const std::atomic_bool *cancel = nullptr, int maxPoints = 4096);
CoverageResult calculateCoverage(const Parameters &p, const std::atomic_bool *cancel = nullptr,
                                 const std::function<void(int)> &progress = {});
CoverageResult calculateCoverage(const Parameters &p, const Elevation &terrain,
                                 const std::atomic_bool *cancel = nullptr,
                                 const std::function<void(int)> &progress = {});
QColor levelColor(double normalized);
QImage coverageImage(const CoverageResult &result, int layer,
                     double colorMinimum = Unknown, double colorMaximum = Unknown);
}
#endif
