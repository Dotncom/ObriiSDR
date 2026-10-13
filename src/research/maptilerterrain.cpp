#include "maptilerterrain.h"
#include "publicdatafetch.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>
#include <functional>

namespace propagation {
namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr double EarthM = 6371000.0;
double mercatorTileY(double latitude, int zoom) {
    const double radians = std::clamp(latitude, -85.0, 85.0) * Pi / 180.0;
    return (1.0 - std::asinh(std::tan(radians)) / Pi) * (1 << zoom) / 2.0;
}
double tileX(double longitude, int zoom) { return (longitude + 180.0) / 360.0 * (1 << zoom); }
int wrappedX(int x, int zoom) { const int n = 1 << zoom; return (x % n + n) % n; }
}

quint64 MapTilerTerrain::tileKey(int level, int x, int y) {
    return (quint64(level) << 48) | (quint64(x) << 24) | quint64(y);
}

MapTilerTerrain::MapTilerTerrain(const Parameters &p, bool coverage) {
    double south, north, west, east;
    if (coverage) {
        const double angular = p.radiusKm * 1000.0 / EarthM;
        const double latitudeSpan = angular * 180.0 / Pi;
        const double longitudeSpan = std::asin(std::min(1.0, std::sin(angular) /
            std::cos(p.tx.latitude * Pi / 180.0))) * 180.0 / Pi;
        south = p.tx.latitude - latitudeSpan;
        north = p.tx.latitude + latitudeSpan;
        west = p.tx.longitude - longitudeSpan;
        east = p.tx.longitude + longitudeSpan;
    } else {
        const double deltaLongitude = std::remainder(p.rx.longitude - p.tx.longitude, 360.0);
        south = std::min(p.tx.latitude, p.rx.latitude) - 0.1;
        north = std::max(p.tx.latitude, p.rx.latitude) + 0.1;
        west = p.tx.longitude + std::min(0.0, deltaLongitude) - 0.1;
        east = p.tx.longitude + std::max(0.0, deltaLongitude) + 0.1;
    }
    for (int level = 13; level >= 6; --level) {
        const int left = int(std::floor(tileX(west, level)));
        const int right = int(std::floor(tileX(east, level)));
        const int top = std::max(0, int(std::floor(mercatorTileY(north, level))));
        const int bottom = std::min((1 << level) - 1, int(std::floor(mercatorTileY(south, level))));
        const qint64 count = qint64(right - left + 1) * (bottom - top + 1);
        if (count <= 0 || count > 96) continue;
        zoom = level;
        for (int y = top; y <= bottom; ++y)
            for (int x = left; x <= right; ++x)
                requests.append({wrappedX(x, level), y});
        break;
    }
}

void MapTilerTerrain::insertTile(int level, int x, int y, const QImage &image) {
    if (image.isNull() || image.width() < 2 || image.height() < 2) return;
    images.insert(tileKey(level, x, y), image.convertToFormat(QImage::Format_ARGB32));
}

double MapTilerTerrain::elevation(double latitude, double longitude) const {
    if (!zoom || !std::isfinite(latitude) || !std::isfinite(longitude) ||
        latitude < -85.0 || latitude > 85.0) return Unknown;
    const int n = 1 << zoom;
    const double x = tileX(std::remainder(longitude, 360.0), zoom);
    const double y = mercatorTileY(latitude, zoom);
    const int tileColumn = wrappedX(int(std::floor(x)), zoom);
    const int tileRow = int(std::floor(y));
    if (tileRow < 0 || tileRow >= n) return Unknown;
    const auto it = images.constFind(tileKey(zoom, tileColumn, tileRow));
    if (it == images.cend()) return Unknown;
    const QImage &image = it.value();
    const double px = (x - std::floor(x)) * image.width() - 0.5;
    const double py = (y - std::floor(y)) * image.height() - 0.5;
    const int x0 = std::clamp(int(std::floor(px)), 0, image.width() - 1);
    const int y0 = std::clamp(int(std::floor(py)), 0, image.height() - 1);
    const int x1 = std::min(x0 + 1, image.width() - 1);
    const int y1 = std::min(y0 + 1, image.height() - 1);
    const double fx = std::clamp(px - x0, 0.0, 1.0);
    const double fy = std::clamp(py - y0, 0.0, 1.0);
    const auto value = [&](int col, int row) {
        const QRgb color = image.pixel(col, row);
        if (qAlpha(color) == 0) return Unknown;
        return -10000.0 + (qRed(color) * 65536.0 + qGreen(color) * 256.0 + qBlue(color)) * 0.1;
    };
    const double a = value(x0, y0), b = value(x1, y0), c = value(x0, y1), d = value(x1, y1);
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || !std::isfinite(d)) return Unknown;
    return (1-fy)*((1-fx)*a+fx*b) + fy*((1-fx)*c+fx*d);
}

