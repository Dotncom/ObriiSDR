#include "clignsssession.h"
#include "climoduleprotocol.h"
#include "gnsssdrstream.h"
#include "gnsssdrdecimator.h"
#include "virtualsignaltransport.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <csignal>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace {
int checks = 0;
QElapsedTimer suiteTime;
void progress(const QString &stage) {
    std::fprintf(stderr, "GNSS live [%lld ms] %s\n", static_cast<long long>(suiteTime.elapsed()),
                 stage.toLocal8Bit().constData());
    std::fflush(stderr);
}
void require(bool okay, const char *message) {
    ++checks;
    if (!okay) throw std::runtime_error(message);
}
void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(), "write fixture");
}
QJsonObject readReport(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
bool alive(qint64 pid) {
#ifdef Q_OS_WIN
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) return false;
    const bool result = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    CloseHandle(handle);
    return result;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}
bool until(const std::function<bool()> &predicate, int ms = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return predicate();
}

int stalledFixture() {
    // Deliberately stubborn, but cannot remain orphaned forever if cleanup fails.
    std::signal(SIGTERM, SIG_IGN);
    QElapsedTimer expiry;
    expiry.start();
    while (expiry.elapsed() < 30000) QThread::msleep(50);
    return 94;
}

// An explicitly selected synthetic byte sink, not PocketSDR or receiver hardware.
int backend(const QStringList &args) {
    const QString path = qEnvironmentVariable("OBRII_GNSS_LIVE_REPORT");
    const QString mode = qEnvironmentVariable("OBRII_GNSS_LIVE_MODE");
    if (path.isEmpty() || mode.isEmpty()) return 90;
    QJsonObject report{{"pid", QCoreApplication::applicationPid()}, {"cwd", QDir::currentPath()},
                       {"arguments", QJsonArray::fromStringList(args)}};
    writeFile(path, QJsonDocument(report).toJson());
    writeFile(QStringLiteral("fixture.cache"), "synthetic");
    if (mode == "early") return 0;
    if (mode == "stall") return stalledFixture();
#ifdef Q_OS_WIN
    QLocalSocket iq;
    iq.connectToServer(args.last());
#else
    QTcpSocket iq;
    iq.connectToHost(QStringLiteral("127.0.0.1"), args.last().section(':', -1).toUShort());
#endif
    if (!iq.waitForConnected(5000)) return 91;
    if (mode == "slow") return stalledFixture();
    QTcpSocket nmea;
    const int nmeaIndex = args.indexOf("-nmea");
    nmea.connectToHost(QStringLiteral("127.0.0.1"), args.value(nmeaIndex + 1).section(':', -1).toUShort());
    if (!nmea.waitForConnected(5000)) return 92;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 bytes = 0;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        const QByteArray data = iq.readAll();
        bytes += data.size();
        hash.addData(data);
#ifdef Q_OS_WIN
        if (iq.state() == QLocalSocket::UnconnectedState) break;
#else
        if (iq.state() == QAbstractSocket::UnconnectedState) break;
#endif
        iq.waitForReadyRead(50);
    }
    const QByteArray tail = iq.readAll();
    bytes += tail.size(); hash.addData(tail);
    report.insert("bytes", QString::number(bytes));
    report.insert("sha256", QString::fromLatin1(hash.result().toHex()));
    writeFile(path, QJsonDocument(report).toJson());
    nmea.write("$GNTEST,late*00\r\n");
    nmea.flush(); nmea.waitForBytesWritten(100);
    std::fputs("backend noise must not enter JSON\n", stdout);
    return mode == "exit9" ? 9 : 0;
}

