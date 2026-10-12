#include "qthmapwidget.h"
#include "diagnosticlogging.h"

#include <QDir>
#include <QApplication>
#include <QDebug>
#include <QCryptographicHash>
#include <QFile>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPointer>
#include <QSizePolicy>
#include <QSslSocket>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMercatorMaxLatitude = 85.05112878;
constexpr int kTileSize = 256;
constexpr double kEarthRadiusKm = 6371.0;
constexpr double kSatelliteSkyOverlayHorizonKm = 1800.0;

double wrapLongitude(double longitude) {
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

int normalizedGridPrecision(int precision) {
    if (precision <= 2) {
        return 2;
    }
    if (precision <= 4) {
        return 4;
    }
    return 6;
}

void gridStepForPrecision(int precision, double *lonStep, double *latStep) {
    switch (normalizedGridPrecision(precision)) {
    case 2:
        *lonStep = 20.0;
        *latStep = 10.0;
        break;
    case 4:
        *lonStep = 2.0;
        *latStep = 1.0;
        break;
    case 6:
    default:
        *lonStep = 1.0 / 12.0;
        *latStep = 1.0 / 24.0;
        break;
    }
}

QString markerDisplayLabel(const qth::UserMarker &marker) {
    QString label = marker.name.trimmed();
    if (label.isEmpty()) {
        label = qth::maidenheadLocator(marker.latitude, marker.longitude, 6);
    }
    if (marker.number > 0) {
        label = QStringLiteral("%1 %2").arg(marker.number).arg(label);
    }
    return label;
}

bool approximateSatelliteSkyPoint(double observerLatitude,
                                  double observerLongitude,
                                  const QthMapWidget::SatelliteMarker &satellite,
                                  QPointF *geo) {
    if (!geo ||
        !std::isfinite(observerLatitude) ||
        !std::isfinite(observerLongitude) ||
        satellite.elevationDeg < 0 ||
        satellite.azimuthDeg < 0) {
        return false;
    }

    const double lat = observerLatitude * kPi / 180.0;
    const double lon = observerLongitude * kPi / 180.0;
    const double az = satellite.azimuthDeg * kPi / 180.0;
    const double elevationFactor =
        (std::clamp)(90.0 - static_cast<double>(satellite.elevationDeg), 0.0, 90.0) / 90.0;
    const double angularDistance = (kSatelliteSkyOverlayHorizonKm * elevationFactor) / kEarthRadiusKm;

    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double sinDistance = std::sin(angularDistance);
    const double cosDistance = std::cos(angularDistance);

    const double targetLat = std::asin(sinLat * cosDistance +
                                       cosLat * sinDistance * std::cos(az));
    const double targetLon =
        lon + std::atan2(std::sin(az) * sinDistance * cosLat,
                         cosDistance - sinLat * std::sin(targetLat));

    *geo = QPointF((std::clamp)(targetLat * 180.0 / kPi, -90.0, 90.0),
                   wrapLongitude(targetLon * 180.0 / kPi));
    return true;
}

#ifdef _WIN32
QString winHttpTileCachePath(const QString &url) {
    QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheRoot.isEmpty()) {
        cacheRoot = QDir::tempPath();
    }
    QDir cacheDir(QDir(cacheRoot).filePath(QStringLiteral("map_tiles_winhttp")));
    QDir().mkpath(cacheDir.absolutePath());
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    return cacheDir.filePath(QString::fromLatin1(hash) + QStringLiteral(".tile"));
}

QByteArray fetchTileWithWinHttp(const QUrl &url, QString *error) {
    auto setError = [error](const QString &message) {
        if (error) {
            *error = message;
        }
    };

    const QString host = url.host();
    QString resource = url.path(QUrl::FullyEncoded);
    if (resource.isEmpty()) {
        resource = QStringLiteral("/");
    }
    const QString query = url.query(QUrl::FullyEncoded);
    if (!query.isEmpty()) {
        resource += QStringLiteral("?") + query;
    }

    HINTERNET session = WinHttpOpen(L"ObriiSDR/4.8 (+https://github.com/Dotncom/FobosAPP)",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) {
        setError(QStringLiteral("WinHTTP session failed: %1").arg(GetLastError()));
        return {};
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 15000);

    const std::wstring hostWide = host.toStdWString();
    HINTERNET connection = WinHttpConnect(session,
                                         hostWide.c_str(),
                                         static_cast<INTERNET_PORT>(url.port(443)),
                                         0);
    if (!connection) {
        setError(QStringLiteral("WinHTTP connect failed: %1").arg(GetLastError()));
        WinHttpCloseHandle(session);
        return {};
    }

    const std::wstring resourceWide = resource.toStdWString();
    HINTERNET request = WinHttpOpenRequest(connection,
                                          L"GET",
                                          resourceWide.c_str(),
                                          nullptr,
                                          WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
    if (!request) {
        setError(QStringLiteral("WinHTTP request failed: %1").arg(GetLastError()));
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return {};
    }

    QByteArray result;
    if (!WinHttpSendRequest(request,
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        setError(QStringLiteral("WinHTTP receive failed: %1").arg(GetLastError()));
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return {};
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode,
                            &statusSize,
                            WINHTTP_NO_HEADER_INDEX) &&
        (statusCode < 200 || statusCode >= 300)) {
        setError(QStringLiteral("HTTP status %1").arg(statusCode));
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return {};
    }

    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        QByteArray chunk;
        chunk.resize(static_cast<int>(available));
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            setError(QStringLiteral("WinHTTP read failed: %1").arg(GetLastError()));
            result.clear();
            break;
        }
        if (read == 0) {
            break;
        }
        result.append(chunk.constData(), static_cast<int>(read));
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}
#endif

} // namespace

QthMapWidget::QthMapWidget(QWidget *parent)
    : QWidget(parent) {
    setMinimumSize(640, 360);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::OpenHandCursor);

    tileNetwork = new QNetworkAccessManager(this);
    auto *diskCache = new QNetworkDiskCache(tileNetwork);
    QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheRoot.isEmpty()) {
        cacheRoot = QDir::tempPath();
    }
    QDir().mkpath(QDir(cacheRoot).filePath(QStringLiteral("map_tiles")));
    diskCache->setCacheDirectory(QDir(cacheRoot).filePath(QStringLiteral("map_tiles")));
    diskCache->setMaximumCacheSize(256 * 1024 * 1024);
    tileNetwork->setCache(diskCache);
    if (!QSslSocket::supportsSsl()) {
        lastOnlineError = QStringLiteral("Qt SSL support is unavailable; HTTPS tiles need OpenSSL DLLs");
        qWarning() << "[QTH map] SSL unavailable"
                   << "build" << QSslSocket::sslLibraryBuildVersionString()
                   << "runtime" << QSslSocket::sslLibraryVersionString();
    } else {
        qDebug() << "[QTH map] SSL available"
                 << "runtime" << QSslSocket::sslLibraryVersionString();
    }

    setPosition(0.0, 0.0);
}

void QthMapWidget::setPosition(double nextLatitude, double nextLongitude) {
    latitude = (std::clamp)(nextLatitude, -90.0, 90.0);
    longitude = (std::clamp)(nextLongitude, -180.0, 180.0);
    locator = qth::maidenheadLocator(latitude, longitude, 6);
    if (!viewCenterInitialized) {
        centerOnPosition();
        return;
    }
    update();
}

void QthMapWidget::setTileLayerMode(TileLayerMode mode) {
    if (tileLayerMode == mode) {
        return;
    }
    tileLayerMode = mode;
    update();
}

void QthMapWidget::setTileDirectory(const QString &path) {
    const QString cleaned = QDir::fromNativeSeparators(path.trimmed());
    if (tileDirectoryPath == cleaned) {
        return;
    }
    tileDirectoryPath = cleaned;
    update();
}

