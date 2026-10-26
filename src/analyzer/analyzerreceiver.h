#ifndef ANALYZERRECEIVER_H
#define ANALYZERRECEIVER_H

#include "analyzerframechannel.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

struct AnalyzerReceiverDevice {
    QString backendId;
    QString label;
    int nativeIndex = 0;
    int rxChannels = 1;
    int txChannels = 0;
    bool fullDuplex = false;
    QVector<double> sampleRatesHz;
    QVector<double> bandwidthsHz;
    double minimumFrequencyHz = 0.0;
    double maximumFrequencyHz = 0.0;
    QString capabilitySummary;
};

struct AnalyzerReceiverSettings {
    QString backendId;
    int nativeIndex = 0;
    double centerHz = 100.0e6;
    double sampleRateHz = 10.0e6;
    int fftLength = 65536;
    int displayReductionRatio = 0;
    int displayReductionMode = 0;
    int rxChannel = 0;
    bool directSampling = false;
    int directChannel = 1;
    int lnaGainDb = 0;
    int vgaGainDb = 0;
    int fftWorkerThreads = 0;
    QVector<double> agileScanFrequenciesHz;
};

class AnalyzerReceiver final : public QObject {
    Q_OBJECT
public:
    explicit AnalyzerReceiver(QObject *parent = nullptr);
    ~AnalyzerReceiver() override;

    QVector<AnalyzerReceiverDevice> enumerate();
    static QVector<AnalyzerReceiverDevice> enumerateDevices();
    bool start(const AnalyzerReceiverSettings &settings);
    bool retune(double centerHz);
    bool setGains(int lnaGainDb, int vgaGainDb);
    bool stopNativeAgileScan();
    bool isNativeAgileScanActive() const;
    void stop();
    bool isRunning() const;
    QString errorString() const;

signals:
    void frameReady(const AnalyzerSpectrumFrame &frame);
    // Emitted synchronously from the acquisition thread. Consumers that use
    // the raw pointer must use a direct connection and copy it before return.
    void iqBlockReady(const float *samples, quint64 floatCount,
                      double sampleRateHz, double centerHz);
    void runningChanged(bool running);
    void receiverError(const QString &message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif
