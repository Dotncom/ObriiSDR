#include "clilivetransmit.h"
#include "climicrophonecapture.h"
#include "clijsonoutput.h"
#include "diagnosticlogging.h"
#include "hackrftransmitterbackend.h"
#include "transmitwaveformgenerator.h"
#include "uhdbackend.h"
#include "virtualtransmitterbackend.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHostAddress>
#include <QMap>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QSysInfo>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <functional>
#include <memory>

namespace {
volatile std::sig_atomic_t cancelled = 0;
void cancel(int) { cancelled = 1; }
double value(const QString &text, bool *okay) {
    QString digits = text.trimmed(); double multiplier = 1;
    if (digits.endsWith('k', Qt::CaseInsensitive)) { multiplier = 1e3; digits.chop(1); }
    else if (digits.endsWith('m', Qt::CaseInsensitive)) { multiplier = 1e6; digits.chop(1); }
    else if (digits.endsWith('g', Qt::CaseInsensitive)) { multiplier = 1e9; digits.chop(1); }
    const double result = digits.toDouble(okay) * multiplier;
    *okay &= std::isfinite(result); return result;
}
int command(const QStringList &words, quint16 port) {
    QJsonObject request;
    if (words.size() == 1 && QStringList{"status", "arm", "disarm", "stop"}.contains(words[0]))
        request.insert("action", words[0]);
    else if (words.size() == 2 && (words[0] == "ptt" || words[0] == "key") &&
             (words[1] == "on" || words[1] == "off")) {
        request.insert("action", words[0]); request.insert("enabled", words[1] == "on");
    } else if (words.size() == 2 && words[0] == "gain") {
        bool okay; const double gain = value(words[1], &okay);
        if (!okay || gain < 0 || gain > 16) return 2;
        request.insert("action", "gain"); request.insert("value", gain);
    } else { std::fprintf(stderr, "tx control status|arm|disarm|stop|ptt on|off|key on|off|gain 0..16\n"); return 2; }
    QTcpSocket socket; socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(2000)) return 1;
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    QByteArray reply; QElapsedTimer timer; timer.start();
    while (!reply.contains('\n') && timer.elapsed() < 5000) {
        if (socket.bytesAvailable() || socket.waitForReadyRead(100)) reply += socket.read(65536 - reply.size());
        if (reply.size() >= 65536) return 1;
    }
    const auto object = QJsonDocument::fromJson(reply).object();
    if (object.isEmpty()) return 1;
    std::fwrite(reply.constData(), 1, static_cast<size_t>(reply.size()), stdout); std::fflush(stdout);
    return object.value("ok").toBool() ? 0 : 1;
}
}

