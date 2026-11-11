#include "usrplabjob.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QResource>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

static QByteArray zmqWorker() {
    static const QByteArray scriptContents = [] {
        Q_INIT_RESOURCE(usrplabresources);
        QFile script(QStringLiteral(":/usrp-lab/usrp_zmq_loopback.py"));
        return script.open(QIODevice::ReadOnly) ? script.readAll() : QByteArray{};
    }();
    return scriptContents;
}

namespace {
const QString zmqAction = QStringLiteral("zmq-loopback");
const QStringList actions{QStringLiteral("contracts"), QStringLiteral("wifi-loopback"),
    QStringLiteral("lte-zmq"), QStringLiteral("gsm-demo"), QStringLiteral("gsm-decode"),
    QStringLiteral("gsm-replay"), QStringLiteral("umts-decode"), zmqAction};
constexpr qint64 maxResultBytes = 64 * 1024;
constexpr int maxDiagnosticBytes = 4096;

QJsonObject error(const QString &message) { return {{"ok", false}, {"error", message}}; }

QString labRoot() {
    const QString configured = qEnvironmentVariable("OBRII_USRP_LAB_ROOT");
    const auto checked = [](const QDir &dir) {
        return QFileInfo(dir.filePath(QStringLiteral("app_job.py"))).isFile()
            ? QFileInfo(dir.absolutePath()).canonicalFilePath() : QString{};
    };
    if (!configured.isEmpty())
        return QDir::isAbsolutePath(configured) ? checked(QDir(configured)) : QString{};
    QDir dir(QCoreApplication::applicationDirPath());
    do {
        const QString root = checked(QDir(dir.filePath(QStringLiteral("sandbox/usrp-u0-u2"))));
        if (!root.isEmpty()) return root;
    } while (dir.cdUp());
    return {};
}

QString interpreter() {
    const QString configured = qEnvironmentVariable("OBRII_USRP_LAB_PYTHON");
    if (!configured.isEmpty()) {
        const QFileInfo file(configured);
        return file.isAbsolute() && file.isFile() && file.isExecutable() ? file.absoluteFilePath() : QString{};
    }
#ifdef Q_OS_WIN
    return QStandardPaths::findExecutable(QStringLiteral("python.exe"));
#else
    return QStandardPaths::findExecutable(QStringLiteral("python3"));
#endif
}
}

UsrpLabJob::UsrpLabJob(QObject *parent) : QObject(parent) {
    process_.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    connect(&process_, &QProcess::readyRead, this, &UsrpLabJob::readDiagnostics);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError code) {
        if (code == QProcess::FailedToStart) finish(-1, process_.errorString());
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int code, QProcess::ExitStatus state) {
            finish(state == QProcess::NormalExit ? code : -1);
        });
}

UsrpLabJob::~UsrpLabJob() {
    if (!active_) return;
    disconnect(&process_, nullptr, this, nullptr);
    requestCancel();
    process_.closeWriteChannel();
    process_.waitForFinished(1500);
}

UsrpLabJob *UsrpLabJob::forOwner(QObject *owner) {
    if (auto *job = owner->findChild<UsrpLabJob *>(QString{}, Qt::FindDirectChildrenOnly)) return job;
    return new UsrpLabJob(owner);
}

QJsonObject UsrpLabJob::arguments(const QStringList &words) {
    const QString action = words.value(0, QStringLiteral("status"));
    QJsonObject request{{"action", action}};
    if (action == QStringLiteral("umts-decode")) {
        if (words.size() < 3 || words.size() > 6) return {};
        bool valid = false;
        const int rate = words[2].toInt(&valid);
        if (!valid) return {};
        const int offset = words.value(3, QStringLiteral("0")).toInt(&valid);
        if (!valid) return {};
        const qint64 start = words.value(5, QStringLiteral("0")).toLongLong(&valid);
        if (!valid || start < 0 || start > 9007199254740991LL) return {};
        request.insert("input", words[1]);
        request.insert("rate", rate);
        request.insert("offset", offset);
        request.insert("format", words.value(4, QStringLiteral("cf32")));
        request.insert("start", double(start));
        return request;
    }
    const bool decode = action == QStringLiteral("gsm-decode");
    const bool replay = action == QStringLiteral("gsm-replay");
    if (!decode && !replay) return words.size() <= 1 ? request : QJsonObject{};
    if (words.size() < 3 || words.size() > 4 || (replay && words.size() != 4)) return {};
    bool valid = false;
    const int arfcn = words[2].toInt(&valid);
    if (!valid) return {};
    request.insert("input", words[1]);
    request.insert("arfcn", arfcn);
    if (replay) request.insert("context", words[3]);
    else if (words.size() == 4) {
        const int timeslot = words[3].toInt(&valid);
        if (!valid) return {};
        request.insert("timeslot", timeslot);
    }
    return request;
}

