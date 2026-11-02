#include "clidspgraph.h"
#include "diagnosticlogging.h"
#include "dspiqsourceregistry.h"
#include "iqbuffer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
constexpr double SourceRate = 384000.0, Center = 100000000.0, Offset = 24000.0;
constexpr double ChannelRate = 96000.0;
constexpr std::size_t Samples = 32768;
const QString Token = QStringLiteral("cli-selftest-session-0123456789-ABCDEFGHIJKLMNOPQRSTUVWXYZ");
const QString WrongToken = QStringLiteral("cli-selftest-wrong-0123456789-ABCDEFGHIJKLMNOPQRSTUVWXYZ");
const DspSourceIdentity Auxiliary = DspSourceIdentity::normalized("auxiliary-test", "rx7");
std::mutex logMutex;
QStringList messages;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void captureLog(QtMsgType, const QMessageLogContext &, const QString &message) {
    std::lock_guard<std::mutex> lock(logMutex);
    messages.append(message);
}
void noSecrets(const QJsonObject &object) {
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    require(!bytes.contains(Token.toUtf8()) && !bytes.contains(WrongToken.toUtf8()),
            "A command response or graph snapshot exposed a session token");
}
bool waitUntil(const std::function<bool()> &done, int timeout = 3000) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        if (done()) return true;
        QThread::msleep(2);
    } while (timer.elapsed() < timeout);
    return false;
}
QJsonObject graphDefinition(bool remote, bool auxiliary = false, bool auxiliaryRemote = false,
                            const QString &sourceType = QStringLiteral("iq_source")) {
    QJsonArray blocks{QJsonObject{{"id", "source"}, {"type", sourceType}}}, edges;
    const auto add = [&](const QString &id, bool external, bool aux) {
        QJsonObject settings{{"enabled", true}, {"externalProcessing", external},
                             {"frequencyHz", Center + Offset}, {"bandwidthHz", 12500.0},
                             {"outputRateHz", ChannelRate}};
        if (aux) {
            settings.insert("receiverId", Auxiliary.receiverId);
            settings.insert("channelId", Auxiliary.channelId);
        }
        blocks.append(QJsonObject{{"id", id}, {"type", "ddc_channelizer"}, {"settings", settings}});
        blocks.append(QJsonObject{{"id", id + "-fft"}, {"type", "branch_fft"},
            {"settings", QJsonObject{{"fftLength", 512}, {"refreshMs", 20}}}});
        blocks.append(QJsonObject{{"id", id + "-spectrum"}, {"type", "branch_spectrum"}});
        blocks.append(QJsonObject{{"id", id + "-demod"}, {"type", "branch_demodulator"},
            {"settings", QJsonObject{{"modulation", MOD_NFM}, {"bandwidthHz", 12500.0},
                                     {"lowPassHz", 3000.0}, {"squelchDb", -160.0}}}});
        blocks.append(QJsonObject{{"id", id + "-audio"}, {"type", "branch_audio_output"},
            {"settings", QJsonObject{{"enabled", true}}}});
        edges.append(QJsonObject{{"source", "source"}, {"target", id}});
        edges.append(QJsonObject{{"source", id}, {"target", id + "-fft"}});
        edges.append(QJsonObject{{"source", id + "-fft"}, {"target", id + "-spectrum"}});
        edges.append(QJsonObject{{"source", id}, {"target", id + "-demod"}});
        edges.append(QJsonObject{{"source", id + "-demod"}, {"target", id + "-audio"}});
    };
    add("z-local", false, false);
    if (remote) add("a-remote", true, false);
    if (auxiliary) add("b-auxiliary", auxiliaryRemote, true);
    return {{"blocks", blocks}, {"connections", edges}};
}

