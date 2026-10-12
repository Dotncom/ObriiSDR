#include "main.h"
#include "gnsssdrpvtdialog.h"
#include "lrptdecodedialog.h"

void YourClassName::showGnssSdrPvt() {
    if (!gnssSdrPvtDialog) {
        gnssSdrPvtDialog = new GnssSdrPvtDialog([this](const QString &key, const QString &fallback) {
            return uiText(key, fallback);
        }, this);
        gnssSdrPvtDialog->setAttribute(Qt::WA_DeleteOnClose);
        auto *timer = new QTimer(gnssSdrPvtDialog);
        const auto update = [this] {
            if (!gnssSdrPvtDialog) return;
            const bool allowed = (!gnssSerialPort || !gnssSerialPort->isOpen()) &&
                (!agileScanCheckbox || !agileScanCheckbox->isChecked()) &&
                (!standardScanCheckbox || !standardScanCheckbox->isChecked());
            gnssSdrPvtDialog->setReceiverContext(runState == RadioRunState::Running && processor && processor->isRunning(),
                                                allowed, pendingSettings.sampleRate, pendingSettings.centerFrequency);
        };
        connect(timer, &QTimer::timeout, gnssSdrPvtDialog, update); timer->start(200); update();
        connect(gnssSdrPvtDialog, &GnssSdrPvtDialog::mapSourceStarted, this, [this] {
            gnssNmeaSatellites.clear(); gnssNmeaSatelliteEnabled.clear();
            gnssSerialSentenceCount = 0; gnssSerialFixCount = 0;
        });
        connect(gnssSdrPvtDialog, &GnssSdrPvtDialog::positionSentence, this, [this](const QString &line) {
            if (!gnssSerialPort || !gnssSerialPort->isOpen()) processGnssNmeaLine(line, false, true);
        });
    }
    gnssSdrPvtDialog->show(); gnssSdrPvtDialog->raise();
}

void YourClassName::showLrptDecoder() {
    if (!lrptDecodeDialog) {
        lrptDecodeDialog = new LrptDecodeDialog([this](const QString &key, const QString &fallback) {
            return uiText(key, fallback);
        }, this);
        lrptDecodeDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    lrptDecodeDialog->show(); lrptDecodeDialog->raise();
}