QByteArray control(QJsonObject object) {
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return CliModuleProtocol::frame(CliModuleProtocol::Configure, 0, data.constData(), data.size());
}
QJsonObject startObject(int rate = 8000000) {
    return {{"version", 1}, {"action", "start"}, {"source", "full"},
            {"sampleRateHz", rate}, {"centerHz", 1575420000.0}};
}
QByteArray eof(quint64 count) { return control({{"version", 1}, {"action", "eof"}, {"sourceSamples", QString::number(count)}}); }
QByteArray raw(const std::vector<float> &iq) {
    QByteArray data(static_cast<int>(iq.size() * 4), Qt::Uninitialized);
    for (std::size_t i = 0; i < iq.size(); ++i) {
        quint32 bits; std::memcpy(&bits, &iq[i], 4);
        qToLittleEndian(bits, data.data() + i * 4);
    }
    return data;
}
QByteArray expected(const std::vector<float> &iq, int factor) {
    GnssSdrDecimator decimator(factor);
    std::vector<float> samples;
    decimator.process(iq.data(), iq.size(), samples);
    return GnssSdr::floatToCs16(samples.data(), samples.size());
}
void verifyBytes(const QJsonObject &report, const QByteArray &bytes) {
    require(report.value("bytes").toString().toULongLong() == static_cast<quint64>(bytes.size()), "exact final IQ byte count");
    require(report.value("sha256").toString().toLatin1() == QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(), "exact live CF32 conversion/decimation bytes");
}
void verifyCleanup(const QJsonObject &report) {
    if (report.isEmpty()) return;
    require(!alive(report.value("pid").toVariant().toLongLong()), "backend child reaped");
    require(!QFileInfo::exists(report.value("cwd").toString()), "private backend directory removed");
}

class Worker {
public:
    Worker(const QString &executable, const QString &directory, const QString &mode = "normal", int rate = 8000000) {
        static int id = 0;
        reportPath = directory + QStringLiteral("/report-%1.json").arg(++id);
        cancelPath = directory + QStringLiteral("/cancel-%1").arg(id);
        progress(QStringLiteral("worker %1: %2, %3 Hz; report %4").arg(id).arg(mode).arg(rate).arg(reportPath));
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("OBRII_GNSS_LIVE_REPORT", reportPath);
        env.insert("OBRII_GNSS_LIVE_MODE", mode);
        process.setProcessEnvironment(env);
        process.start(executable, {"--live", "--sample-rate", QString::number(rate), "--center", "1575420000",
                                  "--systems", "GPS", "--backend", QCoreApplication::applicationFilePath(),
                                  "--cancel-file", cancelPath});
        require(process.waitForStarted(3000), "start live worker");
    }
    ~Worker() {
        if (process.state() != QProcess::NotRunning) {
            process.write(CliModuleProtocol::frame(CliModuleProtocol::Reset, 0, nullptr, 0));
            process.closeWriteChannel();
            if (!process.waitForFinished(7000)) { process.kill(); process.waitForFinished(3000); }
        }
    }
    void send(const QByteArray &bytes) { require(process.write(bytes) == bytes.size(), "send framed stdin"); }
    void pump() {
        buffer += process.readAllStandardOutput();
        int end;
        while ((end = buffer.indexOf('\n')) >= 0) {
            const auto line = buffer.left(end); buffer.remove(0, end + 1);
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            require(error.error == QJsonParseError::NoError && document.isObject(), "JSON-only stdout");
            events.append(document.object());
        }
    }
    bool hasState(const QString &state) {
        pump();
        for (const auto &event : events) if (event.value("state") == state) return true;
        return false;
    }
    void configure() { send(control(startObject())); }
    void waitReady() {
        progress(QStringLiteral("await ready: %1").arg(reportPath));
        require(until([&] { return hasState("ready"); }), "ready after backend IQ connection");
    }
    void finish(int code, const QString &state = "failed", bool closeInput = true) {
        progress(QStringLiteral("await exit %1: %2").arg(code).arg(reportPath));
        if (closeInput) process.closeWriteChannel();
        const bool finished = until([&] { pump(); return process.state() == QProcess::NotRunning; }, 12000);
        if (!finished) {
            std::cerr << "Timed out: " << reportPath.toStdString() << ", expected " << code << '\n';
            for (const auto &event : events) std::cerr << QJsonDocument(event).toJson(QJsonDocument::Compact).constData() << '\n';
            std::cerr << QJsonDocument(report()).toJson().constData();
        }
        require(finished, "bounded worker completion");
        pump();
        if (process.exitCode() != code) {
            std::cerr << "Expected exit " << code << ", actual " << process.exitCode() << '\n';
            std::cerr << process.readAllStandardError().constData();
            for (const auto &event : events) std::cerr << QJsonDocument(event).toJson(QJsonDocument::Compact).constData() << '\n';
            std::cerr << QJsonDocument(report()).toJson().constData();
        }
        require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == code, "worker exit code");
        require(!events.isEmpty() && events.last().value("state") == state, "terminal worker state");
        verifyCleanup(report());
        progress(QStringLiteral("worker done: %1").arg(reportPath));
    }
    QJsonObject report() const { return readReport(reportPath); }
    QProcess process;
    QList<QJsonObject> events;
    QString reportPath, cancelPath;
    QByteArray buffer;
};

