#include "main.h"
#include "wirelesslabdialog.h"

void YourClassName::openWirelessLab() {
    if (!wirelessLabDialog) {
        wirelessLabDialog = new WirelessLabDialog(
            normalizedUiLanguage(uiLanguage) == QStringLiteral("uk"), this);
        connect(wirelessLabDialog, &WirelessLabDialog::frequencyRequested,
                this, [this](double frequency) {
            if (frequencyControl) {
                frequencyControl->setValueHz(frequency);
                onFrequencyEntered();
            }
        });
    }
    wirelessLabDialog->show();
    wirelessLabDialog->raise();
    wirelessLabDialog->activateWindow();
}