void QthMapWidget::setOnlineTileTemplate(const QString &urlTemplate) {
    const QString cleaned = urlTemplate.trimmed();
    if (onlineTileUrlTemplate == cleaned) {
        return;
    }
    onlineTileUrlTemplate = cleaned;
    onlineTilePixmaps.clear();
    onlineTileErrors.clear();
    pendingTileRequests.clear();
    lastOnlineError.clear();
    update();
}

void QthMapWidget::setOnlineAttribution(const QString &text) {
    const QString cleaned = text.trimmed();
    if (onlineAttribution == cleaned) {
        return;
    }
    onlineAttribution = cleaned;
    update();
}

void QthMapWidget::setOnlineDiskCacheEnabled(bool enabled) {
    if (onlineDiskCacheEnabled == enabled) {
        return;
    }
    onlineDiskCacheEnabled = enabled;
    onlineTilePixmaps.clear();
    onlineTileErrors.clear();
    pendingTileRequests.clear();
    lastOnlineError.clear();
    update();
}

void QthMapWidget::setUiLanguage(const QString &language) {
    const QString cleaned = language.trimmed().toLower();
    if (uiLanguage == cleaned) {
        return;
    }
    uiLanguage = cleaned;
    update();
}

void QthMapWidget::setMapZoomRange(int minimumZoom, int maximumZoom) {
    minimumZoom = (std::clamp)(minimumZoom, 0, 19);
    maximumZoom = (std::clamp)(maximumZoom, minimumZoom, 19);
    if (minimumMapZoom == minimumZoom && maximumMapZoom == maximumZoom) {
        return;
    }
    minimumMapZoom = minimumZoom;
    maximumMapZoom = maximumZoom;
    setMapZoom(mapZoom);
}

void QthMapWidget::setMapZoom(int zoom) {
    zoom = (std::clamp)(zoom, minimumMapZoom, maximumMapZoom);
    if (mapZoom == zoom) {
        return;
    }
    mapZoom = zoom;
    update();
    emit mapZoomChanged(mapZoom);
}

void QthMapWidget::setGridPrecision(int precision) {
    precision = normalizedGridPrecision(precision);
    if (gridPrecision == precision) {
        return;
    }
    gridPrecision = precision;
    update();
}

void QthMapWidget::setOverlayMode(OverlayMode mode) {
    if (overlayMode == mode) {
        return;
    }
    overlayMode = mode;
    update();
}

void QthMapWidget::setSatelliteMarkers(const QVector<SatelliteMarker> &markers) {
    satelliteMarkers.clear();
    satelliteMarkers.reserve(markers.size());
    for (SatelliteMarker marker : markers) {
        if (marker.elevationDeg < 0 || marker.azimuthDeg < 0) {
            continue;
        }
        marker.elevationDeg = (std::clamp)(marker.elevationDeg, 0, 90);
        marker.azimuthDeg = (std::clamp)(marker.azimuthDeg, 0, 359);
        marker.label = marker.label.trimmed().left(16);
        if (marker.label.isEmpty()) {
            marker.label = QStringLiteral("SAT");
        }
        satelliteMarkers.append(marker);
    }
    update();
}

void QthMapWidget::setPositionVisible(bool visible) {
    if (positionVisible == visible) {
        return;
    }
    positionVisible = visible;
    update();
}

void QthMapWidget::setPositionStatusVisible(bool visible) {
    positionStatusVisible = visible;
    update();
}

void QthMapWidget::centerOnPosition() {
    viewCenterLatitude = (std::clamp)(latitude, -kMercatorMaxLatitude, kMercatorMaxLatitude);
    viewCenterLongitude = longitude;
    viewCenterInitialized = true;
    update();
}

void QthMapWidget::centerOn(double nextLatitude, double nextLongitude) {
    viewCenterLatitude = (std::clamp)(nextLatitude, -kMercatorMaxLatitude, kMercatorMaxLatitude);
    viewCenterLongitude = wrapLongitude(nextLongitude);
    viewCenterInitialized = true;
    update();
}

qth::GeoPosition QthMapWidget::mapCenter() const {
    return {viewCenterLatitude, viewCenterLongitude};
}

bool QthMapWidget::positionAt(const QPointF &point, qth::GeoPosition *position) const {
    const QRectF mapRect = rect().adjusted(18, 18, -18, -34);
    if (!position || !mapRect.contains(point)) return false;
    const QPointF geo = screenPointToGeo(point, mapRect, mapZoom);
    *position = {geo.x(), geo.y()};
    return true;
}

void QthMapWidget::setSearchMarker(double nextLatitude, double nextLongitude, const QString &label) {
    searchMarker.latitude = (std::clamp)(nextLatitude, -90.0, 90.0);
    searchMarker.longitude = (std::clamp)(nextLongitude, -180.0, 180.0);
    searchMarker.name = label.trimmed();
    if (searchMarker.name.isEmpty()) {
        searchMarker.name = qth::maidenheadLocator(searchMarker.latitude, searchMarker.longitude, 6);
    }
    hasSearchMarker = true;
    update();
}

void QthMapWidget::clearSearchMarker() {
    if (!hasSearchMarker) {
        return;
    }
    hasSearchMarker = false;
    searchMarker = {};
    update();
}

void QthMapWidget::setUserMarkers(const QVector<qth::UserMarker> &markers) {
    userMarkers.clear();
    userMarkers.reserve(markers.size());
    for (qth::UserMarker marker : markers) {
        if (!qth::isValidLatitude(marker.latitude) || !qth::isValidLongitude(marker.longitude)) {
            continue;
        }
        marker.latitude = (std::clamp)(marker.latitude, -90.0, 90.0);
        marker.longitude = (std::clamp)(marker.longitude, -180.0, 180.0);
        marker.name = marker.name.trimmed();
        marker.description = marker.description.trimmed();
        if (marker.name.isEmpty()) {
            marker.name = qth::maidenheadLocator(marker.latitude, marker.longitude, 6);
        }
        userMarkers.append(marker);
    }
    update();
}

QVector<qth::UserMarker> QthMapWidget::userMarkerList() const {
    return userMarkers;
}
void QthMapWidget::setUserMarkerDetailLabels(const QHash<int, QString> &labels) {
    if (userMarkerDetailLabels == labels) return;
    userMarkerDetailLabels = labels;
    update();
}

void QthMapWidget::clearUserMarkers() {
    if (userMarkers.isEmpty()) {
        return;
    }
    userMarkers.clear();
    emit userMarkersChanged();
    update();
}

bool QthMapWidget::hasLocalTileDirectory() const {
    return !tileDirectoryPath.isEmpty() && QDir(tileDirectoryPath).exists();
}

QPointF QthMapWidget::geoToMercatorPixel(double lat, double lon, int zoom) const {
    lat = (std::clamp)(lat, -kMercatorMaxLatitude, kMercatorMaxLatitude);
    lon = wrapLongitude(lon);

    const double worldSize = static_cast<double>(kTileSize) * static_cast<double>(1 << zoom);
    const double sinLat = std::sin(lat * kPi / 180.0);
    const double x = (lon + 180.0) / 360.0 * worldSize;
    const double y = (0.5 - std::log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * kPi)) * worldSize;
    return QPointF(x, y);
}

QPointF QthMapWidget::mercatorPixelToGeo(double x, double y, int zoom) const {
    const double worldSize = static_cast<double>(kTileSize) * static_cast<double>(1 << zoom);
    while (x < 0.0) {
        x += worldSize;
    }
    while (x >= worldSize) {
        x -= worldSize;
    }
    y = (std::clamp)(y, 0.0, worldSize);

    const double lon = x / worldSize * 360.0 - 180.0;
    const double n = kPi - 2.0 * kPi * y / worldSize;
    const double lat = 180.0 / kPi * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
    return QPointF(lat, lon);
}

