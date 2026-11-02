#ifndef OBRII_CLI_FREQUENCY_SCANNER_H
#define OBRII_CLI_FREQUENCY_SCANNER_H

#include <QElapsedTimer>
#include <QJsonObject>

#include <vector>

class CliFrequencyScanner {
public:
    QJsonObject command(const QJsonObject &request);
    QJsonObject status() const;
    QJsonObject configuration() const;
    bool running() const { return state != State::Stopped; }
    bool takeTune(double *frequencyHz);
    void tuned();
    QJsonObject observe(const QJsonObject &spectrum, double centerHz);
    void stop();

private:
    enum class State { Stopped, Scanning, Locked };
    State state = State::Stopped;
    std::vector<double> centers;
    int dwellMs = 500;
    int holdMs = 0;
    double thresholdDbfs = -70.0;
    double minSnrDb = 8.0;
    int index = 0;
    int confirmed = 0;
    bool tunePending = false;
    quint64 visits = 0;
    quint64 hits = 0;
    QElapsedTimer timer;
    QJsonObject lastHit;
};

#endif
