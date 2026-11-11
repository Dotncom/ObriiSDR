#include "main.h"
#include "appconstants.h"
#include "appruntimeutils.h"
#include "bladerfbackend.h"
#include "hackrfbackend.h"
#include "receiverbackendregistry.h"
#include "receiverdeviceutils.h"

#include <QDebug>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
void YourClassName::buildLocalReceiverDeviceChoices(QStringList &labels, QVector<int> &values) const {
    labels.clear();
    values.clear();

    for (int i = 0; i < availableFobosDevices.size(); ++i) {
        const auto &info = availableFobosDevices.at(i);
        labels << (info.present ? info.label : info.label + QStringLiteral(" - ") +
            uiText(QStringLiteral("receiver_not_detected"), QStringLiteral("not detected")));
        values << i;
    }
    if (hasActiveFobosDevice()) {
        const int activeLogicalIndex = openedDeviceIndex >= 0
                                           ? openedDeviceIndex
                                           : pendingSettings.deviceIndex;
        bool activeAlreadyListed = false;
        for (int value : values) {
            if (value == activeLogicalIndex) {
                activeAlreadyListed = true;
                break;
            }
        }
        if (!activeAlreadyListed) {
            labels << QStringLiteral("[%1] active device #%2")
                          .arg(fobosApiDisplayName(activeFobosApiKind))
                          .arg(openedNativeDeviceIndex >= 0
                                   ? openedNativeDeviceIndex
                                   : std::max(0, activeLogicalIndex));
            values << activeLogicalIndex;
        }
    }

    const QVector<RtlSdrDeviceInfo> rtlDevices = enumerateRtlSdrDevices();
    if (rtlDevices.isEmpty()) {
        labels << QStringLiteral("RTL-SDR native auto (rtlsdr.dll)");
        values << rtlSdrNativeComboValue(0);
    } else {
        for (const RtlSdrDeviceInfo &rtlInfo : rtlDevices) {
            labels << rtlSdrNativeDeviceLabel(rtlInfo.nativeIndex, rtlInfo.name);
            values << rtlSdrNativeComboValue(rtlInfo.nativeIndex);
        }
    }

    labels << QStringLiteral("RTL-SDR via rtl_tcp (127.0.0.1:1234)");
    values << RTL_TCP_DEVICE_INDEX;

    if (BLADERF_NATIVE_BACKEND_ENABLED) {
        const QVector<BladeRfDeviceInfo> bladeRfDevices = enumerateBladeRfDevices();
        if (bladeRfDevices.isEmpty()) {
            labels << QStringLiteral("bladeRF native auto (bladeRF.dll)");
            values << bladeRfNativeComboValue(0);
        } else {
            for (const BladeRfDeviceInfo &bladeInfo : bladeRfDevices) {
                labels << bladeInfo.label;
                values << bladeRfNativeComboValue(bladeInfo.nativeIndex);
            }
        }
    }

    HackRfRuntimeStatus hackRfStatus;
    const QVector<HackRfDeviceInfo> hackRfDevices = enumerateHackRfDevices(&hackRfStatus);
    if (hackRfDevices.isEmpty()) {
        labels << (hackRfStatus.libraryAvailable
                       ? QStringLiteral("HackRF native auto (no device detected)")
                       : QStringLiteral("HackRF native auto (runtime unavailable)"));
        values << hackRfNativeComboValue(0);
    } else {
        for (const HackRfDeviceInfo &hackRfInfo : hackRfDevices) {
            labels << hackRfInfo.label;
            values << hackRfNativeComboValue(hackRfInfo.nativeIndex);
            qDebug().noquote() << QStringLiteral("[HackRF] enumerated index=%1 serial=%2 label=%3")
                                     .arg(hackRfInfo.nativeIndex)
                                     .arg(hackRfInfo.serial.isEmpty() ? QStringLiteral("-")
                                                                      : hackRfInfo.serial,
                                          hackRfInfo.label);
        }
    }
    qDebug().noquote() << QStringLiteral("[HackRF] runtime=%1 path=%2 devices=%3 modernList=%4 error=%5")
                             .arg(hackRfStatus.libraryAvailable ? QStringLiteral("ready")
                                                               : QStringLiteral("unavailable"),
                                  hackRfStatus.loadedPath.isEmpty() ? QStringLiteral("-")
                                                                    : hackRfStatus.loadedPath)
                             .arg(hackRfStatus.deviceCount)
                             .arg(hackRfStatus.modernDeviceListAvailable ? QStringLiteral("yes")
                                                                        : QStringLiteral("no"),
                                  hackRfStatus.errorMessage.isEmpty() ? QStringLiteral("-")
                                                                      : hackRfStatus.errorMessage);

    labels << QStringLiteral("SoapySDR auto (SoapySDR.dll)");
    values << SOAPY_SDR_DEVICE_INDEX;

    labels << QStringLiteral("USRP UHD native auto (uhd.dll / libuhd.so)");
    values << uhdNativeComboValue(0);

    labels << uiText(QStringLiteral("virtual_iq_receiver"),
                     QStringLiteral("Virtual IQ receiver (local / network)"));
    values << VIRTUAL_SIGNAL_DEVICE_INDEX;

    if (labels.isEmpty()) {
        labels << uiText(QStringLiteral("no_fobos_devices_detected"),
                         QStringLiteral("No Fobos devices detected"));
        values << 0;
    }
}

void YourClassName::rebuildReceiverDeviceCombo() {
    if (!comboBox) {
        return;
    }

    QStringList labels;
    QVector<int> values;
    if (isNetworkClientMode() && remoteReceiverDeviceListValid) {
        const int count = std::min(remoteReceiverDeviceLabels.size(), remoteReceiverDeviceValues.size());
        for (int i = 0; i < count; ++i) {
            labels << QStringLiteral("[Server] %1").arg(remoteReceiverDeviceLabels.at(i));
            values << remoteReceiverComboValue(remoteReceiverDeviceValues.at(i));
        }
    } else {
        buildLocalReceiverDeviceChoices(labels, values);
    }

    if (labels.size() != values.size()) {
        labels.clear();
        values.clear();
    }
    if (labels.isEmpty()) {
        labels << (isNetworkClientMode()
                       ? QStringLiteral("Waiting for server receiver list...")
                       : uiText(QStringLiteral("no_fobos_devices_detected"),
                                QStringLiteral("No Fobos devices detected")));
        values << pendingSettings.deviceIndex;
    }

    QSignalBlocker blocker(comboBox);
    comboBox->clear();
    for (int i = 0; i < labels.size(); ++i) {
        comboBox->addItem(labels.at(i), values.at(i));
    }

    const int selectedComboValue =
        isNetworkClientMode() && remoteReceiverDeviceListValid
            ? remoteReceiverComboValue(pendingSettings.deviceIndex)
            : pendingSettings.deviceIndex;
    int selectedIndex = comboBox->findData(selectedComboValue);
    if (selectedIndex < 0) {
        // A transiently empty list must not silently select RTL-SDR (or another radio).
        const QString name = pendingSettings.deviceIndex >= 0
            ? QStringLiteral("Fobos") : uiText(QStringLiteral("receiver"), QStringLiteral("Receiver"));
        comboBox->addItem(name + QStringLiteral(" - ") +
                         uiText(QStringLiteral("receiver_not_detected"), QStringLiteral("not detected")),
                         selectedComboValue);
        selectedIndex = comboBox->count() - 1;
    }
    if (selectedIndex >= 0) {
        comboBox->setCurrentIndex(selectedIndex);
        bool ok = false;
        const int selectedDevice = comboBox->itemData(selectedIndex).toInt(&ok);
        if (ok) {
            pendingSettings.deviceIndex = receiverDeviceIndexFromComboValue(selectedDevice);
        }
    }
}

