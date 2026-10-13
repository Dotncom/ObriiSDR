#ifndef PROPAGATIONLABDIALOG_H
#define PROPAGATIONLABDIALOG_H
#include "radiopropagation.h"
#include <QDialog>
#include <QPointer>
#include <QHash>
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSplitter;
class QTabWidget;
class QThread;
class QTimer;
class QthMapWidget;
class PropagationProfileWidget;
namespace propagation { class MapTilerTerrain; }
class PropagationLabDialog : public QDialog {
    Q_OBJECT
public:
    explicit PropagationLabDialog(bool ukrainian, QWidget *parent = nullptr,
                                  const QString &settingsFile = QString());
    ~PropagationLabDialog() override;
    void setReferenceMarkers(const QVector<qth::UserMarker> &markers, const QString &tileDirectory);
    void setUiTranslator(std::function<QString(const QString &, const QString &)> translate) { uiTranslator = std::move(translate); }
    void setOnlineMapSource(const QString &providerName, const QString &resolvedTemplate,
                            const QString &attribution, int layerMode, const QString &mapTilerKey = QString());
protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QString text(const char *en, const char *ua) const;
    void invalidate();
    void updateMarkers();
    void updateReferenceActions();
    void updateEndpointElevations();
    void cancelEndpointElevationRequest();
    void updateMapSource();
    void calculate(bool coverage);
    void updateRaster();
    void showProfile();
    void exportProfile();
    void setPoint(bool tx, const qth::GeoPosition &point);
    void swapEndpoints();
    void loadSettings();
    void saveSettings();
    void scheduleSave();
    bool uk;
    QString settingsPath;
    bool settingsReady = false, mapSourceRestored = false, mapProviderInitialized = false;
    QTimer *saveTimer = nullptr;
    QTimer *elevationTimer = nullptr;
    QVector<QWidget *> settingsControls;
    QVector<QPair<QDoubleSpinBox *, double propagation::Parameters::*>> parameterSpins;
    QDoubleSpinBox *txLat = nullptr, *txLon = nullptr, *rxLat = nullptr, *rxLon = nullptr;
    QDoubleSpinBox *rxColorMinimum = nullptr, *rxColorMaximum = nullptr;
    QDoubleSpinBox *marginColorMinimum = nullptr, *marginColorMaximum = nullptr;
    QSplitter *splitter = nullptr;
    propagation::Parameters parameters;
    propagation::CoverageResult result;
    std::atomic_bool cancel{false};
    QThread *worker = nullptr;
    QVector<qth::UserMarker> referenceMarkers;
    bool txPresent = true, rxPresent = true;
    QString savedTileTemplate, savedAttribution, onlineTerrainKey;
    std::shared_ptr<propagation::MapTilerTerrain> loadedOnlineTerrain;
    QHash<QString, double> endpointElevationCache;
    std::shared_ptr<std::atomic_bool> endpointRequestCancel;
    QString pendingEndpointRequest;
    QthMapWidget *map = nullptr;
    QComboBox *picking = nullptr, *reference = nullptr, *layer = nullptr, *tiles = nullptr, *terrainSource = nullptr;
    QCheckBox *online = nullptr, *onlineTerrainPermission = nullptr;
    QLineEdit *directory = nullptr;
    QSlider *opacity = nullptr;
    QTabWidget *controls = nullptr;
    QLabel *status = nullptr, *legend = nullptr, *profileStatus = nullptr, *distanceLabel = nullptr;
    QLabel *txElevationLabel = nullptr, *rxElevationLabel = nullptr;
    QProgressBar *progress = nullptr;
    QPushButton *coverageButton = nullptr, *pathButton = nullptr, *cancelButton = nullptr;
    QPushButton *swapButton = nullptr;
    QPushButton *useTxButton = nullptr, *useRxButton = nullptr;
    QDialog *profileDialog = nullptr;
    QPointer<QDialog> hfDialog;
    std::function<QString(const QString &, const QString &)> uiTranslator;
    PropagationProfileWidget *profile = nullptr;
};
#endif
