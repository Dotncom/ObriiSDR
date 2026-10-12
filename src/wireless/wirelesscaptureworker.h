#ifndef WIRELESS_CAPTURE_WORKER_H
#define WIRELESS_CAPTURE_WORKER_H
#include "wirelesspackettools.h"
#include <QObject>
#include <QProcess>
#include <QMutex>

class WirelessCaptureWorker : public QObject, public Wireless::PacketBackend {
    Q_OBJECT
public:
    explicit WirelessCaptureWorker(QObject *parent = nullptr);
    ~WirelessCaptureWorker() override;
    Wireless::BackendCapabilities capabilities() const override;
    void start(const QString &program, const QStringList &arguments, bool listInterfaces = false);
    void stop();
    QVector<QStringList> takePackets(quint64 &dropped);
signals:
    void statusChanged(const QString &text);
    void interfacesReady(const QString &text);
    void finished();
private:
    void readOutput();
    void consumeLines();
    QProcess *process;
    QByteArray pending;
    QByteArray interfaceOutput;
    QByteArray diagnostics;
    QMutex mutex;
    QVector<QStringList> packets;
    quint64 droppedPackets = 0;
    bool listing = false;
    bool scheduled = false;
};
#endif
