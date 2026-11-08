#pragma once

#include "analyzerreceiver.h"
#include "dspsourceidentity.h"
#include "spectrummosaicengine.h"

#include <QJsonObject>
#include <QObject>

#include <map>
#include <memory>

class CliMultiSdr final : public QObject {
public:
    CliMultiSdr(QString primaryBackend, int primaryDeviceIndex, QObject *parent = nullptr);
    ~CliMultiSdr() override;

    QJsonObject command(const QJsonObject &request, double primaryCenterHz,
                        double primaryRateHz, bool primaryRunning);
    QJsonObject status(double primaryCenterHz, double primaryRateHz, bool primaryRunning) const;
    bool hasSlots() const { return !sourceSlots.empty(); }
    bool hasRunningAuxiliary() const;
    bool hasSource(const DspSourceIdentity &source, double *centerHz, double *rateHz) const;
    void updatePrimarySpectrum(const std::vector<float> &levels, double centerHz,
                               double sampleRateHz);
    void clearPrimarySpectrum();

private:
    struct Slot {
        AnalyzerReceiverSettings settings;
        std::unique_ptr<AnalyzerReceiver> receiver;
        AnalyzerSpectrumFrame frame;
        QString error;
        bool hasFrame = false;
    };
    QJsonObject frameReply(int id, const AnalyzerSpectrumFrame &frame) const;
    void rebuildMosaic();
    bool duplicatesHardware(const QString &backend, int index, int excludingId = -1) const;

    QString primaryBackend;
    int primaryDeviceIndex = 0;
    std::map<int, std::unique_ptr<Slot>> sourceSlots;
    SpectrumMosaicEngine mosaic;
    AnalyzerSpectrumFrame primaryFrame;
    bool hasPrimaryFrame = false;
};
