#include "main.h"

#include "appconstants.h"
#include "gnssqthhelpers.h"
#include "presethelpers.h"
#include "qthlocator.h"
#include "qthmapwidget.h"
#include "tuningutils.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTextCursor>
#include <QUrl>

#include <algorithm>
#include <cmath>

void YourClassName::updateQthMapControls() {
    if (qthMapLayerCombo) {
        QSignalBlocker blocker(qthMapLayerCombo);
        const int index = qthMapLayerCombo->findData(qthMapLayer);
        if (index >= 0) {
            qthMapLayerCombo->setCurrentIndex(index);
        }
    }
    if (qthOnlineProviderCombo) {
        QSignalBlocker blocker(qthOnlineProviderCombo);
        const int index = qthOnlineProviderCombo->findData(qthOnlineProviderId);
        qthOnlineProviderCombo->setCurrentIndex(index >= 0
                                                    ? index
                                                    : qthOnlineProviderCombo->findData(QStringLiteral("custom")));
    }
    if (qthMapZoomSpin) {
        QSignalBlocker blocker(qthMapZoomSpin);
        const int maxZoom = (std::clamp)(qthOnlineProviderPreset(qthOnlineProviderId).maxZoom, 0, 19);
        qthMapZoomSpin->setRange(0, maxZoom);
        qthMapZoom = (std::clamp)(qthMapZoom, 0, maxZoom);
        qthMapZoomSpin->setValue((std::clamp)(qthMapZoom, 0, 19));
    }
    if (qthGridPrecisionCombo) {
        QSignalBlocker blocker(qthGridPrecisionCombo);
        const int index = qthGridPrecisionCombo->findData(qthGridPrecision);
        qthGridPrecisionCombo->setCurrentIndex(index >= 0 ? index : qthGridPrecisionCombo->findData(6));
    }
    if (qthMapOverlayCombo) {
        QSignalBlocker blocker(qthMapOverlayCombo);
        const int index = qthMapOverlayCombo->findData(qthMapOverlayMode);
        qthMapOverlayCombo->setCurrentIndex(index >= 0 ? index : qthMapOverlayCombo->findData(1));
    }
    if (qthTileDirectoryEdit) {
        QSignalBlocker blocker(qthTileDirectoryEdit);
        qthTileDirectoryEdit->setText(qthTileDirectory);
    }
    if (qthOnlineTileUrlEdit) {
        QSignalBlocker blocker(qthOnlineTileUrlEdit);
        qthOnlineTileUrlEdit->setText(qthOnlineTileUrlTemplate);
    }
    if (qthOnlineAttributionEdit) {
        QSignalBlocker blocker(qthOnlineAttributionEdit);
        qthOnlineAttributionEdit->setText(qthOnlineAttribution);
    }
    if (qthOnlineApiKeyEdit) {
        QSignalBlocker blocker(qthOnlineApiKeyEdit);
        qthOnlineApiKeyEdit->setText(qthOnlineApiKey);
    }
    if (qthOnlineNoDiskCacheCheckbox) {
        QSignalBlocker blocker(qthOnlineNoDiskCacheCheckbox);
        qthOnlineNoDiskCacheCheckbox->setChecked(qthOnlineNoDiskCache);
    }
    if (qthMapStatusLabel) {
        const bool localLayer = qthMapLayer == 1;
        const bool onlineLayer = qthMapLayer == 2;
        const bool folderOk = !qthTileDirectory.trimmed().isEmpty() && QDir(qthTileDirectory).exists();
        const QthOnlineProviderPreset provider = qthOnlineProviderPreset(qthOnlineProviderId);
        const QString providerName = uiText(provider.textKey, provider.fallbackName);
        QString text;
        if (!localLayer) {
            if (onlineLayer) {
                if (qthOnlineTemplateNeedsKey(qthOnlineTileUrlTemplate) && qthOnlineApiKey.trimmed().isEmpty()) {
                    text = uiText(QStringLiteral("qth_map_online_key_required"),
                                  QStringLiteral("%1 requires an API key/token. Enter it before loading tiles."))
                               .arg(providerName);
                } else {
                    text = uiText(qthOnlineNoDiskCache
                                      ? QStringLiteral("qth_map_online_memory_status")
                                      : QStringLiteral("qth_map_online_status"),
                                  qthOnlineNoDiskCache
                                      ? QStringLiteral("%1 online tiles are fetched only for the current view and kept in memory only.")
                                      : QStringLiteral("%1 online tiles are fetched only for the current view and cached locally."))
                               .arg(providerName);
                }
            } else {
                text = uiText(QStringLiteral("qth_map_grid_status"),
                              QStringLiteral("Grid-only map works offline without tile files."));
            }
        } else if (folderOk) {
            text = uiText(QStringLiteral("qth_map_tiles_status"),
                          QStringLiteral("Offline XYZ tiles: %1"))
                       .arg(QDir::toNativeSeparators(qthTileDirectory));
        } else {
            text = uiText(QStringLiteral("qth_map_tiles_missing_status"),
                          QStringLiteral("Select an offline XYZ tile folder with z/x/y.png files."));
        }
        qthMapStatusLabel->setText(text);
    }
}