void sourceTests() {
    using Source = GnssSdr::LiveSource;
    const QByteArray one(8, '\0');
    Source early(4000000, 1575420000);
    require(!early.submit(one, 4000000, 1575420000) && !early.error().isEmpty(), "queue rejects pre-ready IQ");
    for (bool changeRate : {false, true}) {
        Source changed(4000000, 1575420000); changed.setReady();
        require(!changed.submit(one, changeRate ? 8000000 : 4000000, changeRate ? 1575420000 : 1575420001), "rate/center changes fail closed");
    }
    Source blocks(4000000, 1575420000); blocks.setReady();
    for (int i = 0; i < Source::MaximumQueuedBlocks; ++i) require(blocks.submit(one, 4000000, 1575420000), "bounded small block accepted");
    require(!blocks.submit(one, 4000000, 1575420000), "block-count overrun fails");
    Source bytes(4000000, 1575420000); bytes.setReady();
    for (int i = 0; i < 4; ++i) require(bytes.submit(QByteArray(Source::MaximumBlockBytes, '\0'), 4000000, 1575420000), "bounded large block accepted");
    require(!bytes.submit(one, 4000000, 1575420000), "byte-count overrun fails");
    Source drain(4000000, 1575420000); drain.setReady();
    require(drain.submit(one, 4000000, 1575420000) && drain.finish(1), "EOF validates accepted source cursor");
    QByteArray data;
    require(drain.read(data) == Source::Read::Data && data == one && drain.read(data) == Source::Read::End, "EOF cannot discard last queued block");
    require(!drain.submit(one, 4000000, 1575420000), "post-EOF IQ rejected");
    Source mismatch(4000000, 1575420000); mismatch.setReady();
    require(!mismatch.finish(1), "incorrect EOF sample count rejected");
    Source cancelled(4000000, 1575420000); cancelled.setReady();
    cancelled.cancel();
    require(cancelled.read(data) == Source::Read::Cancelled && !cancelled.submit(one, 4000000, 1575420000), "cancel wakes reader and rejects producer");
}