QPointF QthMapWidget::geoToPoint(double lat, double lon, const QRectF &mapRect) const {
    const bool mercatorView = mapZoom > 0 ||
                              tileLayerMode == TileLayerMode::OnlineXyz ||
                              (tileLayerMode == TileLayerMode::LocalXyz && hasLocalTileDirectory());
    if (!mercatorView) {
        const double x = mapRect.left() + ((wrapLongitude(lon) + 180.0) / 360.0) * mapRect.width();
        const double y = mapRect.top() + ((90.0 - (std::clamp)(lat, -90.0, 90.0)) / 180.0) * mapRect.height();
        return QPointF(x, y);
    }

    const QPointF center = viewCenterMercatorPixel(mapZoom);
    const QPointF target = geoToMercatorPixel(lat, lon, mapZoom);
    const double worldSize = static_cast<double>(kTileSize) * static_cast<double>(1 << mapZoom);
    double dx = target.x() - center.x();
    if (dx > worldSize * 0.5) {
        dx -= worldSize;
    } else if (dx < -worldSize * 0.5) {
        dx += worldSize;
    }
    return QPointF(mapRect.center().x() + dx,
                   mapRect.center().y() + (target.y() - center.y()));
}

QPointF QthMapWidget::viewCenterMercatorPixel(int zoom) const {
    const double centerLat = viewCenterInitialized ? viewCenterLatitude : latitude;
    const double centerLon = viewCenterInitialized ? viewCenterLongitude : longitude;
    return geoToMercatorPixel(centerLat, centerLon, zoom);
}

QPointF QthMapWidget::screenPointToGeo(const QPointF &point, const QRectF &mapRect, int zoom) const {
    const QPointF center = viewCenterMercatorPixel(zoom);
    const QPointF offset = point - mapRect.center();
    return mercatorPixelToGeo(center.x() + offset.x(),
                              center.y() + offset.y(),
                              zoom);
}

QString QthMapWidget::tilePath(int zoom, int x, int y) const {
    const QDir root(tileDirectoryPath);
    const QString base = QStringLiteral("%1/%2/%3").arg(zoom).arg(x).arg(y);
    const QStringList extensions = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("webp")
    };
    for (const QString &extension : extensions) {
        const QString candidate = root.filePath(QStringLiteral("%1.%2").arg(base, extension));
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

QString QthMapWidget::onlineTileUrl(int zoom, int x, int y) const {
    QString url = onlineTileUrlTemplate.trimmed();
    if (url.isEmpty()) {
        return {};
    }
    url.replace(QStringLiteral("{zoom}"), QString::number(zoom));
    url.replace(QStringLiteral("{level}"), QString::number(zoom));
    url.replace(QStringLiteral("{col}"), QString::number(x));
    url.replace(QStringLiteral("{row}"), QString::number(y));
    url.replace(QStringLiteral("{z}"), QString::number(zoom));
    url.replace(QStringLiteral("{x}"), QString::number(x));
    url.replace(QStringLiteral("{y}"), QString::number(y));
    return url;
}

bool QthMapWidget::drawLocalTiles(QPainter &painter, const QRectF &mapRect) {
    lastTilesDrawn = 0;
    lastTilesMissing = 0;
    lastTilesPending = 0;
    if (tileLayerMode != TileLayerMode::LocalXyz || !hasLocalTileDirectory()) {
        return false;
    }

    painter.save();
    painter.setClipRect(mapRect);

    const QPointF center = viewCenterMercatorPixel(mapZoom);
    const QPointF topLeft(center.x() - mapRect.width() * 0.5,
                          center.y() - mapRect.height() * 0.5);
    const int tileCount = 1 << mapZoom;
    const int minTileX = static_cast<int>(std::floor(topLeft.x() / kTileSize));
    const int maxTileX = static_cast<int>(std::floor((topLeft.x() + mapRect.width()) / kTileSize));
    const int minTileY = static_cast<int>(std::floor(topLeft.y() / kTileSize));
    const int maxTileY = static_cast<int>(std::floor((topLeft.y() + mapRect.height()) / kTileSize));

    for (int tileY = minTileY; tileY <= maxTileY; ++tileY) {
        if (tileY < 0 || tileY >= tileCount) {
            continue;
        }
        for (int tileX = minTileX; tileX <= maxTileX; ++tileX) {
            const int wrappedX = ((tileX % tileCount) + tileCount) % tileCount;
            const QString path = tilePath(mapZoom, wrappedX, tileY);
            const QRectF dest(mapRect.left() + tileX * kTileSize - topLeft.x(),
                              mapRect.top() + tileY * kTileSize - topLeft.y(),
                              kTileSize,
                              kTileSize);
            if (path.isEmpty()) {
                ++lastTilesMissing;
                painter.fillRect(dest, QColor(35, 42, 50));
                painter.setPen(QPen(QColor(56, 66, 78), 1));
                painter.drawRect(dest);
                continue;
            }
            QPixmap pixmap(path);
            if (pixmap.isNull()) {
                ++lastTilesMissing;
                continue;
            }
            ++lastTilesDrawn;
            painter.drawPixmap(dest, pixmap, QRectF(QPointF(0, 0), pixmap.size()));
        }
    }

    painter.restore();
    return lastTilesDrawn > 0;
}

bool QthMapWidget::drawOnlineTiles(QPainter &painter, const QRectF &mapRect) {
    lastTilesDrawn = 0;
    lastTilesMissing = 0;
    lastTilesPending = 0;
    lastTilesFailed = 0;
    if (tileLayerMode != TileLayerMode::OnlineXyz || !tileNetwork || onlineTileUrlTemplate.trimmed().isEmpty()) {
        return false;
    }
#ifndef _WIN32
    if (onlineDiskCacheEnabled &&
        onlineTileUrlTemplate.trimmed().startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) &&
        !QSslSocket::supportsSsl()) {
        lastOnlineError = QStringLiteral("Qt SSL support is unavailable; HTTPS tiles need OpenSSL DLLs");
        return false;
    }
#endif

    painter.save();
    painter.setClipRect(mapRect);

    const QPointF center = viewCenterMercatorPixel(mapZoom);
    const QPointF topLeft(center.x() - mapRect.width() * 0.5,
                          center.y() - mapRect.height() * 0.5);
    const int tileCount = 1 << mapZoom;
    const int minTileX = static_cast<int>(std::floor(topLeft.x() / kTileSize));
    const int maxTileX = static_cast<int>(std::floor((topLeft.x() + mapRect.width()) / kTileSize));
    const int minTileY = static_cast<int>(std::floor(topLeft.y() / kTileSize));
    const int maxTileY = static_cast<int>(std::floor((topLeft.y() + mapRect.height()) / kTileSize));

    for (int tileY = minTileY; tileY <= maxTileY; ++tileY) {
        if (tileY < 0 || tileY >= tileCount) {
            continue;
        }
        for (int tileX = minTileX; tileX <= maxTileX; ++tileX) {
            const int wrappedX = ((tileX % tileCount) + tileCount) % tileCount;
            const QString url = onlineTileUrl(mapZoom, wrappedX, tileY);
            const QRectF dest(mapRect.left() + tileX * kTileSize - topLeft.x(),
                              mapRect.top() + tileY * kTileSize - topLeft.y(),
                              kTileSize,
                              kTileSize);
            if (url.isEmpty()) {
                ++lastTilesMissing;
                continue;
            }
            const auto pixmapIt = onlineTilePixmaps.constFind(url);
            if (pixmapIt != onlineTilePixmaps.constEnd() && !pixmapIt.value().isNull()) {
                ++lastTilesDrawn;
                painter.drawPixmap(dest, pixmapIt.value(), QRectF(QPointF(0, 0), pixmapIt.value().size()));
                continue;
            }
            const auto errorIt = onlineTileErrors.constFind(url);
            if (errorIt != onlineTileErrors.constEnd()) {
                ++lastTilesFailed;
                painter.fillRect(dest, QColor(52, 32, 36));
                painter.setPen(QPen(QColor(118, 62, 72), 1));
                painter.drawRect(dest);
                continue;
            }

            ++lastTilesPending;
            requestOnlineTile(url);
            painter.fillRect(dest, QColor(35, 42, 50));
            painter.setPen(QPen(QColor(56, 66, 78), 1));
            painter.drawRect(dest);
        }
    }

    painter.restore();
    return lastTilesDrawn > 0;
}