QJsonArray YourClassName::receiverDeviceListToJson() const {
    QStringList labels;
    QVector<int> values;

    for (int i = 0; i < availableFobosDevices.size(); ++i) {
        const auto &info = availableFobosDevices.at(i);
        labels << (info.present ? info.label : info.label + QStringLiteral(" - ") +
            uiText(QStringLiteral("receiver_not_detected"), QStringLiteral("not detected")));
        values << i;
    }
    if (hasActiveFobosDevice()) {
        const int activeLogicalIndex = openedDeviceIndex >= 0
                                           ? openedDeviceIndex
                                           : pendingSettings.deviceIndex;
        bool activeAlreadyListed = false;
        for (int value : values) {
            if (value == activeLogicalIndex) {
                activeAlreadyListed = true;
                break;
            }
        }
        if (!activeAlreadyListed) {
            labels << QStringLiteral("[%1] active device #%2")
                          .arg(fobosApiDisplayName(activeFobosApiKind))
                          .arg(openedNativeDeviceIndex >= 0
                                   ? openedNativeDeviceIndex
                                   : std::max(0, activeLogicalIndex));
            values << activeLogicalIndex;
        }
    }

    labels << QStringLiteral("RTL-SDR native auto (server)");
    values << rtlSdrNativeComboValue(0);
    labels << QStringLiteral("RTL-SDR via rtl_tcp (server 127.0.0.1:1234)");
    values << RTL_TCP_DEVICE_INDEX;
    if (BLADERF_NATIVE_BACKEND_ENABLED) {
        labels << QStringLiteral("bladeRF native auto (server)");
        values << bladeRfNativeComboValue(0);
    }
    labels << QStringLiteral("HackRF native auto (server)");
    values << hackRfNativeComboValue(0);
    labels << QStringLiteral("SoapySDR auto (server)");
    values << SOAPY_SDR_DEVICE_INDEX;
    labels << QStringLiteral("USRP UHD native auto (server)");
    values << uhdNativeComboValue(0);
    labels << QStringLiteral("Virtual IQ receiver (server local / TCP)");
    values << VIRTUAL_SIGNAL_DEVICE_INDEX;

    QJsonArray array;
    const int count = std::min(labels.size(), values.size());
    for (int i = 0; i < count; ++i) {
        QJsonObject object;
        object["label"] = labels.at(i);
        object["deviceIndex"] = values.at(i);
        array.append(object);
    }
    return array;
}

bool YourClassName::applyReceiverDeviceListFromJson(const QJsonArray &devices) {
    QStringList labels;
    QVector<int> values;
    for (const QJsonValue &value : devices) {
        const QJsonObject object = value.toObject();
        const QString label = object.value(QStringLiteral("label")).toString().trimmed();
        if (label.isEmpty()) {
            continue;
        }
        labels << label;
        values << object.value(QStringLiteral("deviceIndex")).toInt(values.size());
    }

    remoteReceiverDeviceLabels = labels;
    remoteReceiverDeviceValues = values;
    remoteReceiverDeviceListValid = !labels.isEmpty() && labels.size() == values.size();
    rebuildReceiverDeviceCombo();
    return remoteReceiverDeviceListValid;
}

void YourClassName::clearRemoteReceiverDeviceList() {
    remoteReceiverDeviceLabels.clear();
    remoteReceiverDeviceValues.clear();
    remoteReceiverDeviceListValid = false;
    rebuildReceiverDeviceCombo();
}

QString YourClassName::formatFobosDeviceLabel(const FobosDeviceInfo &info) const {
    const QString serial = info.serial.isEmpty() ? QStringLiteral("unknown SN") : info.serial;
    const QString hw = info.hardwareRevision.isEmpty() ? QStringLiteral("?") : info.hardwareRevision;
    const QString fw = info.firmwareVersion.isEmpty() ? QStringLiteral("?") : info.firmwareVersion;
    return QString("[%1] #%2  SN %3  HW %4  FW %5")
        .arg(fobosApiDisplayName(info.apiKind))
        .arg(info.nativeIndex)
        .arg(serial)
        .arg(hw)
        .arg(fw);
}

void YourClassName::requestUsbDeviceRefresh() {
    if (dualFobosSwitchPending) return;
    if (isNetworkClientMode()) {
        sendRemoteControlCommand(QStringLiteral("requestServerState"));
        return;
    }
    if (closeShutdownInProgress || !isIdle() || (processor && processor->isRunning())) {
        return;
    }
    if (!usbDeviceRefreshTimer) {
        usbDeviceRefreshTimer = new QTimer(this);
        usbDeviceRefreshTimer->setInterval(1000);
        connect(usbDeviceRefreshTimer, &QTimer::timeout, this, &YourClassName::continueUsbDeviceRefresh);
    }
    usbDeviceRefreshPassesRemaining = 11;
    usbDeviceRefreshTimer->start();
    continueUsbDeviceRefresh();
}

void YourClassName::continueUsbDeviceRefresh() {
    if (closeShutdownInProgress || dualFobosSwitchPending || isNetworkClientMode() || !isIdle() ||
        (processor && processor->isRunning())) {
        usbDeviceRefreshTimer->stop();
        return;
    }
    refreshFobosDeviceList();
    rebuildReceiverDeviceCombo();
    updateAgileScanControls();
    if (networkMode == NetworkMode::Server) {
        sendServerStateToClients();
    }
    const bool found = std::any_of(availableFobosDevices.cbegin(), availableFobosDevices.cend(),
                                  [](const FobosDeviceInfo &info) { return info.present; });
    const bool selectedMissing = pendingSettings.deviceIndex >= 0 && !selectedFobosDeviceInfo().present;
    const bool selectedFobosReady = !isExternalReceiverBackendSelected() &&
                                    selectedFobosDeviceInfo().present;
    if (!hasActiveFobosDevice() && selectedFobosReady) {
        // Enumeration uses temporary handles. Give the USB driver a brief moment
        // to release them, then prepare the same idle session that startup creates.
        // Start intentionally launches only the reader, so without this step a
        // hot-plugged Fobos is visible in the list but cannot be started.
        usbDeviceRefreshTimer->stop();
        QTimer::singleShot(250, this, [this]() {
            if (closeShutdownInProgress || isNetworkClientMode() || !isIdle() ||
                (processor && processor->isRunning()) || hasActiveFobosDevice() ||
                isExternalReceiverBackendSelected() || !selectedFobosDeviceInfo().present) {
                return;
            }

            if (prepareFobosSessionFromSettings(QStringLiteral("USB refresh hot-plug"))) {
                updateUiForRunState();
                if (networkMode == NetworkMode::Server) {
                    sendServerStateToClients();
                }
                return;
            }

            if (--usbDeviceRefreshPassesRemaining > 0 && usbDeviceRefreshTimer) {
                usbDeviceRefreshTimer->start();
            }
        });
        return;
    }
    if (hasActiveFobosDevice() || (found && !selectedMissing) || --usbDeviceRefreshPassesRemaining <= 0) {
        usbDeviceRefreshTimer->stop();
    }
}

