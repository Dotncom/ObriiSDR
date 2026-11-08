#pragma once

#include "dspbranchengine.h"
#include "dspstreammanager.h"

#include <QJsonObject>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

class CliDspGraph {
public:
    CliDspGraph() = default;
    ~CliDspGraph();
    CliDspGraph(const CliDspGraph &) = delete;
    CliDspGraph &operator=(const CliDspGraph &) = delete;

    QJsonObject command(const QJsonObject &request, double centerHz, double sampleRateHz,
                        bool receiverRunning);
    QJsonObject status() const;
    QJsonObject definitionSnapshot() const { return definition; }
    bool running() const { return active; }
    void setAuxiliarySourceLookup(
        std::function<bool(const DspSourceIdentity &, double *, double *)> lookup) {
        auxiliarySourceLookup = std::move(lookup);
    }
    void updateSource(double centerHz, double sampleRateHz);
    void stop();
    void publishIq(const float *iq, std::size_t samples);
    void poll();

private:
    struct PcmPort {
        std::unique_ptr<QTcpServer> server;
        std::vector<QTcpSocket *> clients;
        quint64 frames = 0;
        quint64 drops = 0;
    };
    struct AudioItem {
        QString sinkId;
        QByteArray pcm;
        bool reset = false;
    };
    void enqueueAudio(const DspBranchPcmFrame &frame);
    void drainAudio();
    QJsonObject trace(const QString &id) const;
    QJsonObject remoteCommand(const QJsonObject &request);
    QJsonObject computeCommand(const QJsonObject &request);
    QString validate(const QJsonObject &candidate, const DspGraphPlan &compiled,
                     double center, double rate) const;

    QString path;
    bool modified = false;
    QJsonObject definition;
    DspGraphPlan plan;
    std::unique_ptr<DspBranchEngine> engine;
    DspStreamEndpoint remoteEndpoint;
    std::unique_ptr<DspComputeServer> computeServer;
    QString computeAddress;
    quint16 computeRequestedPort = 0;
    bool active = false;
    double centerHz = 0.0;
    double sampleRateHz = 0.0;
    std::map<QString, PcmPort> ports;
    std::map<QString, DspBranchFrame> frames;
    QHash<QString, QString> recordingDirectories;
    mutable std::mutex audioMutex;
    std::deque<AudioItem> audioQueue;
    std::size_t queuedAudioBytes = 0;
    quint64 droppedAudioFrames = 0;
    std::function<bool(const DspSourceIdentity &, double *, double *)> auxiliarySourceLookup;
};