void QthMapWidget::requestOnlineTile(const QString &url) {
    if (!tileNetwork || url.isEmpty() || pendingTileRequests.contains(url)) {
        return;
    }
    const QUrl parsedUrl(url);
    if (!parsedUrl.isValid() || parsedUrl.scheme() != QStringLiteral("https")) {
        return;
    }

    pendingTileRequests.insert(url);
#ifdef _WIN32
    if (!QSslSocket::supportsSsl()) {
        const QString cachePath = onlineDiskCacheEnabled ? winHttpTileCachePath(url) : QString();
        if (!cachePath.isEmpty()) {
            QFile cacheFile(cachePath);
            if (cacheFile.exists() && cacheFile.open(QIODevice::ReadOnly)) {
                const QByteArray cachedData = cacheFile.readAll();
                if (!cachedData.isEmpty()) {
                    finishOnlineTileRequest(url, cachedData, QString());
                    return;
                }
            }
        }

        QPointer<QthMapWidget> guard(this);
        QThread *thread = QThread::create([guard, parsedUrl, url, cachePath]() {
            QString error;
            const QByteArray data = fetchTileWithWinHttp(parsedUrl, &error);
            if (error.isEmpty() && !cachePath.isEmpty() && !data.isEmpty()) {
                QFile cacheFile(cachePath);
                if (cacheFile.open(QIODevice::WriteOnly)) {
                    cacheFile.write(data);
                }
            }
            QthMapWidget *receiver = guard.data();
            if (!receiver) {
                return;
            }
            QMetaObject::invokeMethod(receiver,
                                      [guard, url, data, error]() {
                                          if (guard) {
                                              guard->finishOnlineTileRequest(url, data, error);
                                          }
                                      },
                                      Qt::QueuedConnection);
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
        return;
    }
#endif

    QNetworkRequest request(parsedUrl);
    request.setRawHeader("User-Agent",
                         QByteArrayLiteral("ObriiSDR/4.8 (+https://github.com/Dotncom/FobosAPP)"));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         onlineDiskCacheEnabled
                             ? QNetworkRequest::PreferCache
                             : QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, onlineDiskCacheEnabled);
    QNetworkReply *reply = tileNetwork->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
        const QString error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        const QByteArray data = error.isEmpty() ? reply->readAll() : QByteArray();
        finishOnlineTileRequest(url, data, error);
        reply->deleteLater();
    });
}

void QthMapWidget::finishOnlineTileRequest(const QString &url, const QByteArray &data, const QString &error) {
    pendingTileRequests.remove(url);
    if (error.isEmpty()) {
        QPixmap pixmap;
        if (pixmap.loadFromData(data)) {
            if (onlineTilePixmaps.size() > 768) {
                onlineTilePixmaps.clear();
            }
            onlineTilePixmaps.insert(url, pixmap);
            onlineTileErrors.remove(url);
            lastOnlineError.clear();
        } else {
            lastOnlineError = QStringLiteral("invalid image response");
            onlineTileErrors.insert(url, lastOnlineError);
            if (fobosVerboseLoggingEnabled()) {
                qDebug() << "[QTH map] online tile invalid image";
            }
        }
    } else {
        // Network error text may contain the complete URL, including an API key.
        lastOnlineError = QStringLiteral("tile request failed");
        onlineTileErrors.insert(url, lastOnlineError);
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[QTH map] online tile request failed";
        }
    }
    update();
}

qth::GeoBounds QthMapWidget::visibleGeoBounds(const QRectF &mapRect) const {
    qth::GeoBounds bounds;
    bounds.valid = true;

    const bool mercatorView = mapZoom > 0 ||
                              tileLayerMode == TileLayerMode::OnlineXyz ||
                              (tileLayerMode == TileLayerMode::LocalXyz && hasLocalTileDirectory());
    if (!mercatorView) {
        bounds.minLatitude = -90.0;
        bounds.maxLatitude = 90.0;
        bounds.minLongitude = -180.0;
        bounds.maxLongitude = 180.0;
        return bounds;
    }

    const QPointF center = viewCenterMercatorPixel(mapZoom);
    const QPointF topLeftGeo = mercatorPixelToGeo(center.x() - mapRect.width() * 0.5,
                                                  center.y() - mapRect.height() * 0.5,
                                                  mapZoom);
    const QPointF bottomRightGeo = mercatorPixelToGeo(center.x() + mapRect.width() * 0.5,
                                                      center.y() + mapRect.height() * 0.5,
                                                      mapZoom);
    bounds.maxLatitude = (std::clamp)(topLeftGeo.x(), -90.0, 90.0);
    bounds.minLatitude = (std::clamp)(bottomRightGeo.x(), -90.0, 90.0);
    bounds.minLongitude = topLeftGeo.y();
    bounds.maxLongitude = bottomRightGeo.y();
    if (bounds.maxLongitude < bounds.minLongitude) {
        bounds.minLongitude = -180.0;
        bounds.maxLongitude = 180.0;
    }
    return bounds;
}

void QthMapWidget::drawMaidenheadGrid(QPainter &painter, const QRectF &mapRect) {
    qth::GeoBounds bounds = visibleGeoBounds(mapRect);
    if (!bounds.valid) {
        return;
    }

    const double lonSpan = bounds.maxLongitude - bounds.minLongitude;
    const double latSpan = bounds.maxLatitude - bounds.minLatitude;
    if (!std::isfinite(lonSpan) || !std::isfinite(latSpan) || lonSpan <= 0.0 || latSpan <= 0.0) {
        return;
    }

    drawMaidenheadLayer(painter, mapRect, bounds, 2, true);
    if (normalizedGridPrecision(gridPrecision) >= 4) {
        drawMaidenheadLayer(painter, mapRect, bounds, 4, true);
    }
    if (normalizedGridPrecision(gridPrecision) >= 6) {
        drawMaidenheadLayer(painter, mapRect, bounds, 6, true);
    }
}