void YourClassName::refreshFobosDeviceList() {
    // Enumeration opens temporary handles. Never contend with an owned USB session.
    if (hasActiveFobosDevice() || deviceOpened || (processor && processor->isRunning()) ||
        isNetworkClientMode() || closeShutdownInProgress) {
        return;
    }

    QVector<FobosDeviceInfo> discovered;

    auto addDevice = [&discovered](const FobosDeviceInfo &info) {
        if (!info.serial.isEmpty()) {
            for (int i = 0; i < discovered.size(); ++i) {
                FobosDeviceInfo &existing = discovered[i];
                if (existing.serial != info.serial) {
                    continue;
                }
                const bool preferAgile =
                    info.apiKind == FobosApiKind::Agile &&
                    (firmwareLooksAgile(info.firmwareVersion) ||
                     firmwareLooksAgile(existing.firmwareVersion));
                if (preferAgile) {
                    existing = info;
                }
                return;
            }
        }
        discovered.append(info);
    };

    const int standardCount = getFobosStandardDeviceCountSafely();
    for (int i = 0; i < standardCount; ++i) {
        fobos_dev_t *infoDevice = nullptr;
        const int openResult = openFobosDeviceSafely(&infoDevice, static_cast<uint32_t>(i));
        if (openResult != FOBOS_ERR_OK || !infoDevice) {
            qDebug() << "[FobosDevices] standard open failed" << "index" << i << "result" << openResult;
            continue;
        }

        char hw[256] = {};
        char fw[256] = {};
        char manufacturer[256] = {};
        char product[256] = {};
        char serial[256] = {};
        const int infoResult = getFobosBoardInfoSafely(infoDevice, hw, fw, manufacturer, product, serial);
        closeFobosDeviceSafely(infoDevice);
        if (infoResult != FOBOS_ERR_OK) {
            qDebug() << "[FobosDevices] standard board info failed" << "index" << i << "result" << infoResult;
            continue;
        }

        FobosDeviceInfo info;
        info.apiKind = FobosApiKind::Standard;
        info.nativeIndex = i;
        info.hardwareRevision = QString::fromLocal8Bit(hw).trimmed();
        info.firmwareVersion = QString::fromLocal8Bit(fw).trimmed();
        info.manufacturer = QString::fromLocal8Bit(manufacturer).trimmed();
        info.product = QString::fromLocal8Bit(product).trimmed();
        info.serial = QString::fromLocal8Bit(serial).trimmed();
        info.label = formatFobosDeviceLabel(info);
        addDevice(info);
    }

    const int agileCount = getFobosAgileDeviceCountSafely();
    for (int i = 0; i < agileCount; ++i) {
        fobos_sdr_dev_t *infoDevice = nullptr;
        const int openResult = openFobosAgileDeviceSafely(&infoDevice, static_cast<uint32_t>(i));
        if (openResult != FOBOS_ERR_OK || !infoDevice) {
            qDebug() << "[FobosDevices] agile open failed" << "index" << i << "result" << openResult;
            continue;
        }

        char hw[256] = {};
        char fw[256] = {};
        char manufacturer[256] = {};
        char product[256] = {};
        char serial[256] = {};
        const int infoResult = getFobosAgileBoardInfoSafely(infoDevice, hw, fw, manufacturer, product, serial);
        closeFobosAgileDeviceSafely(infoDevice);
        if (infoResult != FOBOS_ERR_OK) {
            qDebug() << "[FobosDevices] agile board info failed" << "index" << i << "result" << infoResult;
            continue;
        }

        FobosDeviceInfo info;
        info.apiKind = FobosApiKind::Agile;
        info.nativeIndex = i;
        info.hardwareRevision = QString::fromLocal8Bit(hw).trimmed();
        info.firmwareVersion = QString::fromLocal8Bit(fw).trimmed();
        info.manufacturer = QString::fromLocal8Bit(manufacturer).trimmed();
        info.product = QString::fromLocal8Bit(product).trimmed();
        info.serial = QString::fromLocal8Bit(serial).trimmed();
        info.label = formatFobosDeviceLabel(info);
        addDevice(info);
    }

    const int detectedCount = discovered.size();
    preserveFobosDeviceSelection(availableFobosDevices, discovered, pendingSettings.deviceIndex);
    availableFobosDevices = std::move(discovered);
    qDebug() << "[FobosDevices] refreshed without USB reset"
             << "standardCount" << standardCount
             << "agileCount" << agileCount
             << "usable" << detectedCount
             << "rtlNativeLazy" << true;
}

QStringList YourClassName::getFobosDevices() {
    if (!deviceOpened && !(processor && processor->isRunning())) {
        refreshFobosDeviceList();
    }

    QStringList deviceList;
    QVector<int> values;
    buildLocalReceiverDeviceChoices(deviceList, values);
    return deviceList;
}

FobosDeviceInfo YourClassName::selectedFobosDeviceInfo() const {
    int selected = pendingSettings.deviceIndex;
    if (comboBox) {
        bool ok = false;
        const int value = comboBox->currentData().toInt(&ok);
        if (ok) {
            selected = receiverDeviceIndexFromComboValue(value);
        }
    }
    if (selected >= 0 && selected < availableFobosDevices.size()) {
        return availableFobosDevices[selected];
    }

    FobosDeviceInfo fallback;
    fallback.present = false;
    fallback.apiKind = FobosApiKind::Standard;
    fallback.nativeIndex = std::max(0, selected);
    fallback.label = QString("Standard device #%1").arg(fallback.nativeIndex);
    return fallback;
}

void YourClassName::selectDualFobosLibrary(FobosApiKind kind, bool stock) {
    if (dualFobosSwitchPending || closeShutdownInProgress || !isIdle() || deviceOpened ||
        (processor && processor->isRunning()) || isNetworkClientMode() || isExternalReceiverBackendSelected()) return;
    const FobosDeviceInfo selected = selectedFobosDeviceInfo();
    if (!selected.present || selected.serial.isEmpty() ||
        !isObriiDualFirmware(selected.hardwareRevision, selected.firmwareVersion) ||
        (stock && !fobosStockLibraryAvailable(kind))) return;
    const bool previousStock = dualFobosStockLibrary.value(selected.serial);
    if (kind == selected.apiKind && stock == previousStock) return;

    const bool uk = uiLanguage == QStringLiteral("uk");
    if (hasActiveFobosDevice() && !closeFobosSession(false)) {
        dualFobosSwitchStatus = uk ? QStringLiteral("Фобос не підтвердив закриття USB-сеансу.")
                                  : QStringLiteral("Fobos did not confirm USB session closure.");
        return;
    }
    if (usbDeviceRefreshTimer) usbDeviceRefreshTimer->stop();
    dualFobosSwitchPending = true;
    dualFobosSwitchStatus = uk ? QStringLiteral("Очікування USB-профілю…") : QStringLiteral("Waiting for USB profile…");
    updateUiForRunState();
    const int result = kind == selected.apiKind ? FOBOS_ERR_OK : setObriiDualUsbProfile(selected.serial, kind);
    if (result != FOBOS_ERR_OK) {
        dualFobosSwitchPending = false;
        dualFobosSwitchStatus = (uk ? QStringLiteral("Помилка команди USB (%1).")
                                    : QStringLiteral("USB command failed (%1).")).arg(result);
        requestUsbDeviceRefresh();
        updateUiForRunState();
        return;
    }

    auto *retry = new QTimer(this);
    retry->setInterval(300);
    connect(retry, &QTimer::timeout, this, [this, retry, serial = selected.serial, kind, stock, previousStock, uk,
                                         attempts = 0]() mutable {
        if (closeShutdownInProgress) { retry->deleteLater(); return; }
        refreshFobosDeviceList();
        const auto match = std::find_if(availableFobosDevices.cbegin(), availableFobosDevices.cend(),
            [&serial, kind](const FobosDeviceInfo &info) {
                return info.present && info.serial == serial && info.apiKind == kind &&
                    isObriiDualFirmware(info.hardwareRevision, info.firmwareVersion);
            });
        if (match == availableFobosDevices.cend() && ++attempts < 20) return;
        retry->stop();
        retry->deleteLater();
        dualFobosSwitchPending = false;
        if (match == availableFobosDevices.cend()) {
            dualFobosSwitchStatus = uk ? QStringLiteral("Фобос не з'явився у вибраному USB-профілі.")
                                      : QStringLiteral("Fobos did not reappear in the selected USB profile.");
            requestUsbDeviceRefresh();
        } else {
            pendingSettings.deviceIndex = static_cast<int>(match - availableFobosDevices.cbegin());
            dualFobosStockLibrary.insert(serial, stock);
            rebuildReceiverDeviceCombo();
            rebuildReceiverModeChoices();
            updateReceiverSpecificControls();
            if (prepareFobosSessionFromSettings(QStringLiteral("Dual firmware library selection"))) {
                dualFobosSwitchStatus.clear();
            } else {
                dualFobosStockLibrary.insert(serial, previousStock);
                dualFobosSwitchStatus = uk ? QStringLiteral("Вибрана бібліотека не відкрила Фобос.")
                                          : QStringLiteral("The selected library could not open Fobos.");
                requestUsbDeviceRefresh();
            }
        }
        updateUiForRunState();
    });
    retry->start();
}

