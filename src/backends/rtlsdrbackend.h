#ifndef RTLSDRBACKEND_H
#define RTLSDRBACKEND_H

#include <QString>
#include <QVector>

#include <cstdint>

struct RtlSdrDeviceInfo {
    int nativeIndex = 0;
    QString name;
    QString label;
};

using RtlSdrAsyncCallback = void (*)(unsigned char *buf, uint32_t len, void *ctx);

bool rtlSdrLibraryAvailable(QString *loadedPath = nullptr, QString *errorMessage = nullptr);
QVector<RtlSdrDeviceInfo> enumerateRtlSdrDevices();

int openRtlSdrDeviceSafely(void **dev, uint32_t index);
int closeRtlSdrDeviceSafely(void *dev);
int setRtlSdrCenterFrequencySafely(void *dev, uint32_t frequencyHz);
int setRtlSdrSampleRateSafely(void *dev, uint32_t sampleRateHz);
int setRtlSdrDirectSamplingSafely(void *dev, int enabled);
int setRtlSdrFrequencyCorrectionSafely(void *dev, int ppm);
int setRtlSdrTunerGainModeSafely(void *dev, int manual);
int setRtlSdrTunerGainSafely(void *dev, int gainTenthsDb);
int setRtlSdrAgcModeSafely(void *dev, int enabled);
int resetRtlSdrBufferSafely(void *dev);
int readRtlSdrSyncSafely(void *dev, void *buffer, int bytes, int *bytesRead);
int readRtlSdrAsyncSafely(void *dev,
                          RtlSdrAsyncCallback callback,
                          void *ctx,
                          uint32_t bufferCount,
                          uint32_t blockBytes);
int cancelRtlSdrAsyncSafely(void *dev);

#endif // RTLSDRBACKEND_H
