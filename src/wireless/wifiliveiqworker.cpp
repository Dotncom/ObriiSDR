#include "wifiliveiqworker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
constexpr qint64 maxQueuedBytes = 16 * 1024 * 1024;
constexpr qint64 maxProcessBytes = 2 * 1024 * 1024;
constexpr qint64 maxWritePerTick = 1024 * 1024;
}

WifiLiveIqWorker::WifiLiveIqWorker(QObject *parent) : QObject(parent) {}

void WifiLiveIqWorker::enqueue(const QByteArray &iq, double sampleRate,
                               double centerFrequency, int samples) {
    if (!accepting.load(std::memory_order_acquire)) return;
    if (samples <= 0 || iq.size() != samples * 2 ||
        !std::isfinite(sampleRate) || !std::isfinite(centerFrequency) ||
        std::abs(sampleRate - expectedRate.load(std::memory_order_relaxed)) > 1.0 ||
        std::abs(centerFrequency - expectedCenter.load(std::memory_order_relaxed)) > 1000.0) {
        sourceChanged.store(true, std::memory_order_release);
        accepting.store(false, std::memory_order_release);
        return;
    }
    if (!queueMutex.tryLock()) {
        droppedSamples.fetch_add(static_cast<quint64>(samples), std::memory_order_relaxed);
        return;
    }
    if (queuedBytes + iq.size() > maxQueuedBytes) {
        droppedSamples.fetch_add(static_cast<quint64>(samples), std::memory_order_relaxed);
    } else {
        queue.enqueue(iq);
        queuedBytes += iq.size();
    }
    queueMutex.unlock();
}

void WifiLiveIqWorker::stopAccepting() {
    accepting.store(false, std::memory_order_release);
}

void WifiLiveIqWorker::start(const QString &program, const QStringList &arguments,
                             const QString &workingDirectory, double sampleRate,
                             double centerFrequency) {
    if (process) return;
    expectedRate.store(sampleRate, std::memory_order_relaxed);
    expectedCenter.store(centerFrequency, std::memory_order_relaxed);
    sourceChanged.store(false);
    droppedSamples.store(0);
    writtenSamples = lastReportedFedSamples = 0;
    decodedSamples = 0;
    packets = 0;
    stopping = false;
    outputBuffer.clear();
    const int outputIndex = arguments.indexOf(QStringLiteral("--output"));
    outputPath = outputIndex >= 0 && outputIndex + 1 < arguments.size()
        ? arguments.at(outputIndex + 1) : QString();
    process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(workingDirectory);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("OPENBLAS_NUM_THREADS"), QStringLiteral("1"));
    environment.insert(QStringLiteral("OMP_NUM_THREADS"), QStringLiteral("1"));
    environment.remove(QStringLiteral("PYTHONPATH"));
#ifdef Q_OS_WIN
    environment.insert(QStringLiteral("APPDATA"), workingDirectory);
#endif
    process->setProcessEnvironment(environment);
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= 0x08000000;
    });
#endif
    connect(process, &QProcess::readyReadStandardOutput, this, &WifiLiveIqWorker::readOutput);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus state) {
        stopAccepting();
        if (timer) { timer->stop(); timer->deleteLater(); timer = nullptr; }
        readOutput();
        {
            QMutexLocker lock(&queueMutex);
            queue.clear();
            queuedBytes = 0;
        }
        const bool success = state == QProcess::NormalExit && code == 0 && !stopping;
        const QString detail = stopping ? QStringLiteral("Stopped") :
            success ? QStringLiteral("Completed") : QStringLiteral("PHY exited with code %1").arg(code);
        const double seconds = sessionTimer.isValid() ? sessionTimer.elapsed() / 1000.0 : 0.0;
        const double decoderRate = seconds > 0.0 ? decodedSamples / seconds / 1e6 : 0.0;
        const QString totals = QStringLiteral(" | PHY %1 MS/s, read %2 M, queue drops %3 M")
            .arg(decoderRate, 0, 'f', 1)
            .arg(decodedSamples / 1e6, 0, 'f', 1)
            .arg(droppedSamples.load(std::memory_order_relaxed) / 1e6, 0, 'f', 1);
        QFile metadata(outputPath + QStringLiteral(".json"));
        if (!outputPath.isEmpty() && metadata.exists() && metadata.size() <= 256 * 1024 &&
            metadata.open(QIODevice::ReadOnly)) {
            const QJsonDocument document = QJsonDocument::fromJson(metadata.readAll());
            metadata.close();
            if (document.isObject()) {
                QJsonObject root = document.object();
                root.insert(QStringLiteral("transport"), QJsonObject{
                    {QStringLiteral("offered_samples"), static_cast<double>(writtenSamples)},
                    {QStringLiteral("decoded_samples_reported"), static_cast<double>(decodedSamples)},
                    {QStringLiteral("queue_dropped_samples"), static_cast<double>(droppedSamples.load(std::memory_order_relaxed))},
                    {QStringLiteral("elapsed_s"), seconds},
                    {QStringLiteral("effective_phy_msps"), decoderRate}
                });
                QSaveFile saved(outputPath + QStringLiteral(".json"));
                if (saved.open(QIODevice::WriteOnly)) {
                    saved.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                    if (!saved.commit()) emit message(QStringLiteral("Could not save live PHY transport counters."));
                }
            }
        }
        process->deleteLater();
        process = nullptr;
        emit finished(success, detail + totals);
    });
    process->start(program, arguments);
    if (!process->waitForStarted(3000)) {
        const QString error = process->errorString();
        process->deleteLater(); process = nullptr;
        emit finished(false, error);
        return;
    }
    timer = new QTimer(this);
    timer->setInterval(5);
    connect(timer, &QTimer::timeout, this, &WifiLiveIqWorker::flush);
    timer->start();
    reportTimer.start();
    sessionTimer.start();
    accepting.store(true, std::memory_order_release);
    emit started();
}

