#include "gnsssdrstream.h"
#include "climoduleprotocol.h"
#include "clijsonoutput.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <memory>
#include <thread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
void onSignal(int signal) { interrupted = signal; }
#ifdef Q_OS_WIN
std::atomic_bool consoleCancelled{false};
BOOL WINAPI onConsoleControl(DWORD control) {
    if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT)
        return FALSE;
    consoleCancelled.store(true);
    return TRUE;
}
#endif

// Native cancellable reads keep binary stdin off Qt's event queue. At most one
// protocol payload is in flight in addition to LiveSource's bounded queue.
class LiveStdin {
public:
    LiveStdin(const GnssSdr::Input &value, std::shared_ptr<GnssSdr::LiveSource> source)
        : input(value), source(std::move(source)), reader([this] { run(); done.store(true); }) {}
    ~LiveStdin() { stop(); }
    void stop() {
        abort.store(true);
        while (!done.load()) {
#ifdef Q_OS_WIN
            CancelSynchronousIo(reader.native_handle());
#endif
            QThread::msleep(5);
        }
        if (reader.joinable()) reader.join();
    }
    std::atomic_bool configured{false}, eof{false}, done{false}, cancelled{false};
private:
    // 1: complete; 0: physical EOF; -1: cancelled/read failure.
    int readExact(char *target, int size) {
        int offset = 0;
        while (offset < size && !abort.load()) {
#ifdef Q_OS_WIN
            DWORD count = 0;
            DWORD request = static_cast<DWORD>(size - offset);
            if (pipeInput) {
                DWORD available = 0;
                // QProcess may supply an overlapped named-pipe handle, for
                // which CancelSynchronousIo alone does not unblock ReadFile.
                // One reader owns stdin: never request more than is available.
                if (!PeekNamedPipe(stdinHandle, nullptr, 0, nullptr, &available, nullptr)) {
                    if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_HANDLE_EOF) {
                        if (offset) source->fail(QStringLiteral("Truncated live stdin frame."));
                        return 0;
                    }
                    if (!abort.load()) source->fail(QStringLiteral("Live stdin pipe inspection failed."));
                    return -1;
                }
                if (!available) { QThread::msleep(5); continue; }
                request = (std::min)(request, available);
            }
            if (!ReadFile(stdinHandle, target + offset, request, &count, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_HANDLE_EOF) count = 0;
                else {
                    if (!abort.load()) source->fail(QStringLiteral("Live stdin read failed."));
                    return -1;
                }
            }
#else
            pollfd descriptor{STDIN_FILENO, POLLIN, 0};
            const int result = ::poll(&descriptor, 1, 50);
            if (result < 0 && errno == EINTR) continue;
            if (result < 0 || (result > 0 && (descriptor.revents & (POLLERR | POLLNVAL)))) {
                source->fail(QStringLiteral("Live stdin poll failed.")); return -1;
            }
            if (!result) continue;
            const ssize_t count = ::read(STDIN_FILENO, target + offset, static_cast<std::size_t>(size - offset));
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count < 0) { source->fail(QStringLiteral("Live stdin read failed.")); return -1; }
#endif
            if (!count) {
                if (offset) source->fail(QStringLiteral("Truncated live stdin frame."));
                return 0;
            }
            offset += static_cast<int>(count);
        }
        return offset == size ? 1 : -1;
    }
    void run() {
        while (!abort.load()) {
            char header[12];
            const int result = readExact(header, sizeof(header));
            if (result != 1) {
                if (!abort.load() && result == 0 && !eof.load())
                    source->fail(QStringLiteral("Live stdin closed without versioned EOF."));
                return;
            }
            const auto *bytes = reinterpret_cast<const uchar *>(header);
            const quint32 kind = qFromLittleEndian<quint32>(bytes);
            const quint32 rate = qFromLittleEndian<quint32>(bytes + 4);
            const quint32 size = qFromLittleEndian<quint32>(bytes + 8);
            if (eof.load() || size > CliModuleProtocol::MaximumPayloadBytes ||
                (kind != CliModuleProtocol::Configure && kind != CliModuleProtocol::ChannelIqF32 &&
                 kind != CliModuleProtocol::Reset) ||
                (kind == CliModuleProtocol::Configure && (rate || !size || size > 4096)) ||
                (kind == CliModuleProtocol::Reset && (rate || size)) ||
                (kind == CliModuleProtocol::ChannelIqF32 && (rate != input.sampleRateHz || !size || size % 8))) {
                source->fail(QStringLiteral("Invalid live GNSS frame kind, rate, size or state.")); return;
            }
            QByteArray payload(static_cast<int>(size), Qt::Uninitialized);
            if (readExact(payload.data(), payload.size()) != 1) {
                if (!abort.load()) source->fail(QStringLiteral("Truncated live stdin payload."));
                return;
            }
            if (kind == CliModuleProtocol::Reset) {
                cancelled.store(true); source->cancel(); return;
            }
            if (kind == CliModuleProtocol::ChannelIqF32) {
                if (!source->submit(payload, rate, input.centerHz)) return;
                continue;
            }
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(payload, &error);
            const QJsonObject object = document.object();
            if (error.error != QJsonParseError::NoError || !document.isObject() ||
                object.value("version") != QJsonValue(1)) {
                source->fail(QStringLiteral("Live GNSS Configure requires JSON version 1.")); return;
            }
            if (!configured.load()) {
                if (object.size() != 5 || object.value("action") != QJsonValue("start") ||
                    object.value("source") != QJsonValue("full") ||
                    object.value("sampleRateHz") != QJsonValue(input.sampleRateHz) ||
                    object.value("centerHz") != QJsonValue(input.centerHz)) {
                    source->fail(QStringLiteral("Live Configure/start must match full-source rate and RF center.")); return;
                }
                configured.store(true);
            } else {
                bool valid = false;
                const QString text = object.value("sourceSamples").toString();
                const quint64 count = text.toULongLong(&valid);
                if (object.size() != 3 || object.value("action") != QJsonValue("eof") ||
                    !valid || QString::number(count) != text) {
                    source->fail(QStringLiteral("Expected Configure/eof with a decimal sourceSamples string; reconfigure requires restart.")); return;
                }
                if (!source->finish(count)) return;
                eof.store(true);
            }
        }
    }
    GnssSdr::Input input;
    std::shared_ptr<GnssSdr::LiveSource> source;
    std::atomic_bool abort{false};
