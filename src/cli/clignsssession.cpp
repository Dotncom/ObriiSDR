#include "clignsssession.h"
#include "climoduleprotocol.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSysInfo>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
QString workerPath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/ObriiCliGnssModule") +
#ifdef Q_OS_WIN
        QStringLiteral(".exe");
#else
        QString();
#endif
}
QJsonObject errorReply(const QString &message) { return {{"ok", false}, {"error", message}}; }
bool sampleCount(const QJsonValue &value, quint64 &count) {
    bool valid = false;
    const QString text = value.toString();
    count = text.toULongLong(&valid);
    return valid && text == QString::number(count);
}
}

CliGnssSession::CliGnssSession(QObject *parent) : QObject(parent), process(this), watchdog(this) {
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    connect(&process, &QProcess::started, this, [this] {
        if (stopping || failed) {
            sendReset();
            return;
        }
        sendControl({{"version", 1}, {"action", "start"}, {"source", "full"},
                     {"sampleRateHz", rate}, {"centerHz", center}});
    });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] { consumeOutput(); });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!stopping && error != QProcess::Crashed) fail(QStringLiteral("GNSS worker error: %1").arg(process.errorString()));
    });
    connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus exit) {
        consumeOutput();
        accepting = false;
        watchdog.stop();
        if (!stopping && !failed && (exit != QProcess::NormalExit || code != 0 ||
            !terminalReceived || !eofRequested || !eofAcknowledged))
            fail(QStringLiteral("GNSS worker exited without acknowledged EOF and successful completion."));
        if (stopping && !failed) state = QStringLiteral("cancelled");
        stopping = false;
    });
    watchdog.setInterval(50);
    connect(&watchdog, &QTimer::timeout, this, [this] {
        if (process.state() == QProcess::NotRunning) { watchdog.stop(); return; }
        if (!phaseTimer.isValid()) return;
        if (stopping || failed) {
            const qint64 elapsed = phaseTimer.elapsed();
            if (elapsed >= 7000 && !terminateSent) { terminateSent = true; process.terminate(); }
            if (elapsed >= 7300 && !killSent) { killSent = true; process.kill(); }
            return;
        }
        const int limit = eofRequested ? 15000 : accepting ? 0 : 40000;
        if (limit && phaseTimer.elapsed() >= limit) {
            fail(QStringLiteral("GNSS worker readiness/cleanup timed out."));
        }
    });
}
CliGnssSession::~CliGnssSession() {
    onEvent = {};
    stop();
    watchdog.stop();
    if (running() && !process.waitForFinished(7000)) {
        process.terminate();
        if (!process.waitForFinished(300)) { process.kill(); process.waitForFinished(3000); }
    }
}

bool CliGnssSession::ready() const {
    return accepting && !failed && !stopping && !eofRequested && process.state() == QProcess::Running;
}
bool CliGnssSession::running() const { return process.state() != QProcess::NotRunning; }
QJsonObject CliGnssSession::status() const {
    QJsonObject result{{"ok", true}, {"name", "gnss"}, {"state", state}, {"ready", ready()},
                       {"running", running()},
                       {"sourceSampleRateHz", rate}, {"centerHz", center}, {"decimation", decimation},
                       {"sourceSamples", QString::number(sourceSamples)},
                       {"rejectedBlocks", QString::number(rejectedBlocks)},
                       {"queuedBytes", static_cast<double>(process.bytesToWrite())},
                       {"eofAcknowledged", eofAcknowledged}};
    if (!lastError.isEmpty()) result.insert(QStringLiteral("error"), lastError);
    return result;
}
bool CliGnssSession::sendControl(const QJsonObject &object) {
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const auto framed = CliModuleProtocol::frame(CliModuleProtocol::Configure, 0,
                                                bytes.constData(), static_cast<quint32>(bytes.size()));
    if (process.write(framed) == framed.size()) return true;
    fail(QStringLiteral("Cannot write GNSS control frame."));
    return false;
}
void CliGnssSession::fail(const QString &message) {
    if (failed) return;
    failed = true;
    accepting = false;
    state = QStringLiteral("failed");
    lastError = message;
    phaseTimer.restart();
    sendReset();
    if (onEvent) onEvent({{"module", "gnss"}, {"event", "status"}, {"state", "failed"}, {"text", message}});
}