void WifiLiveIqWorker::stop() {
    stopAccepting();
    stopping = true;
    if (timer) timer->stop();
    {
        QMutexLocker lock(&queueMutex);
        queue.clear(); queuedBytes = 0;
    }
    if (!process) return;
    process->closeWriteChannel();
    if (!process->waitForFinished(1500)) {
        process->kill();
        process->waitForFinished(500);
    }
}

void WifiLiveIqWorker::flush() {
    if (!process || process->state() != QProcess::Running) return;
    if (sourceChanged.exchange(false)) {
        emit message(QStringLiteral("Receiver sample rate or center changed; restart live Wi-Fi decoding."));
        stop();
        return;
    }
    qint64 written = 0;
    while (process->bytesToWrite() < maxProcessBytes && written < maxWritePerTick) {
        QByteArray chunk;
        {
            QMutexLocker lock(&queueMutex);
            if (queue.isEmpty()) break;
            chunk = queue.dequeue();
            queuedBytes -= chunk.size();
        }
        if (process->write(chunk) != chunk.size()) {
            emit message(QStringLiteral("Live IQ pipe write failed."));
            stop();
            return;
        }
        written += chunk.size();
        writtenSamples += static_cast<quint64>(chunk.size() / 2);
    }
    if (reportTimer.elapsed() >= 1000) {
        qint64 queued;
        { QMutexLocker lock(&queueMutex); queued = queuedBytes; }
        const qint64 processBytes = process->bytesToWrite();
        const quint64 fed = writtenSamples > static_cast<quint64>(processBytes / 2)
            ? writtenSamples - static_cast<quint64>(processBytes / 2) : 0;
        const double rate = (fed - lastReportedFedSamples) * 1000.0 /
                            (std::max)(qint64(1), reportTimer.elapsed()) / 1e6;
        emit progress(packets, droppedSamples.load(std::memory_order_relaxed),
                      queued + processBytes, rate, fed, decodedSamples);
        lastReportedFedSamples = fed;
        reportTimer.restart();
    }
}

void WifiLiveIqWorker::readOutput() {
    if (!process) return;
    outputBuffer += process->readAllStandardOutput();
    if (outputBuffer.size() > 1024 * 1024) outputBuffer = outputBuffer.right(65536);
    int newline;
    while ((newline = outputBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = outputBuffer.left(newline).trimmed();
        outputBuffer.remove(0, newline + 1);
        if (line.startsWith("[HT-SIG]") || line.startsWith("[HT-DATA]")) continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) {
            if (!line.isEmpty()) emit message(QString::fromLocal8Bit(line.left(512)));
            continue;
        }
        const QJsonObject event = document.object();
        if (event.contains(QStringLiteral("row"))) {
            const QJsonArray values = event.value(QStringLiteral("row")).toArray();
            if (values.size() != 17) continue;
            QStringList row;
            for (const auto &value : values) row << value.toString();
            emit packetRow(row);
        } else if (event.contains(QStringLiteral("packets"))) {
            packets = static_cast<quint64>(event.value(QStringLiteral("packets")).toDouble());
            if (event.value(QStringLiteral("limit_reached")).toBool()) {
                emit message(QStringLiteral("Live PCAP limit reached."));
                stop();
            }
        } else if (event.contains(QStringLiteral("iq_samples"))) {
            decodedSamples = static_cast<quint64>(event.value(QStringLiteral("iq_samples")).toDouble());
        } else if (event.value(QStringLiteral("phy")).isObject()) {
            emit phyStats(event.value(QStringLiteral("phy")).toObject());
        }
    }
}