#ifdef Q_OS_WIN
    const HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    const bool pipeInput = GetFileType(stdinHandle) == FILE_TYPE_PIPE;
#endif
    std::thread reader;
};

QString resolveBackend(const QString &selection) {
    QString executable = selection;
    if (selection == QStringLiteral("installed")) {
        // Same installed resolver as GnssSdrPvtDialog; no download or shell.
        executable = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("pocketsdr/pocket_trk"));
#ifdef Q_OS_WIN
        executable += QStringLiteral(".exe");
#endif
        if (!QFileInfo::exists(executable)) executable = QStandardPaths::findExecutable(QStringLiteral("pocket_trk"));
    }
    const QFileInfo info(executable);
    return !executable.isEmpty() && info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString();
}

QJsonObject status(const QString &state, const QString &text = {}) {
    QJsonObject object{{QStringLiteral("event"), QStringLiteral("status")},
                       {QStringLiteral("state"), state}};
    if (!text.isEmpty()) object.insert(QStringLiteral("text"), text);
    return object;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ObriiCliGnssModule"));
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef Q_OS_WIN
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Optional headless recorded/live-IQ PocketSDR worker (JSON Lines)."));
    parser.addHelpOption();
    parser.addOptions({
        QCommandLineOption("file", "Recorded IQ file (not stdin).", "path"),
        QCommandLineOption("live", "Version-1 framed full-source CF32 LE on stdin; requires Configure/start, ready and EOF."),
        QCommandLineOption("backend", "Explicit executable path, or 'installed' (adjacent pocketsdr/, then PATH).", "path|installed"),
        QCommandLineOption("systems", "Comma-separated GPS,GLONASS,Galileo,BeiDou,QZSS,SBAS; no default.", "list"),
        QCommandLineOption("format", "Raw IQ format: cf32, cs16, cs8 (little endian I,Q). Default: cf32.", "format"),
        QCommandLineOption("sample-rate", "Source complex samples/second; overrides raw sidecar metadata.", "Hz"),
        QCommandLineOption("center", "Recorded RF center in Hz; overrides metadata.", "Hz"),
        QCommandLineOption("timeout-ms", "Optional total runtime limit; zero disables it.", "ms", "0"),
        QCommandLineOption("cancel-file", "Cancel when this path exists (also supports Ctrl+C/SIGTERM).", "path")
    });
    CliJsonOutput output(QStringLiteral("gnss"));
    const auto fail = [&](const QString &message, int code) {
        output.send(status(QStringLiteral("failed"), message));
        std::fprintf(stderr, "GNSS: %s\n", message.toLocal8Bit().constData());
        output.finish();
        return code;
    };
    if (!parser.parse(app.arguments())) return fail(parser.errorText(), 2);
    if (parser.isSet(QStringLiteral("help"))) {
        output.finish();
        std::fputs(parser.helpText().toLocal8Bit().constData(), stdout);
        return 0;
    }
    const bool live = parser.isSet("live");
    if (!parser.positionalArguments().isEmpty() || (live == parser.isSet("file")) ||
        !parser.isSet("backend") || !parser.isSet("systems"))
        return fail(QStringLiteral("Required: --file PATH OR --live, --backend PATH|installed --systems LIST. See --help."), 2);
    if (parser.value("file") == QStringLiteral("-"))
        return fail(QStringLiteral("Use --live with versioned framed CF32 stdin, not --file -."), 2);
    QStringList systems;
    const QStringList known{"GPS", "GLONASS", "Galileo", "BeiDou", "QZSS", "SBAS"};
    for (const QString &entry : parser.value("systems").split(',')) {
        QString canonical;
        for (const QString &name : known)
            if (name.compare(entry.trimmed(), Qt::CaseInsensitive) == 0) canonical = name;
        if (canonical.isEmpty()) return fail(QStringLiteral("Unknown or empty GNSS system: %1").arg(entry), 2);
        if (!systems.contains(canonical)) systems.append(canonical);
    }
    bool valid = false;
    const int timeoutMs = parser.value("timeout-ms").toInt(&valid);
    if (!valid || timeoutMs < 0) return fail(QStringLiteral("--timeout-ms must be an integer in 0..2147483647."), 2);
    GnssSdr::Input input;
    input.live = live;
    if (!live) input.path = QFileInfo(parser.value("file")).absoluteFilePath();
    input.format = parser.isSet("format") ? parser.value("format").toLower() : QStringLiteral("cf32");
    if (input.format != "cf32" && input.format != "cs16" && input.format != "cs8")
        return fail(QStringLiteral("--format must be cf32, cs16 or cs8."), 2);
    QString error;
    if (live && (input.format != "cf32" || !parser.isSet("sample-rate") || !parser.isSet("center")))
        return fail(QStringLiteral("--live requires CF32, --sample-rate and --center."), 2);
    if (!live && !GnssSdr::readInputMetadata(input, &error)) return fail(error, 2);
    const bool wav = QFileInfo(input.path).suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0;
    if (wav && parser.isSet("format") && parser.value("format").compare(input.format, Qt::CaseInsensitive) != 0)
        return fail(QStringLiteral("WAV input requires stereo PCM16 IQ (--format cs16)."), 2);
    if (parser.isSet("sample-rate")) {
        const double rate = parser.value("sample-rate").toDouble(&valid);
        if (!valid || !std::isfinite(rate) || rate <= 0 || (wav && rate != input.sampleRateHz))
            return fail(QStringLiteral("Invalid --sample-rate, or it disagrees with the WAV header."), 2);
        input.sampleRateHz = rate;
    }
    if (parser.isSet("center")) {
        input.centerHz = parser.value("center").toDouble(&valid);
        if (!valid) return fail(QStringLiteral("Invalid --center."), 2);
    }
    if (!std::isfinite(input.sampleRateHz) || input.sampleRateHz <= 0 ||
        !std::isfinite(input.centerHz) || input.centerHz <= 0)
        return fail(QStringLiteral("Finite positive sample rate and RF center are required (metadata or --sample-rate/--center)."), 2);
    if (live && (input.sampleRateHz > 4294967295.0 || std::floor(input.sampleRateHz) != input.sampleRateHz))
        return fail(QStringLiteral("Live framed sample rate must be an integer in 1..4294967295 Hz."), 2);
    if (!GnssSdr::configureTrackingRate(input, systems, &error)) return fail(error, 2);
    const int bytesPerComplex = input.format == "cf32" ? 8 : input.format == "cs16" ? 4 : 2;
    const quint64 fileExpectedSamples = input.dataBytes / bytesPerComplex / input.decimation;
    if (!live && !fileExpectedSamples) return fail(QStringLiteral("IQ recording is empty or shorter than one decimated sample."), 2);
    const QString backend = resolveBackend(parser.value("backend"));
    if (backend.isEmpty())
        return fail(QStringLiteral("PocketSDR runtime is missing or not executable: %1 (no fallback for an explicit path).")
                    .arg(parser.value("backend")), 3);
    QTemporaryDir working(QDir::tempPath() + QStringLiteral("/obrii-cli-gnss-XXXXXX"));
    if (!working.isValid()) return fail(QStringLiteral("Cannot create private temporary backend directory."), 3);

    auto source = live ? std::make_shared<GnssSdr::LiveSource>(input.sampleRateHz, input.centerHz) : nullptr;
    GnssSdrStream stream(input, source);
    std::unique_ptr<LiveStdin> stdinReader;
    const auto expectedSamples = [&] { return source ? source->acceptedSamples() / input.decimation : fileExpectedSamples; };
    QProcess process;
    process.setWorkingDirectory(working.path());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#endif
    QEventLoop loop;
    QElapsedTimer elapsed, completionWait, startup;
    elapsed.start();
    std::atomic<quint64> samples{0};
    bool streamDone = false, backendDone = false, started = false, streamStarted = false;
    std::atomic_bool drained{false};
    int result = 0;
    QString terminalState = QStringLiteral("completed"), terminalText;
    const auto stop = [&](int code, const QString &state, const QString &message) {
        if (result) return;
        result = code;
        terminalState = state;
        terminalText = message;
        loop.quit();
    };
    const auto complete = [&] {
        if (stdinReader && stdinReader->cancelled.load()) {
            stop(130, QStringLiteral("cancelled"), QStringLiteral("Cancellation requested.")); return;
        }
        if (source && !source->error().isEmpty()) {
            stop(4, QStringLiteral("failed"), source->error()); return;
        }
        if (!streamDone || !backendDone || (stdinReader && !stdinReader->done.load())) return;
        if (samples.load() != expectedSamples() || (live && (!drained.load() || !stdinReader->eof.load())))
            stop(4, QStringLiteral("failed"), QStringLiteral("Backend closed before all IQ was supplied."));
        else loop.quit();
    };
    QObject::connect(&stream, &GnssSdrStream::listening, &app, [&](const QString &iq, quint16 nmea) {
        if (result) return;
        const QStringList arguments = GnssSdr::trackingArguments(input, systems, iq, nmea, &error);
        if (arguments.isEmpty()) { stop(2, "failed", error); return; }
        startup.start();
        process.start(backend, arguments);
    });
    QObject::connect(&stream, &GnssSdrStream::nmeaLine, &app, [&](const QString &line) {
        output.send({{QStringLiteral("event"), QStringLiteral("nmea")}, {QStringLiteral("sentence"), line}});
    }, Qt::DirectConnection);
    QObject::connect(&stream, &GnssSdrStream::samplesSent, &app,
                     [&](quint64 count) { samples.store(count); }, Qt::DirectConnection);
    QObject::connect(&stream, &GnssSdrStream::sourceReady, &app, [&] {
        QJsonObject event = status(QStringLiteral("ready"));
        event.insert(QStringLiteral("version"), 1);
        output.send(event);
    });
    QObject::connect(&stream, &GnssSdrStream::sourceDrained, &app,
                     [&](quint64 sourceCount, quint64 trackingCount) {
        drained.store(true);
        output.send({{"event", "eof"}, {"version", 1}, {"sourceSamples", QString::number(sourceCount)},
                     {"samplesSent", QString::number(trackingCount)},
                     {"trailingSourceSamples", QString::number(sourceCount % input.decimation)}});
    }, Qt::DirectConnection);
    QObject::connect(&stream, &GnssSdrStream::streamError, &app,
                     [&](const QString &message) { stop(4, "failed", message); });
    QObject::connect(&stream, &QThread::finished, &app, [&] {
        streamDone = true;
        if (!completionWait.isValid()) completionWait.start();
        complete();
    });
    QObject::connect(&process, &QProcess::started, &app, [&] {
        started = true;
        output.send(status(QStringLiteral("running")));
    });
    QObject::connect(&process, &QProcess::errorOccurred, &app, [&](QProcess::ProcessError processError) {
        stop(processError == QProcess::FailedToStart ? 3 : 4, "failed",
             QStringLiteral("PocketSDR process error: %1").arg(process.errorString()));
    });
    QObject::connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &app,
                     [&](int code, QProcess::ExitStatus exit) {
        backendDone = true;
        if (exit != QProcess::NormalExit || code != 0) {
            stop(4, "failed", QStringLiteral("PocketSDR exited unsuccessfully: %1.").arg(code));
            return;
        }
        if (!completionWait.isValid()) completionWait.start();
        complete();
    });
    QTimer timer;
    timer.setInterval(20);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        bool cancelled = interrupted != 0;
