#ifndef AX25LIVEDECODER_H
#define AX25LIVEDECODER_H

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>

class Ax25LiveDecoder final : public QThread {
    Q_OBJECT
public:
    explicit Ax25LiveDecoder(QObject *parent = nullptr);
    ~Ax25LiveDecoder() override;
    void submitPcm16(const QByteArray &pcm, int sampleRate);
    void stop();

signals:
    void frameDecoded(const QByteArray &ax25WithoutFcs);

protected:
    void run() override;

private:
    QMutex mutex;
    QWaitCondition ready;
    QByteArray bufferedPcm;
    QHash<QByteArray, qint64> recentlyDecoded;
    int rate = 48000;
    qint64 totalSamples = 0;
    qint64 processedSamples = 0;
    bool stopping = false;
};

#endif // AX25LIVEDECODER_H
