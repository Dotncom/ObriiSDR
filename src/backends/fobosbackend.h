#ifndef FOBOSBACKEND_H
#define FOBOSBACKEND_H

#include "receiverbackend.h"

#include <fobos.h>
#include <fobos_sdr.h>

#include <QString>

#include <cstdint>

extern fobos_dev_t *device;
extern fobos_sdr_dev_t *agileDevice;
extern FobosApiKind activeFobosApiKind;

const char *fobosApiKindName(FobosApiKind kind);
QString fobosApiDisplayName(FobosApiKind kind);
void *activeFobosDevice();
bool hasActiveFobosDevice();
ReceiverBackendFeatures fobosBackendFeatures(FobosApiKind kind);
ReceiverStreamDescriptor makeFobosStreamDescriptor(void *nativeDevice,
                                                   FobosApiKind apiKind,
                                                   bool syncReader,
                                                   double sampleRateHz,
                                                   double centerFrequencyHz,
                                                   bool queueAudioBlocks,
                                                   bool publishIqSnapshot,
                                                   bool emitIqFrames,
                                                   bool agileScanEnabled,
                                                   const QVector<double> &agileScanFrequenciesHz = QVector<double>(),
                                                   double frequencyCalibrationOffsetHz = 0.0);

int getFobosStandardApiInfoSafely(char *libVersion, char *driverVersion);
int getFobosAgileApiInfoSafely(char *libVersion, char *driverVersion);
int getFobosStandardDeviceCountSafely();
int getFobosAgileDeviceCountSafely();
int openFobosDeviceSafely(fobos_dev_t **dev, uint32_t index, const char *stockSerial = nullptr);
int openFobosAgileDeviceSafely(fobos_sdr_dev_t **dev, uint32_t index, const char *stockSerial = nullptr);
bool isObriiDualFirmware(const QString &hardware, const QString &firmware);
bool fobosStockLibraryAvailable(FobosApiKind kind);
bool activeFobosUsesStockLibrary();
int setObriiDualUsbProfile(const QString &serial, FobosApiKind kind);
int getFobosBoardInfoSafely(fobos_dev_t *dev,
                            char *hwRevision,
                            char *firmwareVersion,
                            char *manufacturer,
                            char *product,
                            char *serial);
int getFobosAgileBoardInfoSafely(fobos_sdr_dev_t *dev,
                                 char *hwRevision,
                                 char *firmwareVersion,
                                 char *manufacturer,
                                 char *product,
                                 char *serial);
int getFobosSampleRatesSafely(fobos_dev_t *dev, double *values, unsigned int *count);
int getFobosAgileSampleRatesSafely(fobos_sdr_dev_t *dev, double *values, unsigned int *count);
bool fobosAgileSupportsRf80(fobos_sdr_dev_t *dev);
void logFobosApiInfo();
int closeFobosDeviceSafely(fobos_dev_t *dev);
int resetFobosDeviceSafely(fobos_dev_t *dev);
int setFobosClockSourceSafely(fobos_dev_t *dev, unsigned int value);
int setFobosDirectSamplingSafely(fobos_dev_t *dev, unsigned int enabled);
int setFobosSampleRateSafely(fobos_dev_t *dev, double value, double *actual);
int setFobosFrequencySafely(fobos_dev_t *dev, double value, double *actual);
int setFobosLnaGainSafely(fobos_dev_t *dev, unsigned int value);
int setFobosVgaGainSafely(fobos_dev_t *dev, unsigned int value);
int setFobosGpoSafely(fobos_dev_t *dev, uint8_t value);
int closeFobosAgileDeviceSafely(fobos_sdr_dev_t *dev);
int resetFobosAgileDeviceSafely(fobos_sdr_dev_t *dev);
int setFobosAgileClockSourceSafely(fobos_sdr_dev_t *dev, int value);
int setFobosAgileDirectSamplingSafely(fobos_sdr_dev_t *dev, unsigned int enabled);
int setFobosAgileSampleRateSafely(fobos_sdr_dev_t *dev, double value);
int setFobosAgileFrequencySafely(fobos_sdr_dev_t *dev, double value);
int getFobosAgileIfModeSafely(fobos_sdr_dev_t *dev, unsigned int *mode, unsigned int *state = nullptr);
int setFobosAgileIfModeSafely(fobos_sdr_dev_t *dev, unsigned int mode);
int getFobosAgileIfTestModeSafely(fobos_sdr_dev_t *dev, unsigned int *mode, unsigned int *state = nullptr,
    unsigned int *row3IfMhz = nullptr, unsigned int *capabilities = nullptr);
