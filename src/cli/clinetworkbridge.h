#ifndef OBRII_CLI_NETWORK_BRIDGE_H
#define OBRII_CLI_NETWORK_BRIDGE_H

#include "networkcontroller.h"
#include "iqchannelizer.h"

#include <QJsonArray>
#include <QJsonObject>

#include <functional>
#include <vector>

class CliNetworkBridge {
public:
    struct ServerCallbacks {
        std::function<QJsonObject()> settings;
        std::function<QJsonArray()> receivers;
        std::function<bool(const QJsonObject &, QString *)> applySettings;
        std::function<bool(bool, QString *)> setRunning;
    };

    explicit CliNetworkBridge(QObject *parent = nullptr);
    bool startServer(const QString &bindAddress, quint16 port, ServerCallbacks callbacks);
    void connectClient(const QString &address, quint16 port);
    bool ready() const { return network.isControlReady(); }
    bool clientHasControl() const { return network.clientHasControl(); }
    int processingMode() const { return mode; }
    int peerCount() const { return network.readyPeerIds().size(); }
    QJsonObject metrics() const;
    QJsonObject clientState() const { return receivedState; }
    void publishState() { broadcastState(); }
    bool setServerProcessingMode(int requested);
    bool sendClientControl(const QString &action, const QJsonObject &settings = {});
    bool requestClientProcessingMode(int requested);
    void sendSpectrum(const std::vector<float> &orderedDbfs, double centerHz,
                      double sampleRateHz, double listeningHz, double bandwidthHz,
                      int modulation, int inputMode, int sourceFftLength = 0);
    void sendAudio(const QByteArray &pcm);
    void sendIq(const float *samples, int sampleCount, const RadioSettings &settings);

    std::function<void(const QJsonObject &)> onClientState;
    std::function<void(const QJsonObject &)> onClientSpectrum;
    std::function<void(const QByteArray &)> onClientAudio;
    std::function<void(const QJsonObject &, const QByteArray &)> onClientIq;
    std::function<void(const QString &)> onStatus;

private:
    void handleServerCommand(const QJsonObject &command);
    void broadcastState();
    void sendAck(const QString &peerId, const QJsonObject &request,
                 bool okay, const QString &reason = {});
    NetworkController network;
    ServerCallbacks serverCallbacks;
    QJsonObject receivedState;
    int mode = 0;
    quint64 spectrumSequence = 0;
    quint64 audioSequence = 0;
    quint64 droppedSpectrum = 0;
    quint64 droppedAudio = 0;
    quint64 iqSequence = 0;
    quint64 droppedIq = 0;
    IqChannelizer iqChannelizer;
    std::vector<float> channelIq;
    double previousIqCenter = 0.0;
    double previousIqListen = 0.0;
    double previousIqRate = 0.0;
};

#endif