void QthMapWidget::drawMaidenheadLayer(QPainter &painter,
                                       const QRectF &mapRect,
                                       const qth::GeoBounds &bounds,
                                       int precision,
                                       bool drawLabels) {
    precision = normalizedGridPrecision(precision);
    double lonStep = 20.0;
    double latStep = 10.0;
    gridStepForPrecision(precision, &lonStep, &latStep);

    const double lonSpan = bounds.maxLongitude - bounds.minLongitude;
    const double latSpan = bounds.maxLatitude - bounds.minLatitude;
    if (!std::isfinite(lonSpan) || !std::isfinite(latSpan) || lonSpan <= 0.0 || latSpan <= 0.0) {
        return;
    }

    const double sampleLat = (bounds.minLatitude + bounds.maxLatitude) * 0.5;
    const double sampleLon = (bounds.minLongitude + bounds.maxLongitude) * 0.5;
    const QPointF cellA = geoToPoint(sampleLat, sampleLon, mapRect);
    const QPointF cellB = geoToPoint((std::min)(bounds.maxLatitude, sampleLat + latStep),
                                     (std::min)(bounds.maxLongitude, sampleLon + lonStep),
                                     mapRect);
    const double cellWidthPx = std::abs(cellB.x() - cellA.x());
    const double cellHeightPx = std::abs(cellB.y() - cellA.y());

    const double minLineWidthPx = precision == 2 ? 6.0 : (precision == 4 ? 18.0 : 30.0);
    const double minLineHeightPx = precision == 2 ? 6.0 : (precision == 4 ? 14.0 : 24.0);
    const double layerLineEstimate = (lonSpan / lonStep) + (latSpan / latStep);
    if (cellWidthPx < minLineWidthPx ||
        cellHeightPx < minLineHeightPx ||
        layerLineEstimate > (precision == 6 ? 900.0 : 1400.0)) {
        return;
    }

    QColor gridColor;
    qreal lineWidth = 1.0;
    Qt::PenStyle penStyle = Qt::SolidLine;
    if (precision == 2) {
        gridColor = QColor(235, 240, 246, 190);
        lineWidth = 2.2;
    } else if (precision == 4) {
        gridColor = QColor(128, 204, 255, 160);
        lineWidth = 1.4;
    } else {
        gridColor = QColor(255, 199, 95, 95);
        lineWidth = 0.8;
        penStyle = Qt::DotLine;
    }
    painter.setPen(QPen(gridColor, lineWidth, penStyle));

    const double lonStart = std::floor(bounds.minLongitude / lonStep) * lonStep;
    for (double lon = lonStart; lon <= bounds.maxLongitude + lonStep * 0.5; lon += lonStep) {
        if (lon < -180.0 || lon > 180.0) {
            continue;
        }
        painter.drawLine(geoToPoint(bounds.minLatitude, lon, mapRect),
                         geoToPoint(bounds.maxLatitude, lon, mapRect));
    }

    const double latStart = std::floor(bounds.minLatitude / latStep) * latStep;
    for (double lat = latStart; lat <= bounds.maxLatitude + latStep * 0.5; lat += latStep) {
        if (lat < -90.0 || lat > 90.0) {
            continue;
        }
        painter.drawLine(geoToPoint(lat, bounds.minLongitude, mapRect),
                         geoToPoint(lat, bounds.maxLongitude, mapRect));
    }

    if (!drawLabels) {
        return;
    }

    const double minLabelWidthPx = precision == 2 ? 48.0 : (precision == 4 ? 58.0 : 74.0);
    const double minLabelHeightPx = precision == 2 ? 20.0 : (precision == 4 ? 22.0 : 26.0);
    if (cellWidthPx < minLabelWidthPx || cellHeightPx < minLabelHeightPx) {
        return;
    }

    QFont labelFont = painter.font();
    if (precision == 2) {
        labelFont.setBold(true);
        labelFont.setPointSize((std::max)(10, labelFont.pointSize() + 2));
    } else if (precision == 4) {
        labelFont.setBold(true);
        labelFont.setPointSize((std::max)(9, labelFont.pointSize()));
    } else {
        labelFont.setPointSize((std::max)(8, labelFont.pointSize() - 1));
    }
    painter.setFont(labelFont);
    painter.setPen(precision == 2
                       ? QColor(255, 255, 255, 210)
                       : (precision == 4 ? QColor(210, 235, 255, 205)
                                         : QColor(255, 221, 150, 175)));

    int labelsDrawn = 0;
    const int labelLimit = precision == 2 ? 180 : (precision == 4 ? 260 : 220);
    const double labelLonStart = std::floor(bounds.minLongitude / lonStep) * lonStep;
    const double labelLatStart = std::floor(bounds.minLatitude / latStep) * latStep;
    for (double lon = labelLonStart; lon < bounds.maxLongitude && labelsDrawn < labelLimit; lon += lonStep) {
        for (double lat = labelLatStart; lat < bounds.maxLatitude && labelsDrawn < labelLimit; lat += latStep) {
            const double cellLon = lon + lonStep * 0.5;
            const double cellLat = lat + latStep * 0.5;
            if (cellLon < -180.0 || cellLon > 180.0 || cellLat < -90.0 || cellLat > 90.0) {
                continue;
            }
            const QPointF a = geoToPoint(lat, lon, mapRect);
            const QPointF b = geoToPoint(lat + latStep, lon + lonStep, mapRect);
            const double cellWidth = std::abs(b.x() - a.x());
            const double cellHeight = std::abs(b.y() - a.y());
            if (cellWidth < minLabelWidthPx || cellHeight < minLabelHeightPx) {
                continue;
            }
            const QPointF center = geoToPoint(cellLat, cellLon, mapRect);
            const QString text = qth::maidenheadLocator(cellLat, cellLon, precision);
            const double labelWidth = precision == 2 ? 72.0 : (precision == 4 ? 82.0 : 94.0);
            const double labelHeight = precision == 2 ? 24.0 : 20.0;
            QRectF labelRect(center.x() - labelWidth * 0.5,
                             center.y() - labelHeight * 0.5,
                             labelWidth,
                             labelHeight);
            if (precision <= 4) {
                painter.fillRect(labelRect.adjusted(-3, -1, 3, 1), QColor(21, 26, 32, 115));
            }
            painter.drawText(labelRect,
                             Qt::AlignCenter,
                             text);
            ++labelsDrawn;
        }
    }
}

void QthMapWidget::drawCurrentLocator(QPainter &painter, const QRectF &mapRect) {
    const qth::GeoBounds bounds = qth::maidenheadBounds(qth::maidenheadLocator(latitude, longitude, gridPrecision));
    if (!bounds.valid) {
        return;
    }

    const QPointF topLeft = geoToPoint(bounds.maxLatitude, bounds.minLongitude, mapRect);
    const QPointF bottomRight = geoToPoint(bounds.minLatitude, bounds.maxLongitude, mapRect);
    const QRectF qthRect(topLeft, bottomRight);
    painter.fillRect(qthRect, QColor(255, 199, 95, 42));
    painter.setPen(QPen(QColor(255, 199, 95), 2));
    painter.drawRect(qthRect);
}

void QthMapWidget::drawMapMarker(QPainter &painter,
                                 const QRectF &mapRect,
                                 double markerLatitude,
                                 double markerLongitude,
                                 const QColor &color,
                                 const QString &label,
                                 bool drawLabel,
                                 bool labelOnLeft) {
    const QPointF marker = geoToPoint(markerLatitude, markerLongitude, mapRect);
    if (!mapRect.adjusted(-32, -32, 32, 32).contains(marker)) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(21, 26, 32, 210), 5));
    painter.drawLine(QPointF(marker.x() - 12, marker.y()), QPointF(marker.x() + 12, marker.y()));
    painter.drawLine(QPointF(marker.x(), marker.y() - 12), QPointF(marker.x(), marker.y() + 12));
    painter.setPen(QPen(color, 2));
    painter.drawLine(QPointF(marker.x() - 12, marker.y()), QPointF(marker.x() + 12, marker.y()));
    painter.drawLine(QPointF(marker.x(), marker.y() - 12), QPointF(marker.x(), marker.y() + 12));
    painter.setBrush(color);
    painter.setPen(QPen(QColor(21, 26, 32, 220), 1));
    painter.drawEllipse(marker, 4.0, 4.0);

    if (drawLabel && !label.trimmed().isEmpty()) {
        QFont labelFont = painter.font();
        labelFont.setBold(true);
        labelFont.setPointSize((std::max)(8, labelFont.pointSize() - 1));
        painter.setFont(labelFont);
        const QFontMetrics metrics(labelFont);
        const QString text = label.trimmed().left(160);
        const QRect textBounds = metrics.boundingRect(QRect(0, 0, 600, 200), Qt::AlignLeft, text);
        QRectF labelRect(marker.x() + 10.0,
                         marker.y() - textBounds.height() - 8.0,
                         textBounds.width() + 14.0,
                         textBounds.height() + 6.0);
        if (labelOnLeft) labelRect.moveRight(marker.x() - 10.0);
        if (labelRect.right() > mapRect.right() - 4.0) {
            labelRect.moveRight(marker.x() - 10.0);
        }
        if (labelRect.left() < mapRect.left() + 4.0) {
            labelRect.moveLeft(mapRect.left() + 4.0);
        }
        if (labelRect.top() < mapRect.top() + 4.0) {
            labelRect.moveTop(marker.y() + 10.0);
        }
        painter.fillRect(labelRect, QColor(21, 26, 32, 190));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, 1));
        painter.drawRect(labelRect);
        painter.setPen(QColor(245, 248, 252));
        painter.drawText(labelRect, Qt::AlignCenter, text);
    }
    painter.restore();
}