void YourClassName::selectQthTileDirectory() {
    const QString startDir = !qthTileDirectory.isEmpty() && QDir(qthTileDirectory).exists()
                                 ? qthTileDirectory
                                 : QDir::homePath();
    const QString directory =
        QFileDialog::getExistingDirectory(this,
                                          uiText(QStringLiteral("select_offline_tiles"),
                                                 QStringLiteral("Select offline map tiles")),
                                          startDir);
    if (directory.isEmpty()) {
        return;
    }

    qthTileDirectory = QDir::fromNativeSeparators(directory);
    qthMapLayer = 1;
    if (qthMapWidget) {
        qthMapWidget->setTileLayerMode(QthMapWidget::TileLayerMode::LocalXyz);
        qthMapWidget->setTileDirectory(qthTileDirectory);
    }
    updateQthMapControls();
    savePersistentSettings();
}

void YourClassName::openQthTileDirectory() {
    if (qthTileDirectory.trimmed().isEmpty()) {
        selectQthTileDirectory();
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(qthTileDirectory));
}

void YourClassName::openQthMapWindow() {
    if (!qthMapDialog) {
        qthMapDialog = new QDialog(this);
        qthMapDialog->setAttribute(Qt::WA_DeleteOnClose);
        qthMapDialog->setWindowTitle(uiText(QStringLiteral("qth_map_title"),
                                            QStringLiteral("QTH Map")));
        qthMapDialog->resize(780, 520);

        QVBoxLayout *rootLayout = new QVBoxLayout(qthMapDialog);

        QGridLayout *mapControlLayout = new QGridLayout();
        mapControlLayout->setContentsMargins(0, 0, 0, 0);
        mapControlLayout->setHorizontalSpacing(6);
        mapControlLayout->setVerticalSpacing(4);

        QLabel *layerLabel = new QLabel(uiText(QStringLiteral("map_layer"),
                                               QStringLiteral("Layer:")),
                                        qthMapDialog);
        qthMapLayerCombo = new QComboBox(qthMapDialog);
        qthMapLayerCombo->addItem(uiText(QStringLiteral("qth_grid_only"),
                                         QStringLiteral("QTH grid only")),
                                  0);
        qthMapLayerCombo->addItem(uiText(QStringLiteral("offline_xyz_tiles"),
                                         QStringLiteral("Offline XYZ tiles")),
                                  1);
        qthMapLayerCombo->addItem(uiText(QStringLiteral("online_xyz_tiles"),
                                         QStringLiteral("Online XYZ tiles")),
                                  2);
        QLabel *overlayLabel = new QLabel(uiText(QStringLiteral("map_overlay"),
                                                 QStringLiteral("Overlay:")),
                                          qthMapDialog);
        qthMapOverlayCombo = new QComboBox(qthMapDialog);
        qthMapOverlayCombo->addItem(uiText(QStringLiteral("map_overlay_none"),
                                           QStringLiteral("None")),
                                    static_cast<int>(QthMapWidget::OverlayMode::None));
        qthMapOverlayCombo->addItem(uiText(QStringLiteral("map_overlay_grid"),
                                           QStringLiteral("QTH grid")),
                                    static_cast<int>(QthMapWidget::OverlayMode::QthGrid));
        qthMapOverlayCombo->addItem(uiText(QStringLiteral("map_overlay_grid_satellites"),
                                           QStringLiteral("Grid + satellites")),
                                    static_cast<int>(QthMapWidget::OverlayMode::GridAndSatellites));
        qthMapOverlayCombo->addItem(uiText(QStringLiteral("map_overlay_satellites"),
                                           QStringLiteral("Satellites")),
                                    static_cast<int>(QthMapWidget::OverlayMode::Satellites));
        qthMapOverlayCombo->setToolTip(uiText(
            QStringLiteral("map_overlay_tooltip"),
            QStringLiteral("Choose QTH grid and a local scaled live NMEA satellite sky overlay drawn over the map.")));
        qthMapOverlayCombo->setMinimumContentsLength(8);
        qthMapOverlayCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        QLabel *providerLabel = new QLabel(uiText(QStringLiteral("provider"),
                                                  QStringLiteral("Provider:")),
                                           qthMapDialog);
        qthOnlineProviderCombo = new QComboBox(qthMapDialog);
        for (const QthOnlineProviderPreset &preset : qthOnlineProviderPresets()) {
            qthOnlineProviderCombo->addItem(uiText(preset.textKey, preset.fallbackName), preset.id);
        }

        QLabel *zoomLabel = new QLabel(uiText(QStringLiteral("zoom"),
                                              QStringLiteral("Zoom:")),
                                       qthMapDialog);
        qthMapZoomSpin = new QSpinBox(qthMapDialog);
        qthMapZoomSpin->setRange(0, 19);
        qthMapZoomSpin->setValue(qthMapZoom);
        qthMapZoomSpin->setMaximumWidth(64);

        QLabel *gridLabel = new QLabel(uiText(QStringLiteral("qth_grid"),
                                              QStringLiteral("QTH grid:")),
                                       qthMapDialog);
        qthGridPrecisionCombo = new QComboBox(qthMapDialog);
        qthGridPrecisionCombo->addItem(QStringLiteral("2"), 2);
        qthGridPrecisionCombo->addItem(QStringLiteral("4"), 4);
        qthGridPrecisionCombo->addItem(QStringLiteral("6"), 6);

        QLabel *tilesLabel = new QLabel(uiText(QStringLiteral("offline_tiles"),
                                               QStringLiteral("Offline tiles:")),
                                        qthMapDialog);
        qthTileDirectoryEdit = new QLineEdit(qthMapDialog);
        qthTileDirectoryEdit->setPlaceholderText(uiText(QStringLiteral("offline_tiles_placeholder"),
                                                        QStringLiteral("Folder with z/x/y.png tiles")));
        qthSelectTilesButton = new QPushButton(uiText(QStringLiteral("browse"),
                                                      QStringLiteral("Browse...")),
                                               qthMapDialog);
        qthOpenTilesButton = new QPushButton(uiText(QStringLiteral("open_folder"),
                                                   QStringLiteral("Open folder")),
                                            qthMapDialog);
        qthCenterMapButton = new QPushButton(uiText(QStringLiteral("center_map"),
                                                    QStringLiteral("Center")),
                                             qthMapDialog);
        QLabel *onlineUrlLabel = new QLabel(uiText(QStringLiteral("online_tiles_url"),
                                                   QStringLiteral("Online URL:")),
                                            qthMapDialog);
        qthOnlineTileUrlEdit = new QLineEdit(qthMapDialog);
        qthOnlineTileUrlEdit->setPlaceholderText(QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"));
        QLabel *apiKeyLabel = new QLabel(uiText(QStringLiteral("api_key_token"),
                                                QStringLiteral("API key/token:")),
                                         qthMapDialog);
        qthOnlineApiKeyEdit = new QLineEdit(qthMapDialog);
        qthOnlineApiKeyEdit->setPlaceholderText(uiText(QStringLiteral("api_key_optional_placeholder"),
                                                       QStringLiteral("Only for providers that require it")));
        qthOnlineApiKeyEdit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
        qthOnlineNoDiskCacheCheckbox =
            new QCheckBox(uiText(QStringLiteral("memory_only_tiles"),
                                 QStringLiteral("Memory only")),
                          qthMapDialog);
        qthOnlineNoDiskCacheCheckbox->setToolTip(uiText(
            QStringLiteral("memory_only_tiles_tooltip"),
            QStringLiteral("Do not store online tiles in the disk cache; useful for satellite providers.")));
        QLabel *onlineAttributionLabel = new QLabel(uiText(QStringLiteral("attribution"),
                                                           QStringLiteral("Attribution:")),
                                                    qthMapDialog);
        qthOnlineAttributionEdit = new QLineEdit(qthMapDialog);
        qthUseOsmButton = new QPushButton(uiText(QStringLiteral("apply_provider"),
                                                 QStringLiteral("Apply")),
                                          qthMapDialog);
        QLabel *searchLabel = new QLabel(uiText(QStringLiteral("map_search"),
                                                QStringLiteral("Search:")),
                                         qthMapDialog);
        qthMapSearchEdit = new QLineEdit(qthMapDialog);
        qthMapSearchEdit->setPlaceholderText(uiText(QStringLiteral("map_search_placeholder"),
                                                    QStringLiteral("QTH locator or lat, lon")));
        qthMapSearchButton = new QPushButton(uiText(QStringLiteral("find"),
                                                    QStringLiteral("Find")),
                                             qthMapDialog);
        qthMapStatusLabel = new QLabel(qthMapDialog);
        qthMapStatusLabel->setWordWrap(true);
        qthMapStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        mapControlLayout->addWidget(layerLabel, 0, 0);
        mapControlLayout->addWidget(qthMapLayerCombo, 0, 1);
        mapControlLayout->addWidget(overlayLabel, 0, 2);
        mapControlLayout->addWidget(qthMapOverlayCombo, 0, 3);
        mapControlLayout->addWidget(zoomLabel, 0, 4);
        mapControlLayout->addWidget(qthMapZoomSpin, 0, 5);
        mapControlLayout->addWidget(gridLabel, 0, 6);
        mapControlLayout->addWidget(qthGridPrecisionCombo, 0, 7);
        mapControlLayout->addWidget(tilesLabel, 1, 0);
        mapControlLayout->addWidget(qthTileDirectoryEdit, 1, 1, 1, 4);
        mapControlLayout->addWidget(qthSelectTilesButton, 1, 5);
        mapControlLayout->addWidget(qthOpenTilesButton, 1, 6, 1, 2);
        mapControlLayout->addWidget(providerLabel, 2, 0);
        mapControlLayout->addWidget(qthOnlineProviderCombo, 2, 1);
        mapControlLayout->addWidget(apiKeyLabel, 2, 2);
        mapControlLayout->addWidget(qthOnlineApiKeyEdit, 2, 3);
        mapControlLayout->addWidget(qthOnlineNoDiskCacheCheckbox, 2, 4);
        mapControlLayout->addWidget(qthUseOsmButton, 2, 5, 1, 3);
        mapControlLayout->addWidget(onlineUrlLabel, 3, 0);
        mapControlLayout->addWidget(qthOnlineTileUrlEdit, 3, 1, 1, 6);
        mapControlLayout->addWidget(qthCenterMapButton, 3, 7);
        mapControlLayout->addWidget(onlineAttributionLabel, 4, 0);
        mapControlLayout->addWidget(qthOnlineAttributionEdit, 4, 1, 1, 7);
        mapControlLayout->addWidget(searchLabel, 5, 0);
        mapControlLayout->addWidget(qthMapSearchEdit, 5, 1, 1, 6);
        mapControlLayout->addWidget(qthMapSearchButton, 5, 7);
        rootLayout->addLayout(mapControlLayout);
        rootLayout->addWidget(qthMapStatusLabel);

        qthMapWidget = new QthMapWidget(qthMapDialog);
        qthMapWidget->setUiLanguage(uiLanguage);
        qthMapWidget->setPosition(qthLatitude, qthLongitude);
        qthMapWidget->setPositionVisible(qthPositionVisible);
        qthMapWidget->setTileLayerMode(qthMapLayer == 1
                                           ? QthMapWidget::TileLayerMode::LocalXyz
                                           : (qthMapLayer == 2
                                                  ? QthMapWidget::TileLayerMode::OnlineXyz
                                                  : QthMapWidget::TileLayerMode::GridOnly));
        qthMapWidget->setTileDirectory(qthTileDirectory);
        qthMapWidget->setOnlineTileTemplate(resolvedQthOnlineTileUrlTemplate());
        qthMapWidget->setOnlineAttribution(qthOnlineAttribution);
        qthMapWidget->setOnlineDiskCacheEnabled(!qthOnlineNoDiskCache);
        qthMapWidget->setMapZoom(qthMapZoom);
        qthMapWidget->setGridPrecision(qthGridPrecision);
        qthMapWidget->setOverlayMode(static_cast<QthMapWidget::OverlayMode>(qthMapOverlayMode));
        qthMapWidget->setUserMarkers(qthUserMarkers);
        updateQthMapSatelliteOverlay();
        rootLayout->addWidget(qthMapWidget, 1);

        QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, qthMapDialog);
        QPushButton *openOsmButton =
            buttonBox->addButton(uiText(QStringLiteral("open_osm"),
                                        QStringLiteral("Open OSM")),
                                 QDialogButtonBox::ActionRole);
        QPushButton *copyButton =
            buttonBox->addButton(uiText(QStringLiteral("copy_qth"),
                                        QStringLiteral("Copy QTH")),
                                 QDialogButtonBox::ActionRole);
        QPushButton *clearQthButton =
            buttonBox->addButton(uiText(QStringLiteral("clear_qth"),
                                        QStringLiteral("Clear")),
                                 QDialogButtonBox::ActionRole);
        if (QPushButton *closeButton = buttonBox->button(QDialogButtonBox::Close)) {
            closeButton->setText(uiText(QStringLiteral("close"), QStringLiteral("Close")));
        }
        rootLayout->addWidget(buttonBox);

        connect(qthMapLayerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), qthMapDialog, [this](int) {
            qthMapLayer = qthMapLayerCombo ? qthMapLayerCombo->currentData().toInt() : 0;
            updateQthControls();
            savePersistentSettings();
        });
        connect(qthOnlineProviderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), qthMapDialog, [this](int) {
            const QString providerId =
                qthOnlineProviderCombo ? qthOnlineProviderCombo->currentData().toString() : QStringLiteral("custom");
            applyQthOnlineProviderPreset(providerId, true);
            qthMapLayer = 2;
            updateQthControls();
            savePersistentSettings();
        });
        connect(qthMapZoomSpin, QOverload<int>::of(&QSpinBox::valueChanged), qthMapDialog, [this](int value) {
            qthMapZoom = value;
            if (qthMapWidget) {
                qthMapWidget->setMapZoom(qthMapZoom);
            }
            updateQthMapControls();
            savePersistentSettings();
        });
        connect(qthGridPrecisionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), qthMapDialog, [this](int) {
            qthGridPrecision = qthGridPrecisionCombo ? qthGridPrecisionCombo->currentData().toInt() : 6;
            if (qthMapWidget) {
                qthMapWidget->setGridPrecision(qthGridPrecision);
            }
            updateQthMapControls();
            savePersistentSettings();
        });
        connect(qthMapOverlayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), qthMapDialog, [this](int) {
            qthMapOverlayMode = qthMapOverlayCombo ? qthMapOverlayCombo->currentData().toInt() : 1;
            qthMapOverlayMode = (std::clamp)(qthMapOverlayMode, 0, 3);
            if (qthMapWidget) {
                qthMapWidget->setOverlayMode(static_cast<QthMapWidget::OverlayMode>(qthMapOverlayMode));
            }
            updateQthMapControls();
            savePersistentSettings();
        });
        connect(qthTileDirectoryEdit, &QLineEdit::editingFinished, qthMapDialog, [this]() {
            qthTileDirectory = qthTileDirectoryEdit
                                   ? QDir::fromNativeSeparators(qthTileDirectoryEdit->text().trimmed())
                                   : QString();
            if (!qthTileDirectory.isEmpty()) {
                qthMapLayer = 1;
    }
            updateQthControls();
            savePersistentSettings();
        });
        connect(qthOnlineTileUrlEdit, &QLineEdit::editingFinished, qthMapDialog, [this]() {
            qthOnlineTileUrlTemplate = qthOnlineTileUrlEdit
                                           ? qthOnlineTileUrlEdit->text().trimmed()
                                           : QString();
            qthOnlineProviderId = QStringLiteral("custom");
            if (!qthOnlineTileUrlTemplate.isEmpty()) {
                qthMapLayer = 2;
            }
            updateQthControls();
            savePersistentSettings();
        });
        connect(qthOnlineAttributionEdit, &QLineEdit::editingFinished, qthMapDialog, [this]() {
            qthOnlineAttribution = qthOnlineAttributionEdit
                                       ? qthOnlineAttributionEdit->text().trimmed()
                                       : QString();
            updateQthControls();
            savePersistentSettings();
        });
        connect(qthOnlineApiKeyEdit, &QLineEdit::editingFinished, qthMapDialog, [this]() {
            qthOnlineApiKey = qthOnlineApiKeyEdit ? qthOnlineApiKeyEdit->text().trimmed() : QString();
            if (!qthOnlineTileUrlTemplate.trimmed().isEmpty()) {
                qthMapLayer = 2;
            }
            updateQthControls();
            savePersistentSettings();
        });
        connect(qthOnlineNoDiskCacheCheckbox, &QCheckBox::toggled, qthMapDialog, [this](bool checked) {
            qthOnlineNoDiskCache = checked;
            updateQthControls();
            savePersistentSettings();
        });
        connect(qthUseOsmButton, &QPushButton::clicked, qthMapDialog, [this]() {
            const QString providerId =
                qthOnlineProviderCombo ? qthOnlineProviderCombo->currentData().toString() : qthOnlineProviderId;
            applyQthOnlineProviderPreset(providerId, true);
            qthMapLayer = 2;
            updateQthControls();
            savePersistentSettings();
        });
        connect(qthSelectTilesButton, &QPushButton::clicked, this, &YourClassName::selectQthTileDirectory);
        connect(qthOpenTilesButton, &QPushButton::clicked, this, &YourClassName::openQthTileDirectory);
        connect(qthMapSearchEdit, &QLineEdit::returnPressed, this, &YourClassName::applyQthMapSearch);
        connect(qthMapSearchButton, &QPushButton::clicked, this, &YourClassName::applyQthMapSearch);
        connect(qthCenterMapButton, &QPushButton::clicked, qthMapDialog, [this]() {
            if (qthMapWidget) {
                qthMapWidget->centerOnPosition();
            }
        });
        connect(qthMapWidget, &QthMapWidget::mapZoomChanged, qthMapDialog, [this](int zoom) {
            qthMapZoom = zoom;
            updateQthMapControls();
            savePersistentSettings();
        });
        connect(qthMapWidget, &QthMapWidget::userMarkersChanged, qthMapDialog, [this]() {
            if (qthMapWidget) {
                qthUserMarkers = qthMapWidget->userMarkerList();
            }
            savePersistentSettings();
        });
        connect(qthMapWidget,
                &QthMapWidget::userMarkerAdded,
                qthMapDialog,
                [this](double markerLat, double markerLon, const QString &markerLabel, int) {
                    if (qthMapStatusLabel) {
                        qthMapStatusLabel->setText(
                            uiText(QStringLiteral("qth_custom_marker_added"),
                                   QStringLiteral("Marker: %1  %2, %3"))
                                .arg(markerLabel,
                                     QString::number(markerLat, 'f', 6),
                                     QString::number(markerLon, 'f', 6)));
                    }
                });
        connect(qthMapWidget,
                &QthMapWidget::userMarkerRemoved,
                qthMapDialog,
                [this](double markerLat, double markerLon, const QString &markerLabel, int) {
                    if (qthMapStatusLabel) {
                        qthMapStatusLabel->setText(
                            uiText(QStringLiteral("qth_custom_marker_removed"),
                                   QStringLiteral("Marker removed: %1  %2, %3"))
                                .arg(markerLabel,
                                     QString::number(markerLat, 'f', 6),
                                     QString::number(markerLon, 'f', 6)));
                    }
                });
        connect(qthMapWidget, &QthMapWidget::currentPositionCleared, qthMapDialog, [this]() {
            clearQthPosition();
        });
        connect(openOsmButton, &QPushButton::clicked, qthMapDialog, [this]() {
            const QString lat = QString::number(qthLatitude, 'f', 6);
            const QString lon = QString::number(qthLongitude, 'f', 6);
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=12/%1/%2")
                                               .arg(lat, lon)));
        });
        connect(copyButton, &QPushButton::clicked, this, &YourClassName::copyQthLocator);
        connect(clearQthButton, &QPushButton::clicked, this, &YourClassName::clearQthPosition);
        connect(buttonBox, &QDialogButtonBox::rejected, qthMapDialog, &QDialog::close);
        connect(qthMapDialog, &QObject::destroyed, this, [this]() {
            qthMapDialog = nullptr;
            qthMapWidget = nullptr;
            qthMapLayerCombo = nullptr;
            qthMapOverlayCombo = nullptr;
            qthMapZoomSpin = nullptr;
            qthGridPrecisionCombo = nullptr;
            qthTileDirectoryEdit = nullptr;
            qthOnlineProviderCombo = nullptr;
            qthOnlineTileUrlEdit = nullptr;
            qthOnlineAttributionEdit = nullptr;
            qthOnlineApiKeyEdit = nullptr;
            qthOnlineNoDiskCacheCheckbox = nullptr;
            qthSelectTilesButton = nullptr;
            qthOpenTilesButton = nullptr;
            qthCenterMapButton = nullptr;
            qthUseOsmButton = nullptr;
            qthMapSearchEdit = nullptr;
            qthMapSearchButton = nullptr;
            qthMapStatusLabel = nullptr;
        });
    }

    updateQthControls();
    qthMapDialog->show();
    qthMapDialog->raise();
    if (!QGuiApplication::platformName().contains(QStringLiteral("wayland"), Qt::CaseInsensitive)) {
        qthMapDialog->activateWindow();
    }
}

