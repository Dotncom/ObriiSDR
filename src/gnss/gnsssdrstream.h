#ifndef GNSSSDRSTREAM_H
#define GNSSSDRSTREAM_H
#include <QThread>
#include <QStringList>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace GnssSdr {
struct Input {
    bool live = true;
    QString path;
    QString format = QStringLiteral("cf32");
    double sampleRateHz = 0;
    double centerHz = 0;
    int decimation = 1;
    quint64 dataOffset = 0;
    quint64 dataBytes = 0;
};
bool readInputMetadata(Input &input, QString *error);
bool configureTrackingRate(Input &input, const QStringList &systems, QString *error);
QStringList trackingArguments(const Input &input, const QStringList &systems,
                              quint16 iqPort, quint16 nmeaPort, QString *error);
QStringList trackingArguments(const Input &input, const QStringList &systems,
                              const QString &iqSource, quint16 nmeaPort, QString *error);
QByteArray floatToCs16(const float *samples, std::size_t count, float gain = 1.0f);

// Opt-in source for headless full-band CF32 LE. Never touches the GUI IQ ring.
// A queue is single-use: ready -> submit* -> finish -> drained, or cancel/error.
class LiveSource {
public:
    static constexpr int MaximumBlockBytes = 1024 * 1024;
    static constexpr int MaximumQueuedBytes = 4 * 1024 * 1024;
    static constexpr int MaximumQueuedBlocks = 64;
    enum class Read { Data, Wait, End, Cancelled, Error };
    explicit LiveSource(double sampleRateHz, double centerHz);
    void setReady();
    bool isReady() const;
    bool submit(const QByteArray &cf32, double sampleRateHz, double centerHz);
    bool finish(quint64 finalSourceSamples);
    void cancel();
    void fail(const QString &message);
    Read read(QByteArray &cf32);
    QString error() const;
    quint64 acceptedSamples() const;
    bool atEof() const;
private:
    bool failLocked(const QString &message);
    const double rate, center;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<QByteArray> queue;
    int queuedBytes = 0;
    quint64 accepted = 0;
    bool ready = false, eof = false, cancelled = false;
    QString failure;
};
}

class GnssSdrStream : public QThread {
    Q_OBJECT
public:
    explicit GnssSdrStream(const GnssSdr::Input &input, QObject *parent = nullptr);
    GnssSdrStream(const GnssSdr::Input &input, std::shared_ptr<GnssSdr::LiveSource> source,
                  QObject *parent = nullptr);
    ~GnssSdrStream() override;
    void requestStop();
signals:
    void listening(const QString &iqSource, quint16 nmeaPort);
    void nmeaLine(const QString &line);
    void streamError(const QString &message);
    void samplesSent(quint64 count);
    void sourceReady();
    void sourceDrained(quint64 sourceSamples, quint64 trackingSamples);
protected:
    void run() override;
private:
    GnssSdr::Input input;
    std::shared_ptr<GnssSdr::LiveSource> liveSource;
    std::atomic<bool> stopping{false};
};
#endif