struct Fixture {
    CliDspGraph graph;
    std::vector<float> iq;
    bool auxiliary = false;
    bool queuePrimaryIq = false;
    std::size_t queuedPublications = 0;
    explicit Fixture(bool aux = false) : iq(2 * Samples), auxiliary(aux) {
        graph.setAuxiliarySourceLookup([](const DspSourceIdentity &source, double *center, double *rate) {
            if (!(source == Auxiliary)) return false;
            *center = Center;
            *rate = SourceRate;
            return true;
        });
        for (std::size_t i = 0; i < Samples; ++i) {
            const double t = double(i) / SourceRate;
            const double phase = 6.283185307179586 * Offset * t +
                                 2.0 * std::sin(6.283185307179586 * 1000.0 * t);
            iq[2*i] = float(0.4 * std::cos(phase));
            iq[2*i+1] = float(0.4 * std::sin(phase));
        }
    }
    ~Fixture() { graph.stop(); DspIqSourceRegistry::instance().clear(Auxiliary); }
    QJsonObject call(const QJsonObject &request) {
        const auto reply = graph.command(request, Center, SourceRate, false);
        noSecrets(reply);
        return reply;
    }
    QJsonObject ok(const QJsonObject &request) {
        const auto reply = call(request);
        require(reply.value("ok").toBool(), "CLI graph command unexpectedly failed");
        return reply;
    }
    void load(const QJsonObject &definition) {
        ok({{"operation", "restoreDefinition"}, {"definition", definition}});
    }
    QJsonObject branch(const QString &id) const {
        const auto snapshot = graph.status();
        noSecrets(snapshot);
        for (const auto &item : snapshot.value("branches").toArray())
            if (item.toObject().value("id") == id) return item.toObject();
        return {};
    }
    double pcm(const QString &id) const { return branch(id).value("pcmFrames").toDouble(); }
    void publish() {
        QElapsedTimer timer;
        timer.start();
        if (queuePrimaryIq) {
            require(IqBuffer::publish(iq.data(), iq.size(), true, true), "Cannot publish main audio IQ");
            ++queuedPublications;
        } else graph.publishIq(iq.data(), Samples);
        require(timer.elapsed() < 200, "Publishing source IQ blocked on remote compute");
        if (auxiliary)
            require(DspIqSourceRegistry::instance().publish(Auxiliary, iq.data(), iq.size(), SourceRate, Center),
                    "Auxiliary engine did not subscribe to its source identity");
    }
    bool drive(const std::function<bool()> &done, int timeout = 3000) {
        QElapsedTimer publishTimer;
        publishTimer.start();
        publish();
        return waitUntil([&] {
            graph.poll();
            if (done()) return true;
            if (publishTimer.elapsed() >= 25) { publish(); publishTimer.restart(); }
            return false;
        }, timeout);
    }
    void endpoint(quint16 port, const QString &token = Token) {
        ok({{"operation", "remote"}, {"remoteOperation", "configure"},
            {"address", "127.0.0.1"}, {"port", int(port)}, {"token", token}});
    }
    quint16 compute() {
        ok({{"operation", "compute"}, {"computeOperation", "start"},
            {"address", "127.0.0.1"}, {"port", 0}, {"token", Token}});
        require(waitUntil([&] { return graph.status().value("compute").toObject().value("port").toInt() != 0; }),
                "Shared compute service did not bind localhost");
        return quint16(graph.status().value("compute").toObject().value("port").toInt());
    }
};