bool YourClassName::isRtlTcpSelected() const {
    int selected = pendingSettings.deviceIndex;
    if (comboBox) {
        bool ok = false;
        const int value = comboBox->currentData().toInt(&ok);
        if (ok) {
            selected = receiverDeviceIndexFromComboValue(value);
        }
    }
    return selected == RTL_TCP_DEVICE_INDEX;
}

bool YourClassName::isRtlSdrNativeSelected() const {
    int selected = pendingSettings.deviceIndex;
    if (comboBox) {
        bool ok = false;
        const int value = comboBox->currentData().toInt(&ok);
        if (ok) {
            selected = receiverDeviceIndexFromComboValue(value);
        }
    }
    return isRtlSdrNativeComboValue(selected);
}

bool YourClassName::isSoapySdrSelected() const {
    int selected = pendingSettings.deviceIndex;
    if (comboBox) {
        bool ok = false;
        const int value = comboBox->currentData().toInt(&ok);
        if (ok) {
            selected = receiverDeviceIndexFromComboValue(value);
        }
    }
    return selected == SOAPY_SDR_DEVICE_INDEX;
}

bool YourClassName::isUhdNativeSelected() const {
    int selected = pendingSettings.deviceIndex;
    if (comboBox) {
        bool ok = false;
        const int value = comboBox->currentData().toInt(&ok);
        if (ok) selected = receiverDeviceIndexFromComboValue(value);
    }
    return isUhdNativeComboValue(selected);
}

bool YourClassName::isBladeRfNativeSelected() const {
    int selected = pendingSettings.deviceIndex;
    if (comboBox) {
        bool ok = false;
        const int value = comboBox->currentData().toInt(&ok);
        if (ok) {
            selected = receiverDeviceIndexFromComboValue(value);
        }
    }
    return isBladeRfNativeComboValue(selected);
}

int YourClassName::selectedRtlSdrNativeIndex() const {
    int selected = pendingSettings.deviceIndex;
    if (comboBox) {
        bool ok = false;
        const int value = comboBox->currentData().toInt(&ok);
        if (ok) {
            selected = receiverDeviceIndexFromComboValue(value);
        }
    }
    return isRtlSdrNativeComboValue(selected) ? rtlSdrNativeIndexFromComboValue(selected) : 0;
}

int YourClassName::selectedBladeRfNativeIndex() const {
    int selected = pendingSettings.deviceIndex;
    if (comboBox) {
        bool ok = false;
        const int value = comboBox->currentData().toInt(&ok);
        if (ok) {
            selected = receiverDeviceIndexFromComboValue(value);
        }
    }
    return isBladeRfNativeComboValue(selected) ? bladeRfNativeIndexFromComboValue(selected) : 0;
}

bool YourClassName::isHackRfNativeSelected() const {
    int selected = pendingSettings.deviceIndex;
    if (comboBox) {
        bool ok = false;
        const int value = comboBox->currentData().toInt(&ok);
        if (ok) {
            selected = receiverDeviceIndexFromComboValue(value);
        }
    }
    return isHackRfNativeComboValue(selected);
}

bool YourClassName::isVirtualSignalSelected() const {
    int selected = pendingSettings.deviceIndex;
    if (comboBox) {
        bool ok = false;
        const int value = comboBox->currentData().toInt(&ok);
        if (ok) selected = receiverDeviceIndexFromComboValue(value);
    }
    return selected == VIRTUAL_SIGNAL_DEVICE_INDEX;
}

int YourClassName::selectedHackRfNativeIndex() const {
    int selected = pendingSettings.deviceIndex;
    if (comboBox) {
        bool ok = false;
        const int value = comboBox->currentData().toInt(&ok);
        if (ok) {
            selected = receiverDeviceIndexFromComboValue(value);
        }
    }
    return isHackRfNativeComboValue(selected) ? hackRfNativeIndexFromComboValue(selected) : 0;
}

bool YourClassName::isRtlBackendSelected() const {
    return isRtlTcpSelected() || isRtlSdrNativeSelected();
}

bool YourClassName::isExternalReceiverBackendSelected() const {
    return isRtlBackendSelected() || isSoapySdrSelected() || isUhdNativeSelected() || isBladeRfNativeSelected() ||
           isHackRfNativeSelected() || isVirtualSignalSelected();
}

bool YourClassName::normalizeRtlSdrSettings() {
    if (!isRtlBackendSelected()) {
        return false;
    }

    if (!isKnownRtlSampleRate(pendingSettings.sampleRate)) {
        qDebug() << "[RTL-TCP] replacing non-RTL sample rate"
                 << pendingSettings.sampleRate
                 << "with" << RTL_TCP_SAFE_SAMPLE_RATE;
        pendingSettings.sampleRate = RTL_TCP_SAFE_SAMPLE_RATE;
        if (sampleBox) {
            QSignalBlocker blocker(sampleBox);
            const int index = sampleBox->findData(pendingSettings.sampleRate);
            if (index >= 0) {
                sampleBox->setCurrentIndex(index);
            }
        }
        settingRange();
        return true;
    }
    return false;
}

