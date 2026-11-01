#include "main.h"
#include "appsettingsutils.h"
#include "diagnosticlogging.h"
#include "remotevfoservice.h"
#include "remotewebserver.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

struct NetworkClientPreset {
    QString name;
    QString address;
    int port = 21090;
};

QVector<NetworkClientPreset> loadNetworkClientPresets() {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    QVector<NetworkClientPreset> presets;
    const int count = settings.beginReadArray(QStringLiteral("networkClientPresets"));
    presets.reserve(count);
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        NetworkClientPreset preset;
        preset.name = settings.value(QStringLiteral("name")).toString().trimmed();
        preset.address = settings.value(QStringLiteral("address")).toString().trimmed();
        preset.port = (std::clamp)(settings.value(QStringLiteral("port"), 21090).toInt(), 1, 65535);
        if (!preset.name.isEmpty() && !preset.address.isEmpty()) presets.push_back(preset);
    }
    settings.endArray();
    return presets;
}

void storeNetworkClientPresets(const QVector<NetworkClientPreset> &presets) {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.remove(QStringLiteral("networkClientPresets"));
    settings.beginWriteArray(QStringLiteral("networkClientPresets"), presets.size());
    for (int index = 0; index < presets.size(); ++index) {
        settings.setArrayIndex(index);
        settings.setValue(QStringLiteral("name"), presets[index].name);
        settings.setValue(QStringLiteral("address"), presets[index].address);
        settings.setValue(QStringLiteral("port"), presets[index].port);
    }
    settings.endArray();
    settings.sync();
}

} // namespace

