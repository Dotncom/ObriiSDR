#ifndef OBRII_CLI_VIRTUAL_IQ_SOURCE_H
#define OBRII_CLI_VIRTUAL_IQ_SOURCE_H

#include "virtualsignaltransport.h"
#include <QJsonObject>
#include <QLocalServer>
#include <QPointer>
#include <QQueue>
#include <QTcpServer>
#include <functional>

class CliVirtualIqSource : public QObject {
public:
    explicit CliVirtualIqSource(QObject *parent = nullptr);
    ~CliVirtualIqSource() override;
    bool start(const VirtualSignalEndpoint &endpoint, quint32 rate, qint64 center,
               qint64 queueBytes, QString *error);
    void stop();
    unsigned int read(float *target, unsigned int capacity);
    bool takeDiscontinuity();
    QJsonObject status() const;
    quint16 listeningPort() const { return tcp.serverPort(); }
    // Optional notification on the owning QObject thread, after blocks arrive.
    std::function<void()> onDataAvailable;
private:
    struct Chunk { QVector<float> iq; int offset = 0; bool gap = false; };
    void accept(QIODevice *socket);
    void pump();
    void reject(const QString &message);
    QLocalServer local;
    QTcpServer tcp;
    QPointer<QIODevice> peer;
    VirtualSignalStreamParser parser;
    QQueue<Chunk> queue;
    quint32 rate = 0, previousSequence = 0;
    qint64 center = 0, limitBytes = 0, queuedBytes = 0;
    quint64 frames = 0, samples = 0, drops = 0, rejected = 0, connections = 0;
    bool haveSequence = false, gap = true, scheduled = false;
    QString lastError;
};
#endif