int QthMapWidget::userMarkerAtPosition(const QPointF &point, const QRectF &mapRect) const {
    constexpr double kHitRadiusPx = 24.0;
    constexpr double kHitRadiusSquared = kHitRadiusPx * kHitRadiusPx;
    int hitIndex = -1;
    double bestDistanceSquared = kHitRadiusSquared;
    for (int i = 0; i < userMarkers.size(); ++i) {
        const qth::UserMarker &marker = userMarkers.at(i);
        const QPointF markerPoint = geoToPoint(marker.latitude, marker.longitude, mapRect);
        const double dx = markerPoint.x() - point.x();
        const double dy = markerPoint.y() - point.y();
        const double distanceSquared = dx * dx + dy * dy;
        if (distanceSquared <= bestDistanceSquared) {
            bestDistanceSquared = distanceSquared;
            hitIndex = i;
        }
    }
    return hitIndex;
}

bool QthMapWidget::searchMarkerAtPosition(const QPointF &point, const QRectF &mapRect) const {
    if (!hasSearchMarker) {
        return false;
    }
    constexpr double kHitRadiusPx = 24.0;
    const QPointF markerPoint = geoToPoint(searchMarker.latitude, searchMarker.longitude, mapRect);
    const double dx = markerPoint.x() - point.x();
    const double dy = markerPoint.y() - point.y();
    return (dx * dx + dy * dy) <= kHitRadiusPx * kHitRadiusPx;
}

bool QthMapWidget::currentPositionAtPosition(const QPointF &point, const QRectF &mapRect) const {
    if (!positionVisible) {
        return false;
    }
    constexpr double kHitRadiusPx = 24.0;
    const QPointF markerPoint = geoToPoint(latitude, longitude, mapRect);
    const double dx = markerPoint.x() - point.x();
    const double dy = markerPoint.y() - point.y();
    return (dx * dx + dy * dy) <= kHitRadiusPx * kHitRadiusPx;
}

void QthMapWidget::drawUserMarkers(QPainter &painter, const QRectF &mapRect) {
    for (int i = 0; i < userMarkers.size(); ++i) {
        const qth::UserMarker &marker = userMarkers.at(i);
        QString label = markerDisplayLabel(marker);
        const QString detail = userMarkerDetailLabels.value(marker.number);
        if (!detail.isEmpty()) label += QLatin1Char('\n') + detail;
        bool labelOnLeft = false;
        if (!detail.isEmpty() && userMarkers.size() == 2) {
            const auto &other = userMarkers.at(1 - i);
            const double x = geoToPoint(marker.latitude, marker.longitude, mapRect).x();
            const double otherX = geoToPoint(other.latitude, other.longitude, mapRect).x();
            labelOnLeft = x < otherX || (x == otherX && i == 0);
        }
        drawMapMarker(painter,
                      mapRect,
                      marker.latitude,
                      marker.longitude,
                      QColor(255, 214, 84),
                      label,
                      true,
                      labelOnLeft);
    }
    if (hasSearchMarker) {
        drawMapMarker(painter,
                      mapRect,
                      searchMarker.latitude,
                      searchMarker.longitude,
                      QColor(255, 86, 86),
                      markerDisplayLabel(searchMarker),
                      true);
    }
}

void QthMapWidget::drawSatelliteMarkers(QPainter &painter, const QRectF &mapRect) {
    if (!positionVisible || satelliteMarkers.isEmpty()) {
        return;
    }

    const QPointF qthPoint = geoToPoint(latitude, longitude, mapRect);
    const bool qthVisible = mapRect.adjusted(-24, -24, 24, 24).contains(qthPoint);

    auto colorForCn0 = [](int cn0) {
        if (cn0 < 0) {
            return QColor(125, 135, 145);
        }
        if (cn0 < 20) {
            return QColor(230, 95, 80);
        }
        if (cn0 < 35) {
            return QColor(235, 190, 70);
        }
        return QColor(105, 215, 125);
    };

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(mapRect);

    QFont labelFont = painter.font();
    labelFont.setBold(true);
    labelFont.setPointSize((std::max)(8, labelFont.pointSize() - 1));
    painter.setFont(labelFont);

    for (const SatelliteMarker &satellite : satelliteMarkers) {
        QPointF satelliteGeo;
        if (!approximateSatelliteSkyPoint(latitude, longitude, satellite, &satelliteGeo)) {
            continue;
        }

        const QPointF point = geoToPoint(satelliteGeo.x(), satelliteGeo.y(), mapRect);
        if (!mapRect.adjusted(-18, -18, 18, 18).contains(point)) {
            continue;
        }

        QColor color = colorForCn0(satellite.cn0DbHz);
        if (!satellite.enabled) {
            color = color.darker(220);
        }

        if (qthVisible) {
            painter.setPen(QPen(QColor(20, 24, 28, satellite.enabled ? 150 : 85), 3));
            painter.drawLine(qthPoint, point);
            painter.setPen(QPen(QColor(245, 248, 252, satellite.enabled ? 90 : 45), 1, Qt::DashLine));
            painter.drawLine(qthPoint, point);
        }

        painter.setBrush(color);
        painter.setPen(QPen(satellite.usedInFix ? QColor(255, 255, 255) : QColor(20, 24, 28),
                            satellite.usedInFix ? 2 : 1));
        painter.drawEllipse(point, 7.0, 7.0);

        painter.setPen(satellite.enabled ? QColor(245, 248, 252) : QColor(135, 145, 155));
        const QString label = QStringLiteral("%1 %2°").arg(satellite.label).arg(satellite.elevationDeg);
        painter.drawText(point + QPointF(10.0, 4.0), label);
    }

    painter.restore();
}

void QthMapWidget::drawMarkerAndTitle(QPainter &painter, const QRectF &mapRect) {
    if (!positionVisible) {
        const bool ukrainian = uiLanguage.startsWith(QStringLiteral("uk"));
        painter.setPen(QColor(230, 235, 240));
        QFont titleFont = painter.font();
        titleFont.setBold(true);
        titleFont.setPointSize(titleFont.pointSize() + 1);
        painter.setFont(titleFont);
        painter.drawText(QRectF(mapRect.left(), mapRect.top() + 8, mapRect.width(), 24),
                         Qt::AlignHCenter,
                         ukrainian
                             ? QStringLiteral("\u041F\u043E\u0442\u043E\u0447\u043D\u0443 QTH-\u043F\u043E\u0437\u0438\u0446\u0456\u044E \u043E\u0447\u0438\u0449\u0435\u043D\u043E")
                             : QStringLiteral("Current QTH position cleared"));
        return;
    }

    drawMapMarker(painter,
                  mapRect,
                  latitude,
                  longitude,
                  QColor(70, 220, 135),
                  locator,
                  false);

    painter.setPen(QColor(230, 235, 240));
    QFont titleFont = painter.font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 1);
    painter.setFont(titleFont);
    painter.drawText(QRectF(mapRect.left(), mapRect.top() + 8, mapRect.width(), 24),
                     Qt::AlignHCenter,
                     QStringLiteral("%1  %2, %3")
                         .arg(locator,
                              QString::number(latitude, 'f', 6),
                              QString::number(longitude, 'f', 6)));

    const double viewLat = viewCenterInitialized ? viewCenterLatitude : latitude;
    const double viewLon = viewCenterInitialized ? viewCenterLongitude : longitude;
    const bool ukrainian = uiLanguage.startsWith(QStringLiteral("uk"));
    const QString viewText =
        (ukrainian
             ? QStringLiteral("\u041F\u0435\u0440\u0435\u0433\u043B\u044F\u0434: %1 / %2 / %3")
             : QStringLiteral("View: %1 / %2 / %3"))
            .arg(qth::maidenheadLocator(viewLat, viewLon, 2),
                 qth::maidenheadLocator(viewLat, viewLon, 4),
                 qth::maidenheadLocator(viewLat, viewLon, 6));
    QFont badgeFont = painter.font();
    badgeFont.setBold(true);
    badgeFont.setPointSize((std::max)(8, badgeFont.pointSize() - 2));
    painter.setFont(badgeFont);
    const QFontMetrics metrics(badgeFont);
    const QRectF badgeRect(mapRect.left() + 10.0,
                           mapRect.top() + 10.0,
                           metrics.horizontalAdvance(viewText) + 18.0,
                           metrics.height() + 8.0);
    painter.fillRect(badgeRect, QColor(21, 26, 32, 180));
    painter.setPen(QPen(QColor(255, 199, 95, 180), 1));
    painter.drawRect(badgeRect);
    painter.setPen(QColor(245, 248, 252));
    painter.drawText(badgeRect, Qt::AlignCenter, viewText);
}

