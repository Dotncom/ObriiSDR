#pragma once

#include "dspiqsourceregistry.h"
#include "radiosettings.h"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>

class RemoteVfoService;
class RemoteWebServer;

// All methods, including IQ submission, belong on the owning Qt event-loop thread.
// Construction is inert; only start() creates the production service/gateway.
class CliWebService final : public QObject {
    Q_OBJECT
public:
    explicit CliWebService(QObject *parent = nullptr);
    ~CliWebService() override;

    bool start(const QString &address, quint16 port);
    void stop();
    bool isListening() const;
    quint16 port() const;
    QString statusText() const;
    QJsonObject status() const;
    // Local CLI dispatcher only. Never route untrusted network control here.
    QJsonObject command(const QJsonObject &request);

    void setSourceState(const RadioSettings &settings, bool running);
    bool needsIq() const;
    // Counts complex samples, not floats. alreadyPublished applies to this block
    // and this destination, not merely to whether a graph exists.
    bool submitIq(const float *iq, std::size_t complexSamples,
                  bool alreadyPublished = false, std::uint64_t expectedEpoch = 0);
    bool submitSourceIq(const DspSourceIdentity &source, const float *iq,
                        std::size_t complexSamples, double sampleRateHz, double centerHz,
                        bool alreadyPublished = false, bool discontinuity = false,
                        const DspIqTimingMetadata *timing = nullptr);

    // Optional native NetworkController hooks; peer IDs are isolated from web IDs.
    bool handleRemoteCommand(const QString &peerId, const QJsonObject &request);
    void closeRemoteEndpoint(const QString &peerId);

signals:
    void jsonFrameReady(const QString &peerId, const QJsonObject &frame);
    void binaryFrameReady(const QString &peerId, const QJsonObject &frame,
                          const QByteArray &payload);

private:
    std::unique_ptr<RemoteVfoService> service_;
    std::unique_ptr<RemoteWebServer> web_;
    RadioSettings sourceSettings_;
    bool running_ = false;
    QString address_;
    quint16 requestedPort_ = 0;
    QString status_ = QStringLiteral("Browser server stopped");
};
