#ifndef GNSSSDRSTREAM_H
#define GNSSSDRSTREAM_H
#include <QThread>
#include <QStringList>
#include <atomic>

namespace GnssSdr {
struct Input {
    bool live = true;
    QString path;
    QString format = QStringLiteral("cf32");
    double sampleRateHz = 0;
    double centerHz = 0;
    quint64 dataOffset = 0;
    quint64 dataBytes = 0;
};
bool readInputMetadata(Input &input, QString *error);
QStringList trackingArguments(const Input &input, const QStringList &systems,
                              quint16 iqPort, quint16 nmeaPort, QString *error);
QByteArray floatToCs16(const float *samples, std::size_t count, float gain = 1.0f);
}

class GnssSdrStream : public QThread {
    Q_OBJECT
public:
    explicit GnssSdrStream(const GnssSdr::Input &input, QObject *parent = nullptr);
    ~GnssSdrStream() override;
    void requestStop();
signals:
    void listening(quint16 iqPort, quint16 nmeaPort);
    void nmeaLine(const QString &line);
    void streamError(const QString &message);
    void samplesSent(quint64 count);
protected:
    void run() override;
private:
    GnssSdr::Input input;
    std::atomic<bool> stopping{false};
};
#endif