void YourClassName::applyQthMapSearch() {
    if (!qthMapWidget || !qthMapSearchEdit) {
        return;
    }

    const QString query = qthMapSearchEdit->text().trimmed();
    if (query.isEmpty()) {
        if (qthMapStatusLabel) {
            qthMapStatusLabel->setText(uiText(QStringLiteral("qth_search_not_found"),
                                             QStringLiteral("Search: enter a QTH locator or lat, lon")));
        }
        return;
    }

    bool found = false;
    double searchLat = 0.0;
    double searchLon = 0.0;
    QString label;

    const qth::GeoBounds bounds = qth::maidenheadBounds(query);
    if (bounds.valid) {
        searchLat = (bounds.minLatitude + bounds.maxLatitude) * 0.5;
        searchLon = (bounds.minLongitude + bounds.maxLongitude) * 0.5;
        label = query.trimmed().toUpper();
        found = true;
    }

    if (!found) {
        const QRegularExpression coordRegex(
            QStringLiteral("^\\s*([+-]?\\d+(?:[\\.,]\\d+)?)\\s*[,;\\s]+\\s*([+-]?\\d+(?:[\\.,]\\d+)?)\\s*$"));
        const QRegularExpressionMatch match = coordRegex.match(query);
        if (match.hasMatch()) {
            bool latOk = false;
            bool lonOk = false;
            searchLat = QString(match.captured(1)).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&latOk);
            searchLon = QString(match.captured(2)).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&lonOk);
            found = latOk && lonOk && qth::isValidLatitude(searchLat) && qth::isValidLongitude(searchLon);
            if (found) {
                label = qth::maidenheadLocator(searchLat, searchLon, 6);
            }
        }
    }

    if (!found) {
        if (qthMapStatusLabel) {
            qthMapStatusLabel->setText(uiText(QStringLiteral("qth_search_not_found"),
                                             QStringLiteral("Search: enter a QTH locator or lat, lon")));
        }
        return;
    }

    qthMapWidget->setSearchMarker(searchLat, searchLon, label);
    qthMapWidget->centerOn(searchLat, searchLon);
    if (qthMapStatusLabel) {
        qthMapStatusLabel->setText(
            uiText(QStringLiteral("qth_search_result"),
                   QStringLiteral("Search: %1  %2, %3"))
                .arg(label,
                     QString::number(searchLat, 'f', 6),
                     QString::number(searchLon, 'f', 6)));
    }
}

