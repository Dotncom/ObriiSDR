#include "virtualtransmitterbackend.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QIODevice>
#include <QLocalSocket>
#include <QTcpSocket>

#include <algorithm>

namespace {
constexpr int MaximumSamplesPerFrame = 16384;
constexpr qint64 MaximumPendingBytes = 16 * 1024 * 1024;

void setError(QString *error, const QString &message) {
    if (error) *error = message;
}
}

VirtualTransmitterBackend::VirtualTransmitterBackend() = default;
VirtualTransmitterBackend::~VirtualTransmitterBackend() = default;

void VirtualTransmitterBackend::setEndpoint(const VirtualSignalEndpoint &endpoint) {
    currentEndpoint = endpoint;
}

VirtualSignalEndpoint VirtualTransmitterBackend::endpoint() const {
    return currentEndpoint;
}

QString VirtualTransmitterBackend::id() const { return QStringLiteral("virtual-signal"); }
QString VirtualTransmitterBackend::displayName() const { return QStringLiteral("Virtual signal transport"); }
bool VirtualTransmitterBackend::isRfCapable() const { return false; }

bool VirtualTransmitterBackend::start(const TxConfiguration &configuration, QString *error) {
    stop();
    currentConfiguration = configuration;
    sequence = 0;
    outputEnabled = true;
    nextDiscontinuity = false;
    lastConnectAttemptMs = 0;
    lastHeartbeatMs = 0;
    lastAcknowledgementMs = 0;
    if (currentEndpoint.mode == VirtualSignalTransportMode::LocalIpc) {
        const QString name = currentEndpoint.localName.trimmed();
        if (name.isEmpty()) {
            setError(error, QStringLiteral("Enter a local virtual-signal channel name"));
            return false;
        }
    } else {
        const QString host = currentEndpoint.host.trimmed();
        if (host.isEmpty() || currentEndpoint.port == 0) {
            setError(error, QStringLiteral("Enter a valid virtual receiver IP address and port"));
            return false;
        }
    }
    running = true;
    pollConnection();
    if (localSocket && localSocket->state() == QLocalSocket::ConnectingState) {
        localSocket->waitForConnected(100);
    }
    if (tcpSocket && tcpSocket->state() == QAbstractSocket::ConnectingState) {
        tcpSocket->waitForConnected(100);
    }
    if (error) error->clear();
    return true;
}

bool VirtualTransmitterBackend::writeIq(const std::complex<float> *samples, int count,
                                        QString *error) {
    if (!running) {
        setError(error, QStringLiteral("Virtual transmitter is stopped"));
        return false;
    }
    if (!samples || count <= 0 || !outputEnabled) {
        if (error) error->clear();
        return true;
    }
    if (!pollConnection()) {
        nextDiscontinuity = true;
        if (currentConfiguration.liveSource) {
            if (error) error->clear();
            return true;
        }
        setError(error, QStringLiteral("Waiting for virtual IQ receiver"));
        return false;
    }
    int offset = 0;
    while (offset < count) {
        const int frameSamples = (std::min)(MaximumSamplesPerFrame, count - offset);
        VirtualSignalFrame frame;
        frame.sequence = sequence++;
        frame.timestampNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
        frame.streamId = QStringLiteral("obrii-tx");
        frame.protocolId = QStringLiteral("raw-iq");
        frame.payloadType = VirtualSignalPayloadType::ComplexFloat32;
        frame.sampleRate = static_cast<quint32>((std::max)(1, currentConfiguration.sampleRate));
        frame.centerFrequencyHz = static_cast<qint64>(currentConfiguration.frequencyHz);
        frame.discontinuity = nextDiscontinuity;
        frame.metadata.insert(QStringLiteral("sampleFormat"), QStringLiteral("cf32le"));
        frame.metadata.insert(QStringLiteral("complexSamples"), frameSamples);
        frame.payload = virtualSignalComplexFloat32Payload(samples + offset, frameSamples);
        nextDiscontinuity = false;
        if (!writeFrame(frame, error)) {
            resetConnection();
            nextDiscontinuity = true;
            if (currentConfiguration.liveSource) {
                if (error) error->clear();
                return true;
            }
            return false;
        }
        offset += frameSamples;
    }
    if (error) error->clear();
    return true;
}

bool VirtualTransmitterBackend::setOutputEnabled(bool enabled, QString *error) {
    if (outputEnabled != enabled && enabled) nextDiscontinuity = true;
    outputEnabled = enabled;
    if (error) error->clear();
    return true;
}

bool VirtualTransmitterBackend::drain(int timeoutMs, QString *error) {
    QIODevice *io = device();
    if (!io) {
        if (error) error->clear();
        return true;
    }
    QElapsedTimer timer;
    timer.start();
    while (io->bytesToWrite() > 0 && timer.elapsed() < timeoutMs) {
        const int remaining = (std::max)(1, timeoutMs - static_cast<int>(timer.elapsed()));
        if (localSocket && !localSocket->waitForBytesWritten(remaining)) break;
        if (tcpSocket && !tcpSocket->waitForBytesWritten(remaining)) break;
    }
    const bool ok = io->bytesToWrite() == 0;
    if (!ok) setError(error, QStringLiteral("Virtual transmitter did not drain before timeout"));
    else if (error) error->clear();
    return ok;
}

