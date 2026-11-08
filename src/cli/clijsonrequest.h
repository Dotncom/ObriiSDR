#pragma once

#include <QDeadlineTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

struct CliJsonReply {
    QByteArray line;
    QString error;
};

// The request is sent once. A missing reply cannot establish that a command
// with side effects failed to run on the server.
template <class Socket>
CliJsonReply exchangeCliJson(Socket &socket, const QJsonObject &request,
                             int writeMs, int replyMs, qint64 maxReplyBytes) {
    socket.setReadBufferSize(maxReplyBytes);
    const QByteArray wire = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    const QDeadlineTimer writeDeadline(writeMs);
    if (socket.write(wire) != wire.size())
        return {{}, QStringLiteral("Could not write the command")};
    while (socket.bytesToWrite() > 0) {
        if (writeDeadline.hasExpired() ||
            (!socket.waitForBytesWritten(static_cast<int>(writeDeadline.remainingTime())) &&
             socket.bytesToWrite() > 0))
            return {{}, QStringLiteral("Command write did not complete")};
    }

    const QDeadlineTimer replyDeadline(replyMs);
    while (!socket.canReadLine()) {
        if (socket.bytesAvailable() >= maxReplyBytes)
            return {{}, QStringLiteral("Server reply exceeds the line limit")};
        if (replyDeadline.hasExpired() ||
            (!socket.waitForReadyRead(static_cast<int>(replyDeadline.remainingTime())) &&
             !socket.canReadLine()))
            return {{}, QStringLiteral("Server did not reply to the written command")};
    }
    const QByteArray line = socket.readLine(maxReplyBytes + 1).trimmed();
    if (!QJsonDocument::fromJson(line).isObject())
        return {{}, QStringLiteral("Server returned an invalid JSON reply")};
    return {line, {}};
}