void YourClassName::copyQthLocator() {
    if (!qthPositionVisible) {
        if (qthSource == QStringLiteral("nmea") &&
            gnssSerialFixCount <= 0 &&
            gnssLastUbxFixMs <= 0) {
            if (qthStatusLabel) {
                qthStatusLabel->setText(uiText(QStringLiteral("qth_position_waiting_for_fix"),
                                               QStringLiteral("QTH position is hidden until a fresh GNSS fix is received.")));
            }
            return;
        }
        if (qth::isValidLatitude(qthLatitude) && qth::isValidLongitude(qthLongitude)) {
            qthPositionVisible = true;
            updateQthControls();
            if (qthMapWidget) {
                qthMapWidget->setPositionVisible(true);
                qthMapWidget->centerOn(qthLatitude, qthLongitude);
            }
            savePersistentSettings();
        } else {
            if (qthStatusLabel) {
                qthStatusLabel->setText(uiText(QStringLiteral("qth_copy_no_position"),
                                               QStringLiteral("QTH copy skipped: current position is cleared.")));
            }
            return;
        }
    }
    const QString locator = qth::maidenheadLocator(qthLatitude, qthLongitude, 6);
    if (QClipboard *clipboard = QApplication::clipboard()) {
        clipboard->setText(locator);
    }
    if (qthStatusLabel) {
        qthStatusLabel->setText(uiText(QStringLiteral("qth_copied"),
                                       QStringLiteral("QTH copied: %1"))
                                    .arg(locator));
    }
}