void workerTests(const QString &worker, const QString &directory) {
    std::vector<float> iq;
    for (int i = 0; i < 8195; ++i) { iq.push_back((i % 17 - 8) / 16.0f); iq.push_back((i % 23 - 11) / 24.0f); }
    const auto source = raw(iq);
    for (int rate : {4000000, 8000000, 16000000, 50000000}) {
        const int factor = rate == 50000000 ? 16 : rate / 4000000;
        Worker run(worker, directory, "normal", rate);
        const auto config = control(startObject(rate));
        run.send(config.left(3)); run.send(config.mid(3, 10)); run.send(config.mid(13));
        run.waitReady();
        // Odd block lengths exercise retained decimation state and the final tail.
        for (int offset = 0; offset < source.size();) {
            const int take = (std::min)(136, source.size() - offset);
            run.send(CliModuleProtocol::frame(CliModuleProtocol::ChannelIqF32, rate, source.constData() + offset, take));
            offset += take;
            QCoreApplication::processEvents();
            if ((offset % (136 * 16)) == 0) QThread::msleep(3);
        }
        run.send(eof(iq.size() / 2));
        run.finish(0, "completed");
        verifyBytes(run.report(), expected(iq, factor));
        bool ack = false, lateNmea = false;
        for (const auto &event : run.events) {
            if (event.value("event") == "eof") {
                ack = event.value("sourceSamples").toString() == QString::number(iq.size() / 2) &&
                    event.value("trailingSourceSamples").toString().toInt() == static_cast<int>((iq.size() / 2) % factor);
            }
            if (event.value("sentence") == "$GNTEST,late*00") lateNmea = true;
        }
        require(ack && lateNmea, "EOF acknowledgment includes tail and preserves late NMEA");
    }
    for (const auto &bad : {QJsonObject{{"version", 2}}, QJsonObject{{"version", 1}, {"action", "start"},
         {"source", "channel"}, {"sampleRateHz", 8000000}, {"centerHz", 1575420000.0}},
         QJsonObject{{"version", 1}, {"action", "start"}, {"source", "full"}, {"sampleRateHz", 4000000}, {"centerHz", 1575420000.0}}}) {
        Worker run(worker, directory); run.send(control(bad)); run.finish(4);
        require(run.report().isEmpty(), "bad Configure never launches external backend");
    }
    {
        Worker run(worker, directory);
        run.send(CliModuleProtocol::frame(CliModuleProtocol::ChannelIqF32, 8000000, source.constData(), 8));
        run.finish(4); require(run.report().isEmpty(), "IQ before Configure/readiness never launches backend");
    }
    for (int variant = 0; variant < 7; ++variant) {
        Worker run(worker, directory); run.configure(); run.waitReady();
        if (variant == 0) run.send(eof(1));
        if (variant == 1) run.send(control(startObject(4000000)));
        if (variant == 2) run.send(CliModuleProtocol::frame(CliModuleProtocol::ChannelIqF32, 4000000, source.constData(), 8));
        if (variant == 3) run.send(CliModuleProtocol::frame(CliModuleProtocol::ChannelIqF32, 8000000, source.constData(), 7));
        if (variant == 4) run.send("short");
        if (variant == 5) run.send(CliModuleProtocol::frame(CliModuleProtocol::ChannelIqF32, 8000000, source.constData(), 8).left(15));
        // Variant 6: physical stdin EOF is not a successful logical EOF.
        run.finish(4);
    }
    {
        Worker run(worker, directory); run.send(CliModuleProtocol::frame(CliModuleProtocol::Reset, 0, nullptr, 0));
        run.finish(130, "cancelled"); require(run.report().isEmpty(), "pre-config Reset cancels without backend");
    }
    {
        Worker run(worker, directory); run.configure(); run.waitReady();
        run.send(CliModuleProtocol::frame(CliModuleProtocol::Reset, 0, nullptr, 0));
        run.finish(130, "cancelled");
    }
    for (const QString &mode : {QStringLiteral("normal"), QStringLiteral("stall")}) {
        Worker run(worker, directory, mode); run.configure();
        require(until([&] { return !run.report().isEmpty(); }), "synthetic backend explicitly started");
        if (mode == "normal") run.waitReady();
        writeFile(run.cancelPath, "cancel");
        run.finish(130, "cancelled", false);
        require(run.hasState("cancelled"), "cancellation interrupts blocked native stdin and waiting IQ source");
    }
    {
        Worker run(worker, directory); run.configure(); run.waitReady(); run.send(eof(0));
        run.finish(0, "completed"); verifyBytes(run.report(), {});
    }
    {
        Worker run(worker, directory, "early"); run.configure(); run.finish(4);
    }
    {
        Worker run(worker, directory, "slow"); run.configure(); run.waitReady();
        const QByteArray data(CliModuleProtocol::MaximumPayloadBytes, '\0');
        for (int i = 0; i < 24; ++i) run.send(CliModuleProtocol::frame(CliModuleProtocol::ChannelIqF32, 8000000, data.constData(), data.size()));
        run.finish(4);
        bool overrun = false;
        for (const auto &event : run.events) if (event.value("text").toString().contains("overrun")) overrun = true;
        require(overrun, "stalled backend causes explicit bounded producer overrun");
    }
}