bool MapTilerTerrain::fetch(const QString &apiKey, const std::atomic_bool *cancel,
                            const std::function<void(int, int)> &progress) {
    failure.clear(); missing = 0;
    if (apiKey.trimmed().isEmpty() || requests.isEmpty()) {
        failure = QStringLiteral("MapTiler key or terrain tile range is unavailable");
        return false;
    }
    const auto tileUrl = [&](const TileId &tile) {
        QUrl url(QStringLiteral("https://api.maptiler.com/tiles/terrain-rgb-v2/%1/%2/%3.webp")
                     .arg(zoom).arg(tile.x).arg(tile.y));
        QUrlQuery query; query.addQueryItem(QStringLiteral("key"), apiKey);
        url.setQuery(query);
        return url;
    };
    const auto decodeTile = [&](const TileId &tile, const QByteArray &bytes) {
        const QImage image = QImage::fromData(bytes, "WEBP");
        if (image.isNull()) {
            failure = QStringLiteral("MapTiler terrain WebP could not be decoded; install the Qt WebP image plugin");
        } else insertTile(zoom, tile.x, tile.y, image);
    };
#ifdef Q_OS_WIN
    // Like the base map, use Windows TLS when the portable Qt build has no OpenSSL.
    // fetch() already runs on the calculation worker, never on the GUI thread.
    if (!QSslSocket::supportsSsl()) {
        std::atomic_bool notCancelled{false};
        const auto &cancellation = cancel ? *cancel : notCancelled;
        QElapsedTimer deadline; deadline.start();
        int finished = 0;
        for (const auto &tile : requests) {
            if (cancellation.load()) return false;
            if (deadline.elapsed() >= 60000) {
                failure = QStringLiteral("MapTiler terrain request timed out");
                break;
            }
            QString error; int http = 0;
            const auto bytes = fetchPublicData(tileUrl(tile), cancellation, &error, &http);
            if (cancellation.load()) return false;
            if (http == 204 || http == 404) ++missing;
            else if (!error.isEmpty()) failure = QStringLiteral("MapTiler terrain: ") + error;
            else if (http == 200) decodeTile(tile, bytes);
            else failure = QStringLiteral("MapTiler terrain request failed (HTTP %1)").arg(http);
            if (progress) progress(++finished, requests.size());
            if (!failure.isEmpty()) break;
        }
        if (failure.isEmpty() && images.isEmpty()) failure = QStringLiteral("No MapTiler terrain tiles were available");
        return failure.isEmpty();
    }
#endif
    QNetworkAccessManager manager;
    QEventLoop loop;
    QTimer watchdog;
    watchdog.setInterval(100);
    QElapsedTimer elapsed;
    elapsed.start();
    int next = 0, active = 0, finished = 0;
    bool stopping = false;
    QList<QNetworkReply *> pending;
    const auto stopPending = [&] {
        if (!stopping) {
            stopping = true;
            const auto aborting = pending;
            for (auto *reply : aborting) reply->abort();
        }
        if (active == 0) loop.quit();
    };
    std::function<void()> startNext;
    startNext = [&] {
        while (!stopping && active < 4 && next < requests.size() && failure.isEmpty() &&
               !(cancel && cancel->load())) {
            const TileId tile = requests[next++];
            QNetworkRequest request(tileUrl(tile));
            request.setRawHeader("Accept", "image/webp");
            request.setTransferTimeout(15000);
            auto *reply = manager.get(request);
            pending.append(reply); ++active;
            QObject::connect(reply, &QNetworkReply::finished, &manager, [&, reply, tile] {
                pending.removeOne(reply); --active; ++finished;
                const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (failure.isEmpty() && !(cancel && cancel->load())) {
                    if (http == 200 && reply->error() == QNetworkReply::NoError) {
                        decodeTile(tile, reply->readAll());
                    } else if (http == 204 || http == 404) {
                        ++missing;
                    } else {
                        failure = http ? QStringLiteral("MapTiler terrain request failed (HTTP %1)").arg(http) :
                            QStringLiteral("MapTiler terrain network/TLS error %1").arg(int(reply->error()));
                    }
                }
                reply->deleteLater();
                if (progress) progress(finished, requests.size());
                if (!failure.isEmpty() || (cancel && cancel->load())) {
                    stopPending();
                } else if (finished == requests.size()) loop.quit();
                else startNext();
            });
        }
    };
    QObject::connect(&watchdog, &QTimer::timeout, &manager, [&] {
        if ((cancel && cancel->load()) || elapsed.elapsed() > 60000) {
            if (!(cancel && cancel->load())) failure = QStringLiteral("MapTiler terrain request timed out");
            stopPending();
        }
    });
    watchdog.start();
    startNext();
    if (active > 0) loop.exec();
    watchdog.stop();
    if (cancel && cancel->load()) return false;
    if (failure.isEmpty() && images.isEmpty()) failure = QStringLiteral("No MapTiler terrain tiles were available");
    return failure.isEmpty();
}
}