void YourClassName::updateGnssSystemSelection() {
    if (gnssSystemCombo) {
        const QString selected = gnssSystemCombo->currentData().toString().trimmed();
        if (!selected.isEmpty()) {
            gnssSystemId = gnssSystemPreset(selected).id;
        }
    }
    const GnssSystemPreset preset = gnssSystemPreset(gnssSystemId);
    gnssSystemId = preset.id;

    if (gnssAcquireStatusLabel) {
        const QString systemName = uiText(preset.textKey, preset.fallbackName);
        const QString status = preset.acquisitionKind != GnssAcquisitionKind::None
                                   ? uiText(QStringLiteral("gnss_acq_selected_gps"),
                                            QStringLiteral("Acquisition selected: %1, correlator ready"))
                                         .arg(systemName)
                                   : uiText(QStringLiteral("gnss_acq_selected_planned"),
                                            QStringLiteral("Acquisition selected: %1, parser planned; RF tune/scan and IQ logging are ready"))
                                         .arg(systemName);
        gnssAcquireStatusLabel->setText(status);
    }
}

void YourClassName::applyGnssSystemPresetToReceiver(const QString &systemId) {
    const GnssSystemPreset preset = gnssSystemPreset(systemId);
    gnssSystemId = preset.id;
    if (gnssSystemCombo) {
        QSignalBlocker blocker(gnssSystemCombo);
        const int index = gnssSystemCombo->findData(gnssSystemId);
        if (index >= 0) {
            gnssSystemCombo->setCurrentIndex(index);
        }
    }

    const RadioSettings previousSettings = pendingSettings;
    pendingSettings.inputMode = INPUT_RF;
    pendingSettings.centerFrequency = preset.centerHz;
    pendingSettings.actualFrequency = preset.centerHz;
    pendingSettings.listeningFrequency = preset.targetHz > 0.0 ? preset.targetHz : preset.centerHz;
    pendingSettings.bandwidth = preset.bandwidthHz > 0.0 ? preset.bandwidthHz : GNSS_RAW_BANDWIDTH_HZ;

    if (isIdle() && sampleBox) {
        int bestIndex = -1;
        double bestRate = 0.0;
        for (int i = 0; i < sampleBox->count(); ++i) {
            bool ok = false;
            const double rate = sampleBox->itemData(i).toDouble(&ok);
            if (ok &&
                rate > bestRate &&
                rate <= GNSS_USEFUL_STANDARD_SPAN_HZ + 0.5) {
                bestRate = rate;
                bestIndex = i;
            }
        }
        if (bestIndex >= 0 && std::abs(pendingSettings.sampleRate - bestRate) > 0.5) {
            pendingSettings.sampleRate = bestRate;
            QSignalBlocker blocker(sampleBox);
            sampleBox->setCurrentIndex(bestIndex);
        }
    }

    normalizeTuning(pendingSettings);
    publishSettingsToGlobals();
    updateUiFromPendingSettings();

    if (runState == RadioRunState::Running && hasActiveFobosDevice()) {
        applyCenterFrequencyToHardwareIfNeeded(previousSettings, "GNSS system preset");
        updateIqFrameProducerSettings();
    }
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
    }

    const QString systemName = uiText(preset.textKey, preset.fallbackName);
    if (qthStatusLabel) {
        qthStatusLabel->setText(
            uiText(QStringLiteral("gnss_system_tuned"),
                   QStringLiteral("%1 tuned: center %2 MHz, target %3 MHz, bandwidth %4 MHz."))
                .arg(systemName,
                     QString::number(pendingSettings.centerFrequency / 1000000.0, 'f', 6),
                     QString::number(pendingSettings.listeningFrequency / 1000000.0, 'f', 6),
                     QString::number(pendingSettings.bandwidth / 1000000.0, 'f', 3)));
    }
    qDebug() << "[GNSS] system preset"
             << "system" << preset.id
             << "name" << systemName
             << "centerHz" << pendingSettings.centerFrequency
             << "targetHz" << pendingSettings.listeningFrequency
             << "bandwidthHz" << pendingSettings.bandwidth
             << "sampleRate" << pendingSettings.sampleRate
             << "qth" << qth::maidenheadLocator(qthLatitude, qthLongitude, 6);
    savePersistentSettings();
}