void helperTests(const QString &worker, const QString &directory) {
    CliGnssSession session;
    require(session.status().value("state") == "stopped" && !session.ready(), "helper construction is inert");
    QJsonObject start{{"operation", "start"}, {"worker", worker}, {"backend", QCoreApplication::applicationFilePath()}, {"systems", QJsonArray{"GPS"}}};
    auto missing = start; missing.remove("backend");
    require(!session.command(missing, 8000000, 1575420000).value("ok").toBool(), "explicit backend required");
    require(!session.command(start, 8000000.5, 1575420000).value("ok").toBool(), "actual integer rate required");
    auto mismatch = start; mismatch.insert("centerHz", 1600000000.0);
    require(!session.command(mismatch, 8000000, 1575420000).value("ok").toBool(), "requested/actual center mismatch rejected");
    const QString reportPath = directory + "/helper.json";
    qputenv("OBRII_GNSS_LIVE_REPORT", reportPath.toUtf8());
    qputenv("OBRII_GNSS_LIVE_MODE", "normal");
    std::vector<float> iq(8198, 0.25f);
    progress(QStringLiteral("helper: normal EOF"));
    require(session.command(start, 8000000, 1575420000).value("ok").toBool(), "helper explicit start");
    require(!session.submitIq(iq.data(), 2, 8000000, 1575420000), "helper rejects pre-ready samples explicitly");
    require(until([&] { return session.ready(); }), "helper readiness handshake");
    require(session.submitIq(iq.data(), 4099, 8000000, 1575420000), "helper supplies full source IQ");
    require(session.command({{"operation", "eof"}}, 0, 0).value("ok").toBool(), "helper sends counted EOF");
    require(until([&] { return !session.status().value("running").toBool(); }), "helper waits for worker cleanup");
    require(session.status().value("state") == "completed" && session.status().value("eofAcknowledged").toBool(), "helper successful EOF status");
    verifyBytes(readReport(reportPath), expected(iq, 2)); verifyCleanup(readReport(reportPath));
    {
        progress(QStringLiteral("helper: multi-frame EOF"));
        std::vector<float> split(2 * 131075);
        for (std::size_t i = 0; i < split.size(); ++i) split[i] = (static_cast<int>(i % 33) - 16) / 32.0f;
        require(session.command(start, 8000000, 1575420000).value("ok").toBool(), "helper start for multi-frame native CF32 copy");
        require(until([&] { return session.ready(); }), "helper ready for multi-frame native CF32 copy");
        require(session.submitIq(split.data(), split.size() / 2, 8000000, 1575420000), "helper splits large native float block");
        session.command({{"operation", "eof"}}, 0, 0);
        require(until([&] { return !session.status().value("running").toBool(); }), "helper multi-frame completion");
        require(session.status().value("state") == "completed", "multi-frame EOF succeeded");
        verifyBytes(readReport(reportPath), expected(split, 2)); verifyCleanup(readReport(reportPath));
    }
    for (int variant = 0; variant < 4; ++variant) {
        progress(QStringLiteral("helper: source failure %1").arg(variant));
        require(session.command(start, 8000000, 1575420000).value("ok").toBool(), "helper restart");
        require(until([&] { return session.ready(); }), "helper ready on restart");
        if (variant == 0) require(!session.submitIq(iq.data(), 2, 4000000, 1575420000), "helper rate change fails");
        if (variant == 1) require(!session.submitIq(iq.data(), 2, 8000000, 1575420001), "helper retune fails");
        if (variant == 2) require(!session.submitIq(iq.data(), 2, 8000000, 1575420000, true), "helper discontinuity fails");
        if (variant == 3) {
            std::vector<float> large(1024 * 1024, 0.0f);
            require(!session.submitIq(large.data(), large.size() / 2, 8000000, 1575420000), "helper bounded pipe queue fails without partial submission");
        }
        session.stop();
        require(session.waitForStopped(), "asynchronous failure cleanup completes");
        require(session.status().value("state") == "failed", "failed source never reports successful EOF");
        verifyCleanup(readReport(reportPath));
    }
    progress(QStringLiteral("helper: reset during startup"));
    require(session.command(start, 8000000, 1575420000).value("ok").toBool(), "start for immediate cancellation");
    QElapsedTimer stopTime; stopTime.start();
    session.reset();
    require(stopTime.elapsed() < 100 && session.status().value("state") == "stopping", "reset during process start never waits on RX thread");
    require(session.waitForStopped(), "bounded shutdown wait after RX ended");
    require(!session.status().value("running").toBool() && session.status().value("state") == "cancelled", "reset cancels even while process is starting");
    progress(QStringLiteral("helper: asynchronous stop command"));
    require(session.command(start, 8000000, 1575420000).value("ok").toBool(), "start for asynchronous stop command");
    require(until([&] { return session.ready(); }), "ready before asynchronous stop command");
    stopTime.restart();
    const auto reply = session.command({{"operation", "stop"}}, 0, 0);
    require(stopTime.elapsed() < 100 && reply.value("state") == "stopping" && session.running(), "stop command returns stopping without waiting");
    require(session.waitForStopped() && session.status().value("state") == "cancelled", "asynchronous stop cleans worker");
    verifyCleanup(readReport(reportPath));
    qunsetenv("OBRII_GNSS_LIVE_REPORT"); qunsetenv("OBRII_GNSS_LIVE_MODE");
}