void testValidationAndSecrets() {
    Fixture f;
    f.load(graphDefinition(true));
    require(!f.call({{"operation", "start"}}).value("ok").toBool(),
            "External graph started without explicit endpoint configuration");
    const QJsonObject valid{{"operation", "remote"}, {"remoteOperation", "configure"},
                            {"address", "127.0.0.1"}, {"port", 7358}, {"token", Token}};
    for (const auto &field : {"address", "port", "token"}) {
        auto request = valid;
        request.remove(field);
        require(!f.call(request).value("ok").toBool(), "Incomplete endpoint was accepted");
    }
    const std::vector<QPair<QString, QJsonValue>> bad{
        {"address", "localhost"}, {"address", "0.0.0.0"}, {"address", "::"},
        {"address", "239.1.2.3"}, {"address", "255.255.255.255"},
        {"port", 0}, {"port", 65536}, {"port", 7358.5}, {"port", "7358"},
        {"token", "short"}, {"token", QString(129, 'x')}, {"token", Token + " "}, {"token", Token + "\n"}};
    for (const auto &entry : bad) {
        auto request = valid;
        request.insert(entry.first, entry.second);
        require(!f.call(request).value("ok").toBool(), "Invalid endpoint was accepted");
    }
    f.ok(valid);
    const auto before = f.graph.status().value("remote");
    auto invalid = valid;
    invalid.insert("token", "bad");
    require(!f.call(invalid).value("ok").toBool() && f.graph.status().value("remote") == before,
            "Rejected endpoint mutation changed the active endpoint");
    noSecrets(f.graph.definitionSnapshot());
    f.ok({{"operation", "blocks"}});
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create graph-save test directory");
    f.ok({{"operation", "save"}, {"path", directory.filePath("graph.json")}});
    QFile saved(directory.filePath("graph.json"));
    require(saved.open(QIODevice::ReadOnly) && !saved.readAll().contains(Token.toUtf8()),
            "Saved graph exposed the in-memory session token");
    f.ok({{"operation", "remote"}, {"remoteOperation", "disable"}});
    const auto disabled = f.graph.status().value("remote").toObject();
    require(!disabled.value("enabled").toBool() && !disabled.value("tokenConfigured").toBool(),
            "Disabling the endpoint retained its credential");
    f.ok({{"operation", "set"}, {"id", "a-remote"}, {"field", "externalProcessing"}, {"value", false}});
    f.ok({{"operation", "start"}});
    require(f.drive([&] { return f.pcm("z-local") > 0; }), "Local-only graph did not deliver PCM");
    require(!f.graph.status().value("remoteExecution").toObject().value("workerStarted").toBool(),
            "Local-only graph unnecessarily started a remote worker");
    require(!f.call({{"operation", "set"}, {"id", "a-remote"},
                     {"field", "externalProcessing"}, {"value", true}}).value("ok").toBool(),
            "Live route mutation bypassed the stop requirement");
    f.graph.stop();
    for (const auto &type : {QStringLiteral("network_input"), QStringLiteral("playback")}) {
        f.load(graphDefinition(false, false, false, type));
        require(!f.call({{"operation", "start"}}).value("ok").toBool(),
                "Unsupported raw graph source was presented as executable");
    }
    f.load({{"blocks", QJsonArray{QJsonObject{{"id", "source"}, {"type", "iq_source"}}}},
            {"connections", QJsonArray{}}});
    require(!f.call({{"operation", "start"}}).value("ok").toBool(),
            "Descriptive graph was presented as executable DSP");
}

void checkTrace(Fixture &f, const QString &id) {
    const auto trace = f.ok({{"operation", "trace"}, {"id", id}});
    const auto levels = trace.value("levelsDbfs").toArray();
    require(levels.size() == 512 && trace.value("sampleRateHz").toDouble() == ChannelRate,
            "Remote FFT did not use the selected channel sample rate");
    double peak = -1000;
    for (const auto &level : levels) {
        require(level.isDouble() && std::isfinite(level.toDouble()), "FFT contains non-finite data");
        peak = std::max(peak, level.toDouble());
    }
    require(peak > -40.0, "Remote FFT did not contain the synthetic signal");
}

