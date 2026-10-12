#ifndef QTHMAPWIDGET_H
#define QTHMAPWIDGET_H

#include "qthlocator.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QPointF>
#include <QPixmap>
#include <QSet>
#include <QVector>
#include <QWidget>

class QMouseEvent;
class QWheelEvent;

class QthMapWidget : public QWidget {
    Q_OBJECT

public:
    enum class TileLayerMode {
        GridOnly = 0,
        LocalXyz = 1,
        OnlineXyz = 2
    };

    enum class OverlayMode {
        None = 0,
        QthGrid = 1,
        GridAndSatellites = 2,
        Satellites = 3
    };

    struct SatelliteMarker {
        QString label;
        QString system;
        int elevationDeg = -1;
        int azimuthDeg = -1;
        int cn0DbHz = -1;
        bool usedInFix = false;
        bool enabled = true;
    };

    explicit QthMapWidget(QWidget *parent = nullptr);

    void setPosition(double latitude, double longitude);
    void setTileLayerMode(TileLayerMode mode);
    void setTileDirectory(const QString &path);
    void setOnlineTileTemplate(const QString &urlTemplate);
    void setOnlineAttribution(const QString &text);
    void setOnlineDiskCacheEnabled(bool enabled);
    void setUiLanguage(const QString &language);
    void setMapZoomRange(int minimumZoom, int maximumZoom);
    void setMapZoom(int zoom);
    void setGridPrecision(int precision);
    void setOverlayMode(OverlayMode mode);
    void setSatelliteMarkers(const QVector<SatelliteMarker> &markers);
    void setPositionVisible(bool visible);
    void setPositionStatusVisible(bool visible);
    void centerOnPosition();
    void centerOn(double latitude, double longitude);
    qth::GeoPosition mapCenter() const;
    int mapZoomLevel() const { return mapZoom; }
    bool positionAt(const QPointF &point, qth::GeoPosition *position) const;
    void setSearchMarker(double latitude, double longitude, const QString &label);
    void clearSearchMarker();
    void setUserMarkers(const QVector<qth::UserMarker> &markers);
    QVector<qth::UserMarker> userMarkerList() const;
    void setUserMarkerDetailLabels(const QHash<int, QString> &labels);
    void clearUserMarkers();
    void setRasterOverlay(const QImage &image, const qth::GeoBounds &bounds, double opacity);
    void setPointPickingEnabled(bool enabled);
    void setMarkerEditingEnabled(bool enabled);
    void setMarkerCreationEnabled(bool enabled);
    void setPathLine(const qth::GeoPosition &a, const qth::GeoPosition &b);
    void clearPathLine();

signals:
    void mapZoomChanged(int zoom);
    void userMarkersChanged();
    void userMarkerAdded(double latitude, double longitude, const QString &label, int number);
    void userMarkerRemoved(double latitude, double longitude, const QString &label, int number);
    void userMarkerMoved(double latitude, double longitude, int number);
    void currentPositionCleared();
    void pointPicked(double latitude, double longitude);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    QPointF geoToPoint(double latitude, double longitude, const QRectF &mapRect) const;
    QPointF geoToMercatorPixel(double latitude, double longitude, int zoom) const;
    QPointF mercatorPixelToGeo(double x, double y, int zoom) const;
    QPointF viewCenterMercatorPixel(int zoom) const;
    QPointF screenPointToGeo(const QPointF &point, const QRectF &mapRect, int zoom) const;
    QString tilePath(int zoom, int x, int y) const;
    QString onlineTileUrl(int zoom, int x, int y) const;
    bool hasLocalTileDirectory() const;
    bool drawLocalTiles(QPainter &painter, const QRectF &mapRect);
    bool drawOnlineTiles(QPainter &painter, const QRectF &mapRect);
    void requestOnlineTile(const QString &url);
    void finishOnlineTileRequest(const QString &url, const QByteArray &data, const QString &error);
    qth::GeoBounds visibleGeoBounds(const QRectF &mapRect) const;
    void drawMaidenheadGrid(QPainter &painter, const QRectF &mapRect);
    void drawMaidenheadLayer(QPainter &painter,
                             const QRectF &mapRect,
                             const qth::GeoBounds &bounds,
                             int precision,
                             bool drawLabels);
    void drawCurrentLocator(QPainter &painter, const QRectF &mapRect);
    void drawMapMarker(QPainter &painter,
                       const QRectF &mapRect,
                       double markerLatitude,
                       double markerLongitude,
                       const QColor &color,
                       const QString &label,
                       bool drawLabel,
                       bool labelOnLeft = false);
    int userMarkerAtPosition(const QPointF &point, const QRectF &mapRect) const;
    void moveDraggedUserMarker(const QPointF &point);
    bool searchMarkerAtPosition(const QPointF &point, const QRectF &mapRect) const;
    bool currentPositionAtPosition(const QPointF &point, const QRectF &mapRect) const;
    void drawUserMarkers(QPainter &painter, const QRectF &mapRect);
    void drawSatelliteMarkers(QPainter &painter, const QRectF &mapRect);
    void drawMarkerAndTitle(QPainter &painter, const QRectF &mapRect);
    void drawFooter(QPainter &painter, const QRectF &mapRect);
    void drawRasterOverlay(QPainter &painter, const QRectF &mapRect);

    double latitude = 0.0;
    double longitude = 0.0;
    bool positionVisible = true;
    bool positionStatusVisible = true;
    double viewCenterLatitude = 0.0;
    double viewCenterLongitude = 0.0;
    bool viewCenterInitialized = false;
    QString locator;
    TileLayerMode tileLayerMode = TileLayerMode::GridOnly;
    QString tileDirectoryPath;
    QString onlineTileUrlTemplate = QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png");
    QString onlineAttribution = QString::fromUtf8("\xC2\xA9 OpenStreetMap contributors");
    bool onlineDiskCacheEnabled = true;
    QString uiLanguage = QStringLiteral("en");
    int mapZoom = 12;
    int minimumMapZoom = 0;
    int maximumMapZoom = 19;
    int gridPrecision = 6;
    OverlayMode overlayMode = OverlayMode::QthGrid;
    int lastTilesDrawn = 0;
    int lastTilesMissing = 0;
    int lastTilesPending = 0;
    int lastTilesFailed = 0;
    bool draggingMap = false;
    int draggedMarkerNumber = -1;
    bool draggedMarkerMoved = false;
    QPointF markerDragOffset;
    QPointF markerDragStart;
    QPointF lastDragPosition;
    QVector<qth::UserMarker> userMarkers;
    QVector<SatelliteMarker> satelliteMarkers;
    bool hasSearchMarker = false;
    qth::UserMarker searchMarker;
    QImage rasterOverlay;
    qth::GeoBounds rasterBounds;
    double rasterOpacity = 0.65;
    bool pointPickingEnabled = false;
    bool markerEditingEnabled = true;
    bool markerCreationEnabled = true;
    bool pathLineVisible = false;
    qth::GeoPosition pathStart, pathEnd;

    class QNetworkAccessManager *tileNetwork = nullptr;
    QHash<QString, QPixmap> onlineTilePixmaps;
    QHash<int, QString> userMarkerDetailLabels;
    QHash<QString, QString> onlineTileErrors;
    QSet<QString> pendingTileRequests;
    QString lastOnlineError;
};

#endif // QTHMAPWIDGET_H
