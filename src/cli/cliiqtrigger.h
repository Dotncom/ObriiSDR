#ifndef OBRII_CLI_IQ_TRIGGER_H
#define OBRII_CLI_IQ_TRIGGER_H

#include <QByteArray>
#include <QJsonObject>

#include <deque>
#include <future>
#include <vector>

class CliIqTrigger {
public:
    CliIqTrigger() = default;
    ~CliIqTrigger();
    QJsonObject command(const QJsonObject &request, double centerHz, double sampleRateHz);
    QJsonObject status() const;
    QJsonObject configuration() const;
    QJsonObject submitIq(const float *interleaved, int complexSamples);
    QJsonObject observe(const std::vector<float> &levelsDbfs, double centerHz, double sampleRateHz);
    void disarm();
    bool active() const { return state == State::Armed || state == State::Capturing; }
    bool needsSpectrum() const { return state == State::Armed; }

private:
    struct WriteResult { QString path; QString error; quint64 samples = 0; };
    enum class State { Unconfigured, Ready, Armed, Capturing, Writing };
    void startWrite();
    State state = State::Unconfigured;
    QString outputPrefix;
    double bandStartHz = 0.0;
    double bandEndHz = 0.0;
    double thresholdDbfs = -60.0;
    double hysteresisDb = 3.0;
    bool falling = false;
    bool autoRearm = false;
    int preMs = 100;
    int postMs = 100;
    double sourceCenterHz = 0.0;
    double sourceRateHz = 0.0;
    bool previousAbove = false;
    bool previousValid = false;
    quint64 preTargetSamples = 0;
    quint64 postTargetSamples = 0;
    quint64 preBufferedSamples = 0;
    quint64 postBufferedSamples = 0;
    quint64 captures = 0;
    qint64 triggerUtcMs = 0;
    double triggerFrequencyHz = 0.0;
    double triggerLevelDbfs = -160.0;
    std::deque<QByteArray> preBuffer;
    std::vector<QByteArray> captureBuffer;
    std::future<WriteResult> writer;
};

#endif
