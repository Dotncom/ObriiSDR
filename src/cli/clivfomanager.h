#ifndef OBRII_CLI_VFO_MANAGER_H
#define OBRII_CLI_VFO_MANAGER_H

#include "cliaudiooutput.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTcpServer>

#include <map>
#include <memory>

class CliVfoManager {
public:
    CliVfoManager(double centerHz, double sampleRateHz);
    ~CliVfoManager();
    QJsonObject command(const QJsonObject &request);
    QJsonArray configuration() const;
    void process(const float *iq, std::size_t samples);
    void setSource(double centerHz, double sampleRateHz);
    void reset();
    bool hasListeners() const;

private:
    struct Branch;
    QJsonObject status(const Branch &branch) const;
    bool inBand(double frequencyHz, double bandwidthHz) const;
    double centerHz;
    double sampleRateHz;
    std::map<int, std::unique_ptr<Branch>> branches;
};

#endif
