#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace {
int checks = 0;
void require(bool condition, const char *message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(), "write fixture");
}
QJsonObject readObject(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool processAlive(qint64 pid) {
#ifdef Q_OS_WIN
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) return false;
    const bool alive = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    CloseHandle(handle);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

class WorkerProcess : public QProcess {
public:
    explicit WorkerProcess(bool blockOutput) : blocked(blockOutput) {
        if (!blocked) return;
#ifdef Q_OS_WIN
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        require(CreatePipe(&readPipe, &writePipe, &attributes, 4096), "blocked stdout pipe");
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
        setCreateProcessArgumentsModifier([this](CreateProcessArguments *arguments) {
            arguments->inheritHandles = true;
            arguments->startupInfo->dwFlags |= STARTF_USESTDHANDLES;
            arguments->startupInfo->hStdOutput = writePipe;
        });
#else
        require(::pipe(pipeEnds) == 0, "blocked stdout pipe");
#endif
    }
    ~WorkerProcess() override {
#ifdef Q_OS_WIN
        if (readPipe) CloseHandle(readPipe);
        if (writePipe) CloseHandle(writePipe);
#else
        if (pipeEnds[0] >= 0) ::close(pipeEnds[0]);
        if (pipeEnds[1] >= 0) ::close(pipeEnds[1]);
#endif
    }
#ifndef Q_OS_WIN
protected:
    void setupChildProcess() override {
        if (blocked) {
            ::dup2(pipeEnds[1], STDOUT_FILENO);
            ::close(pipeEnds[0]);
            ::close(pipeEnds[1]);
        }
    }
#endif
private:
    bool blocked;
#ifdef Q_OS_WIN
    HANDLE readPipe = nullptr, writePipe = nullptr;
#else
    int pipeEnds[2]{-1, -1};
#endif
};

// This executable doubles as a local transport fixture, never a GNSS decoder.
int backendFixture(const QStringList &arguments) {
    const QString mode = qEnvironmentVariable("OBRII_GNSS_TEST_MODE");
    const QString reportPath = qEnvironmentVariable("OBRII_GNSS_TEST_REPORT");
    if (mode.isEmpty() || reportPath.isEmpty()) return 90;
    QJsonObject report{{"pid", QCoreApplication::applicationPid()}, {"cwd", QDir::currentPath()},
                       {"arguments", QJsonArray::fromStringList(arguments)}};
    writeFile(reportPath, QJsonDocument(report).toJson());
    if (mode == "exit9") return 9;
    if (mode == "early") return 0;
    if (mode == "stall") {
        std::signal(SIGTERM, SIG_IGN);
        for (;;) QThread::msleep(50);
    }
    const int nmeaIndex = arguments.indexOf(QStringLiteral("-nmea"));
    if (nmeaIndex < 0 || nmeaIndex + 1 >= arguments.size()) return 91;
#ifdef Q_OS_WIN
    QLocalSocket iq;
    iq.connectToServer(arguments.last());
#else
    QTcpSocket iq;
    iq.connectToHost(QStringLiteral("127.0.0.1"), arguments.last().section(':', -1).toUShort());
#endif
    if (!iq.waitForConnected(5000)) return 92;
    QTcpSocket nmea;
    nmea.connectToHost(QStringLiteral("127.0.0.1"), arguments[nmeaIndex + 1].section(':', -1).toUShort());
    if (!nmea.waitForConnected(5000)) return 93;
    nmea.write("$GNRMC,fixture");
    nmea.flush();
    QThread::msleep(20);
    nmea.write("*00\r\n");
    nmea.flush();
    const QByteArray noise(128 * 1024, 'x');
    std::fwrite(noise.constData(), 1, noise.size(), stdout);
    std::fwrite(noise.constData(), 1, noise.size(), stderr);
    std::fflush(stdout);
    std::fflush(stderr);
    if (mode == "flood") {
        const QByteArray sentence = "$GNTEST," + QByteArray(900, 'a') + "*00\r\n";
        for (;;) {
            nmea.write(sentence);
            nmea.flush();
            nmea.waitForBytesWritten(1);
            QThread::msleep(1);
            if (nmea.state() == QAbstractSocket::UnconnectedState) return 0;
        }
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 received = 0;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        iq.waitForReadyRead(50);
        const QByteArray bytes = iq.readAll();
        hash.addData(bytes);
        received += static_cast<quint64>(bytes.size());
#ifdef Q_OS_WIN
        if (iq.state() == QLocalSocket::UnconnectedState) break;
#else
        if (iq.state() == QAbstractSocket::UnconnectedState) break;
#endif
    }
    report.insert("bytes", QString::number(received));
    report.insert("sha256", QString::fromLatin1(hash.result().toHex()));
    writeFile(reportPath, QJsonDocument(report).toJson());
    return timer.elapsed() < 10000 ? 0 : 94;
}

struct Run {
    int code = -1;
    QList<QJsonObject> events;
    QJsonObject report;
};

Run runWorker(const QString &worker, QStringList arguments, const QString &mode,
              const QString &directory, bool cancel = false, bool blockOutput = false) {
    const QString reportPath = directory + "/report.json";
    const QString cancelPath = directory + "/cancel";
    QFile::remove(reportPath);
    QFile::remove(cancelPath);
    arguments << "--cancel-file" << cancelPath;
    WorkerProcess process(blockOutput);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("OBRII_GNSS_TEST_MODE", mode);
    environment.insert("OBRII_GNSS_TEST_REPORT", reportPath);
    process.setProcessEnvironment(environment);
    process.start(worker, arguments);
    require(process.waitForStarted(5000), "worker start");
    QByteArray output, errors;
    QElapsedTimer timer;
    timer.start();
    bool requested = false;
    while (process.state() != QProcess::NotRunning && timer.elapsed() < 14000) {
        process.waitForFinished(25);
        output += process.readAllStandardOutput();
        errors += process.readAllStandardError();
        if (cancel && !requested && !readObject(reportPath).isEmpty()) {
            writeFile(cancelPath, {});
            requested = true;
        }
    }
    if (process.state() != QProcess::NotRunning) {
        writeFile(cancelPath, {});
        if (!process.waitForFinished(6000)) { process.kill(); process.waitForFinished(3000); }
        throw std::runtime_error("worker test timed out");
    }
    output += process.readAllStandardOutput();
    errors += process.readAllStandardError();
    require(process.exitStatus() == QProcess::NormalExit, "worker must not crash");
    Run result;
    result.code = process.exitCode();
    result.report = readObject(reportPath);
    for (const QByteArray &line : output.split('\n')) {
        if (line.isEmpty()) continue;
        const auto document = QJsonDocument::fromJson(line);
        require(document.isObject(), "stdout must contain JSON objects only (no backend log noise)");
        const auto object = document.object();
        require(object.value("module") == "gnss" && !object.value("utc").toString().isEmpty(), "JSON event envelope");
        result.events.append(object);
        if (object.value("state") == "starting")
            require(!QFileInfo::exists(object.value("workingDirectory").toString()), "temporary backend directory removed");
    }
    if (!blockOutput) require(!result.events.isEmpty(), "worker emits status");
    else require(errors.contains("stdout"), "blocked output diagnosed on stderr");
    if (result.report.contains("pid")) {
        require(!processAlive(static_cast<qint64>(result.report.value("pid").toDouble())), "backend child reaped");
        require(!QFileInfo::exists(result.report.value("cwd").toString()), "backend-created working directory removed");
    }
    return result;
}

void success(const Run &run, const QByteArray &expected, const QString &rate, int decimation) {
    if (run.code) std::cerr << QJsonDocument(run.events.last()).toJson().constData();
    require(run.code == 0 && run.events.last().value("state") == "completed", "successful file processing");
    require(run.events.first().value("decimation").toInt() == decimation, "shared tracking-rate selection");
    require(run.events.last().value("samplesSent").toString().toULongLong() == static_cast<quint64>(expected.size() / 4), "exact sample count");
    require(run.report.value("bytes").toString().toULongLong() == static_cast<quint64>(expected.size()), "backend IQ byte count");
    require(run.report.value("sha256").toString().toLatin1() == QCryptographicHash::hash(expected, QCryptographicHash::Sha256).toHex(), "backend IQ byte order and conversion");
    const auto argsArray = run.report.value("arguments").toArray();
    QStringList args;
    for (const auto &value : argsArray) args << value.toString();
    require(args.value(args.indexOf("-fmt") + 1) == "CS16" && args.value(args.indexOf("-f") + 1) == rate, "shared tracking arguments and rate");
    require(args.value(args.indexOf("-sig") + 1) == "L1CA" && args.value(args.indexOf("-prn") + 1) == "1-32", "explicit GPS signals");
    require(args.value(args.indexOf("-nmea") + 1).startsWith("127.0.0.1:"), "NMEA loopback only");
#ifdef Q_OS_WIN
    require(args.last().startsWith("\\\\.\\pipe\\"), "IQ named pipe only, never receiver hardware");
#else
    require(args.last().startsWith("127.0.0.1:"), "IQ loopback only, never receiver hardware");
#endif
    int sentences = 0;
    for (const auto &event : run.events)
        if (event.value("event") == "nmea" && event.value("sentence") == "$GNRMC,fixture*00") ++sentences;
    require(sentences == 1, "fragmented NMEA forwarded once without claiming a fix");
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().contains("-sig")) return backendFixture(app.arguments().mid(1));
        require(argc == 2, "Usage: cli_gnss_selftest /absolute/path/ObriiCliGnssModule");
        const QString worker = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
        const QString fixture = QCoreApplication::applicationFilePath();
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary fixtures");
        const QString raw = temporary.filePath("recording with spaces.cs16");
        const QStringList base{"--file", raw, "--format", "cs16", "--sample-rate", "4000000",
                               "--center", "1575420000", "--systems", "gps,GPS", "--backend", fixture};
        QByteArray iq;
        for (qint16 value : {qint16(32767), qint16(-32767), qint16(123), qint16(-456)}) {
            char bytes[2];
            qToLittleEndian(value, bytes);
            iq.append(bytes, 2);
        }
        writeFile(raw, iq);
        // Explicit flags must win over stale raw sidecars.
        writeFile(raw + ".json", "{\"sampleRateHz\":8000000,\"centerFrequencyHz\":1500000000}");
        success(runWorker(worker, base, "normal", temporary.path()), iq, "4", 1);

        auto changed = [&](const QString &option, const QString &value) {
            QStringList args = base;
            args[args.indexOf(option) + 1] = value;
            return args;
        };
        const QByteArray zeroInput(128, '\0');
        writeFile(raw, zeroInput);
        success(runWorker(worker, changed("--sample-rate", "8000000"), "normal", temporary.path()), QByteArray(64, '\0'), "4", 2);
        success(runWorker(worker, changed("--format", "cf32"), "normal", temporary.path()), QByteArray(64, '\0'), "4", 1);
        success(runWorker(worker, changed("--format", "cs8"), "normal", temporary.path()), QByteArray(256, '\0'), "4", 1);

        QByteArray cf32;
        for (float value : {1.0f, -1.0f, 0.5f, -0.5f}) {
            quint32 bits;
            std::memcpy(&bits, &value, sizeof(bits));
            char bytes[4];
            qToLittleEndian(bits, bytes);
            cf32.append(bytes, 4);
        }
        QByteArray quantized;
        for (qint16 value : {qint16(32767), qint16(-32767), qint16(16384), qint16(-16384)}) {
            char bytes[2];
            qToLittleEndian(value, bytes);
            quantized.append(bytes, 2);
        }
        writeFile(raw, cf32);
        success(runWorker(worker, changed("--format", "cf32"), "normal", temporary.path()), quantized, "4", 1);

        const QString wav = temporary.filePath("capture.wav");
        QByteArray header(44, '\0');
        std::memcpy(header.data(), "RIFF", 4);
        qToLittleEndian<quint32>(36 + iq.size(), header.data() + 4);
        std::memcpy(header.data() + 8, "WAVEfmt ", 8);
        qToLittleEndian<quint32>(16, header.data() + 16);
        qToLittleEndian<quint16>(1, header.data() + 20);
        qToLittleEndian<quint16>(2, header.data() + 22);
        qToLittleEndian<quint32>(4000000, header.data() + 24);
        qToLittleEndian<quint32>(16000000, header.data() + 28);
        qToLittleEndian<quint16>(4, header.data() + 32);
        qToLittleEndian<quint16>(16, header.data() + 34);
        std::memcpy(header.data() + 36, "data", 4);
        qToLittleEndian<quint32>(iq.size(), header.data() + 40);
        writeFile(wav, header + iq);
        success(runWorker(worker, changed("--file", wav), "normal", temporary.path()), iq, "4", 1);
        writeFile(raw, zeroInput);

        for (const auto &args : {
                 changed("--systems", "GPS,unknown"), changed("--systems", ""),
                 changed("--center", "1500000000"), changed("--center", "nan"),
                 changed("--sample-rate", "0"), changed("--format", "cu8"),
                 changed("--file", "-"), changed("--file", temporary.filePath("missing.raw"))}) {
            const auto run = runWorker(worker, args, "normal", temporary.path());
            require(run.code == 2 && run.events.last().value("state") == "failed" && run.report.isEmpty(), "invalid input rejected before backend launch");
        }
        auto missing = runWorker(worker, changed("--backend", temporary.filePath("missing-pocket-trk")), "normal", temporary.path());
        require(missing.code == 3 && missing.events.last().value("text").toString().contains("missing"), "explicit missing backend fails, no fallback");
        writeFile(raw, "abc");
        require(runWorker(worker, base, "normal", temporary.path()).code == 2, "partial complex IQ rejected");
        writeFile(raw, {});
        require(runWorker(worker, base, "normal", temporary.path()).code == 2, "empty IQ rejected");
        writeFile(raw, iq);
        require(runWorker(worker, base, "exit9", temporary.path()).code == 4, "backend failure propagated");
        require(runWorker(worker, base, "early", temporary.path()).code == 4, "premature zero backend exit is not success");
        auto timeoutArgs = base;
        timeoutArgs << "--timeout-ms" << "400";
        require(runWorker(worker, timeoutArgs, "stall", temporary.path()).code == 4, "stalled backend times out and is killed");
        const auto cancelled = runWorker(worker, base, "stall", temporary.path(), true);
        require(cancelled.code == 130 && cancelled.events.last().value("state") == "cancelled", "cancellation stops waiting stream and stubborn backend");
        require(runWorker(worker, base, "flood", temporary.path(), false, true).code == 5,
                "unconsumed stdout cannot grow queues indefinitely or strand cleanup");

        // Exercise the installed resolver without touching a real installation.
        const QString packaged = temporary.filePath("package");
        require(QDir().mkpath(packaged + "/pocketsdr"), "installed fixture directory");
#ifdef Q_OS_WIN
        const QString workerCopy = packaged + "/ObriiCliGnssModule.exe";
        const QString backendCopy = packaged + "/pocketsdr/pocket_trk.exe";
#else
        const QString workerCopy = packaged + "/ObriiCliGnssModule";
        const QString backendCopy = packaged + "/pocketsdr/pocket_trk";
#endif
        require(QFile::copy(worker, workerCopy) && QFile::copy(fixture, backendCopy), "installed fixture copies");
        success(runWorker(workerCopy, changed("--backend", "installed"), "normal", temporary.path()), iq, "4", 1);
        std::cout << checks << " checks, 0 failures\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "CLI GNSS self-test failed: " << exception.what() << '\n';
        return 1;
    }
}
