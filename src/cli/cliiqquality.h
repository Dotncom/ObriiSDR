#ifndef OBRII_CLI_IQ_QUALITY_H
#define OBRII_CLI_IQ_QUALITY_H

#include <QElapsedTimer>
#include <QJsonObject>

class CliIqQuality {
public:
    QJsonObject request(int blocks);
    QJsonObject status() const;
    QJsonObject submit(const float *iq, int complexSamples, double sourceRateHz);
    void cancel();
    bool pending() const { return remaining > 0; }

private:
    int remaining = 0;
    int requested = 0;
    quint64 samples = 0;
    quint64 clipped = 0;
    double sumI = 0.0, sumQ = 0.0;
    double sumI2 = 0.0, sumQ2 = 0.0;
    double sumIq = 0.0;
    double sourceRate = 0.0;
    QElapsedTimer timer;
    QJsonObject latest;
};

#endif
