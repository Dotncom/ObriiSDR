#ifndef WIRELESS_CAPTURE_WORKER_H
#define WIRELESS_CAPTURE_WORKER_H
#include "wirelesspackettools.h"
#include <QObject>
#include <QProcess>
#include <QMutex>

class WirelessCaptureWorker : public QObject, public Wireless::PacketBackend {
    Q_OBJECT
public:
    enum class Job { Packets, Interfaces, Report };
    explicit WirelessCaptureWorker(QObject *parent = nullptr);
    ~WirelessCaptureWorker() override;
    Wireless::BackendCapabilities capabilities() const override;
    void start(const QString &program, const QStringList &arguments, Job job = Job::Packets);
    void stop();
    QVector<QStringList> takePackets(quint64 &dropped);
signals:
    void statusChanged(const QString &text);
    void interfacesReady(const QString &text);
    void reportReady(const QString &text);
    void finished();
private:
    void readOutput();
    void consumeLines();
    void finishIfDrained();
    QProcess *process;
    QByteArray pending;
    QByteArray interfaceOutput;
    QByteArray diagnostics;
    QMutex mutex;
    QVector<QStringList> packets;
    quint64 droppedPackets = 0;
    Job job = Job::Packets;
    bool scheduled = false;
    bool exited = false;
    int exitCode = 0;
    quint64 generation = 0;
};
#endif
