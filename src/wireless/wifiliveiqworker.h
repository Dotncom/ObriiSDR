#ifndef WIFI_LIVE_IQ_WORKER_H
#define WIFI_LIVE_IQ_WORKER_H

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QQueue>
#include <QStringList>
#include <atomic>

class QProcess;
class QTimer;
class QJsonObject;

// Lives on a dedicated thread. enqueue() is the only method called from the IQ reader.
class WifiLiveIqWorker : public QObject {
    Q_OBJECT
public:
    explicit WifiLiveIqWorker(QObject *parent = nullptr);
    void enqueue(const QByteArray &iq, double sampleRate, double centerFrequency, int samples);
    void stopAccepting();
    void start(const QString &program, const QStringList &arguments,
               const QString &workingDirectory, double sampleRate, double centerFrequency);
    void stop();
signals:
    void started();
    void finished(bool successful, const QString &message);
    void packetRow(const QStringList &row);
    void progress(quint64 packets, quint64 droppedSamples, qint64 bufferedBytes,
                  double fedMegasamplesPerSecond, quint64 fedSamples,
                  quint64 decodedSamples);
    void message(const QString &message);
    void phyStats(const QJsonObject &stats);
private:
    void flush();
    void readOutput();
    QMutex queueMutex;
    QQueue<QByteArray> queue;
    qint64 queuedBytes = 0;
    std::atomic<bool> accepting{false};
    std::atomic<bool> sourceChanged{false};
    std::atomic<quint64> droppedSamples{0};
    quint64 writtenSamples = 0;
    quint64 lastReportedFedSamples = 0;
    quint64 decodedSamples = 0;
    std::atomic<double> expectedRate{0.0};
    std::atomic<double> expectedCenter{0.0};
    quint64 packets = 0;
    bool stopping = false;
    QProcess *process = nullptr;
    QTimer *timer = nullptr;
    QElapsedTimer reportTimer;
    QElapsedTimer sessionTimer;
    QString outputPath;
    QByteArray outputBuffer;
};

#endif
