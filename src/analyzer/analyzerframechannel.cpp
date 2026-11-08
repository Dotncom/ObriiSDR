#include "analyzerframechannel.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QLocalServer>
#include <QLocalSocket>

#include <algorithm>
#include <cmath>

namespace {
constexpr quint32 kFrameMagic = 0x41525a4fU; // OZRA
constexpr quint16 kProtocolVersion = 1;
constexpr int kMaximumTransportBins = 65536;
constexpr int kMaximumPacketBytes = 8 * 1024 * 1024;
constexpr qint64 kMaximumQueuedBytes = 2 * 1024 * 1024;

QByteArray encodeFrame(const std::vector<float> &frequencies,
                       const std::vector<float> &levels,
                       int sourceFftLength,
                       quint64 sequence) {
    if (frequencies.size() != levels.size() || levels.empty()) return {};

    const int sourceCount = static_cast<int>(levels.size());
    const int targetCount = std::min(sourceCount, kMaximumTransportBins);
    std::vector<float> reduced(static_cast<std::size_t>(targetCount));
    if (targetCount == sourceCount) {
        reduced = levels;
    } else {
        for (int target = 0; target < targetCount; ++target) {
            const int first = static_cast<int>((static_cast<qint64>(target) * sourceCount) / targetCount);
            const int last = std::max(first + 1,
                static_cast<int>((static_cast<qint64>(target + 1) * sourceCount) / targetCount));
            float peak = -200.0f;
            // Keep transport work bounded for very large source FFTs. The live
            // attachment is a display stream; full-resolution evidence remains
            // available through spectrum recordings and experiment bundles.
            const int end = std::min(last, sourceCount);
            const int candidates[] = {first, first + (end - first) / 2, end - 1};
            for (int source : candidates) {
                if (source >= first && source < end &&
                    std::isfinite(levels[static_cast<std::size_t>(source)]))
                    peak = std::max(peak, levels[static_cast<std::size_t>(source)]);
            }
            reduced[static_cast<std::size_t>(target)] = peak;
        }
    }

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_5_15);
    const double minimumHz = frequencies.front();
    const double maximumHz = frequencies.back();
    stream << kFrameMagic << kProtocolVersion << sequence
           << QDateTime::currentMSecsSinceEpoch()
           << (minimumHz + maximumHz) * 0.5
           << minimumHz << maximumHz
           << static_cast<qint32>(sourceFftLength)
           << static_cast<qint32>(targetCount);
    stream.writeRawData(reinterpret_cast<const char *>(reduced.data()),
                        targetCount * static_cast<int>(sizeof(float)));

    QByteArray packet;
    QDataStream envelope(&packet, QIODevice::WriteOnly);
    envelope.setByteOrder(QDataStream::LittleEndian);
    envelope << static_cast<quint32>(payload.size());
    packet.append(payload);
    return packet;
}

bool decodeFrame(const QByteArray &payload, AnalyzerSpectrumFrame &frame) {
    QDataStream stream(payload);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_5_15);
    quint32 magic = 0;
    quint16 version = 0;
    qint32 fftLength = 0;
    qint32 count = 0;
    stream >> magic >> version >> frame.sequence >> frame.utcMs
           >> frame.centerHz >> frame.minimumHz >> frame.maximumHz
           >> fftLength >> count;
    if (stream.status() != QDataStream::Ok || magic != kFrameMagic ||
        version != kProtocolVersion || count <= 0 || count > kMaximumTransportBins ||
        !std::isfinite(frame.minimumHz) || !std::isfinite(frame.maximumHz)) {
        return false;
    }
    frame.sourceFftLength = fftLength;
    frame.levelsDb.resize(static_cast<std::size_t>(count));
    const int bytes = count * static_cast<int>(sizeof(float));
    return stream.readRawData(reinterpret_cast<char *>(frame.levelsDb.data()), bytes) == bytes;
}
}

AnalyzerFramePublisher::AnalyzerFramePublisher(QObject *parent)
    : QObject(parent), server(std::make_unique<QLocalServer>()) {
    connect(server.get(), &QLocalServer::newConnection, this, &AnalyzerFramePublisher::acceptConnections);
}

AnalyzerFramePublisher::~AnalyzerFramePublisher() {
    if (server && server->isListening()) server->close();
}