void testRealComputeAndFailure() {
    Fixture f;
    const quint16 port = f.compute();
    f.endpoint(port);
    f.load(graphDefinition(true));
    f.ok({{"operation", "start"}});
    require(f.drive([&] { return f.pcm("z-local") > 0 && f.pcm("a-remote") > 0; }),
            "Real localhost compute did not deliver local and external PCM");
    checkTrace(f, "a-remote-fft");
    require(f.graph.status().value("compute").toObject().value("status").toString().startsWith("Blocks "),
            "External results were not processed by the shared compute service");

    QTcpSocket localAudio;
    localAudio.connectToHost(QHostAddress::LocalHost, quint16(f.branch("z-local").value("pcmPort").toInt()));
    require(localAudio.waitForConnected(500), "Cannot connect to local branch PCM output");
    require(f.drive([&] { return localAudio.bytesAvailable() > 0; }), "Local PCM socket received no data");
    localAudio.readAll();
    f.endpoint(port, WrongToken);
    const double before = f.pcm("z-local");
    require(f.drive([&] {
        return f.pcm("z-local") > before + 2 &&
               f.branch("a-remote").value("error").toString().contains("token");
    }), "Authentication failure blocked local audio or failed to surface the error");
    require(localAudio.state() == QAbstractSocket::ConnectedState && localAudio.bytesAvailable() > 0,
            "Remote endpoint change reset the local branch PCM socket");
    f.endpoint(port);
    require(f.drive([&] { return f.pcm("a-remote") > 0 && f.branch("a-remote").value("error").toString().isEmpty(); }),
            "External branch did not recover after correcting its token");
    f.ok({{"operation", "compute"}, {"computeOperation", "stop"}});
    require(waitUntil([&] { return !f.graph.status().value("compute").toObject().value("running").toBool(); }),
            "Compute stop did not finish asynchronously");
    require(f.drive([&] { return !f.branch("a-remote").value("error").toString().isEmpty(); }),
            "Stopped compute server did not fail closed");
    const double localBefore = f.pcm("z-local"), remoteBefore = f.pcm("a-remote");
    require(f.drive([&] { return f.pcm("z-local") > localBefore + 3; }),
            "Unavailable server blocked the local DSP branch");
    require(f.pcm("a-remote") == remoteBefore, "Unavailable server silently fell back to local processing");
    const quint16 restarted = f.compute();
    f.endpoint(restarted);
    require(f.drive([&] { return f.pcm("a-remote") > remoteBefore; }), "External branch did not reconnect");
    f.ok({{"operation", "remote"}, {"remoteOperation", "disable"}});
    require(f.drive([&] { return !f.branch("a-remote").value("error").toString().isEmpty(); }),
            "Disabling an active endpoint did not report a closed external route");
    require(localAudio.state() == QAbstractSocket::ConnectedState, "Remote disable disconnected local PCM");
}

void testNoServerAndStall() {
    QTcpServer reservation;
    require(reservation.listen(QHostAddress::LocalHost, 0), "Cannot reserve a refused localhost port");
    const auto unusedPort = reservation.serverPort();
    reservation.close();
    Fixture f;
    f.endpoint(unusedPort);
    f.load(graphDefinition(true));
    f.ok({{"operation", "start"}});
    // Model the GUI's queue-enabled producer, not CLI snapshot-only publication,
    // which intentionally clears its unused main-audio queue.
    f.queuePrimaryIq = true;
    require(f.drive([&] { return f.pcm("z-local") > 2 && !f.branch("a-remote").value("error").toString().isEmpty(); }),
            "Absent server did not fail safely beside local audio");
    const auto mainQueue = IqBuffer::stats();
    require(mainQueue.queuedBlocks + mainQueue.droppedQueuedBlocks == f.queuedPublications &&
            mainQueue.queuedFloatCount + mainQueue.droppedQueuedFloats == f.queuedPublications * f.iq.size(),
            "Graph consumed the main audio IQ queue");

    QTcpServer silent;
    require(silent.listen(QHostAddress::LocalHost, 0), "Cannot bind stalled-peer fixture");
    f.endpoint(silent.serverPort());
    const double before = f.pcm("z-local");
    require(f.drive([&] { return silent.hasPendingConnections(); }), "External worker did not connect to stalled peer");
    QTcpSocket *peer = silent.nextPendingConnection();
    require(peer && waitUntil([&] { return peer->bytesAvailable() > 0; }), "Stalled peer did not receive handshake");
    for (int i = 0; i < 8; ++i) {
        const auto count = f.pcm("z-local");
        require(f.drive([&] { return f.pcm("z-local") > count; }), "Stalled handshake blocked local PCM");
        const auto stats = f.graph.status().value("remoteExecution").toObject();
        require(stats.value("pendingBlocks").toInt() <= 2 &&
                stats.value("pendingBytes").toDouble() <= 2.0 * DspStreamProtocol::MaxSamples * 2 * sizeof(float),
                "Remote queue exceeded the shared engine bounds");
    }
    require(f.pcm("z-local") > before &&
            f.graph.status().value("remoteExecution").toObject().value("droppedBlocks").toDouble() > 0,
            "Stalled peer did not exercise bounded remote dropping");
    QElapsedTimer stop;
    stop.start();
    f.graph.stop();
    require(stop.elapsed() < 600, "Graph stop waited for the remote handshake timeout");
}