void YourClassName::openNetworkSettingsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(uiText(QStringLiteral("network_settings"), QStringLiteral("Network Settings")));
    dialog.setMinimumWidth(420);

    QVBoxLayout *rootLayout = new QVBoxLayout(&dialog);
    QFormLayout *formLayout = new QFormLayout();

    QComboBox *modeCombo = new QComboBox(&dialog);
    modeCombo->addItem(uiText(QStringLiteral("disabled"), QStringLiteral("Disabled")), static_cast<int>(NetworkMode::Disabled));
    modeCombo->addItem(uiText(QStringLiteral("server"), QStringLiteral("Server")), static_cast<int>(NetworkMode::Server));
    modeCombo->addItem(uiText(QStringLiteral("client"), QStringLiteral("Client")), static_cast<int>(NetworkMode::Client));
    modeCombo->setCurrentIndex(modeCombo->findData(static_cast<int>(networkMode)));

    QComboBox *processingCombo = new QComboBox(&dialog);
    processingCombo->addItem(uiText(QStringLiteral("network_processing_server"),
                                    QStringLiteral("Server processing (spectrum/audio stream)")),
                             static_cast<int>(NetworkProcessingMode::ServerSide));
    processingCombo->addItem(uiText(QStringLiteral("network_processing_channel_iq"),
                                    QStringLiteral("Channel IQ + client demod")),
                             static_cast<int>(NetworkProcessingMode::ChannelIqClientSide));
    processingCombo->addItem(uiText(QStringLiteral("network_processing_full_iq"),
                                    QStringLiteral("Full IQ client processing (LAN only)")),
                             static_cast<int>(NetworkProcessingMode::FullIqClientSide));
    processingCombo->addItem(uiText(QStringLiteral("network_processing_remote_vfo"),
                                    QStringLiteral("Independent server VFO (multi-user)")),
                             static_cast<int>(NetworkProcessingMode::RemoteVfoServerSide));
    processingCombo->setCurrentIndex(processingCombo->findData(static_cast<int>(networkProcessingMode)));

    QLineEdit *serverAddressEdit = new QLineEdit(networkServerAddress, &dialog);
    serverAddressEdit->setPlaceholderText(uiText(QStringLiteral("server_ip_placeholder"),
                                                 QStringLiteral("Server IP address")));

    QLineEdit *bindAddressEdit = new QLineEdit(networkBindAddress, &dialog);
    bindAddressEdit->setPlaceholderText("0.0.0.0");

    QSpinBox *portSpin = new QSpinBox(&dialog);
    portSpin->setRange(1, 65535);
    portSpin->setValue(networkControlPort);

    QCheckBox *webServerCheck = new QCheckBox(
        uiText(QStringLiteral("network_web_server"), QStringLiteral("Browser Remote VFO interface")),
        &dialog);
    webServerCheck->setChecked(networkWebServerEnabled);
    QSpinBox *webServerPortSpin = new QSpinBox(&dialog);
    webServerPortSpin->setRange(1, 65535);
    webServerPortSpin->setValue(networkWebServerPort);

    QDoubleSpinBox *remoteVfoSquelchSpin = new QDoubleSpinBox(&dialog);
    remoteVfoSquelchSpin->setRange(-200.0, 0.0);
    remoteVfoSquelchSpin->setDecimals(1);
    remoteVfoSquelchSpin->setSuffix(QStringLiteral(" dB"));
    remoteVfoSquelchSpin->setValue(remoteVfoSquelchDb);

    QCheckBox *serverDisableLocalUiCheck = new QCheckBox(uiText(QStringLiteral("network_disable_local_ui"),
                                                                QStringLiteral("Disable local audio/video on server while a client is connected")),
                                                         &dialog);
    serverDisableLocalUiCheck->setChecked(serverDisableLocalVisualAudio);

    QCheckBox *fullResolutionSpectrumCheck = new QCheckBox(uiText(QStringLiteral("network_full_resolution_frames"),
                                                                  QStringLiteral("Send full-resolution spectrum/waterfall frames (heavy LAN only)")),
                                                           &dialog);
    fullResolutionSpectrumCheck->setChecked(networkFullResolutionSpectrumFrames);

    QCheckBox *audioRelayTransmitCheck = new QCheckBox(uiText(QStringLiteral("audio_relay_tx"),
                                                             QStringLiteral("Send ready audio by UDP")),
                                                       &dialog);
    audioRelayTransmitCheck->setChecked(audioRelayTransmitEnabled);

    QLineEdit *audioRelayHostEdit = new QLineEdit(audioRelayHost, &dialog);
    audioRelayHostEdit->setPlaceholderText(uiText(QStringLiteral("target_ip_placeholder"),
                                                  QStringLiteral("Target IP address")));

    QSpinBox *audioRelayPortSpin = new QSpinBox(&dialog);
    audioRelayPortSpin->setRange(1, 65535);
    audioRelayPortSpin->setValue(audioRelayPort);

    QCheckBox *audioRelayReceiveCheck = new QCheckBox(uiText(QStringLiteral("audio_relay_rx"),
                                                            QStringLiteral("Receive ready audio by UDP")),
                                                      &dialog);
    audioRelayReceiveCheck->setChecked(audioRelayReceiveEnabled);

    QSpinBox *audioRelayListenPortSpin = new QSpinBox(&dialog);
    audioRelayListenPortSpin->setRange(1, 65535);
    audioRelayListenPortSpin->setValue(audioRelayListenPort);

    QCheckBox *audioHttpStreamCheck = new QCheckBox(uiText(QStringLiteral("audio_http_stream"),
                                                          QStringLiteral("Serve VLC-compatible HTTP/WAV audio")),
                                                    &dialog);
    audioHttpStreamCheck->setChecked(audioHttpStreamEnabled);

    QSpinBox *audioHttpStreamPortSpin = new QSpinBox(&dialog);
    audioHttpStreamPortSpin->setRange(1, 65535);
    audioHttpStreamPortSpin->setValue(audioHttpStreamPort);

    QLabel *statusLabel = new QLabel(&dialog);
    statusLabel->setWordWrap(true);
    const auto refreshStatus = [this, statusLabel]() {
        QStringList lines;
        lines.append(networkController
                         ? networkController->statusText()
                         : uiText(QStringLiteral("network_controller_unavailable"),
                                  QStringLiteral("Network controller unavailable")));
        if (remoteWebServer && (networkWebServerEnabled || remoteWebServer->isListening())) {
            lines.append(remoteWebServer->statusText());
        }
        if (remoteVfoService && networkMode == NetworkMode::Server) {
            lines.append(uiText(QStringLiteral("remote_vfo_status"),
                                QStringLiteral("Remote VFO: %1 sessions, %2/%3 active"))
                             .arg(remoteVfoService->sessionCount())
                             .arg(remoteVfoService->activeVfoCount())
                             .arg(RemoteVfoService::MaxVfos));
        }
        statusLabel->setText(lines.join(QLatin1Char('\n')));
    };
    refreshStatus();

    formLayout->addRow(uiText(QStringLiteral("mode"), QStringLiteral("Mode:")), modeCombo);
    formLayout->addRow(uiText(QStringLiteral("processing"), QStringLiteral("Processing:")), processingCombo);
    formLayout->addRow(uiText(QStringLiteral("server_ip"), QStringLiteral("Server IP:")), serverAddressEdit);
    formLayout->addRow(uiText(QStringLiteral("bind_address"), QStringLiteral("Bind address:")), bindAddressEdit);
    formLayout->addRow(uiText(QStringLiteral("control_port"), QStringLiteral("Control port:")), portSpin);
    formLayout->addRow(uiText(QStringLiteral("browser_access"), QStringLiteral("Browser access:")), webServerCheck);
    formLayout->addRow(uiText(QStringLiteral("browser_port"), QStringLiteral("Browser port:")), webServerPortSpin);
    formLayout->addRow(uiText(QStringLiteral("remote_vfo_squelch"), QStringLiteral("Remote VFO squelch:")),
                       remoteVfoSquelchSpin);
    formLayout->addRow("", serverDisableLocalUiCheck);
    formLayout->addRow(uiText(QStringLiteral("visual_frames"), QStringLiteral("Visual frames:")), fullResolutionSpectrumCheck);
    formLayout->addRow(uiText(QStringLiteral("audio_relay_tx_label"), QStringLiteral("Audio relay TX:")), audioRelayTransmitCheck);
    formLayout->addRow(uiText(QStringLiteral("relay_target_ip"), QStringLiteral("Relay target IP:")), audioRelayHostEdit);
    formLayout->addRow(uiText(QStringLiteral("relay_target_port"), QStringLiteral("Relay target port:")), audioRelayPortSpin);
    formLayout->addRow(uiText(QStringLiteral("audio_relay_rx_label"), QStringLiteral("Audio relay RX:")), audioRelayReceiveCheck);
    formLayout->addRow(uiText(QStringLiteral("relay_listen_port"), QStringLiteral("Relay listen port:")), audioRelayListenPortSpin);
    formLayout->addRow(uiText(QStringLiteral("vlc_http_audio"), QStringLiteral("VLC HTTP audio:")), audioHttpStreamCheck);
    formLayout->addRow(uiText(QStringLiteral("http_audio_port"), QStringLiteral("HTTP audio port:")), audioHttpStreamPortSpin);

    QGroupBox *presetGroup = new QGroupBox(
        uiText(QStringLiteral("network_client_presets"), QStringLiteral("Client connection presets")),
        &dialog);
    QVBoxLayout *presetLayout = new QVBoxLayout(presetGroup);
    QTableWidget *presetTable = new QTableWidget(0, 3, presetGroup);
    presetTable->setHorizontalHeaderLabels({
        uiText(QStringLiteral("name"), QStringLiteral("Name")),
        uiText(QStringLiteral("server_ip"), QStringLiteral("Server IP")),
        uiText(QStringLiteral("control_port"), QStringLiteral("Control port"))});
    presetTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    presetTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    presetTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    presetTable->verticalHeader()->setVisible(false);
    presetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    presetTable->setSelectionMode(QAbstractItemView::SingleSelection);
    presetTable->setMinimumHeight(110);
    presetTable->setMaximumHeight(190);

    const auto populatePresetTable = [presetTable](const QVector<NetworkClientPreset> &presets) {
        QSignalBlocker blocker(presetTable);
        presetTable->setRowCount(presets.size());
        for (int row = 0; row < presets.size(); ++row) {
            presetTable->setItem(row, 0, new QTableWidgetItem(presets[row].name));
            presetTable->setItem(row, 1, new QTableWidgetItem(presets[row].address));
            presetTable->setItem(row, 2, new QTableWidgetItem(QString::number(presets[row].port)));
        }
    };
    populatePresetTable(loadNetworkClientPresets());

    const auto presetsFromTable = [presetTable]() {
        QVector<NetworkClientPreset> presets;
        presets.reserve(presetTable->rowCount());
        for (int row = 0; row < presetTable->rowCount(); ++row) {
            const auto *nameItem = presetTable->item(row, 0);
            const auto *addressItem = presetTable->item(row, 1);
            const auto *portItem = presetTable->item(row, 2);
            NetworkClientPreset preset;
            preset.name = nameItem ? nameItem->text().trimmed() : QString();
            preset.address = addressItem ? addressItem->text().trimmed() : QString();
            bool portOk = false;
            const int requestedPort = portItem ? portItem->text().toInt(&portOk) : 21090;
            preset.port = portOk ? (std::clamp)(requestedPort, 1, 65535) : 21090;
            if (!preset.name.isEmpty() && !preset.address.isEmpty()) presets.push_back(preset);
        }
        return presets;
    };

    QPushButton *savePresetButton = new QPushButton(
        uiText(QStringLiteral("network_preset_save"), QStringLiteral("Save current")), presetGroup);
    QPushButton *usePresetButton = new QPushButton(
        uiText(QStringLiteral("network_preset_use"), QStringLiteral("Use selected")), presetGroup);
    QPushButton *deletePresetButton = new QPushButton(
        uiText(QStringLiteral("delete"), QStringLiteral("Delete")), presetGroup);
    QHBoxLayout *presetButtons = new QHBoxLayout();
    presetButtons->addWidget(savePresetButton);
    presetButtons->addWidget(usePresetButton);
    presetButtons->addWidget(deletePresetButton);
    presetButtons->addStretch(1);
    presetLayout->addWidget(presetTable);
    presetLayout->addLayout(presetButtons);

    QPushButton *testButton = new QPushButton(uiText(QStringLiteral("apply_test_channel"), QStringLiteral("Apply / Test Channel")), &dialog);
    QPushButton *requestControlButton = new QPushButton(uiText(QStringLiteral("request_control"), QStringLiteral("Request Control")), &dialog);
    QPushButton *stopButton = new QPushButton(uiText(QStringLiteral("stop_network"), QStringLiteral("Stop Network")), &dialog);
    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    if (QPushButton *closeButton = buttonBox->button(QDialogButtonBox::Close)) {
        closeButton->setText(uiText(QStringLiteral("close"), QStringLiteral("Close")));
    }

    QHBoxLayout *actionLayout = new QHBoxLayout();
    actionLayout->addWidget(testButton);
    actionLayout->addWidget(requestControlButton);
    actionLayout->addWidget(stopButton);

    rootLayout->addLayout(formLayout);
    rootLayout->addWidget(presetGroup);
    rootLayout->addWidget(statusLabel);
    rootLayout->addLayout(actionLayout);
    rootLayout->addWidget(buttonBox);

    auto updateFieldState = [=]() {
        const auto selectedMode = static_cast<NetworkMode>(modeCombo->currentData().toInt());
        const bool clientCanControl = !networkController || !networkController->isControlReady() ||
                                      networkController->clientHasControl();
        serverAddressEdit->setEnabled(selectedMode == NetworkMode::Client);
        presetGroup->setVisible(selectedMode == NetworkMode::Client);
        bindAddressEdit->setEnabled(selectedMode == NetworkMode::Server);
        serverDisableLocalUiCheck->setEnabled(selectedMode == NetworkMode::Server ||
            (selectedMode == NetworkMode::Client && clientCanControl &&
             static_cast<NetworkProcessingMode>(processingCombo->currentData().toInt()) !=
                 NetworkProcessingMode::RemoteVfoServerSide));
        fullResolutionSpectrumCheck->setEnabled(selectedMode != NetworkMode::Disabled);
        processingCombo->setEnabled(selectedMode != NetworkMode::Disabled);
        portSpin->setEnabled(selectedMode != NetworkMode::Disabled);
        webServerCheck->setEnabled(selectedMode == NetworkMode::Server);
        webServerPortSpin->setEnabled(selectedMode == NetworkMode::Server && webServerCheck->isChecked());
        const auto selectedProcessing = static_cast<NetworkProcessingMode>(processingCombo->currentData().toInt());
        remoteVfoSquelchSpin->setEnabled(selectedMode == NetworkMode::Client &&
                                         selectedProcessing == NetworkProcessingMode::RemoteVfoServerSide);
        requestControlButton->setEnabled(selectedMode == NetworkMode::Client &&
                                         selectedProcessing != NetworkProcessingMode::RemoteVfoServerSide);
        audioRelayHostEdit->setEnabled(audioRelayTransmitCheck->isChecked());
        audioRelayPortSpin->setEnabled(audioRelayTransmitCheck->isChecked());
        audioRelayListenPortSpin->setEnabled(audioRelayReceiveCheck->isChecked());
        audioHttpStreamPortSpin->setEnabled(audioHttpStreamCheck->isChecked());
        testButton->setText(selectedMode == NetworkMode::Disabled
                                ? uiText(QStringLiteral("apply"), QStringLiteral("Apply"))
                                : uiText(QStringLiteral("apply_test_channel"), QStringLiteral("Apply / Test Channel")));
    };
    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, updateFieldState);
    connect(processingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, updateFieldState);
    connect(audioRelayTransmitCheck, &QCheckBox::toggled, &dialog, updateFieldState);
    connect(audioRelayReceiveCheck, &QCheckBox::toggled, &dialog, updateFieldState);
    connect(audioHttpStreamCheck, &QCheckBox::toggled, &dialog, updateFieldState);
    connect(webServerCheck, &QCheckBox::toggled, &dialog, updateFieldState);
    updateFieldState();

    const auto useSelectedPreset = [=]() {
        const int row = presetTable->currentRow();
        if (row < 0) return;
        const auto *addressItem = presetTable->item(row, 1);
        const auto *portItem = presetTable->item(row, 2);
        if (!addressItem || addressItem->text().trimmed().isEmpty()) return;
        serverAddressEdit->setText(addressItem->text().trimmed());
        bool portOk = false;
        const int presetPort = portItem ? portItem->text().toInt(&portOk) : portSpin->value();
        if (portOk) portSpin->setValue((std::clamp)(presetPort, 1, 65535));
    };
    connect(usePresetButton, &QPushButton::clicked, &dialog, useSelectedPreset);
    connect(presetTable, &QTableWidget::cellDoubleClicked, &dialog,
            [useSelectedPreset](int, int) { useSelectedPreset(); });
    connect(savePresetButton, &QPushButton::clicked, &dialog, [=, &dialog]() {
        bool accepted = false;
        const QString suggestedName = serverAddressEdit->text().trimmed().isEmpty()
                                          ? QStringLiteral("Server")
                                          : serverAddressEdit->text().trimmed();
        const QString name = QInputDialog::getText(
                                 &dialog,
                                 uiText(QStringLiteral("network_preset_save"), QStringLiteral("Save current")),
                                 uiText(QStringLiteral("network_preset_name"), QStringLiteral("Preset name:")),
                                 QLineEdit::Normal,
                                 suggestedName,
                                 &accepted)
                                 .trimmed();
        if (!accepted || name.isEmpty()) return;
        const QString address = serverAddressEdit->text().trimmed();
        if (address.isEmpty()) return;
        QVector<NetworkClientPreset> presets = presetsFromTable();
        auto existing = std::find_if(presets.begin(), presets.end(), [&name](const auto &preset) {
            return preset.name.compare(name, Qt::CaseInsensitive) == 0;
        });
        const NetworkClientPreset value{name, address, portSpin->value()};
        if (existing == presets.end()) presets.push_back(value);
        else *existing = value;
        storeNetworkClientPresets(presets);
        populatePresetTable(presets);
    });
    connect(deletePresetButton, &QPushButton::clicked, &dialog, [=]() {
        const int row = presetTable->currentRow();
        if (row < 0) return;
        presetTable->removeRow(row);
        storeNetworkClientPresets(presetsFromTable());
    });
    connect(presetTable, &QTableWidget::itemChanged, &dialog, [presetsFromTable]() {
        storeNetworkClientPresets(presetsFromTable());
    });

    QMetaObject::Connection statusConnection;
    if (networkController) {
        statusConnection = connect(networkController,
                                   &NetworkController::statusChanged,
                                   &dialog,
                                   [refreshStatus](const QString &) { refreshStatus(); });
        connect(networkController, &NetworkController::controlCommandReceived, &dialog,
                [=](const QJsonObject &command) {
                    if (networkMode != NetworkMode::Server ||
                        command.value(QStringLiteral("type")).toString() != QStringLiteral("control")) {
                        return;
                    }
                    const QString action = command.value(QStringLiteral("action")).toString();
                    if (action == QStringLiteral("setProcessingMode")) {
                        const int index = processingCombo->findData(static_cast<int>(networkProcessingMode));
                        if (index >= 0) {
                            const QSignalBlocker blocker(processingCombo);
                            processingCombo->setCurrentIndex(index);
                        }
                    } else if (action == QStringLiteral("setServerLocalOutput")) {
                        const QSignalBlocker blocker(serverDisableLocalUiCheck);
                        serverDisableLocalUiCheck->setChecked(serverDisableLocalVisualAudio);
                    }
                });
    }
    if (remoteWebServer) {
        connect(remoteWebServer, &RemoteWebServer::statusChanged,
                &dialog, [refreshStatus](const QString &) { refreshStatus(); });
    }
    if (remoteVfoService) {
        connect(remoteVfoService, &RemoteVfoService::statusChanged,
                &dialog, refreshStatus);
    }

    connect(testButton, &QPushButton::clicked, &dialog, [=]() {
        const NetworkMode previousNetworkMode = networkMode;
        const NetworkProcessingMode previousProcessingMode = networkProcessingMode;
        const bool previousServerDisableLocalVisualAudio = serverDisableLocalVisualAudio;
        const QString previousServerAddress = networkServerAddress;
        const quint16 previousControlPort = networkControlPort;
        const auto selectedNetworkMode = static_cast<NetworkMode>(modeCombo->currentData().toInt());
        const auto selectedProcessingMode =
            static_cast<NetworkProcessingMode>(processingCombo->currentData().toInt());
        if (selectedNetworkMode == NetworkMode::Client && previousNetworkMode == NetworkMode::Client &&
            networkController && networkController->isControlReady() &&
            !networkController->clientHasControl() &&
            ((selectedProcessingMode != previousProcessingMode &&
              selectedProcessingMode != NetworkProcessingMode::RemoteVfoServerSide) ||
             serverDisableLocalUiCheck->isChecked() != previousServerDisableLocalVisualAudio)) {
            statusLabel->setText(uiText(QStringLiteral("network_request_control_first"),
                                        QStringLiteral("Request control before changing server settings.")));
            return;
        }

        networkMode = static_cast<NetworkMode>(modeCombo->currentData().toInt());
        networkServerAddress = serverAddressEdit->text().trimmed().isEmpty()
                                   ? QString("127.0.0.1")
                                   : serverAddressEdit->text().trimmed();
        networkBindAddress = bindAddressEdit->text().trimmed().isEmpty()
                                 ? QString("0.0.0.0")
                                 : bindAddressEdit->text().trimmed();
        networkControlPort = static_cast<quint16>(portSpin->value());
        networkWebServerEnabled = webServerCheck->isChecked();
        networkWebServerPort = static_cast<quint16>(webServerPortSpin->value());
        remoteVfoSquelchDb = remoteVfoSquelchSpin->value();
        serverDisableLocalVisualAudio = serverDisableLocalUiCheck->isChecked();
        networkFullResolutionSpectrumFrames = fullResolutionSpectrumCheck->isChecked();
        networkProcessingMode = selectedProcessingMode;
        audioRelayTransmitEnabled = audioRelayTransmitCheck->isChecked();
        audioRelayHost = audioRelayHostEdit->text().trimmed().isEmpty()
                             ? QString("127.0.0.1")
                             : audioRelayHostEdit->text().trimmed();
        audioRelayPort = static_cast<quint16>(audioRelayPortSpin->value());
        audioRelayReceiveEnabled = audioRelayReceiveCheck->isChecked();
        audioRelayListenPort = static_cast<quint16>(audioRelayListenPortSpin->value());
        audioHttpStreamEnabled = audioHttpStreamCheck->isChecked();
        audioHttpStreamPort = static_cast<quint16>(audioHttpStreamPortSpin->value());
        updateAudioRelaySocket();
        updateAudioHttpStreamServer();
        updateRemoteWebServer();
        if (isChannelIqRecordingActive() &&
            networkMode != NetworkMode::Disabled &&
            isFullIqProcessingMode()) {
            stopRecording(false);
            updateRecordingStatus(QStringLiteral("Recording stopped: Channel IQ cannot run during Full IQ streaming"));
        }

        const bool networkModeChanged = previousNetworkMode != networkMode;
        const bool processingModeChanged = previousProcessingMode != networkProcessingMode;
        const bool serverLocalOutputChanged =
            previousServerDisableLocalVisualAudio != serverDisableLocalVisualAudio;
        const bool reconnectClient = networkMode == NetworkMode::Client &&
            (previousNetworkMode != NetworkMode::Client ||
             previousServerAddress != networkServerAddress ||
             previousControlPort != networkControlPort ||
             !networkController || !networkController->isControlReady() ||
             previousProcessingMode == NetworkProcessingMode::RemoteVfoServerSide ||
             networkProcessingMode == NetworkProcessingMode::RemoteVfoServerSide);

        if (!networkController) {
            statusLabel->setText("Network controller unavailable");
            return;
        }

        if (networkMode == NetworkMode::Disabled) {
            networkClientRequestedProcessingMode = -1;
            networkClientRequestedServerLocalOutput = -1;
            if (previousNetworkMode == NetworkMode::Client) {
                stopNetworkClientProcessing();
                clearRemoteReceiverDeviceList();
            } else if (previousNetworkMode == NetworkMode::Server && runState == RadioRunState::Running) {
                if (audioProcessor) {
                    audioProcessor->setLocalPlaybackEnabled(true);
                }
                if (updateTimer) {
                    updateTimer->start();
                }
            }
            if (remoteAudioPlayer) {
                remoteAudioPlayer->stop();
            }
            networkController->stop();
            updateRemoteWebServer();
            if (runState == RadioRunState::Running) {
                updateIqFrameProducerSettings();
            }
            savePersistentSettings();
            return;
        }

        if (previousNetworkMode == NetworkMode::Client && networkMode != NetworkMode::Client) {
            stopNetworkClientProcessing();
            clearRemoteReceiverDeviceList();
        }

        if (remoteAudioPlayer && networkMode != NetworkMode::Client) {
            remoteAudioPlayer->stop();
        }
        if (networkMode == NetworkMode::Server) {
            networkClientRequestedProcessingMode = -1;
            networkClientRequestedServerLocalOutput = -1;
            clearRemoteReceiverDeviceList();
            networkController->startServer(networkBindAddress, networkControlPort);
            updateRemoteWebServer();
            const bool restartRequired =
                runState == RadioRunState::Running &&
                (networkModeChanged || processingModeChanged);
            if (restartRequired) {
                restartStreamForHardwareChange();
            } else if (runState == RadioRunState::Running && serverLocalOutputChanged) {
                applyServerLocalOutputPolicy();
            }
            savePersistentSettings();
            return;
        }

        if (runState == RadioRunState::Running &&
            (networkModeChanged || processingModeChanged)) {
            if (isClientIqProcessingMode()) {
                startNetworkClientProcessing();
            } else {
                stopNetworkClientProcessing();
            }
        }

        if (networkMode == NetworkMode::Client) {
            clearRemoteReceiverDeviceList();
        }
        if (networkProcessingMode == NetworkProcessingMode::RemoteVfoServerSide) {
            networkClientRequestedProcessingMode = -1;
            networkClientRequestedServerLocalOutput = -1;
        } else if (processingModeChanged || reconnectClient) {
            networkClientRequestedProcessingMode = static_cast<int>(networkProcessingMode);
        }
        if (serverLocalOutputChanged && networkProcessingMode != NetworkProcessingMode::RemoteVfoServerSide) {
            networkClientRequestedServerLocalOutput = static_cast<int>(serverDisableLocalVisualAudio);
        }
        if (reconnectClient) {
            networkController->testClientConnection(networkServerAddress, networkControlPort);
        } else {
            if (networkClientRequestedProcessingMode >= 0)
                sendRemoteControlCommand(QStringLiteral("setProcessingMode"));
            if (networkClientRequestedServerLocalOutput >= 0)
                sendRemoteControlCommand(QStringLiteral("setServerLocalOutput"),
                    {{QStringLiteral("serverDisableLocalVisualAudio"), serverDisableLocalVisualAudio}});
            onNetworkStatusChanged(networkController->statusText());
        }
        updateRemoteWebServer();
        savePersistentSettings();
    });

    connect(stopButton, &QPushButton::clicked, &dialog, [=]() {
        const NetworkMode previousNetworkMode = networkMode;
        networkMode = NetworkMode::Disabled;
        networkClientRequestedProcessingMode = -1;
        networkClientRequestedServerLocalOutput = -1;
        if (modeCombo) {
            modeCombo->setCurrentIndex(modeCombo->findData(static_cast<int>(NetworkMode::Disabled)));
        }
        if (networkController) {
            networkController->stop();
        }
        updateRemoteWebServer();
        if (remoteAudioPlayer) {
            remoteAudioPlayer->stop();
        }
        if (previousNetworkMode == NetworkMode::Client) {
            stopNetworkClientProcessing();
            clearRemoteReceiverDeviceList();
        } else if (previousNetworkMode == NetworkMode::Server && runState == RadioRunState::Running) {
            if (audioProcessor) {
                audioProcessor->setLocalPlaybackEnabled(true);
            }
            if (updateTimer) {
                updateTimer->start();
            }
        }
        if (runState == RadioRunState::Running) {
            updateIqFrameProducerSettings();
        }
        savePersistentSettings();
    });

    connect(requestControlButton, &QPushButton::clicked, &dialog, [=]() {
        if (networkMode != NetworkMode::Client) {
            statusLabel->setText("Switch to Client mode and connect first.");
            return;
        }
        if (!networkController || !networkController->isControlReady()) {
            statusLabel->setText("Control channel is not ready.");
            return;
        }
        if (networkController->clientHasControl()) {
            statusLabel->setText("This client already has control.");
            return;
        }
        if (sendRemoteControlCommand("requestPriority")) {
            statusLabel->setText("Control request sent.");
        } else {
            statusLabel->setText("Control request could not be sent.");
        }
    });

    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.exec();
    if (networkController && statusConnection) {
        disconnect(statusConnection);
    }
}
