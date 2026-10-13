#pragma once
#include "dspstreamprocessor.h"
#include <QElapsedTimer>
#include <QTcpSocket>
#include <atomic>

namespace DspStreamTransport {
constexpr int PacketLimit = 16 + DspStreamProtocol::MaxJsonBytes + DspStreamProtocol::MaxPayloadBytes;
inline bool receive(QTcpSocket &socket, QByteArray &buffer, QJsonObject &json, QByteArray &payload,
                    int timeoutMs, const std::atomic<bool> *stop = nullptr) {
    QElapsedTimer timer; timer.start();
    QString error;
    while (timer.elapsed() < timeoutMs && (!stop || !stop->load())) {
        const int result = DspStreamProtocol::extract(buffer, json, payload, error);
        if (result) return result == 1;
        // Drain rejection replies even when the peer has already closed its socket.
        if (!socket.bytesAvailable()) {
            if (socket.state() != QAbstractSocket::ConnectedState) return false;
            socket.waitForReadyRead(25);
            if (!socket.bytesAvailable()) continue;
        }
        const qint64 room = PacketLimit - buffer.size();
        if (room <= 0) return false;
        buffer.append(socket.read(room));
    }
    return false;
}
inline bool send(QTcpSocket &socket, const QByteArray &bytes, const std::atomic<bool> *stop = nullptr,
                 int timeoutMs = 1000) {
    if (bytes.isEmpty() || bytes.size() > PacketLimit || socket.bytesToWrite() || socket.write(bytes) != bytes.size()) return false;
    QElapsedTimer timer; timer.start();
    while (socket.bytesToWrite() && timer.elapsed() < timeoutMs && (!stop || !stop->load())) {
        if (socket.state() != QAbstractSocket::ConnectedState) return false;
        socket.waitForBytesWritten(25);
    }
    return socket.bytesToWrite() == 0 && (!stop || !stop->load());
}
}
