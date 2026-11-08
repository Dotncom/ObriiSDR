#include "climoduleprotocol.h"

#include "videoprocessor.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
bool readExact(char *target, std::size_t bytes) {
    std::size_t offset = 0;
    while (offset < bytes) {
        const std::size_t count = std::fread(target + offset, 1, bytes - offset, stdin);
        if (!count) return false;
        offset += count;
    }
    return true;
}

void send(const QJsonObject &object) {
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
    std::fflush(stdout);
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3 || QByteArray(argv[1]) != "rx" ||
        !QList<QByteArray>{"sstv", "apt", "wefax"}.contains(argv[2])) {
        std::fprintf(stderr, "Usage: ObriiCliImageModule rx sstv|apt|wefax\n");
        return 2;
    }
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) < 0 ||
        _setmode(_fileno(stdout), _O_BINARY) < 0) return 2;
#endif
    const QString mode = QString::fromLatin1(argv[2]);
    VideoProcessor processor;
    QImage latestFrame;
    QString latestStatus;
    QObject::connect(&processor, &VideoProcessor::frameReady, &app,
                     [&](const QImage &frame) { latestFrame = frame; });
    QObject::connect(&processor, &VideoProcessor::statusChanged, &app,
                     [&](const QString &status) {
        if (status == latestStatus) return;
        latestStatus = status;
        send({{QStringLiteral("event"), QStringLiteral("status")},
              {QStringLiteral("module"), mode}, {QStringLiteral("text"), status}});
    });
    const auto enable = [&] {
        if (mode == QStringLiteral("sstv")) processor.configureSstv(true);
        else if (mode == QStringLiteral("apt")) processor.configureApt(true);
        else processor.configureWefax(true);
    };
    enable();

    for (;;) {
        uchar header[12];
        if (!readExact(reinterpret_cast<char *>(header), sizeof(header))) break;
        const quint32 kind = qFromLittleEndian<quint32>(header);
        const quint32 rate = qFromLittleEndian<quint32>(header + 4);
        const quint32 size = qFromLittleEndian<quint32>(header + 8);
        if (size > CliModuleProtocol::MaximumPayloadBytes) return 2;
        QByteArray payload(static_cast<int>(size), Qt::Uninitialized);
        if (size && !readExact(payload.data(), size)) return 1;
        if (kind == CliModuleProtocol::Reset) {
            latestFrame = {};
            if (mode == QStringLiteral("sstv")) processor.resetSstvDecoder();
            else if (mode == QStringLiteral("apt")) {
                processor.configureApt(false);
                processor.configureApt(true);
            } else {
                processor.configureWefax(false);
                processor.configureWefax(true);
            }
        } else if (kind == CliModuleProtocol::Pcm16 && rate == 48000 && size % 2 == 0) {
            if (mode == QStringLiteral("sstv")) processor.processSstvPcmFrame(payload, rate);
            else if (mode == QStringLiteral("apt")) processor.processAptPcmFrame(payload, rate);
            else processor.processWefaxPcmFrame(payload, rate);
        } else if (kind == CliModuleProtocol::Snapshot) {
            const QString path = QString::fromUtf8(payload);
            QFile output(path);
            const bool opened = !latestFrame.isNull() && !path.isEmpty() &&
                output.open(QIODevice::WriteOnly | QIODevice::NewOnly);
            const bool saved = opened && latestFrame.save(&output, "PNG");
            if (opened && !saved) output.remove();
            if (!saved) {
                send({{QStringLiteral("event"), QStringLiteral("imageError")},
                      {QStringLiteral("module"), mode},
                      {QStringLiteral("error"), QStringLiteral("No frame yet, invalid path or file exists")}});
            } else {
                send({{QStringLiteral("event"), QStringLiteral("imageSnapshot")},
                      {QStringLiteral("module"), mode},
                      {QStringLiteral("path"), QFileInfo(path).absoluteFilePath()},
                      {QStringLiteral("width"), latestFrame.width()},
                      {QStringLiteral("height"), latestFrame.height()}});
            }
        } else if (kind != CliModuleProtocol::Configure) return 2;
        QCoreApplication::processEvents();
    }
    return std::feof(stdin) ? 0 : 1;
}
