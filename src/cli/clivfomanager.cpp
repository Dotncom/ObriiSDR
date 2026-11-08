#include "clivfomanager.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpSocket>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
QJsonObject errorReply(const QString &message) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}
}

struct CliVfoManager::Branch {
    int id = 0;
    double frequencyHz = 0.0;
    double bandwidthHz = 0.0;
    int modulation = MOD_NFM;
    quint64 pcmFrames = 0;
    quint64 droppedFrames = 0;
    double lastProcessingMs = 0.0;
    QString lastError;
    QString filterJson = QStringLiteral("[]");
    QTcpServer server;
    std::vector<QTcpSocket *> clients;
    CliAudioOutput audio;

    void sendPcm(const QByteArray &pcm) {
        ++pcmFrames;
        for (QTcpSocket *client : clients) {
            if (client->state() != QAbstractSocket::ConnectedState) continue;
            if (client->bytesToWrite() + pcm.size() > 256 * 1024) {
                ++droppedFrames;
                continue;
            }
            if (client->write(pcm) != pcm.size()) ++droppedFrames;
        }
    }
};

CliVfoManager::CliVfoManager(double center, double rate)
    : centerHz(center), sampleRateHz(rate) {}

CliVfoManager::~CliVfoManager() = default;

bool CliVfoManager::hasListeners() const {
    for (const auto &entry : branches)
        if (!entry.second->clients.empty() && inBand(entry.second->frequencyHz, entry.second->bandwidthHz))
            return true;
    return false;
}

bool CliVfoManager::inBand(double frequency, double bandwidth) const {
    return std::isfinite(frequency) && std::isfinite(bandwidth) &&
        frequency > 0.0 && bandwidth >= 100.0 && bandwidth <= 500000.0 &&
        std::abs(frequency - centerHz) + bandwidth * 0.5 < sampleRateHz * 0.5;
}

QJsonObject CliVfoManager::status(const Branch &branch) const {
    return {{QStringLiteral("id"), branch.id},
            {QStringLiteral("frequencyHz"), branch.frequencyHz},
            {QStringLiteral("bandwidthHz"), branch.bandwidthHz},
            {QStringLiteral("modulationType"), branch.modulation},
            {QStringLiteral("inBand"), inBand(branch.frequencyHz, branch.bandwidthHz)},
            {QStringLiteral("pcmHost"), QStringLiteral("127.0.0.1")},
            {QStringLiteral("pcmPort"), static_cast<int>(branch.server.serverPort())},
            {QStringLiteral("pcmFormat"), QStringLiteral("s16le-mono-48000")},
            {QStringLiteral("clients"), static_cast<int>(branch.clients.size())},
            {QStringLiteral("pcmFrames"), static_cast<double>(branch.pcmFrames)},
            {QStringLiteral("droppedFrames"), static_cast<double>(branch.droppedFrames)},
            {QStringLiteral("lastProcessingMs"), branch.lastProcessingMs},
            {QStringLiteral("filterStages"), QJsonDocument::fromJson(branch.filterJson.toUtf8()).array().size()},
            {QStringLiteral("lastError"), branch.lastError}};
}

QJsonArray CliVfoManager::configuration() const {
    QJsonArray result;
    for (const auto &entry : branches) {
        const Branch &branch = *entry.second;
        result.append(QJsonObject{{QStringLiteral("id"), branch.id},
            {QStringLiteral("frequencyHz"), branch.frequencyHz},
            {QStringLiteral("bandwidthHz"), branch.bandwidthHz},
            {QStringLiteral("modulationType"), branch.modulation},
            {QStringLiteral("filters"), QJsonDocument::fromJson(branch.filterJson.toUtf8()).array()}});
    }
    return result;
}