void VirtualTransmitterBackend::stop() {
    if (running && device() && !currentConfiguration.liveSource) {
        VirtualSignalFrame frame;
        frame.sequence = sequence++;
        frame.timestampNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
        frame.streamId = QStringLiteral("obrii-tx");
        frame.protocolId = QStringLiteral("raw-iq");
        frame.payloadType = VirtualSignalPayloadType::ComplexFloat32;
        frame.sampleRate = static_cast<quint32>((std::max)(1, currentConfiguration.sampleRate));
        frame.centerFrequencyHz = static_cast<qint64>(currentConfiguration.frequencyHz);
        frame.endOfStream = true;
        QString ignored;
        writeFrame(frame, &ignored);
        drain(250, &ignored);
    }
    resetConnection();
    running = false;
}

bool VirtualTransmitterBackend::isRunning() const { return running; }
bool VirtualTransmitterBackend::isConnected() const { return device() != nullptr; }

bool VirtualTransmitterBackend::pollConnection() {
    if (!running) return false;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (QIODevice *io = device()) {
        if (io->bytesAvailable() > 0) acknowledgementParser.append(io->readAll());
        VirtualSignalFrame acknowledgement;
        QString parseError;
        while (acknowledgementParser.takeFrame(&acknowledgement, &parseError)) {
            if (acknowledgement.protocolId == QStringLiteral("keepalive-ack")) {
                lastAcknowledgementMs = nowMs;
            }
        }
        if (!parseError.isEmpty()) acknowledgementParser.clear();
        if (lastAcknowledgementMs > 0 && nowMs - lastAcknowledgementMs > 5000) {
            resetConnection();
            nextDiscontinuity = true;
            return false;
        }
        if (nowMs - lastHeartbeatMs >= 1000) {
            VirtualSignalFrame heartbeat;
            heartbeat.sequence = sequence++;
            heartbeat.timestampNs = nowMs * 1000000LL;
            heartbeat.streamId = QStringLiteral("obrii-tx");
            heartbeat.protocolId = QStringLiteral("keepalive");
            heartbeat.payloadType = VirtualSignalPayloadType::ProtocolFrame;
            QString error;
            if (!writeFrame(heartbeat, &error)) {
                resetConnection();
                nextDiscontinuity = true;
                return false;
            }
            if (localSocket) localSocket->flush();
            if (tcpSocket) tcpSocket->flush();
            lastHeartbeatMs = nowMs;
        }
        return true;
    }
    if ((localSocket && localSocket->state() == QLocalSocket::ConnectingState) ||
        (tcpSocket && tcpSocket->state() == QAbstractSocket::ConnectingState)) return false;
    if (lastConnectAttemptMs != 0 && nowMs - lastConnectAttemptMs < 500) return false;
    lastConnectAttemptMs = nowMs;
    resetConnection();
    nextDiscontinuity = true;
    lastHeartbeatMs = nowMs;
    lastAcknowledgementMs = nowMs;
    if (currentEndpoint.mode == VirtualSignalTransportMode::LocalIpc) {
        localSocket = std::make_unique<QLocalSocket>();
        localSocket->connectToServer(currentEndpoint.localName.trimmed(), QIODevice::ReadWrite);
    } else {
        tcpSocket = std::make_unique<QTcpSocket>();
        tcpSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        tcpSocket->connectToHost(currentEndpoint.host.trimmed(), currentEndpoint.port,
                                 QIODevice::ReadWrite);
    }
    return isConnected();
}

void VirtualTransmitterBackend::resetConnection() {
    if (localSocket) localSocket->abort();
    if (tcpSocket) tcpSocket->abort();
    localSocket.reset();
    tcpSocket.reset();
    acknowledgementParser.clear();
}

bool VirtualTransmitterBackend::writeFrame(const VirtualSignalFrame &frame, QString *error) {
    QIODevice *io = device();
    if (!io) {
        setError(error, QStringLiteral("Virtual signal transport is disconnected"));
        return false;
    }
    const QByteArray wire = VirtualSignalStreamParser::wrap(frame, error);
    if (wire.isEmpty()) return false;
    const qint64 queued = localSocket ? localSocket->bytesToWrite() : tcpSocket->bytesToWrite();
    if (queued > MaximumPendingBytes) {
        const bool drained = localSocket ? localSocket->waitForBytesWritten(250)
                                         : tcpSocket->waitForBytesWritten(250);
        if (!drained && (localSocket ? localSocket->bytesToWrite() : tcpSocket->bytesToWrite()) > MaximumPendingBytes) {
            setError(error, QStringLiteral("Virtual receiver is too slow; transport queue is full"));
            nextDiscontinuity = true;
            return false;
        }
    }
    if (io->write(wire) != wire.size()) {
        setError(error, QStringLiteral("Virtual signal write failed"));
        return false;
    }
    if (error) error->clear();
    return true;
}

QIODevice *VirtualTransmitterBackend::device() const {
    if (localSocket && localSocket->state() == QLocalSocket::ConnectedState &&
        localSocket->error() == QLocalSocket::UnknownSocketError) return localSocket.get();
    if (tcpSocket && tcpSocket->state() == QAbstractSocket::ConnectedState &&
        tcpSocket->error() == QAbstractSocket::UnknownSocketError) return tcpSocket.get();
    return nullptr;
}