#ifdef Q_OS_WIN
        cancelled = cancelled || consoleCancelled.load();
#endif
        if (cancelled || (stdinReader && stdinReader->cancelled.load()) ||
            (parser.isSet("cancel-file") && QFileInfo::exists(parser.value("cancel-file"))))
            stop(130, "cancelled", QStringLiteral("Cancellation requested."));
        else if (!output.okay()) stop(5, "failed", QStringLiteral("JSON stdout failed or its bounded queue overflowed."));
        else if (source && !source->error().isEmpty()) stop(4, "failed", source->error());
        else if (timeoutMs && elapsed.elapsed() >= timeoutMs) stop(4, "failed", QStringLiteral("GNSS runtime limit exceeded."));
        else if (!started && startup.isValid() && startup.elapsed() >= 5000)
            stop(3, "failed", QStringLiteral("PocketSDR startup timed out."));
        else if (completionWait.isValid() && completionWait.elapsed() >= 5000)
            stop(4, "failed", QStringLiteral("PocketSDR/stream did not finish after IQ EOF or backend exit."));
        else if (stdinReader && !streamStarted) {
            if (stdinReader->configured.load()) { streamStarted = true; stream.start(); }
            else if (elapsed.elapsed() >= 5000) stop(4, "failed", QStringLiteral("Live Configure/start timed out."));
        }
        complete();
    });
    QJsonObject initial = status(QStringLiteral("starting"));
    initial.insert(QStringLiteral("backend"), backend);
    initial.insert(QStringLiteral("file"), input.path);
    initial.insert(QStringLiteral("systems"), QJsonArray::fromStringList(systems));
    initial.insert(QStringLiteral("sourceSampleRateHz"), input.sampleRateHz);
    initial.insert(QStringLiteral("trackingSampleRateHz"), input.sampleRateHz / input.decimation);
    initial.insert(QStringLiteral("centerHz"), input.centerHz);
    initial.insert(QStringLiteral("decimation"), input.decimation);
    initial.insert(QStringLiteral("workingDirectory"), working.path());
    if (live) initial.insert(QStringLiteral("version"), 1);
    output.send(initial);
    timer.start();
    if (live) stdinReader = std::make_unique<LiveStdin>(input, source);
    else { streamStarted = true; stream.start(); }
    loop.exec();
    timer.stop();

    // Stop the producer before terminating its consumer; do not destroy a running QThread.
    stream.requestStop();
    if (stdinReader) stdinReader->stop();
    if (process.state() != QProcess::NotRunning) {
        process.terminate();
        if (!process.waitForFinished(300)) { process.kill(); process.waitForFinished(3000); }
    }
    stream.wait();
    QObject::disconnect(&process, nullptr, &app, nullptr);
    if (!working.remove()) {
        if (!terminalText.isEmpty()) terminalText += QLatin1Char(' ');
        terminalText += QStringLiteral("Cannot remove temporary backend directory: %1.").arg(working.path());
        if (!result) { result = 4; terminalState = QStringLiteral("failed"); }
    }
    QJsonObject terminal = status(terminalState, terminalText);
    terminal.insert(QStringLiteral("samplesSent"), QString::number(samples.load()));
    terminal.insert(QStringLiteral("expectedSamples"), QString::number(expectedSamples()));
    if (source) terminal.insert(QStringLiteral("sourceSamples"), QString::number(source->acceptedSamples()));
    terminal.insert(QStringLiteral("exitCode"), result);
    output.send(terminal);
    if (!terminalText.isEmpty()) std::fprintf(stderr, "GNSS: %s\n", terminalText.toLocal8Bit().constData());
    if (!output.finish()) {
        std::fprintf(stderr, "GNSS: JSON stdout failed, overflowed or did not drain within 2 seconds.\n");
        return 5;
    }
    return result;
}
