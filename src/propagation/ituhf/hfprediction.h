#ifndef HF_PREDICTION_H
#define HF_PREDICTION_H

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QVector>
#include <memory>

namespace hf {
struct Point { double latitude = 0, longitude = 0; };
enum class NoiseEnvironment { City, Residential, Rural, QuietRural, Quiet, Noisy };
struct Request {
    Point tx, rx;
    double powerW = 100, txGainDbi = 0, rxGainDbi = 0;
    int year = 2026, month = 1, smoothedSsn = 100, snrPercent = 90;
    double bandwidthHz = 3000, requiredSnrDb = 10;
    bool longPath = false;
    NoiseEnvironment noise = NoiseEnvironment::Rural;
    QVector<int> hoursUtc; // 0..23; empty means all 24 hours
    QVector<double> frequenciesMHz{3.5, 7.1, 10.1, 14.1, 18.1, 21.1, 24.9, 28.1};
};
struct Sample {
    int month = 0, hourUtc = 0;
    double frequencyMHz = 0, distanceKm = 0;
    double basicMufMHz = 0, muf50MHz = 0, muf90MHz = 0, muf10MHz = 0;
    double operationalMufMHz = 0, operationalMuf90MHz = 0, operationalMuf10MHz = 0;
    double receivedDbm = 0, externalNoiseDb = 0;
    double medianSnrDb = 0, upperSnrDecileDb = 0, lowerSnrDecileDb = 0, percentileSnrDb = 0;
    double basicReliabilityPercent = 0; // analogue SNR reliability; digital OCR is not exposed
    bool hasSignal = false; // upstream -307 dBW sentinel is not a real signal
};
struct Result { Request request; QVector<Sample> samples; };
struct Runtime {
    QString helperExecutable, dataDirectory;
    int timeoutMs = 120000;
    static Runtime defaults();
};
QString privateRoot();
QString validate(const Request &request);
QString checkRuntime(const Runtime &runtime, int month);
QByteArray inputConfiguration(const Request &request, const QString &asciiDataDirectory);
bool parseReport(const QByteArray &csv, const Request &request, Result *result, QString *error);
bool isSuccessExit(int code); // pinned main is 0; older official wrappers may use 1000

class PredictionJob final : public QObject {
    Q_OBJECT
public:
    explicit PredictionJob(QObject *parent = nullptr);
    ~PredictionJob() override;
    bool isRunning() const { return active; }
    bool start(const Request &request, const Runtime &runtime, QString *error = nullptr);
public slots:
    void cancel();
signals:
    void completed(const hf::Result &result);
    void failed(const QString &message);
    void cancelled();
private:
    void finish(int code, QProcess::ExitStatus status);
    void fail(const QString &message);
    QProcess process;
    QTimer deadline;
    std::unique_ptr<QTemporaryDir> directory;
    Request snapshot;
    bool active = false, stopping = false, timedOut = false;
};
}
Q_DECLARE_METATYPE(hf::Result)
#endif