int runCliLiveTransmit(QCoreApplication &app) {
    QCommandLineParser parser; parser.addHelpOption();
    parser.setApplicationDescription(QStringLiteral("Interactive microphone/PTT or manual CW; no RF without --rf-confirm"));
    const auto option = [&](const char *name, const char *description, const char *fallback) {
        parser.addOption({QLatin1String(name), QLatin1String(description), QStringLiteral("value"), QLatin1String(fallback)});
    };
    option("tx-control-port", "Loopback-only live TX control port", "17677");
    option("tx-sink", "Explicit file, virtual, hackrf or uhd sink", "");
    option("freq", "RF/virtual center in Hz (k/M/G suffixes accepted)", "");
    option("rate", "PCM and baseband IQ sample rate, 8..192 ksps", "48000");
    option("voice-mode", "am, nfm, wfm, dsb, usb or lsb", "nfm");
    option("deviation", "Peak FM deviation in Hz", "2500");
    option("signal-bandwidth", "Full optional signal bandwidth in Hz", "0");
    option("tx-level", "Complex IQ amplitude, above zero through 0.95", "0.5");
    option("tx-tone", "Manual CW tone in Hz", "700");
    option("mic-device", "Exact microphone name; empty selects system default", "");
    option("mic-gain", "Software microphone gain, 0..16; OS volume is unchanged", "1");
    option("pcm-input", "Explicit regular mono PCM16LE file instead of microphone (same rate)", "");
    option("output", "New gated-burst CF32 output file", "");
    option("virtual-host", "TCP virtual RX; empty selects local IPC", "");
    option("virtual-port", "Virtual RX TCP port", "17661");
    option("virtual-name", "Local virtual IQ channel", "obrii-virtual-iq");
    option("virtual-node-id", "Named publisher for hub routing", "");
    option("tx-watchdog-ms", "Maximum duration of each held PTT/key, 100..60000 ms", "30000");
    option("seconds", "Optional overall wall-clock session duration; zero waits for stop", "0");
    option("tx-device", "RF device index", "0");
    option("device-rate", "Hardware sample rate (HackRF 2..20 MS/s; UHD must match --rate)", "2000000");
    option("tx-vga-db", "HackRF VGA gain, 0..47 dB", "0");
    option("tx-rf-bandwidth", "Hardware RF filter bandwidth, zero is automatic", "0");
    option("tx-channel", "UHD TX channel, 0..7", "0");
    option("tx-gain-db", "UHD TX gain, 0..100 dB (device may allow less)", "0");
    option("tx-antenna", "UHD antenna", "");
    option("tx-fpga-image", "Explicit UHD FPGA image", "");
    parser.addOption({QStringLiteral("rf-confirm"), QStringLiteral("Explicit RF authorization; check antenna/load and legal frequency")});
    parser.addOption({QStringLiteral("verbose"), QStringLiteral("Enable backend diagnostic messages")});
    const auto fail = [](const QString &message) {
        const auto bytes = QJsonDocument(QJsonObject{{"ok", false}, {"error", message}}).toJson(QJsonDocument::Compact) + '\n';
        std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout); return 1;
    };
    if (!parser.parse(app.arguments())) return fail(parser.errorText());
    if (parser.isSet("help")) { std::fputs(parser.helpText().toUtf8().constData(), stdout); return 0; }
    const auto words = parser.positionalArguments();
    bool okay;
    const int port = parser.value("tx-control-port").toInt(&okay);
    if (!okay || port < 1 || port > 65535) return fail("Invalid --tx-control-port");
    if (words.size() >= 3 && words[0] == "tx" && words[1] == "control") return command(words.mid(2), static_cast<quint16>(port));
    if (words.size() == 2 && words[0] == "tx" && words[1] == "microphones") {
        const auto bytes = QJsonDocument(QJsonObject{{"ok", true}, {"devices", CliMicrophoneCapture::devices()}}).toJson(QJsonDocument::Compact) + '\n';
        std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout); return 0;
    }
    if (words.size() != 2 || words[0] != "tx" || (words[1] != "microphone" && words[1] != "manual-cw"))
        return fail("Use tx microphone|manual-cw|microphones|control ...");
    const bool cw = words[1] == "manual-cw";
    const QString sink = parser.value("tx-sink");
    const bool rf = sink == "hackrf" || sink == "uhd";
    if (sink != "file" && sink != "virtual" && !rf) return fail("Explicit --tx-sink file|virtual|hackrf|uhd required");
    if (rf != parser.isSet("rf-confirm")) return fail("RF sinks require --rf-confirm; non-RF sinks must not use it");
    TxConfiguration configuration;
    configuration.liveSource = true; configuration.prebufferMilliseconds = 20;
    configuration.frequencyHz = value(parser.value("freq"), &okay);
    if (!okay || configuration.frequencyHz <= 0 || configuration.frequencyHz > 1e12 ||
        std::floor(configuration.frequencyHz) != configuration.frequencyHz) return fail("--freq requires positive whole Hz");
    const double rate = value(parser.value("rate"), &okay);
    if (!okay || rate < 8000 || rate > 192000 || std::floor(rate) != rate) return fail("--rate must be whole Hz, 8000..192000");
    configuration.sampleRate = static_cast<int>(rate);
    const QMap<QString, TxModulation> modes{{"am", TxModulation::Am}, {"nfm", TxModulation::Nfm},
        {"wfm", TxModulation::Wfm}, {"dsb", TxModulation::Dsb}, {"usb", TxModulation::Usb}, {"lsb", TxModulation::Lsb}};
    if (!cw && !modes.contains(parser.value("voice-mode"))) return fail("Invalid --voice-mode");
    configuration.modulation = cw ? TxModulation::Cw : modes.value(parser.value("voice-mode"));
    configuration.level = static_cast<float>(value(parser.value("tx-level"), &okay));
    if (!okay || configuration.level <= 0 || configuration.level > 0.95f) return fail("Invalid --tx-level");
    configuration.toneHz = value(parser.value("tx-tone"), &okay);
    if (!okay || configuration.toneHz < 0 || configuration.toneHz > rate * 0.4) return fail("Invalid --tx-tone");
    configuration.deviationHz = value(parser.value("deviation"), &okay);
    if (!okay || configuration.deviationHz < 100 || configuration.deviationHz > 100000) return fail("Invalid --deviation");
    if (!cw && (configuration.modulation == TxModulation::Nfm || configuration.modulation == TxModulation::Wfm) &&
        configuration.deviationHz >= rate * 0.45) return fail("FM deviation exceeds baseband Nyquist margin; increase --rate or reduce --deviation");
    configuration.signalBandwidthHz = value(parser.value("signal-bandwidth"), &okay);
    if (!okay || configuration.signalBandwidthHz < 0 || configuration.signalBandwidthHz > rate * 0.9) return fail("Invalid --signal-bandwidth");
    if (!cw && configuration.signalBandwidthHz > 0 && (configuration.modulation == TxModulation::Nfm || configuration.modulation == TxModulation::Wfm) &&
        configuration.signalBandwidthHz * 0.5 - TransmitWaveformGenerator::effectiveFmDeviationHz(configuration) < 100)
        return fail("Signal bandwidth leaves no room for FM deviation and audio");
    double gain = value(parser.value("mic-gain"), &okay);
    if (!okay || gain < 0 || gain > 16) return fail("--mic-gain must be 0..16");
    const int watchdog = parser.value("tx-watchdog-ms").toInt(&okay);
    if (!okay || watchdog < 100 || watchdog > 60000) return fail("--tx-watchdog-ms must be 100..60000");
    const double sessionSeconds = value(parser.value("seconds"), &okay);
    if (!okay || sessionSeconds < 0 || sessionSeconds > 86400) return fail("--seconds must be 0..86400");
    configuration.deviceIndex = parser.value("tx-device").toInt(&okay);
    if (!okay || configuration.deviceIndex < 0 || configuration.deviceIndex > 64) return fail("Invalid TX device index");
    const double deviceRate = value(parser.value("device-rate"), &okay);
    if (!okay || deviceRate < 8000 || deviceRate > 61440000 || std::floor(deviceRate) != deviceRate) return fail("Invalid hardware sample rate");
    configuration.deviceSampleRate = static_cast<int>(deviceRate);
    if (sink == "hackrf" && (deviceRate < 2000000 || deviceRate > 20000000 || configuration.frequencyHz < 1000000 || configuration.frequencyHz > 6000000000.0))
        return fail("HackRF requires 2..20 MS/s and 1 MHz..6 GHz");
    if (sink == "uhd" && deviceRate != rate) return fail("UHD live TX requires --device-rate equal to --rate; no implicit resampling");
    configuration.txVgaGainDb = parser.value("tx-vga-db").toInt(&okay);
    if (!okay || configuration.txVgaGainDb < 0 || configuration.txVgaGainDb > 47) return fail("Invalid HackRF VGA gain");
    const int channel = parser.value("tx-channel").toInt(&okay);
    if (!okay || channel < 0 || channel > 7) return fail("Invalid UHD channel");
    const double hardwareGain = value(parser.value("tx-gain-db"), &okay);
    if (!okay || hardwareGain < 0 || hardwareGain > 100) return fail("Invalid UHD gain");
    const double bandwidth = value(parser.value("tx-rf-bandwidth"), &okay);
    if (!okay || bandwidth < 0 || bandwidth > deviceRate || std::floor(bandwidth) != bandwidth) return fail("Invalid RF filter bandwidth");
    configuration.bandwidthHz = static_cast<quint32>(bandwidth);
    VirtualSignalEndpoint endpoint;
    endpoint.localName = parser.value("virtual-name"); endpoint.host = parser.value("virtual-host");
    endpoint.mode = endpoint.host.isEmpty() ? VirtualSignalTransportMode::LocalIpc : VirtualSignalTransportMode::Tcp;
    const int virtualPort = parser.value("virtual-port").toInt(&okay);
    if (!okay || virtualPort < 1 || virtualPort > 65535) return fail("Invalid virtual port");
    endpoint.port = static_cast<quint16>(virtualPort);
    const QString nodeId = parser.value("virtual-node-id");
    if (!nodeId.isEmpty() && !virtualSignalValidNodeId(nodeId)) return fail("Invalid virtual node ID");
    if (sink != "virtual" && (parser.isSet("virtual-host") || parser.isSet("virtual-name") || parser.isSet("virtual-node-id")))
        return fail("Virtual options apply only to virtual output");
    if (sink != "file" && parser.isSet("output")) return fail("--output applies only to file output");
    if (cw && (parser.isSet("pcm-input") || parser.isSet("mic-device"))) return fail("Microphone/PCM options do not apply to manual CW");
    QFile pcm(parser.value("pcm-input"));
    if (parser.isSet("pcm-input") && (!QFileInfo(pcm.fileName()).isFile() || !pcm.open(QIODevice::ReadOnly) ||
        pcm.size() < 2 || pcm.size() > 64 * 1024 * 1024 || pcm.size() % 2)) return fail("--pcm-input requires a regular PCM16LE file, 2 bytes..64 MiB");
    QTcpServer control;
    control.setMaxPendingConnections(8);
    if (!control.listen(QHostAddress::LocalHost, static_cast<quint16>(port))) return fail("Cannot bind live TX control port");
    QFile capture(parser.value("output")), metadata(parser.value("output") + QStringLiteral(".json"));
    if (sink == "file") {
        if (QSysInfo::ByteOrder != QSysInfo::LittleEndian) return fail("CF32LE capture requires a little-endian host");
        if (capture.fileName().isEmpty() || QFileInfo::exists(metadata.fileName()) || !capture.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return fail("File output requires a new --output path and absent JSON sidecar");
        if (!metadata.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { capture.close(); capture.remove(); return fail("Cannot create new IQ metadata sidecar"); }
    }
    setFobosVerboseLoggingEnabled(parser.isSet("verbose"));
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &message) {
        if (!fobosVerboseLoggingEnabled() && type != QtCriticalMsg && type != QtFatalMsg) return;
        std::fprintf(stderr, "%s\n", message.toLocal8Bit().constData());
    });
    CliJsonOutput output(QStringLiteral("tx"));
    CliMicrophoneCapture microphone;
    std::unique_ptr<TransmitterBackend> backend;
    std::unique_ptr<UhdTxSession> uhd;
    bool armed = true, ptt = false, firstUhd = true, quitting = false;
    int result = 0, clients = 0;
    quint64 pcmFrames = 0, iqSamples = 0, virtualDropped = 0, clipped = 0, segmentStart = 0, activation = 0;
    QByteArray pending;
    QJsonArray segments;
    QString errorText, segmentUtc, releaseReason;
    float previousInput = 0, previousOutput = 0, peak = 0;
    double squares = 0, cwPhase = 0;
    TxModulatorState modulator;
    TxBandwidthLimiterState limiter;
    QElapsedTimer session, held, lastPcm, cwClock;
    session.start();
    qint64 cwDueMs = 0;
    const auto status = [&] {
        return QJsonObject{{"ok", true}, {"state", ptt ? "transmitting" : armed ? "armed" : "disarmed"},
            {"ptt", ptt}, {"armed", armed}, {"source", cw ? "manual-cw" : pcm.isOpen() ? "pcm-file" : "microphone"},
            {"sink", sink}, {"rfCapable", rf}, {"sampleRate", configuration.sampleRate}, {"controlPort", port},
            {"pcmFrames", static_cast<double>(pcmFrames)}, {"iqSamples", static_cast<double>(iqSamples)},
            {"virtualDroppedSamples", static_cast<double>(virtualDropped)}, {"queuedPcmBytes", pending.size()},
            {"captureRunning", microphone.running()}, {"micGain", gain}, {"clippedPcmSamples", static_cast<double>(clipped)},
            {"micRmsDbfs", pcmFrames ? 10.0 * std::log10((std::max)(1e-12, squares / pcmFrames)) : -120.0},
            {"micPeakDbfs", 20.0 * std::log10((std::max)(1e-6f, peak))}, {"watchdogMs", watchdog},
            {"connected", backend && backend->id() == "virtual-signal" ? static_cast<VirtualTransmitterBackend *>(backend.get())->isConnected() : ptt},
            {"lastReleaseReason", releaseReason},
            {"lastError", errorText}};
    };
    const auto writeMetadata = [&](bool complete) {
        if (!metadata.isOpen()) return true;
        const QJsonObject object{{"format", "cf32le"}, {"input", "rf"}, {"sampleRate", configuration.sampleRate},
            {"centerFrequency", configuration.frequencyHz}, {"captureType", "gated-bursts"}, {"complete", complete},
            {"segments", segments}, {"samples", static_cast<double>(iqSamples)},
            {"note", "Only keyed bursts are concatenated; wall-clock gaps are represented by segment UTC, not IQ samples"}};
        const QByteArray bytes = QJsonDocument(object).toJson();
        return metadata.seek(0) && metadata.resize(0) && metadata.write(bytes) == bytes.size() && metadata.flush();
    };
    if (!writeMetadata(false)) return fail("Cannot initialize file metadata");
    const auto writeIq = [&](const QVector<std::complex<float>> &iq) {
        if (iq.isEmpty()) return true;
        QString error;
        QVector<std::complex<float>> shaped;
        const auto *data = iq.constData(); int count = iq.size();
        if (configuration.signalBandwidthHz > 0) {
            if (!TransmitWaveformGenerator::limitSignalBandwidth(configuration, data, count, &shaped, &limiter)) return false;
            data = shaped.constData(); count = shaped.size();
        }
        bool okay = true;
        if (capture.isOpen()) {
            const qint64 bytes = static_cast<qint64>(count) * 8;
            okay = capture.pos() + bytes <= 1024LL * 1024 * 1024 &&
                capture.write(reinterpret_cast<const char *>(data), bytes) == bytes;
            if (!okay) error = QStringLiteral("Gated IQ file write failed or exceeded 1 GiB");
        } else if (uhd) { okay = uhd->send(data, count, firstUhd, &error); firstUhd = false; }
        else if (backend) {
            if (sink == "hackrf" && static_cast<HackRfTransmitterBackend *>(backend.get())->bufferedSourceSamples() > configuration.sampleRate / 2) {
                okay = false; error = QStringLiteral("RF queue is more than 500 ms behind");
            } else okay = backend->writeIq(data, count, &error);
            if (sink == "virtual" && !static_cast<VirtualTransmitterBackend *>(backend.get())->isConnected()) virtualDropped += count;
        } else okay = false;
        if (!okay) { errorText = error.isEmpty() ? QStringLiteral("Live IQ output failed") : error; return false; }
        iqSamples += count; return true;
    };
    std::function<void()> release;
    release = [&] {
        if (!ptt) { microphone.stop(); return; }
        ++activation; microphone.stop(); pending.clear();
        QString ignored;
        const bool graceful = errorText.isEmpty() && releaseReason != QStringLiteral("watchdog");
        if (cw && graceful && iqSamples > segmentStart) {
            QVector<std::complex<float>> tail((std::max)(1, configuration.sampleRate / 200));
            const double step = 2 * 3.14159265358979323846 * configuration.toneHz / rate;
            for (int i = 0; i < tail.size(); ++i) {
                tail[i] = std::polar(configuration.level * (1.0f - static_cast<float>(i + 1) / tail.size()), static_cast<float>(cwPhase));
                cwPhase = std::remainder(cwPhase + step, 2 * 3.14159265358979323846);
            }
            writeIq(tail);
        }
        if (backend) {
            if (graceful) backend->drain(200, &ignored);
            backend->setOutputEnabled(false, &ignored);
            if (rf) { backend->stop(); backend.reset(); }
        }
        if (uhd) { uhd->finish(&ignored); uhd->close(); uhd.reset(); }
        ptt = false;
        if (segments.size() < 10000) segments.append(QJsonObject{{"utc", segmentUtc},
            {"startSample", static_cast<double>(segmentStart)}, {"samples", static_cast<double>(iqSamples - segmentStart)},
            {"heldMilliseconds", held.elapsed()}});
        else { errorText = QStringLiteral("PTT segment limit reached"); quitting = true; result = 1; }
        if (!writeMetadata(false)) { errorText = QStringLiteral("Cannot update gated IQ metadata"); quitting = true; result = 1; }
        auto event = status(); event.insert("event", "pttChanged"); output.send(event);
    };
    microphone.onPcm = [&](const QByteArray &bytes) {
        if (!ptt) return;
        if (pending.size() + bytes.size() > configuration.sampleRate / 2) {
            errorText = QStringLiteral("Microphone output fell over 250 ms behind"); release(); return;
        }
        pending += bytes; lastPcm.restart();
    };
    microphone.onError = [&](const QString &error) {
        errorText = error;
        const auto failedActivation = activation;
        QTimer::singleShot(0, &app, [&, failedActivation] { if (activation == failedActivation) release(); });
    };
    const auto activate = [&] {
        if (ptt) return true;
        if (!armed) { errorText = QStringLiteral("Arm the live session before pressing PTT"); return false; }
        ++activation; errorText.clear(); releaseReason.clear(); pending.clear(); modulator = {}; limiter.reset(); previousInput = previousOutput = 0;
        if (!cw) {
            if (pcm.isOpen()) { if (!pcm.seek(0)) { errorText = pcm.errorString(); return false; } }
            else if (!microphone.start(parser.value("mic-device"), configuration.sampleRate, &errorText)) return false;
        }
        if (!capture.isOpen()) {
            bool opened = true;
            if (sink == "uhd") {
                uhd.reset(new UhdTxSession);
                opened = uhd->open(configuration.deviceIndex, channel, rate, configuration.frequencyHz,
                    hardwareGain, bandwidth, parser.value("tx-antenna"), parser.value("tx-fpga-image"), &errorText);
                firstUhd = true;
            } else if (!backend) {
                if (sink == "virtual") {
                    auto output = std::make_unique<VirtualTransmitterBackend>();
                    output->setEndpoint(endpoint); output->setNodeId(nodeId); backend = std::move(output);
                } else backend.reset(new HackRfTransmitterBackend);
                opened = backend->start(configuration, &errorText);
            }
            if (opened && backend) opened = backend->setOutputEnabled(true, &errorText);
            if (!opened) { microphone.stop(); if (backend) backend->stop(); backend.reset(); uhd.reset(); return false; }
        }
        ptt = true; held.start(); lastPcm.start(); cwClock.start(); cwDueMs = 0;
        segmentStart = iqSamples; segmentUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        auto event = status(); event.insert("event", "pttChanged"); output.send(event); return true;
    };
    QObject::connect(&control, &QTcpServer::newConnection, &app, [&] {
        while (auto *client = control.nextPendingConnection()) {
            if (clients >= 16) { client->abort(); client->deleteLater(); continue; }
            ++clients; client->setReadBufferSize(4096);
            QObject::connect(client, &QTcpSocket::disconnected, &app, [&, client] { --clients; client->deleteLater(); });
            const QPointer<QTcpSocket> guard(client);
            QTimer::singleShot(2000, &app, [guard] { if (guard) guard->abort(); });
            QObject::connect(client, &QTcpSocket::readyRead, &app, [&, client] {
                if (!client->canReadLine()) { if (client->bytesAvailable() >= 4096) client->abort(); return; }
                const auto request = QJsonDocument::fromJson(client->readLine(4096)).object();
                const QString action = request.value("action").toString();
                bool okay = true;
                if (action == "status") {}
                else if (action == "arm") armed = true;
                else if (action == "disarm") { release(); armed = false; }
                else if (action == "stop") { release(); quitting = true; }
                else if (action == "ptt" || (action == "key" && cw)) {
                    if (!request.value("enabled").isBool()) okay = false;
                    else if (request.value("enabled").toBool()) okay = activate();
                    else release();
                } else if (action == "gain" && request.value("value").isDouble() &&
                    std::isfinite(request.value("value").toDouble()) && request.value("value").toDouble() >= 0 && request.value("value").toDouble() <= 16)
                    gain = request.value("value").toDouble();
                else okay = false;
                auto reply = status(); reply.insert("ok", okay);
                if (!okay) reply.insert("error", errorText.isEmpty() ? QStringLiteral("Invalid live TX command") : errorText);
                client->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n'); client->disconnectFromHost();
            });
        }
    });
    cancelled = 0; std::signal(SIGINT, cancel); std::signal(SIGTERM, cancel);
    QTimer timer; timer.setTimerType(Qt::PreciseTimer); timer.setInterval(5);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (cancelled || !output.okay() || (sessionSeconds > 0 && session.elapsed() >= sessionSeconds * 1000)) quitting = true;
        if (quitting) { release(); app.quit(); return; }
        if (backend && sink == "virtual") static_cast<VirtualTransmitterBackend *>(backend.get())->pollConnection();
        if (!ptt) return;
        if (held.elapsed() >= watchdog) {
            releaseReason = QStringLiteral("watchdog"); release(); return;
        }
        const int blockSamples = (std::max)(1, configuration.sampleRate / 50);
        if (!cw && pcm.isOpen() && cwClock.elapsed() >= cwDueMs) {
            const auto bytes = pcm.read(blockSamples * 2);
            if (bytes.isEmpty()) { releaseReason = QStringLiteral("source-eof"); release(); return; }
            pending += bytes; lastPcm.restart(); cwDueMs += 20;
        }
        if (!cw && !pcm.isOpen() && (lastPcm.elapsed() >= 2000 || !microphone.running())) {
            errorText = QStringLiteral("Microphone stopped or no PCM arrived for two seconds"); release(); return;
        }
        int iterations = 0;
        while (ptt && iterations++ < 4) {
            QVector<std::complex<float>> iq;
            if (cw) {
                if (cwClock.elapsed() < cwDueMs) break;
                if (cwClock.elapsed() - cwDueMs > 100) { errorText = QStringLiteral("Manual CW source fell behind"); release(); break; }
                iq.resize(blockSamples);
                const double step = 2 * 3.14159265358979323846 * configuration.toneHz / rate;
                for (int i = 0; i < blockSamples; ++i) {
                    const float envelope = static_cast<float>((std::min)(1.0, (iqSamples - segmentStart + i) / (rate * 0.005)));
                    iq[i] = std::polar(configuration.level * envelope, static_cast<float>(cwPhase));
                    cwPhase = std::remainder(cwPhase + step, 2 * 3.14159265358979323846);
                }
                cwDueMs += 20;
            } else {
                if (pending.size() < 2) break;
                const int count = (std::min)(blockSamples, pending.size() / 2);
                QVector<float> audio(count);
                const float alpha = static_cast<float>(std::exp(-2 * 3.14159265358979323846 * 35 / rate));
                for (int i = 0; i < count; ++i) {
                    const float sample = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(pending.constData()) + i * 2) / 32768.0f;
                    const float filtered = sample - previousInput + alpha * previousOutput;
                    previousInput = sample; previousOutput = filtered;
                    squares += static_cast<double>(filtered) * filtered; peak = (std::max)(peak, std::abs(filtered));
                    const double boosted = filtered * gain;
                    if (std::abs(boosted) > 1) ++clipped;
                    audio[i] = static_cast<float>((std::clamp)(boosted, -1.0, 1.0));
                }
                pcmFrames += count; pending.remove(0, count * 2);
                iq = TransmitWaveformGenerator::modulateAudio(configuration, audio, &modulator);
            }
            if (!writeIq(iq)) { release(); break; }
        }
    });
    auto initial = status(); initial.insert("event", "txArmed"); output.send(initial);
    timer.start(); app.exec(); timer.stop(); release();
    microphone.stop(); if (backend) backend->stop(); if (uhd) uhd->close();
    if (!errorText.isEmpty()) result = 1;
    if (!writeMetadata(result == 0)) result = 1;
    capture.close(); metadata.close();
    auto final = status(); final.insert("event", "txStopped"); output.send(final);
    if (!output.finish()) result = 1;
    return result;
}
