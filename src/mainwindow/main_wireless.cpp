#include "main.h"
#include "wirelesslabdialog.h"
#include "dataprocessor.h"

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
        connect(wirelessLabDialog, &WirelessLabDialog::audioEnabledRequested,
                audioCheckbox, &QCheckBox::setChecked);
        connect(audioCheckbox, &QCheckBox::toggled,
                wirelessLabDialog, &WirelessLabDialog::setAudioEnabled);
        connect(wirelessLabDialog, &WirelessLabDialog::liveIqActiveChanged,
                this, [this](bool active) {
            if (processor) processor->setWifiIqStreamingEnabled(active);
        });
        connect(wirelessLabDialog, &WirelessLabDialog::spectrumUpdateIntervalRequested,
                this, [this](int intervalMs) {
            spectrumUpdateIntervalMs = (std::clamp)(intervalMs,
                SPECTRUM_UPDATE_AUTO_MS, SPECTRUM_UPDATE_MAX_MS);
            updateSpectrumTimerInterval();
            savePersistentSettings();
        });
    }
    if (processor) {
        connect(processor, &DataProcessor::wifiIqFrameReady,
                wirelessLabDialog, &WirelessLabDialog::appendLiveIq,
                static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::UniqueConnection));
        processor->setWifiIqStreamingEnabled(wirelessLabDialog->isLiveIqActive());
    }
    wirelessLabDialog->setAudioEnabled(audioCheckbox->isChecked());
    wirelessLabDialog->setAudioControlEnabled(audioCheckbox->isEnabled());
    wirelessLabDialog->setSpectrumUpdateInterval(spectrumUpdateIntervalMs);
    wirelessLabDialog->setLiveIqSource(pendingSettings.sampleRate,
        pendingSettings.centerFrequency,
        runState == RadioRunState::Running && pendingSettings.inputMode == INPUT_RF &&
            !isNetworkClientMode());
    wirelessLabDialog->show();
    wirelessLabDialog->raise();
    wirelessLabDialog->activateWindow();
}