int setFobosAgileIfTestModeSafely(fobos_sdr_dev_t *dev, unsigned int mode);
// ABI of the optional diagnostics exports. The standard SDK headers need not declare them.
struct FobosStreamMetrics {
    uint32_t abiVersion, structBytes, generation, transfers;
    uint32_t dmaBlocks, headerDisagreements, orderErrors, iqChangesWithinTransfer;
    uint32_t iqOrientationTransitions, shortTransfers, componentsInspected;
    uint32_t railComponents, nearRailComponents, minCode, maxCode;
    uint32_t lastIqSwap, lastFirstOrder, modeFlags, inspectionStrideComplex, reserved;
};
struct FobosDeviceMetrics { uint32_t words[24]; };
int getFobosAgileStreamMetricsSafely(fobos_sdr_dev_t *dev, FobosStreamMetrics *metrics);
int getFobosAgileDeviceMetricsSafely(fobos_sdr_dev_t *dev, FobosDeviceMetrics *metrics);
int setFobosAgileLnaGainSafely(fobos_sdr_dev_t *dev, unsigned int value);
int setFobosAgileVgaGainSafely(fobos_sdr_dev_t *dev, unsigned int value);
int setFobosAgileAutoBandwidthSafely(fobos_sdr_dev_t *dev, double value);
int setFobosAgileGpoSafely(fobos_sdr_dev_t *dev, uint8_t value);
int startFobosAgileScanSafely(fobos_sdr_dev_t *dev, double *frequencies, unsigned int count);
int stopFobosAgileScanSafely(fobos_sdr_dev_t *dev);
int isFobosAgileScanningSafely(fobos_sdr_dev_t *dev);
int getFobosAgileScanIndexSafely(fobos_sdr_dev_t *dev);
int startFobosSyncSafely(fobos_dev_t *dev, uint32_t blockSamples);
int startFobosAgileSyncSafely(fobos_sdr_dev_t *dev, uint32_t blockSamples);
int readFobosSyncSafely(fobos_dev_t *dev, float *buf, uint32_t *actualBufLength);
int readFobosAgileSyncSafely(fobos_sdr_dev_t *dev, float *buf, uint32_t *actualBufLength);
int stopFobosSyncSafely(fobos_dev_t *dev);
int stopFobosAgileSyncSafely(fobos_sdr_dev_t *dev);
int cancelFobosAsyncSafely(fobos_dev_t *dev);
int cancelFobosAgileAsyncSafely(fobos_sdr_dev_t *dev);
int readFobosAsyncSafely(fobos_dev_t *dev,
                         void (*callback)(float *buf, uint32_t bufLength, void *ctx),
                         void *ctx,
                         uint32_t bufferCount,
                         uint32_t blockSamples);
int readFobosAgileAsyncSafely(fobos_sdr_dev_t *dev,
                              void (*callback)(float *buf, uint32_t bufLength, fobos_sdr_dev_t *dev, void *ctx),
                              void *ctx,
                              uint32_t bufferCount,
                              uint32_t blockSamples);
int getActiveFobosIfModeSafely(unsigned int *mode, unsigned int *state = nullptr);
int setActiveFobosIfModeSafely(unsigned int mode);
int getActiveFobosIfPlanSafely(unsigned int *mode, unsigned int *state = nullptr,
    unsigned int *actualIfMhz = nullptr, unsigned int *capabilities = nullptr);
int setActiveFobosIfPlanSafely(unsigned int mode);
int getActiveFobosReceiverConfigSafely(unsigned int *words,unsigned int bytes=64);
int getActiveFobosIfTableSafely(unsigned int *frame,unsigned int *bytes);
int setActiveFobosIfTableSafely(const unsigned int *frame,unsigned int bytes);
int resetActiveFobosIfTableSafely();
int setActiveFobosStreamMetadataSafely(unsigned int enabled,unsigned int settleBlocks);
int setActiveFobosMixerBiasSafely(unsigned int code);
int setActiveFobosDmaCountSafely(unsigned int count);
int getActiveFobosRffcRegistersSafely(unsigned short *registers,unsigned int bytes=64);
int getFobosAgileMetadataMetricsSafely(fobos_sdr_dev_t *dev,unsigned int *words,unsigned int bytes=128);
int getFobosAgileConfiguredRateSafely(fobos_sdr_dev_t *dev,double *actual);
int setActiveClockSourceSafely(int value);
int setActiveDirectSamplingSafely(unsigned int enabled);
int setActiveSampleRateSafely(double value, double *actual);
int setActiveFrequencySafely(double value, double *actual);
int setActiveLnaGainSafely(unsigned int value);
int setActiveVgaGainSafely(unsigned int value);
int setActiveGpoSafely(uint8_t value);

#endif // FOBOSBACKEND_H
