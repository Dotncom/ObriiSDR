#ifndef BLADERFBACKEND_H
#define BLADERFBACKEND_H

#include <QString>
#include <QVector>

#include <cstdint>

struct BladeRfDeviceInfo {
    int nativeIndex = 0;
    QString serial;
    QString manufacturer;
    QString product;
    QString label;
};

bool bladeRfLibraryAvailable(QString *loadedPath = nullptr, QString *errorMessage = nullptr);
QString bladeRfLastErrorMessage();
QVector<BladeRfDeviceInfo> enumerateBladeRfDevices();

int openBladeRfDeviceSafely(void **dev, int nativeIndex);
int closeBladeRfDeviceSafely(void *dev);
int setBladeRfCenterFrequencySafely(void *dev, uint64_t frequencyHz);
int setBladeRfSampleRateSafely(void *dev, uint32_t sampleRateHz, uint32_t *actualSampleRateHz = nullptr);
int setBladeRfBandwidthSafely(void *dev, uint32_t bandwidthHz, uint32_t *actualBandwidthHz = nullptr);
int setBladeRfGainModeSafely(void *dev, int gainMode);
int setBladeRfGainSafely(void *dev, int gainDb);
int configureBladeRfSyncRxSafely(void *dev,
                                 uint32_t bufferCount,
                                 uint32_t bufferSamples,
                                 uint32_t transferCount,
                                 uint32_t timeoutMs);
int enableBladeRfRxSafely(void *dev, bool enabled);
int readBladeRfSyncRxSafely(void *dev, int16_t *samples, uint32_t sampleCount, uint32_t timeoutMs);
const char *bladeRfErrorString(int status);

#endif // BLADERFBACKEND_H