QJsonObject controlRequest(quint16 port, const QJsonObject &request) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(150)) return {};
    const QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(line) != line.size()) return {};
    QByteArray reply;
    QElapsedTimer timer; timer.start();
    while (!reply.contains('\n') && timer.elapsed() < 1000 && reply.size() <= 65536) {
        if (!socket.bytesAvailable()) socket.waitForReadyRead(25);
        reply += socket.read(4096);
        if (socket.state() == QAbstractSocket::UnconnectedState && !socket.bytesAvailable()) break;
    }
    if (!reply.contains('\n') || reply.size() > 65536) return {};
    return QJsonDocument::fromJson(reply.left(reply.indexOf('\n'))).object();
}

class MainCliRun {
public:
    ~MainCliRun() {
        if (process.state() != QProcess::NotRunning) {
            controlRequest(port, {{"action", "gnss"}, {"operation", "stop"}});
            controlRequest(port, {{"action", "quit"}});
            if (!process.waitForFinished(12000)) {
                process.terminate();
                if (!process.waitForFinished(300)) { process.kill(); process.waitForFinished(3000); }
            }
        }
        QObject::disconnect(&process, nullptr, nullptr, nullptr);
    }
    void drain() {
        output += process.readAllStandardOutput();
        errors += process.readAllStandardError();
        if (output.size() > 65536) output = output.right(65536);
        if (errors.size() > 65536) errors = errors.right(65536);
    }
    QProcess process;
    quint16 port = 0;
    QByteArray output, errors;
};

