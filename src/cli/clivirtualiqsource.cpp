#include "clivirtualiqsource.h"
#include <QDateTime>
#include <QHostAddress>
#include <QLocalSocket>
#include <QTcpSocket>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstring>

CliVirtualIqSource::CliVirtualIqSource(QObject *parent) : QObject(parent) {
    connect(&local, &QLocalServer::newConnection, this, [this] {
        while (local.hasPendingConnections()) accept(local.nextPendingConnection());
    });
    connect(&tcp, &QTcpServer::newConnection, this, [this] {
        while (tcp.hasPendingConnections()) accept(tcp.nextPendingConnection());
    });
}
CliVirtualIqSource::~CliVirtualIqSource() { stop(); }

bool CliVirtualIqSource::start(const VirtualSignalEndpoint &endpoint, quint32 sampleRate,
                               qint64 centerHz, qint64 queueLimit, QString *error) {
    stop();
    rate = sampleRate; center = centerHz; limitBytes = queueLimit;
    if (!rate || queueLimit < 1024 * 1024 || queueLimit > 64LL * 1024 * 1024) {
        if (error) *error = QStringLiteral("Invalid virtual RX rate or queue limit");
        return false;
    }
    frames = samples = drops = rejected = connections = 0;
    lastError.clear();
    bool ok;
    if (endpoint.mode == VirtualSignalTransportMode::LocalIpc) {
        ok = !endpoint.localName.trimmed().isEmpty() && local.listen(endpoint.localName);
        if (!ok && error) *error = local.errorString();
    } else {
        QHostAddress address;
        ok = address.setAddress(endpoint.host) && tcp.listen(address, endpoint.port);
        if (!ok && error) *error = tcp.errorString();
    }
    return ok;
}
void CliVirtualIqSource::stop() {
    local.close(); tcp.close();
    if (peer) { peer->disconnect(this); peer->close(); peer->deleteLater(); peer.clear(); }
    queue.clear(); parser.clear(); queuedBytes = 0; haveSequence = false; gap = true;
}
void CliVirtualIqSource::accept(QIODevice *socket) {
    if (!socket) return;
    socket->setParent(this);
    if (peer) { socket->close(); socket->deleteLater(); ++rejected; return; }
    if (auto *tcpSocket = qobject_cast<QTcpSocket *>(socket)) {
        tcpSocket->setReadBufferSize(2 * 1024 * 1024);
        tcpSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(tcpSocket, &QTcpSocket::disconnected, this, [this, socket] {
            if (peer != socket) return;
            peer.clear(); parser.clear(); haveSequence = false; gap = true;
            socket->deleteLater();
        });
    } else if (auto *localSocket = qobject_cast<QLocalSocket *>(socket)) {
        localSocket->setReadBufferSize(2 * 1024 * 1024);
        connect(localSocket, &QLocalSocket::disconnected, this, [this, socket] {
            if (peer != socket) return;
            peer.clear(); parser.clear(); haveSequence = false; gap = true;
            socket->deleteLater();
        });
    }
    peer = socket; parser.clear(); haveSequence = false; gap = true; ++connections;
    connect(socket, &QIODevice::readyRead, this, [this] { pump(); });
    pump();
}
void CliVirtualIqSource::reject(const QString &message) {
    lastError = message; ++rejected; gap = true;
    if (peer) peer->close();
}
void CliVirtualIqSource::pump() {
    if (!peer) return;
    qint64 processed = 0;
    while (peer && peer->bytesAvailable() > 0 && processed < 512 * 1024) {
        const QByteArray bytes = peer->read(64 * 1024);
        if (bytes.isEmpty()) break;
        processed += bytes.size(); parser.append(bytes);
        VirtualSignalFrame frame;
        QString error;
        while (parser.takeFrame(&frame, &error)) {
            if (frame.protocolId == QStringLiteral("keepalive")) {
                if (haveSequence) previousSequence = frame.sequence;
                VirtualSignalFrame ack;
                ack.sequence = frame.sequence; ack.streamId = frame.streamId;
                ack.timestampNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
                ack.protocolId = QStringLiteral("keepalive-ack");
                ack.payloadType = VirtualSignalPayloadType::ProtocolFrame;
                if (peer->bytesToWrite() < 65536) peer->write(VirtualSignalStreamParser::wrap(ack));
                continue;
            }
            if (frame.endOfStream) { haveSequence = false; gap = true; continue; }
            if (frame.payloadType != VirtualSignalPayloadType::ComplexFloat32 ||
                frame.sampleRate != rate || frame.centerFrequencyHz != center) {
                reject(QStringLiteral("Virtual IQ format/rate/center differs from explicit --rate and --freq"));
                return;
            }
            Chunk chunk;
            if (!virtualSignalDecodeComplexFloat32(frame.payload, &chunk.iq, &error)) break;
            if (std::any_of(chunk.iq.cbegin(), chunk.iq.cend(), [](float value) { return !std::isfinite(value); })) {
                error = QStringLiteral("Virtual IQ contains non-finite samples"); break;
            }
            chunk.gap = frame.discontinuity || !haveSequence || frame.sequence != previousSequence + 1;
            previousSequence = frame.sequence; haveSequence = true;
            const qint64 chunkBytes = static_cast<qint64>(chunk.iq.size()) * sizeof(float);
            while (!queue.isEmpty() && (queuedBytes + chunkBytes > limitBytes || queue.size() >= 256)) {
                const Chunk old = queue.dequeue();
                queuedBytes -= static_cast<qint64>(old.iq.size() - old.offset) * sizeof(float);
                drops += (old.iq.size() - old.offset) / 2; gap = true;
            }
            queuedBytes += chunkBytes;
            samples += chunk.iq.size() / 2; ++frames;
            queue.enqueue(std::move(chunk));
        }
        if (!error.isEmpty()) { reject(error); return; }
    }
    if (peer && peer->bytesAvailable() && !scheduled) {
        scheduled = true;
        QTimer::singleShot(0, this, [this] { scheduled = false; pump(); });
    }
}
unsigned int CliVirtualIqSource::read(float *target, unsigned int capacity) {
    if (!target || queue.isEmpty() || !capacity) return 0;
    Chunk &chunk = queue.head();
    const unsigned int count = (std::min)(capacity, static_cast<unsigned int>((chunk.iq.size() - chunk.offset) / 2));
    gap |= chunk.gap; chunk.gap = false;
    std::memcpy(target, chunk.iq.constData() + chunk.offset, static_cast<size_t>(count) * 8);
    chunk.offset += static_cast<int>(count) * 2; queuedBytes -= static_cast<qint64>(count) * 8;
    if (chunk.offset == chunk.iq.size()) queue.dequeue();
    return count;
}
bool CliVirtualIqSource::takeDiscontinuity() { const bool result = gap; gap = false; return result; }
QJsonObject CliVirtualIqSource::status() const {
    return {{"listening", local.isListening() || tcp.isListening()}, {"connected", !peer.isNull()},
            {"frames", static_cast<double>(frames)}, {"samples", static_cast<double>(samples)},
            {"droppedSamples", static_cast<double>(drops)}, {"rejected", static_cast<double>(rejected)},
            {"connections", static_cast<double>(connections)}, {"queuedBytes", queuedBytes},
            {"lastError", lastError}};
}