QString AnalyzerFramePublisher::defaultChannelName() {
    return QStringLiteral("ObriiAnalyzerSpectrum-v1");
}

bool AnalyzerFramePublisher::start(const QString &channelName) {
    if (server->isListening()) return true;
    QLocalServer::removeServer(channelName);
    if (!server->listen(channelName)) {
        lastError = server->errorString();
        return false;
    }
    lastError.clear();
    return true;
}

bool AnalyzerFramePublisher::hasSubscribers() const {
    return std::any_of(clients.cbegin(), clients.cend(), [](const QLocalSocket *client) {
        return client && client->state() == QLocalSocket::ConnectedState;
    });
}

QString AnalyzerFramePublisher::errorString() const { return lastError; }

void AnalyzerFramePublisher::acceptConnections() {
    while (server->hasPendingConnections()) {
        QLocalSocket *client = server->nextPendingConnection();
        clients.append(client);
        connect(client, &QLocalSocket::disconnected, this, [this, client]() {
            clients.removeAll(client);
            client->deleteLater();
        });
    }
}

void AnalyzerFramePublisher::publish(const std::vector<float> &frequencies,
                                     const std::vector<float> &levels,
                                     int sourceFftLength) {
    if (!hasSubscribers()) return;
    if (publishClock.isValid() && publishClock.elapsed() < 33) return;
    publishClock.restart();
    const QByteArray packet = encodeFrame(frequencies, levels, sourceFftLength, ++sequence);
    if (packet.isEmpty()) return;
    for (QLocalSocket *client : clients) {
        if (!client || client->state() != QLocalSocket::ConnectedState ||
            client->bytesToWrite() > kMaximumQueuedBytes) continue;
        client->write(packet);
    }
}

AnalyzerFrameClient::AnalyzerFrameClient(QObject *parent)
    : QObject(parent), socket(std::make_unique<QLocalSocket>()) {
    qRegisterMetaType<AnalyzerSpectrumFrame>();
    connect(socket.get(), &QLocalSocket::connected, this, &AnalyzerFrameClient::connected);
    connect(socket.get(), &QLocalSocket::disconnected, this, &AnalyzerFrameClient::disconnected);
    connect(socket.get(), &QLocalSocket::readyRead, this, &AnalyzerFrameClient::readAvailable);
    connect(socket.get(), &QLocalSocket::errorOccurred,
            this, [this](QLocalSocket::LocalSocketError) {
        lastError = socket->errorString();
        emit connectionError(lastError);
    });
}

AnalyzerFrameClient::~AnalyzerFrameClient() = default;

void AnalyzerFrameClient::connectToPublisher(const QString &channelName) {
    inputBuffer.clear();
    lastError.clear();
    socket->abort();
    socket->connectToServer(channelName, QIODevice::ReadOnly);
}

void AnalyzerFrameClient::disconnectFromPublisher() {
    socket->disconnectFromServer();
}

bool AnalyzerFrameClient::isConnected() const {
    return socket->state() == QLocalSocket::ConnectedState;
}

QString AnalyzerFrameClient::errorString() const { return lastError; }

void AnalyzerFrameClient::readAvailable() {
    inputBuffer.append(socket->readAll());
    while (inputBuffer.size() >= static_cast<int>(sizeof(quint32))) {
        QDataStream sizeStream(inputBuffer.left(static_cast<int>(sizeof(quint32))));
        sizeStream.setByteOrder(QDataStream::LittleEndian);
        quint32 payloadSize = 0;
        sizeStream >> payloadSize;
        if (payloadSize == 0 || payloadSize > static_cast<quint32>(kMaximumPacketBytes)) {
            inputBuffer.clear();
            lastError = QStringLiteral("Invalid analyzer frame size");
            emit connectionError(lastError);
            return;
        }
        const int packetSize = static_cast<int>(sizeof(quint32) + payloadSize);
        if (inputBuffer.size() < packetSize) return;
        const QByteArray payload = inputBuffer.mid(static_cast<int>(sizeof(quint32)),
                                                   static_cast<int>(payloadSize));
        inputBuffer.remove(0, packetSize);
        AnalyzerSpectrumFrame frame;
        if (!decodeFrame(payload, frame)) {
            lastError = QStringLiteral("Invalid analyzer frame payload");
            emit connectionError(lastError);
            continue;
        }
        emit frameReady(frame);
    }
}