QJsonObject CliGnssSession::command(const QJsonObject &request, double actualRate, double actualCenter) {
    const QString operation = request.value("operation").toString();
    if (operation == "status") return status();
    if (operation == "stop" || operation == "reset") { stop(); return status(); }
    if (operation == "eof") {
        if (!ready()) return errorReply(QStringLiteral("GNSS is not ready or EOF was already requested."));
        accepting = false;
        eofRequested = true;
        state = QStringLiteral("draining");
        phaseTimer.restart();
        if (!sendControl({{"version", 1}, {"action", "eof"}, {"sourceSamples", QString::number(sourceSamples)}}))
            return errorReply(lastError);
        process.closeWriteChannel();
        stdinClosed = true;
        return status();
    }
    if (operation != "start") return errorReply(QStringLiteral("GNSS operation must be start, status, eof, stop or reset."));
    if (process.state() != QProcess::NotRunning) return errorReply(QStringLiteral("Stop the existing GNSS session before starting another."));
    if (!std::isfinite(actualRate) || actualRate <= 0 || actualRate > 4294967295.0 ||
        std::floor(actualRate) != actualRate || !std::isfinite(actualCenter) || actualCenter <= 0)
        return errorReply(QStringLiteral("GNSS needs actual full-source integer sample rate and finite positive RF center."));
    for (const auto &field : {QStringLiteral("sampleRateHz"), QStringLiteral("centerHz")}) {
        if (request.contains(field) && (!request.value(field).isDouble() ||
            request.value(field).toDouble() != (field == "sampleRateHz" ? actualRate : actualCenter)))
            return errorReply(QStringLiteral("Requested GNSS rate/center disagrees with the actual source."));
    }
    const QString backend = request.value("backend").toString();
    if (backend.isEmpty() || backend.size() > 4096 || backend.contains(QChar::Null))
        return errorReply(QStringLiteral("GNSS start requires an explicit backend executable path or installed."));
    QStringList entries, systems;
    if (request.value("systems").isString()) entries = request.value("systems").toString().split(',');
    else if (request.value("systems").isArray()) {
        for (const auto &entry : request.value("systems").toArray()) {
            if (!entry.isString()) return errorReply(QStringLiteral("GNSS systems must be strings."));
            entries << entry.toString();
        }
    }
    if (entries.isEmpty() || entries.size() > 32) return errorReply(QStringLiteral("GNSS start requires explicit systems."));
    for (const QString &entry : entries) {
        QString canonical;
        for (const QString &known : QStringList{"GPS", "GLONASS", "Galileo", "BeiDou", "QZSS", "SBAS"})
            if (known.compare(entry.trimmed(), Qt::CaseInsensitive) == 0) canonical = known;
        if (canonical.isEmpty()) return errorReply(QStringLiteral("Unknown or empty GNSS system."));
        if (!systems.contains(canonical)) systems << canonical;
    }
    const QString executable = request.contains("worker") ? request.value("worker").toString() : workerPath();
    if (executable.isEmpty() || executable.size() > 4096 || executable.contains(QChar::Null) ||
        !QFileInfo(executable).isFile() || !QFileInfo(executable).isExecutable())
        return errorReply(QStringLiteral("Optional GNSS worker is missing or not executable."));
    QStringList arguments{"--live", "--backend", backend, "--systems", systems.join(','),
                          "--sample-rate", QString::number(actualRate, 'g', 17),
                          "--center", QString::number(actualCenter, 'g', 17)};
    if (request.contains("timeoutMs")) {
        const auto value = request.value("timeoutMs");
        const double timeout = value.toDouble(-1);
        if (!value.isDouble() || !std::isfinite(timeout) || timeout < 0 || timeout > 2147483647 || std::floor(timeout) != timeout)
            return errorReply(QStringLiteral("GNSS timeoutMs must be an integer in 0..2147483647."));
        arguments << "--timeout-ms" << QString::number(static_cast<int>(timeout));
    }
    rate = actualRate;
    center = actualCenter;
    output.clear();
    sourceSamples = rejectedBlocks = 0;
    accepting = eofRequested = eofAcknowledged = terminalReceived = failed = stopping = false;
    stdinClosed = terminateSent = killSent = false;
    lastError.clear();
    decimation = 1;
    state = QStringLiteral("starting");
    phaseTimer.restart();
    process.start(QFileInfo(executable).absoluteFilePath(), arguments);
    watchdog.start();
    return status();
}