void QthMapWidget::drawFooter(QPainter &painter, const QRectF &mapRect) {
    QFont labelFont = painter.font();
    labelFont.setPointSize((std::max)(8, labelFont.pointSize() - 1));
    painter.setFont(labelFont);
    painter.setPen(QColor(230, 235, 240));

    QString footer;
    const bool ukrainian = uiLanguage.startsWith(QStringLiteral("uk"));
    if (tileLayerMode == TileLayerMode::LocalXyz) {
        if (hasLocalTileDirectory()) {
            footer = ukrainian
                         ? QStringLiteral("\u041E\u0444\u043B\u0430\u0439\u043D XYZ z%1: %2 \u043F\u043E\u043A\u0430\u0437\u0430\u043D\u043E, %3 \u0431\u0440\u0430\u043A\u0443\u0454")
                               .arg(mapZoom)
                               .arg(lastTilesDrawn)
                               .arg(lastTilesMissing)
                         : QStringLiteral("Offline XYZ tiles z%1: %2 drawn, %3 missing")
                               .arg(mapZoom)
                               .arg(lastTilesDrawn)
                               .arg(lastTilesMissing);
        } else {
            footer = ukrainian
                         ? QStringLiteral("\u041F\u0430\u043F\u043A\u0443 \u043E\u0444\u043B\u0430\u0439\u043D XYZ-\u0442\u0430\u0439\u043B\u0456\u0432 \u043D\u0435 \u0432\u0438\u0431\u0440\u0430\u043D\u043E")
                         : QStringLiteral("Offline XYZ tile folder is not selected");
        }
    } else if (tileLayerMode == TileLayerMode::OnlineXyz) {
        const QString errorText =
            lastOnlineError.isEmpty()
                ? QString()
                : (ukrainian
                       ? QStringLiteral("  \u043E\u0441\u0442.: %1").arg(lastOnlineError.left(90))
                       : QStringLiteral("  last: %1").arg(lastOnlineError.left(90)));
        if (ukrainian) {
            footer = QStringLiteral("\u041E\u043D\u043B\u0430\u0439\u043D XYZ z%1: %2 \u043F\u043E\u043A\u0430\u0437\u0430\u043D\u043E, %3 \u0437\u0430\u0432\u0430\u043D\u0442., %4 \u043F\u043E\u043C\u0438\u043B., %5  %6%7")
                         .arg(mapZoom)
                         .arg(lastTilesDrawn)
                         .arg(lastTilesPending)
                         .arg(lastTilesFailed)
                         .arg(onlineDiskCacheEnabled
                                  ? QStringLiteral("\u0434\u0438\u0441\u043A\u043E\u0432\u0438\u0439 \u043A\u0435\u0448")
                                  : QStringLiteral("\u0442\u0456\u043B\u044C\u043A\u0438 \u043F\u0430\u043C'\u044F\u0442\u044C"))
                         .arg(onlineAttribution.isEmpty()
                                  ? QStringLiteral("\u0431\u0435\u0437 \u0430\u0442\u0440\u0438\u0431\u0443\u0446\u0456\u0457")
                                  : onlineAttribution)
                         .arg(errorText);
        } else {
            footer = QStringLiteral("Online XYZ z%1: %2 drawn, %3 loading, %4 failed, %5  %6%7")
                         .arg(mapZoom)
                         .arg(lastTilesDrawn)
                         .arg(lastTilesPending)
                         .arg(lastTilesFailed)
                         .arg(onlineDiskCacheEnabled
                                  ? QStringLiteral("disk cache")
                                  : QStringLiteral("memory only"))
                         .arg(onlineAttribution.isEmpty()
                                  ? QStringLiteral("no attribution")
                                  : onlineAttribution)
                         .arg(errorText);
        }
    } else {
        footer = ukrainian
                     ? QStringLiteral("\u041E\u0444\u043B\u0430\u0439\u043D QTH-\u0441\u0456\u0442\u043A\u0430")
                     : QStringLiteral("Offline QTH grid");
    }

    painter.drawText(QRectF(mapRect.left(), rect().bottom() - 28, mapRect.width(), 20),
                     Qt::AlignRight | Qt::AlignVCenter,
                     footer);
}

void QthMapWidget::setRasterOverlay(const QImage &image, const qth::GeoBounds &bounds, double opacity) {
    rasterOverlay = image;
    rasterBounds = bounds;
    rasterOpacity = std::clamp(opacity, 0.0, 1.0);
    update();
}
void QthMapWidget::setPointPickingEnabled(bool enabled) {
    pointPickingEnabled = enabled;
    draggingMap = false;
    setCursor(enabled ? Qt::CrossCursor : Qt::OpenHandCursor);
}
void QthMapWidget::setMarkerEditingEnabled(bool enabled) {
    markerEditingEnabled = enabled;
    if (!enabled && draggedMarkerNumber >= 0) {
        draggedMarkerNumber = -1;
        setCursor(pointPickingEnabled ? Qt::CrossCursor : Qt::OpenHandCursor);
        if (draggedMarkerMoved) emit userMarkersChanged();
        draggedMarkerMoved = false;
    }
}
void QthMapWidget::setMarkerCreationEnabled(bool enabled) { markerCreationEnabled = enabled; }
void QthMapWidget::setPathLine(const qth::GeoPosition &a, const qth::GeoPosition &b) {
    pathStart = a; pathEnd = b; pathLineVisible = true; update();
}
void QthMapWidget::clearPathLine() { pathLineVisible = false; update(); }
void QthMapWidget::drawRasterOverlay(QPainter &painter, const QRectF &mapRect) {
    painter.save(); painter.setClipRect(mapRect);
    if (!rasterOverlay.isNull() && rasterBounds.valid) {
        const QPointF a = geoToPoint(rasterBounds.maxLatitude, rasterBounds.minLongitude, mapRect);
        const double bottom = geoToPoint(rasterBounds.minLatitude, rasterBounds.minLongitude, mapRect).y();
        const double world = double(kTileSize) * (1 << mapZoom);
        const double width = mapZoom > 0 ? (rasterBounds.maxLongitude-rasterBounds.minLongitude)*world/360.0
                                         : (rasterBounds.maxLongitude-rasterBounds.minLongitude)*mapRect.width()/360.0;
        painter.setOpacity(rasterOpacity);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        for (int shift=-1;shift<=1;++shift) {
            const QRectF target(a.x()+shift*world,a.y(),width,bottom-a.y());
            if(target.intersects(mapRect)) painter.drawImage(target,rasterOverlay);
        }
        painter.setOpacity(1.0);
    }
    if (pathLineVisible) {
        painter.setPen(QPen(QColor(250,250,250),1.5,Qt::DashLine));
        painter.drawLine(geoToPoint(pathStart.latitude,pathStart.longitude,mapRect),
                         geoToPoint(pathEnd.latitude,pathEnd.longitude,mapRect));
    }
    painter.restore();
}
void QthMapWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(21, 26, 32));

    const QRectF mapRect = rect().adjusted(18, 18, -18, -34);
    painter.fillRect(mapRect, QColor(31, 39, 48));

    const bool drewTiles = tileLayerMode == TileLayerMode::OnlineXyz
                               ? drawOnlineTiles(painter, mapRect)
                               : drawLocalTiles(painter, mapRect);
    if ((tileLayerMode == TileLayerMode::LocalXyz || tileLayerMode == TileLayerMode::OnlineXyz) && drewTiles) {
        painter.fillRect(mapRect, QColor(0, 0, 0, 28));
    }
    drawRasterOverlay(painter, mapRect);

    const bool drawGrid = overlayMode == OverlayMode::QthGrid ||
                          overlayMode == OverlayMode::GridAndSatellites;
    const bool drawSatellites = overlayMode == OverlayMode::GridAndSatellites ||
                                overlayMode == OverlayMode::Satellites;

    if (drawGrid) {
        drawMaidenheadGrid(painter, mapRect);
    }
    if (positionVisible && drawGrid) {
        drawCurrentLocator(painter, mapRect);
    }
    drawUserMarkers(painter, mapRect);
    if (drawSatellites) {
        drawSatelliteMarkers(painter, mapRect);
    }
    if (positionStatusVisible) drawMarkerAndTitle(painter, mapRect);
    drawFooter(painter, mapRect);
}