QJsonObject UsrpLabJob::status() const {
    const bool pythonAvailable = !interpreter().isEmpty();
    const bool available = pythonAvailable && !labRoot().isEmpty();
    QStringList listed = actions;
    if (!pythonAvailable) listed.removeAll(zmqAction);
    QJsonObject state{{"ok", true}, {"available", available}, {"active", active_},
        {"zmqPythonAvailable", pythonAvailable}, {"cancelling", cancelling_},
        {"jobs", QJsonArray::fromStringList(listed)}};
    if (active_) state.insert("action", action_);
    if (!result_.isEmpty()) state.insert("lastResult", result_);
    if (!available) state.insert("setup", QStringLiteral("Set OBRII_USRP_LAB_ROOT to the local USRP lab and install Python 3.11+."));
    return state;
}

QJsonObject UsrpLabJob::command(const QJsonObject &request) {
    const QString action = request.value("action").toString();
    if (action == QStringLiteral("status") && request.size() == 1) return status();
    if (action == QStringLiteral("cancel") && request.size() == 1) {
        if (!active_) return error(QStringLiteral("No active USRP lab job."));
        if (!requestCancel()) return error(QStringLiteral("Could not request cancellation."));
        emit changed();
        return status();
    }
    if (!actions.contains(action)) return error(QStringLiteral("Unknown USRP lab action. Use usrp status."));
    if (active_) return error(QStringLiteral("A USRP lab job is already active."));

    QJsonObject payload{{"schema", 1}, {"action", action}};
    const bool umts = action == QStringLiteral("umts-decode");
    const bool decode = action == QStringLiteral("gsm-decode");
    const bool replay = action == QStringLiteral("gsm-replay");
    if (decode || replay) {
        const QFileInfo input(request.value("input").toString());
        const int arfcn = request.value("arfcn").toInt(-1);
        const qint64 maximum = replay ? 128 * 1024 : 32 * 1024 * 1024;
        if (arfcn < 0 || arfcn > 1023 || !input.isFile() || !input.isReadable() ||
            input.isSymLink() || input.size() <= 0 || input.size() > maximum || (decode && input.size() % 8))
            return error(QStringLiteral("Select a regular, bounded input file and ARFCN 0..1023."));
        payload.insert("input", input.absoluteFilePath());
        payload.insert("arfcn", arfcn);
        if (decode && request.contains("timeslot")) {
            const int slot = request.value("timeslot").toInt(-1);
            if (slot < 1 || slot > 7) return error(QStringLiteral("PDCH timeslot must be 1..7."));
            payload.insert("timeslot", slot);
        }
        if (replay) {
            const QFileInfo context(request.value("context").toString());
            if (!context.isDir() || context.isSymLink() ||
                !QFileInfo(QDir(context.absoluteFilePath()).filePath(QStringLiteral("manifest.json"))).isFile())
                return error(QStringLiteral("Select a GSM IQ result directory with a manifest."));
            payload.insert("context", context.absoluteFilePath());
        }
    }
    if (umts) {
        const QStringList fields{"action", "input", "rate", "offset", "format", "start"};
        for (auto field = request.begin(); field != request.end(); ++field)
            if (!fields.contains(field.key())) return error(QStringLiteral("Unexpected UMTS argument."));
        const QFileInfo input(request.value("input").toString());
        const int rate = request.value("rate").toInt(-1);
        const int offset = request.contains("offset") ? request.value("offset").toInt(-32000000) : 0;
        const QString format = request.contains("format") ? request.value("format").toString() : QStringLiteral("cf32");
        const double start = request.contains("start") ? request.value("start").toDouble(-1) : 0;
        const int margin = rate == 3840000 ? 0 : rate / 2 - 2500000;
        if ((rate != 3840000 && (rate < 5000000 || rate > 32000000)) || offset < -margin || offset > margin)
            return error(QStringLiteral("Use 3840000 Hz chip IQ or 5000000..32000000 Hz raw IQ with the carrier inside the passband."));
        const int width = format == QStringLiteral("cf32") ? 8 :
            format == QStringLiteral("cs16") ? 4 : format == QStringLiteral("ci8") ? 2 : 0;
        if (width == 0 || !input.isFile() || !input.isReadable() || input.isSymLink() || input.size() % width ||
            !(start >= 0 && start <= 9007199254740991.0 && start == std::floor(start)) ||
            input.size() / width - qint64(start) < (rate + 49) / 50)
            return error(QStringLiteral("Select a regular aligned CF32LE, CS16LE or CI8 file with at least 20 ms after the start sample."));
        payload.insert("input", input.absoluteFilePath());
        payload.insert("rate", rate);
        payload.insert("offset", offset);
        payload.insert("format", format);
        payload.insert("start", start);
    } else if (payload.size() != request.size() + 1)
        return error(QStringLiteral("Unexpected arguments for this USRP lab action."));

    const QString root = labRoot();
    const QString python = interpreter();
    const bool zmq = action == zmqAction;
    if (zmq && python.isEmpty())
        return {{"ok", false}, {"status", "BLOCKED"}, {"action", action},
                {"error", "ZeroMQ IQ loopback requires Python with PyZMQ."}};
    if (!zmq && (root.isEmpty() || python.isEmpty())) return error(status().value("setup").toString());
    QStringList arguments{QStringLiteral("-I"), QStringLiteral("-B")};
    if (zmq) {
        const QByteArray worker = zmqWorker();
        if (worker.isEmpty()) return error(QStringLiteral("Bundled ZeroMQ IQ worker is missing."));
        arguments << QStringLiteral("-c") << QString::fromUtf8(worker);
    } else {
        arguments << QDir(root).filePath(QStringLiteral("app_job.py"));
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString directory = zmq ? QDir::temp().filePath(QStringLiteral("obrii-usrp-zmq-") + id)
                                  : QDir(root).filePath(QStringLiteral("runs/app-") + id);
    if (!QDir().mkpath(directory)) return error(QStringLiteral("Cannot create USRP result directory."));
    QSaveFile file(QDir(directory).filePath(QStringLiteral("request.json")));
    const QByteArray bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return error(QStringLiteral("Cannot write USRP job request."));

    directory_ = directory;
    action_ = action;
    diagnostics_.clear();
    result_ = {};
    active_ = true;
    cancelling_ = false;
    process_.setWorkingDirectory(zmq ? directory : root);
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto *name : {"OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "BLIS_NUM_THREADS"})
        environment.insert(QString::fromLatin1(name), QStringLiteral("1"));
    process_.setProcessEnvironment(environment);
    process_.start(python, arguments << directory);
    emit changed();
    return status();
}

bool UsrpLabJob::requestCancel() {
    if (cancelling_) return true;
    QFile marker(QDir(directory_).filePath(QStringLiteral("cancel")));
    if (!marker.open(QIODevice::WriteOnly)) return false;
    marker.close();
    cancelling_ = true;
    return true;
}

void UsrpLabJob::readDiagnostics() {
    while (process_.bytesAvailable() > 0) {
        const QByteArray block = process_.read(maxDiagnosticBytes);
        if (block.isEmpty()) break;
        diagnostics_ = (diagnostics_ + block).right(maxDiagnosticBytes);
    }
}

void UsrpLabJob::finish(int exitCode, const QString &processError) {
    if (!active_) return;
    readDiagnostics();
    active_ = false;
    cancelling_ = false;
    QFile file(QDir(directory_).filePath(QStringLiteral("result.json")));
    if (file.open(QIODevice::ReadOnly) && file.size() <= maxResultBytes)
        result_ = QJsonDocument::fromJson(file.read(maxResultBytes + 1)).object();
    const QString outcome = result_.value("status").toString();
    const bool valid = result_.value("schema").toInt() == 1 &&
        result_.value("action").toString() == action_ &&
        (outcome == QStringLiteral("PASS") || outcome == QStringLiteral("FAIL") ||
         outcome == QStringLiteral("BLOCKED") || outcome == QStringLiteral("CANCELLED"));
    if (!valid) {
        result_ = error(processError.isEmpty() ? QStringLiteral("USRP worker returned no valid result.") : processError);
        result_.insert("status", QStringLiteral("FAIL"));
        if (!diagnostics_.isEmpty()) result_.insert("diagnostic", QString::fromUtf8(diagnostics_).trimmed());
    } else if (outcome == QStringLiteral("PASS") && exitCode != 0) {
        result_.insert("status", QStringLiteral("FAIL"));
        result_.insert("error", QStringLiteral("USRP worker or its resource controller exited unsuccessfully."));
    }
    result_.insert("ok", valid && outcome == QStringLiteral("PASS") && exitCode == 0);
    if (!result_.value("ok").toBool() && !diagnostics_.isEmpty())
        result_.insert("diagnostic", QString::fromUtf8(diagnostics_).trimmed());
    result_.insert("action", action_);
    result_.insert("directory", directory_);
    result_.insert("exitCode", exitCode);
    result_.insert("rfOpened", false);
    result_.insert("hardwareQualified", false);
    emit changed();
    emit completed(result_);
}