ReceiverStreamDescriptor YourClassName::makeRtlTcpStreamDescriptor(bool queueAudioBlocks,
                                                                   bool publishIqSnapshot,
                                                                   bool emitIqFrames) const {
    ReceiverStreamDescriptor stream;
    stream.kind = ReceiverBackendStreamKind::RtlTcp;
    stream.backendId = QStringLiteral("rtl-tcp");
    stream.backendName = QStringLiteral("RTL-SDR via rtl_tcp");
    stream.nativeDevice = nullptr;
    stream.sampleRateHz = pendingSettings.sampleRate;
    stream.centerFrequencyHz = pendingSettings.centerFrequency;
    stream.frequencyCalibrationOffsetHz = effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency);
    stream.rtlTcpHost = QString::fromLatin1(RTL_TCP_DEFAULT_HOST);
    stream.rtlTcpPort = RTL_TCP_DEFAULT_PORT;
    stream.rtlTcpAgc = pendingSettings.rtlAgc;
    stream.rtlTcpTunerGainTenthsDb =
        pendingSettings.rtlAgc ? -1 : (std::clamp)(pendingSettings.rtlTunerGainTenthsDb, 0, 496);
    stream.syncReader = false;
    stream.queueAudioBlocks = queueAudioBlocks;
    stream.publishIqSnapshot = publishIqSnapshot;
    stream.emitIqFrames = emitIqFrames;
    stream.agileScanEnabled = false;
    return stream;
}

ReceiverStreamDescriptor YourClassName::makeRtlSdrNativeStreamDescriptor(bool queueAudioBlocks,
                                                                        bool publishIqSnapshot,
                                                                        bool emitIqFrames) const {
    ReceiverStreamDescriptor stream;
    stream.kind = ReceiverBackendStreamKind::RtlSdrNative;
    stream.backendId = QStringLiteral("rtl-sdr-native");
    stream.backendName = QStringLiteral("RTL-SDR native");
    stream.nativeDevice = nullptr;
    stream.sampleRateHz = pendingSettings.sampleRate;
    stream.centerFrequencyHz = pendingSettings.centerFrequency;
    stream.frequencyCalibrationOffsetHz = effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency);
    stream.rtlSdrNativeDeviceIndex = selectedRtlSdrNativeIndex();
    stream.rtlTcpAgc = pendingSettings.rtlAgc;
    stream.rtlTcpTunerGainTenthsDb =
        pendingSettings.rtlAgc ? -1 : (std::clamp)(pendingSettings.rtlTunerGainTenthsDb, 0, 496);
    stream.syncReader = false;
    stream.queueAudioBlocks = queueAudioBlocks;
    stream.publishIqSnapshot = publishIqSnapshot;
    stream.emitIqFrames = emitIqFrames;
    stream.agileScanEnabled = false;
    return stream;
}

ReceiverStreamDescriptor YourClassName::makeSoapySdrStreamDescriptor(bool queueAudioBlocks,
                                                                     bool publishIqSnapshot,
                                                                     bool emitIqFrames) const {
    ReceiverStreamDescriptor stream;
    stream.kind = ReceiverBackendStreamKind::SoapySdr;
    stream.backendId = QStringLiteral("soapy-sdr");
    stream.backendName = QStringLiteral("SoapySDR");
    stream.nativeDevice = nullptr;
    stream.sampleRateHz = pendingSettings.sampleRate;
    stream.centerFrequencyHz = pendingSettings.centerFrequency;
    stream.frequencyCalibrationOffsetHz = effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency);
    stream.soapySdrDeviceIndex = 0;
    stream.syncReader = false;
    stream.queueAudioBlocks = queueAudioBlocks;
    stream.publishIqSnapshot = publishIqSnapshot;
    stream.emitIqFrames = emitIqFrames;
    stream.agileScanEnabled = false;
    return stream;
}

ReceiverStreamDescriptor YourClassName::makeUhdNativeStreamDescriptor(bool queueAudioBlocks,
                                                                      bool publishIqSnapshot,
                                                                      bool emitIqFrames) const {
    ReceiverStreamDescriptor stream;
    stream.kind = ReceiverBackendStreamKind::UhdNative;
    stream.backendId = QStringLiteral("uhd-native");
    stream.backendName = QStringLiteral("USRP UHD native RX");
    stream.sampleRateHz = pendingSettings.sampleRate;
    stream.centerFrequencyHz = pendingSettings.centerFrequency;
    stream.frequencyCalibrationOffsetHz = effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency);
    stream.uhdNativeDeviceIndex = uhdNativeIndexFromComboValue(pendingSettings.deviceIndex);
    stream.uhdRxChannel = pendingSettings.uhdRxChannel;
    stream.uhdClockSource = pendingSettings.uhdClockSource;
    stream.uhdGnssBaud = pendingSettings.uhdGnssBaud;
    stream.uhdRxGainDb = pendingSettings.uhdRxGainDb;
    stream.uhdRxAntenna = pendingSettings.uhdRxAntenna;
    stream.uhdFpgaImagePath = pendingSettings.uhdFpgaImagePath;
    stream.queueAudioBlocks = queueAudioBlocks;
    stream.publishIqSnapshot = publishIqSnapshot;
    stream.emitIqFrames = emitIqFrames;
    return stream;
}