void QthMapWidget::mousePressEvent(QMouseEvent *event) {
    if (!event) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QRectF mapRect = rect().adjusted(18, 18, -18, -34);
    const QPointF mousePosition = event->localPos();
    if (event->button() == Qt::RightButton && !markerEditingEnabled) {
        event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && markerEditingEnabled && mapRect.contains(mousePosition)) {
        const int index = userMarkerAtPosition(mousePosition, mapRect);
        if (index >= 0) {
            const auto &marker = userMarkers.at(index);
            draggedMarkerNumber = marker.number;
            draggedMarkerMoved = false;
            markerDragStart = mousePosition;
            markerDragOffset = mousePosition - geoToPoint(marker.latitude, marker.longitude, mapRect);
            draggingMap = false;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
    }
    if (event->button() == Qt::LeftButton && pointPickingEnabled && mapRect.contains(mousePosition)) {
        const QPointF geo = screenPointToGeo(mousePosition,mapRect,mapZoom);
        emit pointPicked(geo.x(),geo.y()); event->accept(); return;
    }
    if (event->button() == Qt::RightButton && mapRect.contains(mousePosition)) {
        const int markerIndex = userMarkerAtPosition(mousePosition, mapRect);
        if (markerIndex >= 0 && markerIndex < userMarkers.size()) {
            const qth::UserMarker marker = userMarkers.takeAt(markerIndex);
            const QString label = markerDisplayLabel(marker);
            emit userMarkerRemoved(marker.latitude, marker.longitude, label, marker.number);
            emit userMarkersChanged();
            update();
            event->accept();
            return;
        }
        if (searchMarkerAtPosition(mousePosition, mapRect)) {
            const qth::UserMarker marker = searchMarker;
            const QString label = markerDisplayLabel(marker);
            clearSearchMarker();
            emit userMarkerRemoved(marker.latitude, marker.longitude, label, marker.number);
            event->accept();
            return;
        }
        if (currentPositionAtPosition(mousePosition, mapRect)) {
            positionVisible = false;
            emit currentPositionCleared();
            update();
            event->accept();
            return;
        }

        if (!markerCreationEnabled) { event->accept(); return; }
        const QPointF geo = screenPointToGeo(mousePosition, mapRect, mapZoom);
        qth::UserMarker marker;
        marker.latitude = (std::clamp)(geo.x(), -90.0, 90.0);
        marker.longitude = (std::clamp)(geo.y(), -180.0, 180.0);
        int nextNumber = 1;
        for (const qth::UserMarker &existingMarker : userMarkers) {
            nextNumber = (std::max)(nextNumber, existingMarker.number + 1);
        }
        marker.number = nextNumber;
        marker.name = qth::maidenheadLocator(marker.latitude, marker.longitude, 6);
        userMarkers.append(marker);
        emit userMarkerAdded(marker.latitude, marker.longitude, markerDisplayLabel(marker), marker.number);
        emit userMarkersChanged();
        update();
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    draggingMap = true;
    lastDragPosition = event->pos();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void QthMapWidget::moveDraggedUserMarker(const QPointF &point) {
    if (!markerEditingEnabled || draggedMarkerNumber < 0) return;
    if (!draggedMarkerMoved &&
        (point - markerDragStart).manhattanLength() < QApplication::startDragDistance()) return;
    const QRectF mapRect = rect().adjusted(18, 18, -18, -34);
    if (mapRect.isEmpty()) return;
    const QPointF target = point - markerDragOffset;
    const QPointF clipped((std::clamp)(target.x(), mapRect.left(), mapRect.right()),
                          (std::clamp)(target.y(), mapRect.top(), mapRect.bottom()));
    const QPointF geo = screenPointToGeo(clipped, mapRect, mapZoom);
    for (int index = 0; index < userMarkers.size(); ++index) {
        if (userMarkers.at(index).number != draggedMarkerNumber) continue;
        if (userMarkers.at(index).latitude == geo.x() && userMarkers.at(index).longitude == geo.y()) return;
        userMarkers[index].latitude = geo.x();
        userMarkers[index].longitude = geo.y();
        draggedMarkerMoved = true;
        // The receiver of this signal may replace the marker list synchronously.
        const int number = draggedMarkerNumber;
        emit userMarkerMoved(geo.x(), geo.y(), number);
        update();
        return;
    }
}

void QthMapWidget::mouseMoveEvent(QMouseEvent *event) {
    if (event && draggedMarkerNumber >= 0) {
        moveDraggedUserMarker(event->localPos());
        event->accept();
        return;
    }
    if (!event || !draggingMap) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPointF currentPosition = event->pos();
    const QPointF delta = currentPosition - lastDragPosition;
    lastDragPosition = currentPosition;

    QPointF center = viewCenterMercatorPixel(mapZoom);
    center -= delta;
    const QPointF geo = mercatorPixelToGeo(center.x(), center.y(), mapZoom);
    viewCenterLatitude = geo.x();
    viewCenterLongitude = geo.y();
    viewCenterInitialized = true;
    update();
    event->accept();
}

void QthMapWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (!event || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (draggedMarkerNumber >= 0) {
        moveDraggedUserMarker(event->localPos());
        draggedMarkerNumber = -1;
        if (draggedMarkerMoved) emit userMarkersChanged();
        draggedMarkerMoved = false;
    }
    draggingMap = false;
    setCursor(pointPickingEnabled ? Qt::CrossCursor : Qt::OpenHandCursor);
    event->accept();
}

void QthMapWidget::wheelEvent(QWheelEvent *event) {
    if (!event) {
        return;
    }
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }

    const QRectF mapRect = rect().adjusted(18, 18, -18, -34);
    if (!mapRect.contains(event->position())) {
        event->ignore();
        return;
    }

    const int nextZoom = (std::clamp)(mapZoom + (delta > 0 ? 1 : -1),
                                      minimumMapZoom,
                                      maximumMapZoom);
    if (nextZoom == mapZoom) {
        event->accept();
        return;
    }

    const QPointF focusedGeo = screenPointToGeo(event->position(), mapRect, mapZoom);
    const QPointF focusedPixelAtNextZoom = geoToMercatorPixel(focusedGeo.x(), focusedGeo.y(), nextZoom);
    const QPointF cursorOffset = event->position() - mapRect.center();
    const QPointF nextCenterPixel = focusedPixelAtNextZoom - cursorOffset;
    const QPointF nextCenterGeo = mercatorPixelToGeo(nextCenterPixel.x(), nextCenterPixel.y(), nextZoom);

    mapZoom = nextZoom;
    viewCenterLatitude = nextCenterGeo.x();
    viewCenterLongitude = nextCenterGeo.y();
    viewCenterInitialized = true;
    update();
    emit mapZoomChanged(mapZoom);
    event->accept();
}
