#include "receiverdeviceutils.h"

#include "appconstants.h"

#include <cmath>

int rtlSdrNativeComboValue(int nativeIndex) {
    return RTLSDR_NATIVE_DEVICE_INDEX_BASE - nativeIndex;
}

bool isRtlSdrNativeComboValue(int value) {
    return value <= RTLSDR_NATIVE_DEVICE_INDEX_BASE && value > SOAPY_SDR_DEVICE_INDEX;
}

int rtlSdrNativeIndexFromComboValue(int value) {
    return RTLSDR_NATIVE_DEVICE_INDEX_BASE - value;
}

QString rtlSdrNativeDeviceLabel(int nativeIndex, const QString &name) {
    const QString cleanName = name.trimmed().isEmpty()
                                  ? QStringLiteral("RTL-SDR")
                                  : name.trimmed();
    return QStringLiteral("RTL-SDR native #%1 (%2)").arg(nativeIndex).arg(cleanName);
}

bool isKnownRtlSampleRate(double value) {
    static const double rtlRates[] = {
        1024000.0,
        1536000.0,
        2048000.0,
        2400000.0,
        2560000.0,
        3200000.0
    };
    for (double rate : rtlRates) {
        if (std::abs(value - rate) < 0.5) {
            return true;
        }
    }
    return false;
}

int bladeRfNativeComboValue(int nativeIndex) {
    return BLADERF_NATIVE_DEVICE_INDEX_BASE - nativeIndex;
}

bool isBladeRfNativeComboValue(int value) {
    return value <= BLADERF_NATIVE_DEVICE_INDEX_BASE && value > HACKRF_NATIVE_DEVICE_INDEX_BASE;
}

int bladeRfNativeIndexFromComboValue(int value) {
    return BLADERF_NATIVE_DEVICE_INDEX_BASE - value;
}

int hackRfNativeComboValue(int nativeIndex) {
    return HACKRF_NATIVE_DEVICE_INDEX_BASE - nativeIndex;
}

bool isHackRfNativeComboValue(int value) {
    return value <= HACKRF_NATIVE_DEVICE_INDEX_BASE && value > HACKRF_NATIVE_DEVICE_INDEX_BASE - 1000;
}

int hackRfNativeIndexFromComboValue(int value) {
    return HACKRF_NATIVE_DEVICE_INDEX_BASE - value;
}

bool isKnownHackRfSampleRate(double value) {
    static const double hackRfRates[] = {
        2000000.0,
        4000000.0,
        8000000.0,
        10000000.0,
        12500000.0,
        16000000.0,
        20000000.0
    };
    for (double rate : hackRfRates) {
        if (std::abs(value - rate) < 0.5) {
            return true;
        }
    }
    return false;
}

int uhdNativeComboValue(int nativeIndex) {
    return UHD_NATIVE_DEVICE_INDEX_BASE - nativeIndex;
}

bool isUhdNativeComboValue(int value) {
    return value <= UHD_NATIVE_DEVICE_INDEX_BASE && value > UHD_NATIVE_DEVICE_INDEX_BASE - 1000;
}

int uhdNativeIndexFromComboValue(int value) {
    return UHD_NATIVE_DEVICE_INDEX_BASE - value;
}

int remoteReceiverComboValue(int serverDeviceIndex) {
    return NETWORK_REMOTE_RECEIVER_DEVICE_INDEX_BASE + serverDeviceIndex;
}

bool isRemoteReceiverComboValue(int value) {
    return value >= NETWORK_REMOTE_RECEIVER_DEVICE_INDEX_MIN &&
           value <= NETWORK_REMOTE_RECEIVER_DEVICE_INDEX_MAX;
}

int remoteReceiverDeviceIndexFromComboValue(int value) {
    return value - NETWORK_REMOTE_RECEIVER_DEVICE_INDEX_BASE;
}

int receiverDeviceIndexFromComboValue(int value) {
    return isRemoteReceiverComboValue(value)
               ? remoteReceiverDeviceIndexFromComboValue(value)
               : value;
}
