#include "main.h"

#include "ft8stationdialog.h"
#include "transmitdialog.h"
#include "qthmapwidget.h"
#include <algorithm>

void YourClassName::openFt8StationDialog() {
    if (!ft8StationDialog) {
        ft8StationDialog = new Ft8StationDialog(uiLanguage, this);
        connect(ft8StationDialog, &Ft8StationDialog::transmitterSetupRequested,
                this, &YourClassName::openTransmitDialog);
        connect(ft8StationDialog, &Ft8StationDialog::mapExportRequested, this,
                [this](const QVector<qth::UserMarker> &stations) {
            int number = 0;
            for (const auto &marker : qthUserMarkers) number = (std::max)(number, marker.number);
            for (auto station : stations) {
                auto existing = std::find_if(qthUserMarkers.begin(), qthUserMarkers.end(),
                    [&](const qth::UserMarker &marker) {
                        return marker.name.compare(station.name, Qt::CaseInsensitive) == 0 &&
                               marker.description.startsWith(QStringLiteral("FT8 Maidenhead:"));
                    });
                if (existing != qthUserMarkers.end()) {station.number = existing->number; *existing = station;}
                else {station.number = ++number; qthUserMarkers.push_back(station);}
            }
            openQthMapWindow();
            if (qthMapWidget) {
                qthMapWidget->setUserMarkers(qthUserMarkers);
                if (!stations.isEmpty()) qthMapWidget->centerOn(stations.front().latitude, stations.front().longitude);
            }
            savePersistentSettings();
        });
        connect(ft8StationDialog, &Ft8StationDialog::transmitMessageRequested, this,
                [this](const QString &message, double dialFrequencyHz,
                       double audioFrequencyHz, int slotParity) {
            if (!transmitDialog) transmitDialog = new TransmitDialog(uiLanguage, this);
            transmitDialog->setHackRfReceiverUseChecker([this](int deviceIndex) {
                return runState != RadioRunState::Idle &&
                       isHackRfNativeSelected() &&
                       selectedHackRfNativeIndex() == deviceIndex;
            });
            transmitDialog->setLanguage(uiLanguage);
            transmitDialog->prepareFt8Message(message, dialFrequencyHz,
                                              audioFrequencyHz, slotParity);
            transmitDialog->show();
            transmitDialog->raise();
            transmitDialog->activateWindow();
        });
        ft8StationDialog->setAutomaticTransmitHandler(
            [this](const QString &message, double dialFrequencyHz,
                   double audioFrequencyHz, int slotParity, QString *error) {
                if (!transmitDialog) transmitDialog = new TransmitDialog(uiLanguage, this);
                transmitDialog->setHackRfReceiverUseChecker([this](int deviceIndex) {
                    return runState != RadioRunState::Idle &&
                           isHackRfNativeSelected() &&
                           selectedHackRfNativeIndex() == deviceIndex;
                });
                transmitDialog->setLanguage(uiLanguage);
                transmitDialog->prepareFt8Message(message, dialFrequencyHz,
                                                  audioFrequencyHz, slotParity);
                const bool started = transmitDialog->startPreparedFt8Automatically(error);
                if (!started) {
                    transmitDialog->show();
                    transmitDialog->raise();
                    transmitDialog->activateWindow();
                }
                return started;
            });
        ft8StationDialog->setAutomaticCancelHandler([this]() {
            if (transmitDialog) transmitDialog->cancelAutomaticFt8();
        });
    }
    ft8StationDialog->setLanguage(uiLanguage);
    ft8StationDialog->setDialFrequency(pendingSettings.listeningFrequency);
    ft8StationDialog->show();
    ft8StationDialog->raise();
    ft8StationDialog->activateWindow();
}

void YourClassName::onFt8Decoded(const QDateTime &utc,
                                 double rfFrequencyHz,
                                 double audioFrequencyHz,
                                 double dtSeconds,
                                 int snrDbEstimate,
                                 double qualityDb,
                                 int costasHits,
                                 const QString &message) {
    if (!ft8StationDialog) return;
    ft8StationDialog->addDecode(utc, rfFrequencyHz, audioFrequencyHz,
                                dtSeconds, snrDbEstimate, qualityDb,
                                costasHits, message);
}