void YourClassName::showUhdRxSettings() {
    const bool remote = isNetworkClientMode();
    const bool uk = uiLanguage.startsWith(QLatin1String("uk"));
    const bool canEditRx = remote ? networkController && networkController->clientHasControl() : isIdle();
    const auto remoteRx = networkClientConfirmedSettingsJson.value(QStringLiteral("uhdRx")).toObject();
    const auto remoteSecondary = remoteRx.value(QStringLiteral("secondary")).toObject();
    if (remote && (remoteRx.isEmpty() || !networkController || !networkController->isControlReady() ||
                   isRemoteVfoProcessingMode())) return;
    QDialog dialog(this);
    dialog.setWindowTitle(uiText(QStringLiteral("uhd_rx_settings"), QStringLiteral("USRP UHD RX")));
    auto *layout = new QFormLayout(&dialog);
    auto *channel = new QComboBox(&dialog);
    channel->addItem(QStringLiteral("RX 0"), 0);
    channel->addItem(QStringLiteral("RX 1"), 1);
    const int rxChannel = remote ? remoteRx.value(QStringLiteral("channel")).toInt() : pendingSettings.uhdRxChannel;
    channel->setCurrentIndex((std::clamp)(rxChannel, 0, 1));
    channel->setEnabled(canEditRx && (!remote || remoteSecondary.isEmpty()));
    auto *gain = new QDoubleSpinBox(&dialog);
    gain->setRange(0.0, 100.0);
    gain->setSuffix(QStringLiteral(" dB"));
    gain->setValue(remote ? remoteRx.value(QStringLiteral("gainDb")).toDouble() : pendingSettings.uhdRxGainDb);
    gain->setEnabled(canEditRx);
    const auto makeAntenna = [this, &dialog](const QString &selected) {
        auto *combo = new QComboBox(&dialog);
        combo->addItem(uiText(QStringLiteral("uhd_default_antenna"), QStringLiteral("Driver default")), QString());
        combo->addItem(QStringLiteral("RX2"), QStringLiteral("RX2"));
        combo->addItem(QStringLiteral("TX/RX"), QStringLiteral("TX/RX"));
        if (!selected.isEmpty() && combo->findData(selected) < 0)
            combo->addItem(selected, selected);
        combo->setCurrentIndex((std::max)(0, combo->findData(selected)));
        return combo;
    };
    const QString rxAntenna = remote ? remoteRx.value(QStringLiteral("antenna")).toString() : pendingSettings.uhdRxAntenna;
    auto *antenna = makeAntenna(rxAntenna);
    antenna->setEnabled(canEditRx);
    auto *fpgaRow = new QWidget(&dialog);
    auto *fpgaLayout = new QHBoxLayout(fpgaRow);
    fpgaLayout->setContentsMargins(0, 0, 0, 0);
    auto *fpgaPath = new QLineEdit(pendingSettings.uhdFpgaImagePath, fpgaRow);
    auto *browse = new QToolButton(fpgaRow);
    browse->setIcon(dialog.style()->standardIcon(QStyle::SP_DirOpenIcon));
    browse->setToolTip(uiText(QStringLiteral("uhd_browse_fpga"), QStringLiteral("Choose FPGA image")));
    fpgaLayout->addWidget(fpgaPath, 1);
    fpgaLayout->addWidget(browse);
    fpgaRow->setEnabled(canEditRx);
    connect(browse, &QToolButton::clicked, &dialog, [fpgaPath, &dialog]() {
        const QString file = QFileDialog::getOpenFileName(&dialog,
            QStringLiteral("UHD FPGA image"), fpgaPath->text(), QStringLiteral("FPGA image (*.bin);;All files (*)"));
        if (!file.isEmpty()) fpgaPath->setText(file);
    });
    layout->addRow(uiText(QStringLiteral("uhd_rx_channel"), QStringLiteral("RX channel")), channel);
    layout->addRow(uiText(QStringLiteral("uhd_rx_gain"), QStringLiteral("RX gain")), gain);
    layout->addRow(uiText(QStringLiteral("uhd_rx_antenna"), QStringLiteral("RX antenna")), antenna);
    QDoubleSpinBox *secondaryGain = nullptr;
    QComboBox *secondaryAntenna = nullptr;
    if (remote && !remoteSecondary.isEmpty()) {
        const int secondaryChannel = remoteSecondary.value(QStringLiteral("channel")).toInt();
        const QString prefix = QStringLiteral("RX %1 — ").arg(secondaryChannel);
        secondaryGain = new QDoubleSpinBox(&dialog);
        secondaryGain->setRange(gain->minimum(), gain->maximum());
        secondaryGain->setSuffix(gain->suffix());
        secondaryGain->setValue(remoteSecondary.value(QStringLiteral("gainDb")).toDouble());
        secondaryAntenna = makeAntenna(remoteSecondary.value(QStringLiteral("antenna")).toString());
        secondaryGain->setEnabled(canEditRx);
        secondaryAntenna->setEnabled(canEditRx);
        layout->addRow(prefix + uiText(QStringLiteral("uhd_rx_gain"), QStringLiteral("RX gain")), secondaryGain);
        layout->addRow(prefix + uiText(QStringLiteral("uhd_rx_antenna"), QStringLiteral("RX antenna")), secondaryAntenna);
    }
    auto *gnssBaud = new QComboBox(&dialog);
    if (remote) {
        fpgaRow->hide();
        gnssBaud->hide();
    } else {
        layout->addRow(uiText(QStringLiteral("uhd_fpga_image"), QStringLiteral("FPGA image (optional)")), fpgaRow);
        gnssBaud->addItem(uk ? QStringLiteral("Авто (датчики UHD)") : QStringLiteral("Auto (UHD sensors)"), 0);
        gnssBaud->addItem(QStringLiteral("38400"), 38400);
        gnssBaud->addItem(QStringLiteral("9600"), 9600);
        gnssBaud->setCurrentIndex((std::max)(0, gnssBaud->findData(pendingSettings.uhdGnssBaud)));
        gnssBaud->setEnabled(canEditRx);
        gnssBaud->setToolTip(uk
            ? QStringLiteral("Вкажіть швидкість NMEA для сумісного образу FPGA і бібліотеки UHD B210.")
            : QStringLiteral("Set the NMEA baud rate for a compatible B210 FPGA image and UHD library."));
        layout->addRow(uk ? QStringLiteral("Швидкість GPS NMEA") : QStringLiteral("GPS NMEA baud"), gnssBaud);
    }
    const auto internal = uk ? QStringLiteral("Внутрішня") : QStringLiteral("Internal");
    const auto external = uk ? QStringLiteral("Зовнішня 10 МГц") : QStringLiteral("External 10 MHz");
    const auto sourceName = [&](const QString &source) {
        if (source == QLatin1String("internal")) return internal;
        if (source == QLatin1String("external")) return external;
        return source;
    };
    const bool clockControl = remote ? remoteRx.value(QStringLiteral("clockControl")).toBool() : uhdClockControlAvailable();
    auto *clock = new QComboBox(&dialog);
    clock->addItem(uk ? QStringLiteral("Авто (за драйвером)") : QStringLiteral("Auto (driver default)"), QStringLiteral("auto"));
    clock->addItem(internal, QStringLiteral("internal"));
    clock->addItem(external, QStringLiteral("external"));
    const auto selectedClock = remote ? remoteRx.value(QStringLiteral("clockSource")).toString() : pendingSettings.uhdClockSource;
    clock->setCurrentIndex((std::max)(0, clock->findData(selectedClock)));
    clock->setEnabled(clockControl && (!remote || canEditRx));
    layout->addRow(uk ? QStringLiteral("Джерело опори") : QStringLiteral("Clock source"), clock);
    auto *clockStatus = new QLabel(&dialog);
    clockStatus->setTextFormat(Qt::PlainText);
    layout->addRow(uk ? QStringLiteral("Фактична опора") : QStringLiteral("Active reference"), clockStatus);
    if (!remote && !canEditRx) {
        layout->addRow(new QLabel(uk ? QStringLiteral("Нова опора застосовується після Стоп → Старт.")
                                     : QStringLiteral("The new source applies after Stop → Start."), &dialog));
    }
    auto *refreshClock = new QTimer(&dialog);
    const auto updateClockStatus = [&] {
        UhdClockStatus status;
        if (remote && networkController->isControlReady()) {
            const auto state = networkClientConfirmedSettingsJson.value(QStringLiteral("uhdClock")).toObject();
            status.source = state.value(QStringLiteral("source")).toString();
            const auto locked = state.value(QStringLiteral("refLocked"));
            if (locked.isBool()) status.locked = locked.toBool();
        } else if (!remote && processor && !isIdle()) {
            status = processor->uhdClockStatus();
        }
        if (status.source.isEmpty()) {
            clockStatus->setText(uk ? QStringLiteral("Немає даних") : QStringLiteral("No reading"));
        } else if (status.source == QLatin1String("internal")) {
            clockStatus->setText(internal);
        } else {
            const auto lockText = !status.locked
                ? (uk ? QStringLiteral("статус недоступний") : QStringLiteral("lock status unavailable"))
                : (*status.locked ? (uk ? QStringLiteral("захоплено") : QStringLiteral("locked"))
                                  : (uk ? QStringLiteral("немає захоплення") : QStringLiteral("unlocked")));
            clockStatus->setText(sourceName(status.source) + QStringLiteral(" · ") + lockText);
        }
    };
    connect(refreshClock, &QTimer::timeout, &dialog, updateClockStatus);
    refreshClock->start(300);
    updateClockStatus();
    auto *buttons = new QDialogButtonBox(remote
        ? QDialogButtonBox::Apply | QDialogButtonBox::Close
        : QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (remote) {
        auto *apply = buttons->button(QDialogButtonBox::Apply);
        apply->setEnabled(canEditRx);
        auto *result = new QLabel(&dialog);
        result->setWordWrap(true);
        result->setTextFormat(Qt::PlainText);
        layout->insertRow(layout->rowCount() - 1, result);
        QString requestId;
        QTimer ackDeadline;
        ackDeadline.setSingleShot(true);
        connect(apply, &QPushButton::clicked, &dialog, [&] {
            QJsonObject rx = remoteRx;
            rx.insert(QStringLiteral("channel"), channel->currentData().toInt());
            rx.insert(QStringLiteral("gainDb"), gain->value());
            rx.insert(QStringLiteral("antenna"), antenna->currentData().toString());
            if (clockControl) rx.insert(QStringLiteral("clockSource"), clock->currentData().toString());
            if (secondaryGain && secondaryAntenna) {
                auto secondary = remoteSecondary;
                secondary.insert(QStringLiteral("gainDb"), secondaryGain->value());
                secondary.insert(QStringLiteral("antenna"), secondaryAntenna->currentData().toString());
                rx.insert(QStringLiteral("secondary"), secondary);
            }
            if (sendRemoteControlCommand(QStringLiteral("setParameters"),
                    {{QStringLiteral("settings"), QJsonObject{{QStringLiteral("uhdRx"), rx}}}})) {
                requestId = QString::number(networkClientSettingsRequestCounter);
                apply->setEnabled(false);
                result->setText(uk ? QStringLiteral("Очікування відповіді UHD…") : QStringLiteral("Waiting for UHD…"));
                ackDeadline.start(10000);
            } else {
                result->setText(uk ? QStringLiteral("Команду не надіслано.") : QStringLiteral("Command was not sent."));
            }
        });
        connect(networkController, &NetworkController::controlCommandReceived, &dialog, [&](const QJsonObject &command) {
            if (requestId.isEmpty() || command.value("action") != QLatin1String("settingsAck") ||
                command.value("requestId").toString() != requestId) return;
            ackDeadline.stop();
            requestId.clear();
            apply->setEnabled(true);
            result->setText(command.value("ok").toBool()
                ? (uk ? QStringLiteral("Застосовано") : QStringLiteral("Applied"))
                : command.value("reason").toString());
            updateClockStatus();
        });
        connect(&ackDeadline, &QTimer::timeout, &dialog, [&] {
            requestId.clear();
            apply->setEnabled(true);
            result->setText(uk ? QStringLiteral("Сервер не підтвердив зміни.") : QStringLiteral("Server did not acknowledge changes."));
        });
        dialog.exec();
        return;
    }
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted) return;
    if (clockControl) pendingSettings.uhdClockSource = clock->currentData().toString();
    if (canEditRx) pendingSettings.uhdGnssBaud = gnssBaud->currentData().toInt();
    pendingSettings.uhdRxChannel = channel->currentData().toInt();
    pendingSettings.uhdRxGainDb = gain->value();
    pendingSettings.uhdRxAntenna = antenna->currentData().toString();
    pendingSettings.uhdFpgaImagePath = fpgaPath->text().trimmed();
    savePersistentSettings();
}

