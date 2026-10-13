#include "radiopropagation.h"
#include <QCache>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QTextStream>
#include <algorithm>
#include <array>
#include <cmath>
#include "itm.h"

namespace propagation {
namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr double EarthM = 6371000.0;
constexpr double LightMps = 299792458.0;
double radians(double degrees) { return degrees * Pi / 180.0; }
double degrees(double r) { return r * 180.0 / Pi; }
double wrap(double lon) { return std::remainder(lon, 360.0); }
bool cancelled(const std::atomic_bool *flag) { return flag && flag->load(); }
double bearing(const qth::GeoPosition &a, const qth::GeoPosition &b) {
    const double latA = radians(a.latitude), latB = radians(b.latitude);
    const double lon = radians(wrap(b.longitude - a.longitude));
    return std::atan2(std::sin(lon) * std::cos(latB),
                      std::cos(latA) * std::sin(latB) - std::sin(latA) * std::cos(latB) * std::cos(lon));
}
double mercatorY(double lat) { return std::asinh(std::tan(radians(lat))); }
double latitudeFromY(double y) { return degrees(std::atan(std::sinh(y))); }
}
struct SrtmTerrain::Impl {
    struct Tile {
        QFile file;
        uchar *data = nullptr;
        int size = 0;
        ~Tile() { if (data) file.unmap(data); }
        double value(int row, int col) const {
            const qint64 offset = (qint64(row) * size + col) * 2;
            const int raw = (int(data[offset]) << 8) | data[offset + 1];
            const int signedValue = raw >= 32768 ? raw - 65536 : raw;
            return signedValue == -32768 ? Unknown : double(signedValue);
        }
    };
    QString directory;
    QCache<QString, Tile> tiles{8};
    QSet<QString> missing;
    explicit Impl(const QString &path) : directory(path) {}
};
SrtmTerrain::SrtmTerrain(const QString &path) : impl(new Impl(path)) {}
SrtmTerrain::~SrtmTerrain() = default;
double SrtmTerrain::elevation(double lat, double lon) {
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat >= 90) return Unknown;
    lon = wrap(lon);
    if (lon == 180.0) lon = -180.0;
    const int south = int(std::floor(lat)), west = int(std::floor(lon));
    const QString name = QStringLiteral("%1%2%3%4.hgt")
        .arg(south < 0 ? 'S' : 'N').arg(std::abs(south), 2, 10, QLatin1Char('0'))
        .arg(west < 0 ? 'W' : 'E').arg(std::abs(west), 3, 10, QLatin1Char('0'));
    auto *tile = impl->tiles.object(name);
    if (!tile) {
        tile = new Impl::Tile;
        const QString stem=name.chopped(4);
        for (const QString &filename : {name,stem+".SRTMGL1.hgt",stem+".SRTMGL3.hgt"}) {
            for (const QString &candidate : {filename,filename.toUpper(),filename.toLower()}) {
                tile->file.setFileName(QDir(impl->directory).filePath(candidate));
                if (tile->file.open(QIODevice::ReadOnly)) break;
            }
            if (tile->file.isOpen()) break;
        }
        if (tile->file.isOpen()) {
            const qint64 bytes = tile->file.size();
            if (bytes == qint64(1201) * 1201 * 2) tile->size = 1201;
            if (bytes == qint64(3601) * 3601 * 2) tile->size = 3601;
            if (tile->size) tile->data = tile->file.map(0, bytes);
        }
        if (!tile->data) impl->missing.insert(name);
        impl->tiles.insert(name, tile);
    }
    if (!tile->data) return Unknown;
    const double x = (lon - west) * (tile->size - 1);
    const double y = (south + 1.0 - lat) * (tile->size - 1);
    const int left = std::clamp(int(std::floor(x)), 0, tile->size - 2);
    const int top = std::clamp(int(std::floor(y)), 0, tile->size - 2);
    const double fx = std::clamp(x - left, 0.0, 1.0), fy = std::clamp(y - top, 0.0, 1.0);
    const double weights[] = {(1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy};
    const double values[] = {tile->value(top,left), tile->value(top,left+1),
                             tile->value(top+1,left), tile->value(top+1,left+1)};
    double value = 0;
    for (int i = 0; i < 4; ++i) {
        if (weights[i] <= 1e-12) continue;
        if (!std::isfinite(values[i])) { impl->missing.insert(name); return Unknown; }
        value += weights[i] * values[i];
    }
    return value;
}
QStringList SrtmTerrain::unavailableTiles() const {
    QStringList names = impl->missing.values(); names.sort(); return names;
}
double distanceM(const qth::GeoPosition &a, const qth::GeoPosition &b) {
    const double dLat = radians(b.latitude-a.latitude), dLon = radians(wrap(b.longitude-a.longitude));
    const double h = std::pow(std::sin(dLat/2),2) + std::cos(radians(a.latitude)) *
                     std::cos(radians(b.latitude)) * std::pow(std::sin(dLon/2),2);
    return 2 * EarthM * std::asin(std::sqrt(std::clamp(h,0.0,1.0)));
}
qth::GeoPosition destination(const qth::GeoPosition &a, double az, double distance) {
    const double lat = radians(a.latitude), lon = radians(a.longitude), arc = distance / EarthM;
    const double bLat = std::asin(std::clamp(std::sin(lat)*std::cos(arc) +
        std::cos(lat)*std::sin(arc)*std::cos(az), -1.0, 1.0));
    const double bLon = lon + std::atan2(std::sin(az)*std::sin(arc)*std::cos(lat),
        std::cos(arc)-std::sin(lat)*std::sin(bLat));
    return {degrees(bLat), wrap(degrees(bLon))};
}
double freeSpaceLossDb(double distance, double frequency) {
    if (!(distance > 0) || !(frequency > 0)) return Unknown;
    return 20.0 * std::log10(4.0 * Pi * distance * frequency * 1e6 / LightMps);
}
double knifeEdgeLossDb(double v) {
    if (v <= -0.78) return 0;
    const double u = v - 0.1;
    return 6.9 + 20.0 * std::log10(std::sqrt(u*u+1.0)+u);
}
ClutterResult receiverClutter(const Parameters &p) {
    ClutterResult out;
    out.rxHeightM = out.referenceHeightM = p.rxHeightM;
    const auto invalid = [&out](const QString &error) {
        out.status = ClutterStatus::Invalid; out.lossDb = Unknown; out.error = error;
        return out;
    };
    switch (p.rxEnvironment) {
    case ClutterEnvironment::None: return out;
    case ClutterEnvironment::Rural:
    case ClutterEnvironment::Suburban: out.representativeHeightM = 10.0; break;
    case ClutterEnvironment::Urban: out.representativeHeightM = 15.0; break;
    case ClutterEnvironment::DenseUrban: out.representativeHeightM = 20.0; break;
    default: return invalid(QStringLiteral("Unknown RX clutter environment"));
    }
    if (!std::isfinite(p.rxHeightM) || p.rxHeightM < 0.5 || p.rxHeightM > 3000.0)
        return invalid(QStringLiteral("RX height outside application range 0.5..3000 m AGL"));
    if (p.rxClutterMode == ClutterMode::Manual) {
        if (!std::isfinite(p.rxClutterManualDb) || p.rxClutterManualDb < 0.0 || p.rxClutterManualDb > 200.0)
            return invalid(QStringLiteral("Manual RX residual clutter loss must be 0..200 dB (application limit)"));
        out.status = ClutterStatus::Manual; out.lossDb = p.rxClutterManualDb;
        return out;
    }
    if (p.rxClutterMode != ClutterMode::P2108)
        return invalid(QStringLiteral("Unknown RX clutter method"));
    if (!std::isfinite(p.frequencyMHz) || p.frequencyMHz < 30.0 || p.frequencyMHz > 3000.0)
        return invalid(QStringLiteral("ITU-R P.2108-1 section 3.1 requires 30..3000 MHz; select None or Manual residual loss"));
    if (!p.rxClutterExcludedFromTerrain)
        return invalid(QStringLiteral("RX clutter must be excluded from the terrain/base model; confirm this or select None / Manual residual loss"));
    if (p.rxHeightM >= out.representativeHeightM) {
        out.status = ClutterStatus::AboveClutter;
        return out;
    }
    out.status = ClutterStatus::Modelled;
    out.referenceHeightM = out.representativeHeightM;
    // ITU-R P.2108-1, Annex 1, 3.1, Table 3 and (2a)-(2g). f is GHz, theta is degrees.
    const double f = p.frequencyMHz / 1000.0;
    if (p.rxEnvironment == ClutterEnvironment::Rural) {
        out.lossDb = -(21.8 + 6.2 * std::log10(f)) * std::log10(p.rxHeightM / out.representativeHeightM);
    } else {
        const double difference = out.representativeHeightM - p.rxHeightM;
        const double theta = degrees(std::atan(difference / 27.0));
        const double v = 0.342 * std::sqrt(f) * std::sqrt(difference * theta);
        out.lossDb = knifeEdgeLossDb(v) - 6.03;
    }
    return out;
}
QString clutterModelLimits() {
    return QStringLiteral("ITU-R P.2108-1 section 3.1: 30..3000 MHz; median RX end correction to an above-clutter path. "
        "R = 10/10/15/20 m (rural/suburban/urban/dense urban), street width 27 m. "
        "Base path uses max(RX height, R); zero correction at/above R. "
        "RX clutter must be absent from terrain/base loss. No TX clutter, building entry, individual buildings, or HF noise model. "
        "Uniform RX environment for all coverage cells. Application RX height: 0.5..3000 m. "
        "Manual: 0..200 dB user-supplied residual only (application bound), actual RX height, no ITU validity claim.");
}
QString clutterSummary(const Parameters &p, const ClutterResult &c) {
    QString environment;
    switch (p.rxEnvironment) {
    case ClutterEnvironment::None: environment = QStringLiteral("None"); break;
    case ClutterEnvironment::Rural: environment = QStringLiteral("Rural/open"); break;
    case ClutterEnvironment::Suburban: environment = QStringLiteral("Suburban"); break;
    case ClutterEnvironment::Urban: environment = QStringLiteral("Urban"); break;
    case ClutterEnvironment::DenseUrban: environment = QStringLiteral("Dense urban"); break;
    default: environment = QStringLiteral("Invalid"); break;
    }
    QString method;
    switch (c.status) {
    case ClutterStatus::Disabled: method = QStringLiteral("disabled"); break;
    case ClutterStatus::Modelled: method = QStringLiteral("ITU-R P.2108-1 section 3.1 median"); break;
    case ClutterStatus::AboveClutter: method = QStringLiteral("ITU-R P.2108-1 section 3.1; RX at/above R"); break;
    case ClutterStatus::Manual: method = QStringLiteral("manual residual; not an ITU prediction"); break;
    case ClutterStatus::Invalid: return QStringLiteral("RX clutter: %1; INVALID: %2").arg(environment, c.error);
    }
    return QStringLiteral("RX clutter: %1; %2; %3 dB; RX actual/reference = %4/%5 m AGL")
        .arg(environment, method).arg(c.lossDb, 0, 'f', 2).arg(c.rxHeightM, 0, 'f', 1).arg(c.referenceHeightM, 0, 'f', 1);
}
QString profileCsv(const Parameters &p, const PathResult &path) {
    QString csv; QTextStream stream(&csv); stream.setRealNumberPrecision(12);
    stream << "# TX=" << p.tx.latitude << "," << p.tx.longitude << "; RX=" << p.rx.latitude << "," << p.rx.longitude << "; MHz=" << p.frequencyMHz << "\n";
    stream << "# " << clutterSummary(p, path.clutter) << "\n# " << clutterModelLimits() << "\n";
    stream << "# source=https://www.itu.int/rec/R-REC-P.2108-1-202109-I/en\n";
    stream << "# rx_environment=" << int(p.rxEnvironment) << "; rx_clutter_mode=" << int(p.rxClutterMode)
           << "; rx_clutter_excluded_from_terrain=" << p.rxClutterExcludedFromTerrain
           << "; manual_rx_clutter_db=" << p.rxClutterManualDb << "\n";
    stream << "# base_path_loss_db=" << path.basePathLossDb << "; rx_clutter_loss_db=" << path.clutter.lossDb
           << "; total_path_loss_db=" << path.pathLossDb << "; received_dbm=" << path.receivedDbm
           << "; margin_db=" << path.marginDb << "; valid=" << path.valid << "\n";
    if (!path.error.isEmpty()) stream << "# error=" << path.error << "\n";
    stream << "# los_m and Fresnel clearance use the RX reference height, not the actual RX tip when below clutter.\n";
    stream << "distance_m,terrain_m_msl,terrain_curvature_m,los_m,fresnel1_radius_m\n";
    for (const auto &v : path.points)
        stream << v.distanceM << "," << v.terrainM << "," << v.adjustedTerrainM << "," << v.rayM << "," << v.fresnelM << "\n";
    return csv;
}
QString validate(const Parameters &p) {
    const double finite[] = {p.tx.latitude,p.tx.longitude,p.rx.latitude,p.rx.longitude,p.frequencyMHz,
        p.powerW,p.txHeightM,p.rxHeightM,p.txGainDbi,p.rxGainDbi,p.txLossDb,p.rxLossDb,
        p.additionalLossDb,p.sensitivityDbm,p.fadeMarginDb,p.radiusKm,p.profileStepM,p.earthK,
        p.refractivity,p.permittivity,p.conductivity,p.confidencePercent,p.reliabilityPercent};
    for (double v : finite) if (!std::isfinite(v)) return QStringLiteral("Non-finite input");
    if (std::abs(p.tx.latitude)>80 || std::abs(p.rx.latitude)>80 ||
        std::abs(p.tx.longitude)>180 || std::abs(p.rx.longitude)>180)
        return QStringLiteral("Coordinates: latitude -80..80, longitude -180..180");
    if (p.frequencyMHz<0.1 || p.frequencyMHz>100000 || p.powerW<=0 ||
        p.txHeightM<0.5 || p.rxHeightM<0.5 || p.txHeightM>3000 || p.rxHeightM>3000 ||
        p.earthK<0.5 || p.earthK>3 || p.radiusKm<0.1 || p.radiusKm>200 ||
        p.profileStepM<10 || p.profileStepM>2000 || p.gridSize<16 || p.gridSize>512)
        return QStringLiteral("Input outside supported range");
    if (p.model == Model::Itm && (p.frequencyMHz<20 || p.frequencyMHz>20000 ||
        p.refractivity<250 || p.refractivity>400 || p.permittivity<=1 || p.conductivity<=0 ||
        p.confidencePercent<=0 || p.confidencePercent>=100 ||
        p.reliabilityPercent<=0 || p.reliabilityPercent>=100 ||
        p.climate<1 || p.climate>7 || p.polarization<0 || p.polarization>1))
        return QStringLiteral("ITM requires 20..20000 MHz and valid environment parameters");
    return receiverClutter(p).error;
}
PathResult calculatePath(const Parameters &p, const Elevation &terrain,
                         const std::atomic_bool *cancel, int maxPoints) {
    PathResult out;
    out.clutter = receiverClutter(p);
    out.error = validate(p);
    if (!out.error.isEmpty()) return out;
    out.distanceM = distanceM(p.tx,p.rx);
    if (out.distanceM < 1) { out.error=QStringLiteral("TX and RX must be different points"); return out; }
    const int count = std::clamp(int(std::ceil(out.distanceM/p.profileStepM))+1, 17,
                                 std::clamp(maxPoints,17,4096));
    const double step = out.distanceM / (count-1), az = bearing(p.tx,p.rx);
    out.points.resize(count);
    out.completeTerrain = true;
    std::vector<double> pfl(count+2);
    pfl[0] = count-1; pfl[1] = step;
    for (int i=0;i<count;++i) {
        if ((i&31)==0 && cancelled(cancel)) { out.cancelled=true; return out; }
        const auto geo = i==0 ? p.tx : (i==count-1 ? p.rx : destination(p.tx,az,i*step));
        auto &point = out.points[i]; point.distanceM=i*step;
        point.terrainM = p.flatTerrain ? 0.0 : terrain(geo.latitude,geo.longitude);
        pfl[i+2] = point.terrainM;
        if (!std::isfinite(point.terrainM)) out.completeTerrain=false;
    }
    out.freeSpaceDb = freeSpaceLossDb(out.distanceM,p.frequencyMHz);
    if (!out.completeTerrain && p.model != Model::FreeSpace) {
        out.error=QStringLiteral("Missing or void terrain data on this path"); return out;
    }
    double largestV = -1e100;
    if (out.completeTerrain) {
        const double a=out.points.front().terrainM+p.txHeightM;
        const double b=out.points.back().terrainM+out.clutter.referenceHeightM;
        const double wavelength=LightMps/(p.frequencyMHz*1e6);
        out.minimumClearanceM=1e100; out.minimumFresnelRatio=1e100;
        for (int i=0;i<count;++i) {
            auto &v=out.points[i]; const double d=v.distanceM, remaining=out.distanceM-d;
            v.adjustedTerrainM=v.terrainM + d*remaining/(2*EarthM*p.earthK);
            v.rayM=a+(b-a)*(d/out.distanceM);
            v.fresnelM=std::sqrt(wavelength*d*remaining/out.distanceM);
            if (i==0 || i==count-1) continue;
            const double clearance=v.rayM-v.adjustedTerrainM;
            out.minimumClearanceM=(std::min)(out.minimumClearanceM,clearance);
            out.minimumFresnelRatio=(std::min)(out.minimumFresnelRatio,clearance/v.fresnelM);
            largestV=(std::max)(largestV,-clearance*std::sqrt(2.0)/v.fresnelM);
        }
    }
    out.diffractionDb = out.completeTerrain ? knifeEdgeLossDb(largestV) : Unknown;
    out.pathLossDb = out.freeSpaceDb;
    if (p.model==Model::KnifeEdge || (p.model==Model::Itm && out.distanceM<1000)) {
        out.pathLossDb += out.diffractionDb;
        out.shortPathFallback = p.model==Model::Itm;
    } else if (p.model==Model::Itm) {
        out.itmError = ITM_P2P_CR(p.txHeightM,out.clutter.referenceHeightM,pfl.data(),p.climate,p.refractivity,
            p.frequencyMHz,p.polarization,p.permittivity,p.conductivity,12,
            p.confidencePercent,p.reliabilityPercent,&out.pathLossDb,&out.itmWarnings);
        if (out.itmError>1 || !std::isfinite(out.pathLossDb)) {
            out.error=QStringLiteral("ITM error %1").arg(out.itmError); return out;
        }
    }
    out.basePathLossDb = out.pathLossDb;
    out.pathLossDb += out.clutter.lossDb;
    out.receivedDbm=30+10*std::log10(p.powerW)+p.txGainDbi+p.rxGainDbi-
        p.txLossDb-p.rxLossDb-p.additionalLossDb-out.pathLossDb;
    out.marginDb=out.receivedDbm-p.sensitivityDbm-p.fadeMarginDb;
    out.valid=std::isfinite(out.receivedDbm); return out;
}
CoverageResult calculateCoverage(const Parameters &p, const Elevation &elevation,
                                 const std::atomic_bool *cancel,
                                 const std::function<void(int)> &progress) {
    CoverageResult out; out.parameters=p; out.error=validate(p);
    if (!out.error.isEmpty()) {
        out.path.clutter=receiverClutter(p);out.path.error=out.error;
        return out;
    }
    out.path=calculatePath(p,elevation,cancel);
    const double angular=p.radiusKm*1000/EarthM;
    const double dLat=degrees(angular);
    const double dLon=degrees(std::asin(std::sin(angular)/std::cos(radians(p.tx.latitude))));
    out.bounds={p.tx.latitude-dLat,p.tx.latitude+dLat,p.tx.longitude-dLon,p.tx.longitude+dLon,true};
    const double topY=mercatorY(out.bounds.maxLatitude), bottomY=mercatorY(out.bounds.minLatitude);
    out.cells.resize(p.gridSize*p.gridSize);
    Parameters ray=p;
    int lastProgress=-1;
    for (int y=0;y<p.gridSize;++y) {
        if (cancelled(cancel)) {out.cancelled=true;return out;}
        const double lat=latitudeFromY(topY+(bottomY-topY)*(y+0.5)/p.gridSize);
        for (int x=0;x<p.gridSize;++x) {
            ray.rx={lat,wrap(out.bounds.minLongitude+2*dLon*(x+0.5)/p.gridSize)};
            const double d=distanceM(p.tx,ray.rx);
            if (d>p.radiusKm*1000 || d<(std::max)(30.0,10*LightMps/(p.frequencyMHz*1e6))) continue;
            const auto path=calculatePath(ray,elevation,cancel,256);
            if (path.cancelled) {out.cancelled=true;return out;}
            auto &cell=out.cells[y*p.gridSize+x];
            if (!path.points.isEmpty()) cell.terrainM=path.points.back().terrainM;
            if (path.valid) {
                cell.receivedDbm=path.receivedDbm;cell.marginDb=path.marginDb;++out.validCells;
                if(path.itmWarnings) ++out.warningCells;
                if(path.shortPathFallback) ++out.shortPathCells;
            } else ++out.unknownCells;
        }
        const int percent=(y+1)*100/p.gridSize;
        if(progress&&percent!=lastProgress){lastProgress=percent;progress(percent);}
    }
    if (!out.validCells) out.error=QStringLiteral("No valid coverage cells; check terrain data and model parameters");
    return out;
}
CoverageResult calculateCoverage(const Parameters &p, const std::atomic_bool *cancel,
                                 const std::function<void(int)> &progress) {
    SrtmTerrain dem(p.terrainDirectory);
    auto out = calculateCoverage(p, [&](double lat,double lon){return dem.elevation(lat,lon);}, cancel, progress);
    out.missingTiles=dem.unavailableTiles();
    return out;
}
QColor levelColor(double value) {
    if(!std::isfinite(value))return QColor(Qt::transparent);
    static const QColor colors[]={QColor(66,27,110),QColor(40,68,193),QColor(19,169,222),
        QColor(27,194,115),QColor(229,223,50),QColor(249,132,36),QColor(225,39,50),QColor(255,236,220)};
    const double t=std::clamp(value,0.0,1.0)*7;
    const int a=(std::min)(6,int(t)); const double f=t-a;
    return QColor(int(colors[a].red()*(1-f)+colors[a+1].red()*f),
        int(colors[a].green()*(1-f)+colors[a+1].green()*f),
        int(colors[a].blue()*(1-f)+colors[a+1].blue()*f));
}
QImage coverageImage(const CoverageResult &r,int layer,double colorMinimum,double colorMaximum) {
    const int n=r.parameters.gridSize;
    if(n<=0||n>512||r.cells.size()!=n*n) return {};
    QImage image(n,n,QImage::Format_ARGB32); image.fill(Qt::transparent);
    double low=layer==1?-30.0:-140.0,high=layer==1?30.0:-40.0;
    if(std::isfinite(colorMinimum)&&std::isfinite(colorMaximum)&&colorMaximum>colorMinimum){
        low=colorMinimum;high=colorMaximum;
    }
    if(layer==2) {
        low=1e100; high=-1e100;
        for(const auto &c:r.cells) if(std::isfinite(c.terrainM)) {low=(std::min)(low,c.terrainM);high=(std::max)(high,c.terrainM);}
        if(high<=low) high=low+1;
    }
    static const auto palette=[](){
        std::array<QRgb,1024> values;
        for(std::size_t i=0;i<values.size();++i)values[i]=levelColor(double(i)/(values.size()-1)).rgba();
        return values;
    }();
    for(int y=0;y<n;++y) {
      auto *pixels=reinterpret_cast<QRgb *>(image.scanLine(y));
      for(int x=0;x<n;++x) {
        const auto &c=r.cells[y*n+x];
        const double v=layer==2?c.terrainM:(layer==1?c.marginDb:c.receivedDbm);
        if(!std::isfinite(v)) continue;
        const double t=std::clamp((v-low)/(high-low),0.0,1.0);
        pixels[x]=palette[std::size_t(t*(palette.size()-1)+0.5)];
      }
    }
    return image;
}
}