QJsonObject CliVfoManager::command(const QJsonObject &request) {
    const QString operation = request.value(QStringLiteral("operation")).toString();
    if (operation == QStringLiteral("list")) {
        QJsonArray values;
        for (const auto &entry : branches) values.append(status(*entry.second));
        return {{QStringLiteral("ok"), true}, {QStringLiteral("vfos"), values},
                {QStringLiteral("centerFrequencyHz"), centerHz},
                {QStringLiteral("sampleRateHz"), sampleRateHz}};
    }
    const int id = request.value(QStringLiteral("id")).toInt(-1);
    if (id < 1 || id > 4) return errorReply(QStringLiteral("VFO ID must be 1..4; ID 0 is the primary audio channel"));
    auto found = branches.find(id);
    if (operation == QStringLiteral("remove")) {
        if (found == branches.end()) return errorReply(QStringLiteral("Unknown VFO"));
        branches.erase(found);
        return {{QStringLiteral("ok"), true}, {QStringLiteral("removed"), id}};
    }
    if (operation == QStringLiteral("status")) {
        if (found == branches.end()) return errorReply(QStringLiteral("Unknown VFO"));
        return {{QStringLiteral("ok"), true}, {QStringLiteral("vfo"), status(*found->second)}};
    }
    if (operation == QStringLiteral("add")) {
        if (found != branches.end()) return errorReply(QStringLiteral("VFO ID already exists"));
        const double frequency = request.value(QStringLiteral("frequencyHz")).toDouble();
        const double bandwidth = request.value(QStringLiteral("bandwidthHz")).toDouble();
        const int modulation = request.value(QStringLiteral("modulationType")).toInt(-1);
        if (!inBand(frequency, bandwidth) || modulation < MOD_AM || modulation > MOD_WFM)
            return errorReply(QStringLiteral("Invalid VFO mode or channel outside source IQ band"));
        auto branch = std::make_unique<Branch>();
        branch->id = id;
        branch->frequencyHz = frequency;
        branch->bandwidthHz = bandwidth;
        branch->modulation = modulation;
        if (!branch->server.listen(QHostAddress::LocalHost, 0))
            return errorReply(branch->server.errorString());
        Branch *pointer = branch.get();
        QObject::connect(&branch->server, &QTcpServer::newConnection, &branch->server, [pointer] {
            while (pointer->server.hasPendingConnections()) {
                QTcpSocket *client = pointer->server.nextPendingConnection();
                pointer->clients.push_back(client);
                QObject::connect(client, &QTcpSocket::disconnected, &pointer->server, [pointer, client] {
                    auto &clients = pointer->clients;
                    clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
                    client->deleteLater();
                });
            }
        });
        QString error;
        if (!branch->audio.open({}, false, INPUT_RF, modulation, bandwidth,
                                sampleRateHz, centerHz, frequency, false, &error,
                                [pointer](const QByteArray &pcm) { pointer->sendPcm(pcm); }))
            return errorReply(error);
        branches.emplace(id, std::move(branch));
        return {{QStringLiteral("ok"), true}, {QStringLiteral("vfo"), status(*pointer)}};
    }
    if (found == branches.end()) return errorReply(QStringLiteral("Unknown VFO"));
    Branch &branch = *found->second;
    if (operation == QStringLiteral("tune")) {
        const double frequency = request.value(QStringLiteral("frequencyHz")).toDouble();
        if (!inBand(frequency, branch.bandwidthHz))
            return errorReply(QStringLiteral("VFO channel outside source IQ band"));
        branch.frequencyHz = frequency;
        branch.audio.setListening(frequency);
    } else if (operation == QStringLiteral("bandwidth")) {
        const double bandwidth = request.value(QStringLiteral("bandwidthHz")).toDouble();
        if (!inBand(branch.frequencyHz, bandwidth))
            return errorReply(QStringLiteral("VFO bandwidth outside source IQ band"));
        branch.bandwidthHz = bandwidth;
        branch.audio.setBandwidth(bandwidth);
    } else if (operation == QStringLiteral("demod")) {
        const int modulation = request.value(QStringLiteral("modulationType")).toInt(-1);
        if (modulation < MOD_AM || modulation > MOD_WFM)
            return errorReply(QStringLiteral("Unsupported VFO demodulator"));
        branch.modulation = modulation;
        branch.audio.setModulation(modulation);
    } else if (operation == QStringLiteral("filters")) {
        branch.filterJson = request.value(QStringLiteral("json")).toString();
        branch.audio.setFilterChain(branch.filterJson);
    } else return errorReply(QStringLiteral("Unknown VFO operation"));
    return {{QStringLiteral("ok"), true}, {QStringLiteral("vfo"), status(branch)}};
}

void CliVfoManager::process(const float *iq, std::size_t samples) {
    for (auto &entry : branches) {
        Branch &branch = *entry.second;
        if (!inBand(branch.frequencyHz, branch.bandwidthHz) || branch.clients.empty()) continue;
        QElapsedTimer timer;
        timer.start();
        QString error;
        if (!branch.audio.process(iq, samples, &error)) branch.lastError = error;
        branch.lastProcessingMs = timer.nsecsElapsed() / 1.0e6;
    }
}

void CliVfoManager::setSource(double center, double rate) {
    if (std::abs(center - centerHz) < 0.5 && std::abs(rate - sampleRateHz) < 0.5) return;
    centerHz = center;
    sampleRateHz = rate;
    for (auto &entry : branches) {
        entry.second->audio.setCenter(center);
        entry.second->audio.setSampleRate(rate);
    }
}

void CliVfoManager::reset() {
    for (auto &entry : branches)
        entry.second->audio.setListening(entry.second->frequencyHz);
}
