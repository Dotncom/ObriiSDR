#ifndef SOAPYSDRBACKEND_H
#define SOAPYSDRBACKEND_H

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

struct SoapySdrDeviceInfo {
    int nativeIndex = 0;
    QString label;
    QString driver;
    QString hardware;
    QString serial;
};

struct SoapySdrRangeInfo {
    double minimum = 0.0;
    double maximum = 0.0;
    double step = 0.0;
};

struct SoapySdrDeviceCapabilities {
    bool valid = false;
    int rxChannels = 0;
    int txChannels = 0;
    bool fullDuplex = false;
    QVector<double> rxSampleRatesHz;
    QVector<double> rxBandwidthsHz;
    QVector<SoapySdrRangeInfo> rxFrequencyRangesHz;
    QString error;
};

bool soapySdrLibraryAvailable(QString *loadedPath = nullptr, QString *errorMessage = nullptr);
QString soapySdrLastErrorMessage();
QVector<SoapySdrDeviceInfo> enumerateSoapySdrDevices();
SoapySdrDeviceCapabilities querySoapySdrDeviceCapabilities(int nativeIndex,
                                                           int rxChannel = 0);

int openSoapySdrDeviceSafely(void **dev, int nativeIndex);
int closeSoapySdrDeviceSafely(void *dev);
int setSoapySdrSampleRateSafely(void *dev, double sampleRateHz, int channel = 0);
int setSoapySdrCenterFrequencySafely(void *dev, double frequencyHz, int channel = 0);
int setSoapySdrBandwidthSafely(void *dev, double bandwidthHz, int channel = 0);
int setSoapySdrGainSafely(void *dev, double gainDb, int channel = 0);
void *setupSoapySdrRxStreamSafely(void *dev, int channel = 0);
int activateSoapySdrStreamSafely(void *dev, void *stream);
int readSoapySdrStreamSafely(void *dev,
                             void *stream,
                             float *buffer,
                             uint32_t sampleCount,
                             long timeoutUs);
int deactivateSoapySdrStreamSafely(void *dev, void *stream);
int closeSoapySdrStreamSafely(void *dev, void *stream);

#endif // SOAPYSDRBACKEND_H
