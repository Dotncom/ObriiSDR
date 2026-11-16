#ifndef OBRII_UHD_TRANSMITTER_BACKEND_H
#define OBRII_UHD_TRANSMITTER_BACKEND_H

#include "transmitterbackend.h"
#include "../backends/uhdiqtxstream.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>
#include <thread>

class NetworkController;
class UhdRxSession;

class UhdTransmitterBackend final : public TransmitterBackend {
public:
    explicit UhdTransmitterBackend(bool remote = false);
    ~UhdTransmitterBackend() override;

    QString id() const override;
    QString displayName() const override;
    bool isRfCapable() const override { return true; }
    bool start(const TxConfiguration &configuration, QString *error) override;
    bool writeIq(const UhdIqTx::Sample *samples, int count, QString *error) override;
    bool setOutputEnabled(bool enabled, QString *error) override;
    bool drain(int, QString *) override { return isRunning(); }
    void stop() override;
    bool isRunning() const override;

    void setSharedSessionProvider(std::function<std::shared_ptr<UhdRxSession>(int)> provider);
    void setNetwork(NetworkController *network);
    void finish();
    QString error() const;
    int bufferedSourceSamples() const;

private:
    bool sendRemote(const QString &operation) const;
    void pumpRemote();
    void failRemote(const QString &error);

    const bool remote_;
    std::atomic<bool> cancel_{false}, running_{false};
    bool outputEnabled_ = false;
    bool finishing_ = false;
    int rate_ = 0;
    std::size_t queueLimit_ = 0;
    mutable std::mutex errorMutex_;
    QString error_;
    UhdIqTx::Queue queue_;
    std::thread worker_;
    std::function<std::shared_ptr<UhdRxSession>(int)> sharedSessionProvider_;
    std::shared_ptr<UhdRxSession> sharedSession_;
    QPointer<NetworkController> network_;
    QString session_;
    QTimer pumpTimer_;
    QMetaObject::Connection networkReply_;
    QElapsedTimer timer_;
    qint64 keepaliveMs_ = 0;
    qint64 finishSentMs_ = -1;
    qint64 creditTimeNs_ = 0;
    double credit_ = 0.0;
    bool remoteReady_ = false;
    std::deque<UhdIqTx::Block> pending_;
    std::size_t pendingSamples_ = 0, offset_ = 0;
};

#endif