ReceiverStreamDescriptor YourClassName::makeBladeRfNativeStreamDescriptor(bool queueAudioBlocks,
                                                                         bool publishIqSnapshot,
                                                                         bool emitIqFrames) const {
    ReceiverStreamDescriptor stream;
    stream.kind = ReceiverBackendStreamKind::BladeRfNative;
    stream.backendId = QStringLiteral("bladerf-native");
    stream.backendName = QStringLiteral("bladeRF native");
    stream.nativeDevice = nullptr;
    stream.sampleRateHz = pendingSettings.sampleRate;
    stream.centerFrequencyHz = pendingSettings.centerFrequency;
    stream.frequencyCalibrationOffsetHz = effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency);
    stream.bladeRfNativeDeviceIndex = selectedBladeRfNativeIndex();
    stream.syncReader = false;
    stream.queueAudioBlocks = queueAudioBlocks;
    stream.publishIqSnapshot = publishIqSnapshot;
    stream.emitIqFrames = emitIqFrames;
    stream.agileScanEnabled = false;
    return stream;
}

ReceiverStreamDescriptor YourClassName::makeHackRfNativeStreamDescriptor(bool queueAudioBlocks,
                                                                         bool publishIqSnapshot,
                                                                         bool emitIqFrames) const {
    ReceiverStreamDescriptor stream;
    stream.kind = ReceiverBackendStreamKind::HackRfNative;
    stream.backendId = QStringLiteral("hackrf-native");
    stream.backendName = QStringLiteral("HackRF native");
    stream.nativeDevice = nullptr;
    stream.sampleRateHz = pendingSettings.sampleRate;
    stream.centerFrequencyHz = pendingSettings.centerFrequency;
    stream.frequencyCalibrationOffsetHz = effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency);
    stream.hackRfNativeDeviceIndex = selectedHackRfNativeIndex();
    stream.hackRfLnaGainDb = (std::clamp)(pendingSettings.hackRfLnaGainDb, 0, 40) / 8 * 8;
    stream.hackRfVgaGainDb = (std::clamp)(pendingSettings.hackRfVgaGainDb, 0, 62) / 2 * 2;
    stream.hackRfAmpEnabled = pendingSettings.hackRfAmpEnabled;
    stream.hackRfBiasTeeEnabled = pendingSettings.hackRfBiasTeeEnabled;
    stream.hackRfAutomaticBandwidth = pendingSettings.hackRfAutomaticBandwidth;
    stream.hackRfBandwidthHz =
        static_cast<std::uint32_t>((std::max)(0, pendingSettings.hackRfBandwidthHz));
    stream.hackRfExplicitTuningEnabled = pendingSettings.hackRfExplicitTuningEnabled;
    stream.hackRfExplicitIfHz = static_cast<std::uint64_t>(
        (std::max)(0.0, pendingSettings.hackRfExplicitIfHz));
    stream.hackRfExplicitLoHz = static_cast<std::uint64_t>(
        (std::max)(0.0, pendingSettings.hackRfExplicitLoHz));
    stream.hackRfExplicitPath = (std::clamp)(pendingSettings.hackRfExplicitPath, 0, 2);
    stream.hackRfClockOutEnabled = pendingSettings.hackRfClockOutEnabled;
    stream.hackRfHardwareSyncEnabled = pendingSettings.hackRfHardwareSyncEnabled;
    stream.hackRfRxOverrunLimit =
        static_cast<std::uint32_t>((std::max)(0, pendingSettings.hackRfRxOverrunLimit));
    stream.hackRfOperaCakeEnabled = pendingSettings.hackRfOperaCakeEnabled;
    stream.hackRfOperaCakeAddress = (std::clamp)(pendingSettings.hackRfOperaCakeAddress, 0, 7);
    stream.hackRfOperaCakeMode = (std::clamp)(pendingSettings.hackRfOperaCakeMode, 0, 2);
    stream.hackRfOperaCakePortA = (std::clamp)(pendingSettings.hackRfOperaCakePortA, 0, 7);
    stream.hackRfOperaCakePortB = (std::clamp)(pendingSettings.hackRfOperaCakePortB, 0, 7);
    stream.hackRfOperaCakeRangesJson = pendingSettings.hackRfOperaCakeRangesJson;
    stream.hackRfOperaCakeDwellsJson = pendingSettings.hackRfOperaCakeDwellsJson;
    stream.syncReader = false;
    stream.queueAudioBlocks = queueAudioBlocks;
    stream.publishIqSnapshot = publishIqSnapshot;
    stream.emitIqFrames = emitIqFrames;
    stream.agileScanEnabled = false;
    return stream;
}

