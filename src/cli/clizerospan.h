#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>

#include <vector>

class CliZeroSpan {
public:
    QJsonObject command(const QJsonObject &request, double centerHz, double sampleRateHz);
    QJsonObject observe(const std::vector<float> &levels, double centerHz, double sampleRateHz);
    QJsonObject status() const;
    bool running() const { return active; }
    void stop() { active = false; }

private:
    struct Point {
        qint64 utcMs = 0;
        double elapsedMs = 0.0;
        double meanPowerDbfs = 0.0;
        double peakDbfs = 0.0;
    };
    QJsonObject exportCsv(const QString &path) const;
    QJsonObject read(int offset, int limit) const;

    bool active = false;
    double startHz = 0.0;
    double endHz = 0.0;
    double sourceCenterHz = 0.0;
    double sourceRateHz = 0.0;
    int maxPoints = 10000;
    QElapsedTimer clock;
    std::vector<Point> points;
};