void testAuxiliaryRecording(bool external) {
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create auxiliary recording directory");
    Fixture f(true);
    if (external) f.endpoint(f.compute());
    f.load(graphDefinition(false, true, external));
    f.ok({{"operation", "start"}});
    f.ok({{"operation", "record"}, {"recordOperation", "start"},
          {"id", "b-auxiliary"}, {"directory", directory.path()}});
    const bool recorded = f.drive([&] {
        for (const auto &info : QDir(directory.path()).entryInfoList(QStringList{QStringLiteral("*.cf32")}, QDir::Files)) {
            // Windows directory entries can retain size zero while a writer is open.
            QFile data(info.absoluteFilePath());
            if (data.open(QIODevice::ReadOnly) && data.size() > 0) return true;
        }
        return false;
    });
    if (!recorded) {
        noSecrets(f.graph.status());
        std::cerr << "Auxiliary route " << (external ? "external" : "local") << ": "
                  << QJsonDocument(f.graph.status()).toJson(QJsonDocument::Compact).constData() << '\n';
        std::cerr << "Recording files: " << QDir(directory.path()).entryList(QDir::Files).join(", ").toStdString() << '\n';
    }
    require(recorded, "Auxiliary source produced no recorded IQ");
    f.ok({{"operation", "record"}, {"recordOperation", "stop"}, {"id", "b-auxiliary"}});
    f.graph.stop();
    bool verified = false;
    for (const auto &file : QDir(directory.path()).entryInfoList(QStringList{QStringLiteral("*.json")}, QDir::Files)) {
        QFile sidecar(file.absoluteFilePath());
        require(sidecar.open(QIODevice::ReadOnly), "Cannot read auxiliary recording sidecar");
        const auto metadata = QJsonDocument::fromJson(sidecar.readAll()).object();
        const auto count = metadata.value("sampleCount").toString().toULongLong();
        if (!count) continue;
        QFile data(QDir(directory.path()).filePath(metadata.value("iqFile").toString()));
        require(metadata.value("receiverId") == Auxiliary.receiverId &&
                metadata.value("channelId") == Auxiliary.channelId &&
                metadata.value("sampleRate").toDouble() == ChannelRate &&
                metadata.value("centerHz").toDouble() == Center + Offset &&
                metadata.value("finalized").toBool() && metadata.value("generation").isString() &&
                metadata.value("epoch").isString() && data.size() == qint64(count * 8) &&
                metadata.value("byteCount").toString().toULongLong() == count * 8,
                "Auxiliary recording lost source identity, clock, rate or finalized byte count");
        verified = true;
    }
    require(verified, "No finalized nonempty auxiliary recording was verified");
}

void testComputeBindFailure() {
    QTcpServer occupied;
    require(occupied.listen(QHostAddress::LocalHost, 0), "Cannot reserve compute bind-failure port");
    Fixture f;
    f.ok({{"operation", "compute"}, {"computeOperation", "start"},
          {"address", "127.0.0.1"}, {"port", int(occupied.serverPort())}, {"token", Token}});
    require(waitUntil([&] { return !f.graph.status().value("compute").toObject().value("running").toBool(); }),
            "Compute bind failure did not leave starting state");
    const auto status = f.graph.status().value("compute").toObject();
    require(status.value("port").toInt() == 0 && status.value("status").toString().startsWith("Cannot bind"),
            "Compute bind failure was incorrectly reported as listening");
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto previousLog = qInstallMessageHandler(captureLog);
    setFobosVerboseLoggingEnabled(true);
    int result = 0;
    try {
        testValidationAndSecrets();
        testRealComputeAndFailure();
        testNoServerAndStall();
        testAuxiliaryRecording(false);
        testAuxiliaryRecording(true);
        testComputeBindFailure();
        {
            std::lock_guard<std::mutex> lock(logMutex);
            for (const auto &message : messages)
                require(!message.contains(Token) && !message.contains(WrongToken), "Verbose diagnostics printed a token");
        }
        std::cout << "CLI remote DSP: real localhost FFT/PCM, endpoint validation/redaction, failure/recovery, "
                     "local audio isolation, bounded stall/cancellation and local/remote auxiliary recording passed\n";
    } catch (const std::exception &error) {
        std::cerr << "CLI remote DSP self-test failed: " << error.what() << '\n';
        result = 1;
    }
    qInstallMessageHandler(previousLog);
    return result;
}