ReceiverStreamDescriptor YourClassName::makeVirtualSignalStreamDescriptor(bool queueAudioBlocks,
                                                                           bool publishIqSnapshot,
                                                                           bool emitIqFrames) const {
    ReceiverStreamDescriptor stream;
    stream.kind = ReceiverBackendStreamKind::VirtualSignal;
    stream.backendId = QStringLiteral("virtual-signal");
    stream.backendName = QStringLiteral("Virtual IQ receiver");
    stream.sampleRateHz = pendingSettings.sampleRate;
    stream.centerFrequencyHz = pendingSettings.centerFrequency;
    stream.virtualSignalUseTcp = pendingSettings.virtualSignalUseTcp;
    stream.virtualSignalLocalName = pendingSettings.virtualSignalLocalName.trimmed();
    stream.virtualSignalListenAddress = pendingSettings.virtualSignalListenAddress.trimmed();
    stream.virtualSignalPort = static_cast<quint16>(
        (std::clamp)(pendingSettings.virtualSignalPort, 1, 65535));
    stream.virtualSignalFollowMetadata = pendingSettings.virtualSignalFollowMetadata;
    stream.queueAudioBlocks = queueAudioBlocks;
    stream.publishIqSnapshot = publishIqSnapshot;
    stream.emitIqFrames = emitIqFrames;
    return stream;
}

void YourClassName::showVirtualSignalRxSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(uiText(QStringLiteral("virtual_iq_receiver_settings"),
                                 QStringLiteral("Virtual IQ receiver")));
    QFormLayout *layout = new QFormLayout(&dialog);
    QComboBox *transport = new QComboBox(&dialog);
    transport->addItem(uiText(QStringLiteral("virtual_iq_local_ipc"),
                              QStringLiteral("Local IPC (same computer)")), 0);
    transport->addItem(QStringLiteral("TCP/IP"), 1);
    transport->setCurrentIndex(pendingSettings.virtualSignalUseTcp ? 1 : 0);
    QLineEdit *localName = new QLineEdit(pendingSettings.virtualSignalLocalName, &dialog);
    QLineEdit *listenAddress = new QLineEdit(pendingSettings.virtualSignalListenAddress, &dialog);
    QSpinBox *port = new QSpinBox(&dialog);
    port->setRange(1, 65535);
    port->setValue(pendingSettings.virtualSignalPort);
    QCheckBox *followMetadata = new QCheckBox(
        uiText(QStringLiteral("virtual_iq_follow_metadata"),
               QStringLiteral("Follow incoming center frequency and sample rate")), &dialog);
    followMetadata->setChecked(pendingSettings.virtualSignalFollowMetadata);
    QLabel *hint = new QLabel(uiText(
        QStringLiteral("virtual_iq_rx_hint"),
        QStringLiteral("Use Local IPC for two Obrii SDR instances on one computer. "
                       "For another computer select TCP and listen on 0.0.0.0 or a local interface address.")),
        &dialog);
    hint->setWordWrap(true);
    layout->addRow(uiText(QStringLiteral("transport"), QStringLiteral("Transport:")), transport);
    layout->addRow(uiText(QStringLiteral("local_channel"), QStringLiteral("Local channel:")), localName);
    layout->addRow(uiText(QStringLiteral("listen_address"), QStringLiteral("Listen address:")), listenAddress);
    layout->addRow(uiText(QStringLiteral("port"), QStringLiteral("Port:")), port);
    layout->addRow(followMetadata);
    layout->addRow(hint);
    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    auto updateFields = [transport, localName, listenAddress, port]() {
        const bool tcp = transport->currentData().toInt() == 1;
        localName->setEnabled(!tcp);
        listenAddress->setEnabled(tcp);
        port->setEnabled(tcp);
    };
    connect(transport, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
            [updateFields](int) { updateFields(); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    updateFields();
    if (dialog.exec() != QDialog::Accepted) return;
    pendingSettings.virtualSignalUseTcp = transport->currentData().toInt() == 1;
    pendingSettings.virtualSignalLocalName = localName->text().trimmed().isEmpty()
        ? QStringLiteral("obrii-virtual-iq") : localName->text().trimmed();
    pendingSettings.virtualSignalListenAddress = listenAddress->text().trimmed().isEmpty()
        ? QStringLiteral("127.0.0.1") : listenAddress->text().trimmed();
    pendingSettings.virtualSignalPort = port->value();
    pendingSettings.virtualSignalFollowMetadata = followMetadata->isChecked();
    savePersistentSettings();
}

void YourClassName::listFobosDevices() {
    const bool canRefreshUsbList =
        !hasActiveFobosDevice() &&
        !deviceOpened &&
        !(processor && processor->isRunning()) &&
        !isRunningOrTransitioning();
    if (canRefreshUsbList) {
        refreshFobosDeviceList();
        rebuildReceiverDeviceCombo();
        updateAgileScanControls();
    }

    QVector<FobosDeviceInfo> devicesForReport = availableFobosDevices;
    devicesForReport.erase(std::remove_if(devicesForReport.begin(), devicesForReport.end(),
        [](const FobosDeviceInfo &info) { return !info.present; }), devicesForReport.end());
    if (devicesForReport.isEmpty() && hasActiveFobosDevice()) {
        FobosDeviceInfo activeInfo;
        activeInfo.apiKind = activeFobosApiKind;
        activeInfo.nativeIndex = openedNativeDeviceIndex >= 0
                                     ? openedNativeDeviceIndex
                                     : std::max(0, pendingSettings.deviceIndex);
        activeInfo.label = QStringLiteral("[%1] active device #%2")
                               .arg(fobosApiDisplayName(activeInfo.apiKind))
                               .arg(activeInfo.nativeIndex);
        devicesForReport.append(activeInfo);
    }

    char standardLib[256] = {};
    char standardDriver[256] = {};
    char agileLib[256] = {};
    char agileDriver[256] = {};
    getFobosStandardApiInfoSafely(standardLib, standardDriver);
    getFobosAgileApiInfoSafely(agileLib, agileDriver);

    QString deviceInfo = QStringLiteral("%1 %2 (%3)\n%4 %5 (%6)\n\n%7 %8\n")
                             .arg(uiText(QStringLiteral("standard_api"), QStringLiteral("Standard API:")))
                             .arg(standardLib)
                             .arg(standardDriver)
                             .arg(uiText(QStringLiteral("agile_api"), QStringLiteral("Agile API:")))
                             .arg(agileLib)
                             .arg(agileDriver)
                             .arg(uiText(QStringLiteral("detected_devices"), QStringLiteral("Detected devices:")))
                             .arg(devicesForReport.size());
    if (!canRefreshUsbList) {
        deviceInfo += uiText(QStringLiteral("detected_devices_cached"),
                             QStringLiteral("Receiver is active; showing cached/active device information.\n"));
    }
    for (int i = 0; i < devicesForReport.size(); ++i) {
        const FobosDeviceInfo &info = devicesForReport[i];
        deviceInfo += QStringLiteral("%1. %2\n    %3 %4\n    %5 %6\n")
                          .arg(i)
                          .arg(info.label)
                          .arg(uiText(QStringLiteral("manufacturer"), QStringLiteral("manufacturer:")))
                          .arg(info.manufacturer)
                          .arg(uiText(QStringLiteral("product"), QStringLiteral("product:")))
                          .arg(info.product);
    }
    QMessageBox::information(this,
                             uiText(QStringLiteral("fobos_devices"), QStringLiteral("Fobos Devices")),
                             deviceInfo);
}
