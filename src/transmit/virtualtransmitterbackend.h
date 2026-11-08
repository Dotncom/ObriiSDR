#ifndef VIRTUALTRANSMITTERBACKEND_H
#define VIRTUALTRANSMITTERBACKEND_H

#include "transmitterbackend.h"
#include "openradio/virtualsignaltransport.h"

#include <memory>

class QIODevice;
class QLocalSocket;
class QTcpSocket;

class VirtualTransmitterBackend final : public TransmitterBackend {
public:
    VirtualTransmitterBackend();
    ~VirtualTransmitterBackend() override;
    void setEndpoint(const VirtualSignalEndpoint &endpoint);
    void setNodeId(const QString &id);
    VirtualSignalEndpoint endpoint() const;

    QString id() const override;
    QString displayName() const override;
    bool isRfCapable() const override;
    bool start(const TxConfiguration &configuration, QString *error) override;
    bool writeIq(const std::complex<float> *samples, int count, QString *error) override;
    bool setOutputEnabled(bool enabled, QString *error) override;
    bool drain(int timeoutMs, QString *error) override;
    void stop() override;
    bool isRunning() const override;
    bool isConnected() const;
    bool pollConnection();

private:
    bool writeFrame(const VirtualSignalFrame &frame, QString *error);
    QIODevice *device() const;
    void resetConnection();

    VirtualSignalEndpoint currentEndpoint;
    QString nodeId;
    TxConfiguration currentConfiguration;
    std::unique_ptr<QLocalSocket> localSocket;
    std::unique_ptr<QTcpSocket> tcpSocket;
    quint32 sequence = 0;
    qint64 sampleEpochNs = 0;
    qint64 sampleCursor = 0;
    bool running = false;
    bool outputEnabled = true;
    bool nextDiscontinuity = false;
    qint64 lastConnectAttemptMs = 0;
    qint64 lastHeartbeatMs = 0;
    qint64 lastAcknowledgementMs = 0;
    VirtualSignalStreamParser acknowledgementParser;
};

#endif // VIRTUALTRANSMITTERBACKEND_H
