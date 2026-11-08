#include "wirelesscaptureworker.h"
#include <QMutexLocker>
#include <QTimer>

WirelessCaptureWorker::WirelessCaptureWorker(QObject *parent)
    : QObject(parent), process(new QProcess(this)) {
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000 | 0x00004000; });
#endif
    connect(process, &QProcess::readyReadStandardOutput, this, &WirelessCaptureWorker::readOutput);
    connect(process, &QProcess::readyReadStandardError, this, [this]() {
        diagnostics += process->readAllStandardError();
        if (diagnostics.size() > 4096) diagnostics = diagnostics.right(4096);
    });
    connect(process, &QProcess::errorOccurred, this, [this]() {
        emit statusChanged(process->errorString());
        emit finished();
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
        exited = true; exitCode = code;
        readOutput();
        finishIfDrained();
    });
}
WirelessCaptureWorker::~WirelessCaptureWorker() { stop(); }
Wireless::BackendCapabilities WirelessCaptureWorker::capabilities() const {
    Wireless::BackendCapabilities result;
    result.id = QStringLiteral("tshark");
    result.receivesPackets = true;
    return result;
}
void WirelessCaptureWorker::start(const QString &program, const QStringList &arguments, Job requestedJob) {
    stop();
    ++generation;
    pending.clear(); interfaceOutput.clear(); diagnostics.clear(); job = requestedJob;
    scheduled = exited = false; exitCode = 0;
    { QMutexLocker lock(&mutex); packets.clear(); droppedPackets = 0; }
    process->setProgram(program);
    process->setArguments(arguments);
    process->start();
    emit statusChanged(QStringLiteral("TShark: starting"));
}
void WirelessCaptureWorker::stop() {
    if (process->state() == QProcess::NotRunning) return;
    process->terminate();
    if (!process->waitForFinished(750)) {
        process->kill(); process->waitForFinished(750);
    }
}
void WirelessCaptureWorker::readOutput() {
    const QByteArray output = process->readAllStandardOutput();
    if (job != Job::Packets) {
        if (interfaceOutput.size() < 65536) interfaceOutput += output.left(65536 - interfaceOutput.size());
        return;
    }
    pending += output;
    if (pending.size() > 4 * 1024 * 1024) {
        pending.clear();
        emit statusChanged(QStringLiteral("TShark: analysis overflow, output discarded"));
        return;
    }
    consumeLines();
}
void WirelessCaptureWorker::consumeLines() {
    scheduled = false;
    QVector<QStringList> batch;
    int lineEnd = -1;
    for (int count = 0; count < 256 && (lineEnd = pending.indexOf('\n')) >= 0; ++count) {
        QStringList fields;
        if (Wireless::parsePacketLine(pending.left(lineEnd), fields)) batch << fields;
        pending.remove(0, lineEnd + 1);
    }
    if (!batch.isEmpty()) {
        QMutexLocker lock(&mutex);
        const int overflow = (std::max)(0, packets.size() + batch.size() - 2000);
        if (overflow) { packets.remove(0, overflow); droppedPackets += overflow; }
        packets += batch;
    }
    if (pending.contains('\n') && !scheduled) {
        scheduled = true;
        const auto run = generation;
        QTimer::singleShot(0, this, [this, run]() { if (run == generation) consumeLines(); });
    }
    finishIfDrained();
}
void WirelessCaptureWorker::finishIfDrained() {
    if (!exited || (job == Job::Packets && pending.contains('\n'))) return;
    exited = false;
    if (job == Job::Packets && !pending.isEmpty()) {
        QStringList row;
        if (Wireless::parsePacketLine(pending, row)) {
            QMutexLocker lock(&mutex);
            if (packets.size() == 2000) { packets.removeFirst(); ++droppedPackets; }
            packets.append(row);
        }
        pending.clear();
    }
    emit statusChanged(exitCode ? QString::fromUtf8(diagnostics).trimmed().left(1024) : QStringLiteral("TShark: complete"));
    if (job == Job::Interfaces) emit interfacesReady(QString::fromUtf8(interfaceOutput));
    if (job == Job::Report) emit reportReady(QString::fromUtf8(interfaceOutput));
    emit finished();
}
QVector<QStringList> WirelessCaptureWorker::takePackets(quint64 &dropped) {
    QMutexLocker lock(&mutex);
    QVector<QStringList> result;
    result.swap(packets);
    dropped = droppedPackets;
    droppedPackets = 0;
    return result;
}