void mainIntegration(const QString &worker, const QString &cli, const QString &directory) {
    progress(QStringLiteral("main integration: virtual RF RX/control, no hardware"));
    QTcpServer iqReservation, controlReservation;
    require(iqReservation.listen(QHostAddress::LocalHost, 0) &&
            controlReservation.listen(QHostAddress::LocalHost, 0), "reserve distinct loopback test ports");
    const quint16 iqPort = iqReservation.serverPort(), port = controlReservation.serverPort();
    iqReservation.close(); controlReservation.close();
    const QString reportPath = directory + "/main-backend.json";
    const QString requestPath = directory + "/main gnss request.json";
    writeFile(requestPath, QJsonDocument(QJsonObject{
        {"backend", QCoreApplication::applicationFilePath()}, {"systems", QJsonArray{"GPS"}},
        {"worker", worker}, {"timeoutMs", 15000}}).toJson());
    MainCliRun run;
    run.port = port;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("OBRII_GNSS_LIVE_REPORT", reportPath);
    environment.insert("OBRII_GNSS_LIVE_MODE", "normal");
    run.process.setProcessEnvironment(environment);
    run.process.setWorkingDirectory(directory);
    QObject::connect(&run.process, &QProcess::readyReadStandardOutput, &run.process, [&] { run.drain(); });
    QObject::connect(&run.process, &QProcess::readyReadStandardError, &run.process, [&] { run.drain(); });
    run.process.start(cli, {"run", "--backend", "virtual", "--input", "rf", "--rate", "8000000",
        "--freq", "1575420000", "--virtual-rx-transport", "tcp", "--virtual-rx-bind", "127.0.0.1",
        "--virtual-rx-port", QString::number(iqPort), "--control-port", QString::number(port),
        "--spectrum", "--fft", "1024"});
    require(run.process.waitForStarted(3000), "launch main CLI with explicit virtual-only source");
    progress(QStringLiteral("main integration: receiver pid %1").arg(run.process.processId()));
    const auto check = [&](bool okay, const char *message) {
        if (!okay) {
            run.drain();
            std::cerr << run.output.constData() << run.errors.constData();
        }
        require(okay, message);
    };
    QJsonObject receiver;
    check(until([&] {
        receiver = controlRequest(port, {{"action", "status"}});
        return receiver.value("ok").toBool() && receiver.value("running").toBool();
    }), "main RX serves control while no IQ has arrived");
    require(receiver.value("sampleRateHz").toDouble() == 8000000 &&
            receiver.value("centerFrequencyHz").toDouble() == 1575420000 &&
            !receiver.value("gnss").toObject().value("running").toBool(), "actual virtual source rate/center; GNSS initially inert");

    QTcpSocket publisher;
    publisher.connectToHost(QHostAddress::LocalHost, iqPort);
    require(publisher.waitForConnected(2000), "connect virtual-only full IQ publisher");
    quint32 sequence = 0;
    const auto sendIq = [&](const std::vector<float> &iq) {
        VirtualSignalFrame frame;
        frame.streamId = QStringLiteral("gnss-main-fixture");
        frame.protocolId = QStringLiteral("raw-iq");
        frame.sequence = sequence++;
        frame.timestampNs = 1000000000LL + frame.sequence * 1000000LL;
        frame.payloadType = VirtualSignalPayloadType::ComplexFloat32;
        frame.sampleRate = 8000000;
        frame.centerFrequencyHz = 1575420000;
        frame.payload = raw(iq);
        QString error;
        const auto wrapped = VirtualSignalStreamParser::wrap(frame, &error);
        require(!wrapped.isEmpty() && error.isEmpty() && publisher.write(wrapped) == wrapped.size(), "send wrapped contiguous virtual CF32 frame");
        QElapsedTimer timer; timer.start();
        while (publisher.bytesToWrite() && timer.elapsed() < 2000) publisher.waitForBytesWritten(25);
        require(!publisher.bytesToWrite(), "bounded virtual IQ write drain");
    };
    const std::vector<float> prime(2048, -0.75f);
    sendIq(prime);
    check(until([&] {
        receiver = controlRequest(port, {{"action", "status"}});
        return receiver.value("samples").toDouble() == 1024 &&
            receiver.value("virtualRx").toObject().value("queuedBytes").toDouble(-1) == 0;
    }), "prime first virtual discontinuity before explicit GNSS start");
    require(controlRequest(port, {{"action", "gnss"}, {"operation", "start"}, {"path", requestPath}})
                .value("ok").toBool(), "main starts GNSS through request JSON");
    QJsonObject gnss;
    const auto readGnss = [&] { gnss = controlRequest(port, {{"action", "gnss"}, {"operation", "status"}}); };
    check(until([&] { readGnss(); return gnss.value("ready").toBool(); }), "main GNSS readiness with no further virtual samples");
    require(gnss.value("sourceSamples").toString() == "0", "priming IQ excluded from GNSS capture");
    progress(QStringLiteral("main integration: feed post-ready contiguous full IQ"));
    std::vector<float> all;
    for (int count : {17, 4099, 65539}) {
        std::vector<float> block(static_cast<std::size_t>(count) * 2);
        for (std::size_t i = 0; i < block.size(); ++i)
            block[i] = (static_cast<int>((all.size() + i) % 47) - 23) / 32.0f;
        all.insert(all.end(), block.begin(), block.end());
        sendIq(block);
    }
    const quint64 count = all.size() / 2;
    check(until([&] {
        readGnss();
        return gnss.value("sourceSamples").toString() == QString::number(count);
    }), "main supplies every source sample including a split RX block tail");
    require(gnss.value("ready").toBool() && gnss.value("rejectedBlocks").toString() == "0", "contiguous virtual frames do not invalidate GNSS");
    require(controlRequest(port, {{"action", "gnss"}, {"operation", "eof"}}).value("ok").toBool(), "main control sends counted EOF");
    check(until([&] { readGnss(); return gnss.value("ok").toBool() && !gnss.value("running").toBool(); }), "main drains GNSS EOF while virtual RX remains idle");
    require(gnss.value("state") == "completed" && gnss.value("eofAcknowledged").toBool() &&
            gnss.value("sourceSamples").toString() == QString::number(count), "main GNSS completed with exact acknowledged source cursor");
    verifyBytes(readReport(reportPath), expected(all, 2));
    verifyCleanup(readReport(reportPath));
    receiver = controlRequest(port, {{"action", "status"}});
    require(receiver.value("running").toBool() && receiver.value("samples").toDouble() == 1024 + count &&
            receiver.value("virtualRx").toObject().value("droppedSamples").toDouble(-1) == 0 &&
            receiver.value("virtualRx").toObject().value("rejected").toDouble(-1) == 0,
            "main RX remains running; no virtual IQ dropped or rejected");

    progress(QStringLiteral("main integration: active GNSS stop, then receiver quit"));
    require(controlRequest(port, {{"action", "gnss"}, {"operation", "start"}, {"path", requestPath}})
                .value("ok").toBool(), "main restarts GNSS explicitly");
    check(until([&] { readGnss(); return gnss.value("ready").toBool(); }), "main restarted GNSS ready");
    QElapsedTimer stopTime; stopTime.start();
    const auto stopping = controlRequest(port, {{"action", "gnss"}, {"operation", "stop"}});
    require(stopping.value("ok").toBool() && stopping.value("state") == "stopping" && stopTime.elapsed() < 1000,
            "main GNSS stop is asynchronous");
    check(until([&] { readGnss(); return gnss.value("ok").toBool() && !gnss.value("running").toBool(); }), "main active GNSS stop reaps worker");
    require(gnss.value("state") == "cancelled" && !gnss.value("eofAcknowledged").toBool(), "main stop is cancellation, not EOF completion");
    verifyCleanup(readReport(reportPath));
    require(controlRequest(port, {{"action", "quit"}}).value("ok").toBool(), "quit main receiver over control");
    require(run.process.waitForFinished(5000) && run.process.exitStatus() == QProcess::NormalExit &&
            run.process.exitCode() == 0, "main receiver exits and releases virtual/control endpoints");
    publisher.abort();
    progress(QStringLiteral("main integration: passed"));
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().contains("-sig")) return backend(app.arguments().mid(1));
        suiteTime.start();
        require(argc == 2 || argc == 3, "Usage: cli_gnss_live_selftest /absolute/path/ObriiCliGnssModule [/absolute/path/ObriiCli]");
        QTemporaryDir directory;
        require(directory.isValid(), "temporary live fixtures");
        const QString worker = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
        progress(QStringLiteral("start pid %1, worker %2").arg(QCoreApplication::applicationPid()).arg(worker));
        progress(QStringLiteral("source queue tests"));
        sourceTests();
        workerTests(worker, directory.path());
        helperTests(worker, directory.path());
        if (argc == 3) mainIntegration(worker, QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath(), directory.path());
        std::cout << checks << " checks, 0 failures, " << suiteTime.elapsed() << " ms\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "CLI GNSS live self-test failed: " << error.what() << '\n';
        return 1;
    }
}