bool CliGnssSession::submitIq(const float *iq, int count, double actualRate, double actualCenter, bool discontinuity) {
    if (!ready()) { ++rejectedBlocks; return false; }
    if (!iq || count <= 0 || actualRate != rate || actualCenter != center || discontinuity) {
        ++rejectedBlocks;
        fail(QStringLiteral("GNSS full IQ source changed, retuned or became discontinuous; explicit restart required."));
        return false;
    }
    const qint64 frameSamples = CliModuleProtocol::MaximumPayloadBytes / 8;
    const qint64 frames = (static_cast<qint64>(count) + frameSamples - 1) / frameSamples;
    const qint64 bytes = static_cast<qint64>(count) * 8 + frames * 12;
    if (bytes > MaximumQueuedBytes - process.bytesToWrite()) {
        ++rejectedBlocks;
        fail(QStringLiteral("GNSS parent IQ pipe queue overrun; explicit restart required."));
        return false;
    }
    for (int offset = 0; offset < count;) {
        const int take = static_cast<int>((std::min)(frameSamples, static_cast<qint64>(count - offset)));
        QByteArray raw(take * 8, Qt::Uninitialized);
        const float *block = iq + static_cast<qint64>(offset) * 2;
        if (QSysInfo::ByteOrder == QSysInfo::LittleEndian) {
            std::memcpy(raw.data(), block, static_cast<std::size_t>(raw.size()));
        } else {
            for (int i = 0; i < take * 2; ++i) {
                quint32 bits;
                std::memcpy(&bits, block + i, 4);
                qToLittleEndian(bits, raw.data() + i * 4);
            }
        }
        const auto frame = CliModuleProtocol::frame(CliModuleProtocol::ChannelIqF32, static_cast<quint32>(rate),
                                                   raw.constData(), static_cast<quint32>(raw.size()));
        if (process.write(frame) != frame.size()) { fail(QStringLiteral("GNSS IQ pipe write failed.")); return false; }
        sourceSamples += take;
        offset += take;
    }
    return true;
}

void CliGnssSession::consumeOutput() {
    process.setReadChannel(QProcess::StandardOutput);
    while (process.bytesAvailable() > 0) {
        output += process.read(4096);
        int end;
        while ((end = output.indexOf('\n')) >= 0) {
            const QByteArray line = output.left(end);
            output.remove(0, end + 1);
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(line, &error);
            if (line.size() > 16384 || error.error != QJsonParseError::NoError || !doc.isObject() ||
                doc.object().value("module") != QJsonValue("gnss")) {
                fail(QStringLiteral("Invalid GNSS worker JSON output.")); continue;
            }
            const auto event = doc.object();
            const QString next = event.value("state").toString();
            if (event.value("event") == QJsonValue("status")) {
                if (next == "starting") decimation = event.value("decimation").toInt(1);
                if (next == "ready" && !failed && !stopping && !eofRequested) {
                    if (event.value("version") != QJsonValue(1)) fail(QStringLiteral("Unsupported GNSS readiness version."));
                    else { accepting = true; state = next; }
                }
                if (next == "failed") fail(event.value("text").toString(QStringLiteral("GNSS worker failed.")));
                if (next == "completed" || next == "failed" || next == "cancelled") {
                    terminalReceived = true;
                    accepting = false;
                    if (!failed && !stopping) state = next;
                }
            } else if (event.value("event") == QJsonValue("eof")) {
                quint64 sourceCount = 0, trackingCount = 0;
                if (!eofRequested || event.value("version") != QJsonValue(1) || decimation < 1 ||
                    !sampleCount(event.value("sourceSamples"), sourceCount) || sourceCount != sourceSamples ||
                    !sampleCount(event.value("samplesSent"), trackingCount) || trackingCount != sourceSamples / decimation)
                    fail(QStringLiteral("Invalid GNSS EOF acknowledgment."));
                else eofAcknowledged = true;
            }
            if (onEvent) onEvent(event);
        }
        if (output.size() > 16384) { output.clear(); fail(QStringLiteral("GNSS worker output line exceeded limit.")); }
    }
}
void CliGnssSession::reset() { stop(); }
void CliGnssSession::sendReset() {
    if (process.state() != QProcess::Running || stdinClosed) return;
    process.write(CliModuleProtocol::frame(CliModuleProtocol::Reset, 0, nullptr, 0));
    process.closeWriteChannel();
    stdinClosed = true;
}
void CliGnssSession::stop() {
    accepting = false;
    if (!running() || stopping) return;
    stopping = true;
    if (!failed) state = QStringLiteral("stopping");
    phaseTimer.restart();
    terminateSent = killSent = false;
    sendReset();
    watchdog.start();
}
bool CliGnssSession::waitForStopped(int timeoutMs) {
    if (!running()) return true;
    if (timeoutMs <= 0) return false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const auto connection = connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                                    &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    if (running()) loop.exec();
    disconnect(connection);
    return !running();
}