void YourClassName::tuneGnssL1Preset() {
    updateGnssSystemSelection();
    applyGnssSystemPresetToReceiver(gnssSystemId);
}

void YourClassName::applyGnssScanPreset() {
    updateGnssSystemSelection();
    const GnssSystemPreset preset = gnssSystemPreset(gnssSystemId);
    const RadioSettings previousSettings = pendingSettings;
    const QString systemName = uiText(preset.textKey, preset.fallbackName);
    const QString agileName = QStringLiteral("GNSS %1 agile").arg(systemName);
    const QString standardName = QStringLiteral("GNSS %1 standard").arg(systemName);
    const QString listeningName = QStringLiteral("GNSS %1 listening").arg(systemName);
    agileScanPresets[agileName] =
        agileScanPresetSpec(preset.agileScanRangesMhz, GNSS_USEFUL_STANDARD_SPAN_HZ / 1000000.0);
    if (preset.id != QStringLiteral("all_l1")) {
        standardScanPresets[standardName] =
            standardScanPresetSpec(preset.standardScanCentersMhz,
                                   (std::clamp)(preset.standardDwellMs,
                                                STANDARD_SCAN_MIN_DWELL_MS,
                                                STANDARD_SCAN_MAX_DWELL_MS),
                                   (std::clamp)(preset.standardSettleMs,
                                                STANDARD_SCAN_MIN_SETTLE_MS,
                                                STANDARD_SCAN_MAX_SETTLE_MS));
    }

    agileScanRangesMhz = agileScanPresetRanges(agileScanPresets.value(agileName));
    agileScanStepMhz = agileScanPresetStepMhz(agileScanPresets.value(agileName), 50.0);
    if (standardScanPresets.contains(standardName)) {
        standardScanCentersMhz = standardScanPresetCenters(standardScanPresets.value(standardName));
        standardScanDwellMs = standardScanPresetDwellMs(standardScanPresets.value(standardName), standardScanDwellMs);
        standardScanSettleMs = standardScanPresetSettleMs(standardScanPresets.value(standardName), standardScanSettleMs);
    } else {
        standardScanCentersMhz = preset.standardScanCentersMhz;
        standardScanDwellMs = (std::clamp)(preset.standardDwellMs,
                                           STANDARD_SCAN_MIN_DWELL_MS,
                                           STANDARD_SCAN_MAX_DWELL_MS);
        standardScanSettleMs = (std::clamp)(preset.standardSettleMs,
                                            STANDARD_SCAN_MIN_SETTLE_MS,
                                            STANDARD_SCAN_MAX_SETTLE_MS);
    }
    const QStringList rangeParts = preset.agileScanRangesMhz.split(QLatin1Char('-'));
    if (rangeParts.size() == 2) {
        standardScanRangeStartMhz = rangeParts.at(0).trimmed();
        standardScanRangeEndMhz = rangeParts.at(1).trimmed();
    }
    pendingSettings.inputMode = INPUT_RF;
    pendingSettings.centerFrequency = preset.centerHz;
    pendingSettings.actualFrequency = preset.centerHz;
    pendingSettings.listeningFrequency = preset.targetHz > 0.0 ? preset.targetHz : preset.centerHz;
    pendingSettings.bandwidth = preset.bandwidthHz > 0.0 ? preset.bandwidthHz : GNSS_RAW_BANDWIDTH_HZ;
    if (preset.id == QStringLiteral("all_l1")) {
        const QString targets = QStringLiteral("1561.098, 1575.420, 1602.000");
        listeningScanPresets[listeningName] =
            listeningScanPresetSpec(targets, preset.standardDwellMs, preset.standardSettleMs);
        listeningScanTargetsMhz = targets;
        listeningScanDwellMs = (std::clamp)(preset.standardDwellMs,
                                            LISTENING_SCAN_MIN_DWELL_MS,
                                            LISTENING_SCAN_MAX_DWELL_MS);
        listeningScanSettleMs = (std::clamp)(preset.standardSettleMs,
                                             LISTENING_SCAN_MIN_SETTLE_MS,
                                             LISTENING_SCAN_MAX_SETTLE_MS);
        listeningScanEnabled = true;
        standardScanEnabled = false;
        agileScanEnabled = false;
    } else {
        listeningScanPresets[listeningName] =
            listeningScanPresetSpec(QString::number(preset.targetHz / 1000000.0, 'f', 6),
                                    preset.standardDwellMs,
                                    preset.standardSettleMs);
    }

    if (isIdle() && sampleBox) {
        int bestIndex = -1;
        double bestRate = 0.0;
        for (int i = 0; i < sampleBox->count(); ++i) {
            bool ok = false;
            const double rate = sampleBox->itemData(i).toDouble(&ok);
            if (ok &&
                rate > bestRate &&
                rate <= GNSS_USEFUL_STANDARD_SPAN_HZ + 0.5) {
                bestRate = rate;
                bestIndex = i;
            }
        }
        if (bestIndex >= 0 && std::abs(pendingSettings.sampleRate - bestRate) > 0.5) {
            pendingSettings.sampleRate = bestRate;
            QSignalBlocker blocker(sampleBox);
            sampleBox->setCurrentIndex(bestIndex);
        }
    }

    normalizeStandardScanCentersUi(false);
    normalizeTuning(pendingSettings);
    publishSettingsToGlobals();
    updateUiFromPendingSettings();
    updateFrequencyPresetControls();
    if (runState == RadioRunState::Running && hasActiveFobosDevice()) {
        applyCenterFrequencyToHardwareIfNeeded(previousSettings, "GNSS scan preset");
        updateIqFrameProducerSettings();
        applyListeningScanSettings(false);
    } else if (listeningScanEnabled) {
        applyListeningScanSettings(false);
    }
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
    }
    if (qthStatusLabel) {
        qthStatusLabel->setText(
            uiText(QStringLiteral("gnss_scan_applied"),
                   QStringLiteral("%1 scan preset applied: centers %2, listening targets %3, dwell %4 ms. Use <=50 MHz sample rate on standard firmware."))
                .arg(systemName,
                     standardScanCentersMhz,
                     listeningScanTargetsMhz,
                     QString::number(listeningScanDwellMs)));
    }
    qDebug() << "[GNSS] scan preset"
             << "system" << preset.id
             << "name" << systemName
             << "agileRanges" << agileScanRangesMhz
             << "agileStepMhz" << agileScanStepMhz
             << "standardCenters" << standardScanCentersMhz
             << "standardDwellMs" << standardScanDwellMs
             << "standardSettleMs" << standardScanSettleMs
             << "listeningTargets" << listeningScanTargetsMhz
             << "listeningEnabled" << listeningScanEnabled
             << "sampleRate" << pendingSettings.sampleRate;
    savePersistentSettings();
}
