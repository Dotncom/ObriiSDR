#include "ax25afsk1200.h"
#include "ax25codec.h"
#include "dmrtransmitencoder.h"
#include "hackrftransmitterbackend.h"
#include "signallabgenerator.h"
#include "transmitmediagenerator.h"
#include "transmitwaveformgenerator.h"
#include "uhdbackend.h"
#include "virtualtransmitterbackend.h"
#include "clilivetransmit.h"
#include "kissvirtualtx.h"

extern "C" {
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/message.h"
}

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QImageReader>
#include <QHostAddress>
#include <QPainter>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <csignal>
#include <atomic>
#include <mutex>

std::mutex &fftwPlannerGlobalMutex() {
    static std::mutex mutex;
    return mutex;
}

namespace {
std::atomic_bool interrupted{false};

void interruptHandler(int) {
    interrupted.store(true, std::memory_order_relaxed);
}

void report(const QJsonObject &message) {
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
    std::fflush(stdout);
}

int fail(const QString &message) {
    report({{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}});
    return 1;
}

double number(const QString &text, bool *okay) {
    const QString value = text.trimmed();
    double multiplier = 1.0;
    QString digits = value;
    if (value.endsWith(QLatin1Char('k'), Qt::CaseInsensitive)) {
        multiplier = 1e3; digits.chop(1);
    } else if (value.endsWith(QLatin1Char('M'), Qt::CaseInsensitive)) {
        multiplier = 1e6; digits.chop(1);
    } else if (value.endsWith(QLatin1Char('G'), Qt::CaseInsensitive)) {
        multiplier = 1e9; digits.chop(1);
    }
    const double parsed = digits.toDouble(okay) * multiplier;
    if (!std::isfinite(parsed)) *okay = false;
    return parsed;
}

int runKissVirtualTx(QCoreApplication &app) {
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("ObriiCli module tx kiss PORT LOCAL_IQ_CHANNEL CENTER_HZ [--seconds N]"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("seconds"), QStringLiteral("Optional session duration"), QStringLiteral("N"), QStringLiteral("0")});
    parser.addPositionalArgument(QStringLiteral("port"), QStringLiteral("Local KISS TCP port"));
    parser.addPositionalArgument(QStringLiteral("channel"), QStringLiteral("Local virtual IQ channel"));
    parser.addPositionalArgument(QStringLiteral("center"), QStringLiteral("Virtual IQ center frequency"));
    parser.process(app.arguments().mid(2));
    const auto args = parser.positionalArguments();
    if (args.size() != 3) return fail(parser.applicationDescription());
    bool portOk = false, centerOk = false, durationOk = false;
    const int port = args[0].toInt(&portOk);
    const QString channel = args[1].trimmed();
    const double center = number(args[2], &centerOk);
    const double duration = parser.value(QStringLiteral("seconds")).toDouble(&durationOk);
    if (!portOk || port < 1 || port > 65535 || channel.isEmpty() || channel.size() > 128 ||
        !centerOk || center < 100e3 || center > 7250e6 || !durationOk || !std::isfinite(duration) ||
        duration < 0 || duration > 86400)
        return fail(QStringLiteral("Invalid port, local IQ channel, center frequency or duration"));

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, static_cast<quint16>(port))) return fail(server.errorString());
    KissVirtualTx tx;
    tx.enable(channel, center);
    int clients = 0;
    quint64 rejected = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &tx, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            if (clients >= 16) {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            ++clients;
            socket->setReadBufferSize(16 * 1024);
            auto consume = [&, socket, decoder = KissStreamDecoder{}]() mutable {
                QStringList errors;
                const auto frames = decoder.push(socket->readAll(), &errors);
                rejected += errors.size();
                for (const auto &frame : frames)
                    if (!tx.enqueue(frame, nullptr)) ++rejected;
            };
            consume();
            QObject::connect(socket, &QTcpSocket::readyRead, &tx, std::move(consume));
            QObject::connect(socket, &QTcpSocket::disconnected, &tx, [&, socket] {
                --clients;
                socket->deleteLater();
            });
        }
    });
    QObject::connect(&tx, &KissVirtualTx::sent, &app, [&](int bytes) {
        report({{"ok", true}, {"event", "sent"}, {"frameBytes", bytes}, {"rejected", double(rejected)}});
    });
    QObject::connect(&tx, &KissVirtualTx::failed, &app, [&](const QString &error) {
        fail(error);
        app.exit(1);
    });
    interrupted = false;
    std::signal(SIGINT, interruptHandler);
    std::signal(SIGTERM, interruptHandler);
    QTimer stop;
    QObject::connect(&stop, &QTimer::timeout, &app, [&] {
        if (interrupted) app.quit();
    });
    stop.start(100);
    if (duration > 0) QTimer::singleShot(qMax(1, qRound(duration * 1000)), &app, &QCoreApplication::quit);
    report({{"ok", true}, {"event", "ready"}, {"address", "127.0.0.1"}, {"port", port},
            {"virtualIqChannel", channel}, {"centerHz", center}, {"rate", 48000}});
    return app.exec();
}

bool loadLabPreset(const QString &path, SignalLabConfiguration *configuration,
                   QJsonObject *metadata, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) {
        *error = QStringLiteral("Cannot read signal-lab JSON preset (maximum 64 KiB)");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("Signal-lab preset must be a JSON object");
        return false;
    }
    const QJsonObject object = document.object();
    const QMap<QString, SignalLabWaveform> waveforms{
        {QStringLiteral("carrier"), SignalLabWaveform::Carrier},
        {QStringLiteral("sine"), SignalLabWaveform::Sine},
        {QStringLiteral("square"), SignalLabWaveform::Square},
        {QStringLiteral("triangle"), SignalLabWaveform::Triangle},
        {QStringLiteral("sawtooth"), SignalLabWaveform::Sawtooth},
        {QStringLiteral("pulse"), SignalLabWaveform::Pulse},
        {QStringLiteral("multi-tone"), SignalLabWaveform::MultiTone},
        {QStringLiteral("chirp"), SignalLabWaveform::LinearChirp},
        {QStringLiteral("noise"), SignalLabWaveform::WhiteNoise},
        {QStringLiteral("bpsk"), SignalLabWaveform::Bpsk},
        {QStringLiteral("qpsk"), SignalLabWaveform::Qpsk},
        {QStringLiteral("8psk"), SignalLabWaveform::Psk8},
        {QStringLiteral("pi4dqpsk"), SignalLabWaveform::Pi4Dqpsk},
        {QStringLiteral("16qam"), SignalLabWaveform::Qam16},
        {QStringLiteral("64qam"), SignalLabWaveform::Qam64},
        {QStringLiteral("256qam"), SignalLabWaveform::Qam256},
        {QStringLiteral("2fsk"), SignalLabWaveform::Fsk2},
        {QStringLiteral("2gfsk"), SignalLabWaveform::Gfsk2},
        {QStringLiteral("ofdm"), SignalLabWaveform::Ofdm},
        {QStringLiteral("sweeping-beacon"), SignalLabWaveform::SweepingBeacon}};
    const QString waveform = object.value(QStringLiteral("waveform")).toString().toLower();
    if (!waveforms.contains(waveform)) {
        *error = QStringLiteral("Unknown signal-lab waveform");
        return false;
    }
    configuration->waveform = waveforms.value(waveform);
    const QMap<QString, double SignalLabConfiguration::*> doubles{
        {QStringLiteral("durationSeconds"), &SignalLabConfiguration::durationSeconds},
        {QStringLiteral("amplitude"), &SignalLabConfiguration::amplitude},
        {QStringLiteral("centerOffsetHz"), &SignalLabConfiguration::centerOffsetHz},
        {QStringLiteral("toneFrequencyHz"), &SignalLabConfiguration::toneFrequencyHz},
        {QStringLiteral("secondFrequencyHz"), &SignalLabConfiguration::secondFrequencyHz},
        {QStringLiteral("sweepBandwidthHz"), &SignalLabConfiguration::sweepBandwidthHz},
        {QStringLiteral("sweepPeriodSeconds"), &SignalLabConfiguration::sweepPeriodSeconds},
        {QStringLiteral("dutyCycle"), &SignalLabConfiguration::dutyCycle},
        {QStringLiteral("symbolRate"), &SignalLabConfiguration::symbolRate},
        {QStringLiteral("rolloff"), &SignalLabConfiguration::rolloff},
        {QStringLiteral("fskDeviationHz"), &SignalLabConfiguration::fskDeviationHz},
        {QStringLiteral("ofdmEdgeWindowPercent"), &SignalLabConfiguration::ofdmEdgeWindowPercent}};
    const QMap<QString, int SignalLabConfiguration::*> integers{
        {QStringLiteral("sampleRate"), &SignalLabConfiguration::sampleRate},
        {QStringLiteral("ofdmFftSize"), &SignalLabConfiguration::ofdmFftSize},
        {QStringLiteral("ofdmActiveCarriers"), &SignalLabConfiguration::ofdmActiveCarriers},
        {QStringLiteral("ofdmCyclicPrefix"), &SignalLabConfiguration::ofdmCyclicPrefix},
        {QStringLiteral("ofdmSymbolCount"), &SignalLabConfiguration::ofdmSymbolCount},
        {QStringLiteral("ofdmPilotSpacing"), &SignalLabConfiguration::ofdmPilotSpacing}};
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key() == QStringLiteral("waveform")) continue;
        if (doubles.contains(it.key()) && it.value().isDouble() && std::isfinite(it.value().toDouble()))
            configuration->*doubles.value(it.key()) = it.value().toDouble();
        else if (integers.contains(it.key()) && it.value().isDouble() &&
                 it.value().toDouble() >= 0 && it.value().toDouble() <= 1000000000 &&
                 std::floor(it.value().toDouble()) == it.value().toDouble())
            configuration->*integers.value(it.key()) = it.value().toInt();
        else if (it.key() == QStringLiteral("multiToneFrequencies") && it.value().isString())
            configuration->multiToneFrequencies = it.value().toString();
        else if (it.key() == QStringLiteral("ofdmNullDc") && it.value().isBool())
            configuration->ofdmNullDc = it.value().toBool();
        else if (it.key() == QStringLiteral("randomSeed") && it.value().isDouble() &&
                 it.value().toDouble() >= 0 && it.value().toDouble() <= 4294967295.0 &&
                 std::floor(it.value().toDouble()) == it.value().toDouble())
            configuration->randomSeed = static_cast<quint32>(it.value().toDouble());
        else if (it.key() == QStringLiteral("sweepMode") && it.value().isString()) {
            const QString value = it.value().toString();
            if (value == QStringLiteral("ping-pong")) configuration->sweepMode = SignalLabSweepMode::PingPong;
            else if (value == QStringLiteral("restart-forward")) configuration->sweepMode = SignalLabSweepMode::RestartForward;
            else if (value == QStringLiteral("restart-reverse")) configuration->sweepMode = SignalLabSweepMode::RestartReverse;
            else { *error = QStringLiteral("Invalid sweepMode"); return false; }
        } else if (it.key() == QStringLiteral("ofdmConstellation") && it.value().isString()) {
            const QString value = it.value().toString().toLower();
            const QMap<QString, SignalLabConstellation> constellations{
                {QStringLiteral("bpsk"), SignalLabConstellation::Bpsk},
                {QStringLiteral("qpsk"), SignalLabConstellation::Qpsk},
                {QStringLiteral("8psk"), SignalLabConstellation::Psk8},
                {QStringLiteral("16qam"), SignalLabConstellation::Qam16},
                {QStringLiteral("64qam"), SignalLabConstellation::Qam64},
                {QStringLiteral("256qam"), SignalLabConstellation::Qam256}};
            if (!constellations.contains(value)) { *error = QStringLiteral("Invalid ofdmConstellation"); return false; }
            configuration->ofdmConstellation = constellations.value(value);
        } else {
            *error = QStringLiteral("Unknown or invalid signal-lab field: %1").arg(it.key());
            return false;
        }
    }
    *metadata = object;
    return true;
}

TxGenerationResult generateFt4(const TxConfiguration &configuration, const QString &text) {
    TxGenerationResult result;
    const QByteArray messageBytes = text.trimmed().toUpper().toLatin1();
    if (messageBytes.isEmpty()) {
        result.error = QStringLiteral("Enter an FT4 message");
        return result;
    }
    ftx_message_t message;
    ftx_message_init(&message);
    const ftx_message_rc_t code = ftx_message_encode(&message, nullptr, messageBytes.constData());
    if (code != FTX_MESSAGE_RC_OK) {
        result.error = QStringLiteral("FT4 message cannot be encoded (code %1)").arg(static_cast<int>(code));
        return result;
    }
    std::array<std::uint8_t, FT4_NN> tones{};
    ft4_encode(message.payload, tones.data());
    const int samplesPerSymbol = (std::max)(1, qRound(FT4_SYMBOL_PERIOD * configuration.sampleRate));
    const int samples = FT4_NN * samplesPerSymbol;
    QVector<float> pulse(3 * samplesPerSymbol);
    for (int i = 0; i < pulse.size(); ++i) {
        const float time = static_cast<float>(i - samplesPerSymbol) / samplesPerSymbol;
        const float a = 5.336446f * 2.0f * (time + 0.5f);
        const float b = 5.336446f * 2.0f * (time - 0.5f);
        pulse[i] = 0.5f * (std::erf(a) - std::erf(b));
    }
    QVector<double> phaseSteps(samples + 2 * samplesPerSymbol, 0.0);
    constexpr double Pi = 3.14159265358979323846;
    const double peakStep = 2.0 * Pi / (FT4_SYMBOL_PERIOD * configuration.sampleRate);
    for (int symbol = 0; symbol < FT4_NN; ++symbol)
        for (int j = 0; j < pulse.size(); ++j)
            phaseSteps[symbol * samplesPerSymbol + j] += peakStep * tones[symbol] * pulse[j];
    for (int j = 0; j < 2 * samplesPerSymbol; ++j) {
        phaseSteps[j] += peakStep * tones.front() * pulse[j + samplesPerSymbol];
        phaseSteps[j + samples] += peakStep * tones.back() * pulse[j];
    }
    result.iq.resize(samples);
    double phase = 0.0;
    const double baseStep = 2.0 * Pi * configuration.toneHz / configuration.sampleRate;
    for (int i = 0; i < samples; ++i) {
        phase += baseStep + phaseSteps[i + samplesPerSymbol];
        float envelope = configuration.level;
        if (i < samplesPerSymbol)
            envelope *= 0.5f * (1.0f - std::cos(Pi * i / samplesPerSymbol));
        else if (i >= samples - samplesPerSymbol)
            envelope *= 0.5f * (1.0f - std::cos(Pi * (samples - 1 - i) / samplesPerSymbol));
        result.iq[i] = std::polar(envelope, static_cast<float>(phase));
        if (std::abs(phase) > 2.0 * Pi) phase = std::fmod(phase, 2.0 * Pi);
    }
    result.durationSeconds = static_cast<double>(samples) / configuration.sampleRate;
    result.summary = QStringLiteral("FT4: 105 symbols, %1 s").arg(result.durationSeconds, 0, 'f', 3);
    return result;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == QStringLiteral("tx") && app.arguments().value(2) == QStringLiteral("kiss"))
        return runKissVirtualTx(app);
    if (app.arguments().size() >= 3 && app.arguments()[1] == QStringLiteral("tx") &&
        QStringList{QStringLiteral("microphone"), QStringLiteral("manual-cw"), QStringLiteral("microphones"), QStringLiteral("control")}.contains(app.arguments()[2]))
        return runCliLiveTransmit(app);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("tx-sink"), QStringLiteral("file, virtual, hackrf or uhd"),
                      QStringLiteral("sink")});
    parser.addOption({QStringLiteral("rf-confirm"),
                      QStringLiteral("Required for intentional RF transmission")});
    parser.addOption({QStringLiteral("tx-device"), QStringLiteral("HackRF/UHD TX device index"),
                      QStringLiteral("index"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("device-rate"), QStringLiteral("RF hardware sample rate"),
                      QStringLiteral("Hz"), QStringLiteral("2M")});
    parser.addOption({QStringLiteral("tx-vga-db"), QStringLiteral("HackRF TX VGA gain (0..47 dB)"),
                      QStringLiteral("dB"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("tx-rf-bandwidth"), QStringLiteral("RF filter bandwidth; 0=driver default"),
                      QStringLiteral("Hz"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("tx-channel"), QStringLiteral("UHD TX channel index"),
                      QStringLiteral("index"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("tx-antenna"), QStringLiteral("UHD TX antenna; empty=driver default"),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("tx-fpga-image"), QStringLiteral("Optional UHD FPGA image path"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("tx-gain-db"), QStringLiteral("UHD TX gain in dB"),
                      QStringLiteral("dB"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("output"), QStringLiteral("New CF32LE IQ file"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("virtual-host"), QStringLiteral("Virtual RX host; omit for local IPC"),
                      QStringLiteral("host")});
    parser.addOption({QStringLiteral("virtual-port"), QStringLiteral("Virtual RX TCP port"),
                      QStringLiteral("port"), QStringLiteral("17661")});
    parser.addOption({QStringLiteral("virtual-name"), QStringLiteral("Local IPC channel name"),
                      QStringLiteral("name"), QStringLiteral("obrii-virtual-iq")});
    parser.addOption({QStringLiteral("virtual-node-id"), QStringLiteral("Stable virtual IQ publisher ID for hub routing"),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("freq"), QStringLiteral("RF center frequency"),
                      QStringLiteral("Hz")});
    parser.addOption({QStringLiteral("rate"), QStringLiteral("Generated IQ sample rate"),
                      QStringLiteral("Hz"), QStringLiteral("48000")});
    parser.addOption({QStringLiteral("tx-tone"), QStringLiteral("Audio/baseband tone offset"),
                      QStringLiteral("Hz")});
    parser.addOption({QStringLiteral("tx-level"), QStringLiteral("IQ amplitude, 0..0.95"),
                      QStringLiteral("level"), QStringLiteral("0.5")});
    parser.addOption({QStringLiteral("cw-wpm"), QStringLiteral("CW speed, 5..60"),
                      QStringLiteral("wpm"), QStringLiteral("18")});
    parser.addOption({QStringLiteral("pcm-file"), QStringLiteral("DMR input: raw mono PCM16LE at --pcm-rate"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("pcm-rate"), QStringLiteral("DMR PCM input sample rate"),
                      QStringLiteral("Hz"), QStringLiteral("48000")});
    parser.addOption({QStringLiteral("lab-preset"), QStringLiteral("Signal-lab JSON preset for lab TX"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("wav-file"), QStringLiteral("Audio TX input: RIFF/WAVE file"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("image-file"), QStringLiteral("SSTV TX input image"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("sstv-mode"), QStringLiteral("robot36, robot72, martin-m1 or martin-m2"),
                      QStringLiteral("mode"), QStringLiteral("robot36")});
    parser.addOption({QStringLiteral("voice-mode"), QStringLiteral("am, nfm, wfm, dsb, usb or lsb"),
                      QStringLiteral("mode"), QStringLiteral("nfm")});
    parser.addOption({QStringLiteral("deviation"), QStringLiteral("FM peak deviation in Hz"),
                      QStringLiteral("Hz"), QStringLiteral("2500")});
    parser.addOption({QStringLiteral("signal-bandwidth"), QStringLiteral("Optional full TX signal bandwidth in Hz"),
                      QStringLiteral("Hz"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("dmr-cc"), QStringLiteral("DMR color code, 0..15"),
                      QStringLiteral("code"), QStringLiteral("1")});
    parser.addOption({QStringLiteral("dmr-slot"), QStringLiteral("DMR time slot, 1 or 2"),
                      QStringLiteral("slot"), QStringLiteral("1")});
    parser.addOption({QStringLiteral("dmr-source"), QStringLiteral("DMR source radio ID"),
                      QStringLiteral("id"), QStringLiteral("1")});
    parser.addOption({QStringLiteral("dmr-target"), QStringLiteral("DMR destination radio ID"),
                      QStringLiteral("id"), QStringLiteral("9")});
    parser.addOption({QStringLiteral("dmr-private"), QStringLiteral("DMR private rather than group call")});
    parser.addOption({QStringLiteral("no-ft8-align"), QStringLiteral("Do not wait for the next UTC FT8 slot")});
    parser.addOption({QStringLiteral("no-slot-align"), QStringLiteral("Do not wait for the next FT8/FT4 UTC slot")});
    parser.addOption({QStringLiteral("ax25-source"), QStringLiteral("AX.25 source callsign-SSID"),
                      QStringLiteral("call")});
    parser.addOption({QStringLiteral("ax25-destination"), QStringLiteral("AX.25 destination callsign-SSID"),
                      QStringLiteral("call"), QStringLiteral("APRS")});
    parser.addOption({QStringLiteral("ax25-path"), QStringLiteral("Comma-separated digipeater path"),
                      QStringLiteral("calls")});
    if (!parser.parse(app.arguments())) return fail(parser.errorText());
    if (parser.isSet(QStringLiteral("help"))) {
        std::fputs(parser.helpText().toUtf8().constData(), stdout);
        return 0;
    }
    const QStringList args = parser.positionalArguments();
    if ((args.size() != 3 && args.size() != 2) || args[0] != QStringLiteral("tx"))
        return fail(QStringLiteral("Usage: module tx ft8|ft4|cw|rtty MESSAGE, tx ax25 APRS_INFO --ax25-source CALL, tx dmr --pcm-file PATH, tx lab --lab-preset JSON, tx audio --wav-file WAV, or tx sstv --image-file IMAGE"));
    TxConfiguration configuration;
    const QString mode = args[1].toLower();
    if (mode == QStringLiteral("ft8")) configuration.modulation = TxModulation::Ft8;
    else if (mode == QStringLiteral("ft4")) configuration.modulation = TxModulation::Ft8;
    else if (mode == QStringLiteral("cw")) configuration.modulation = TxModulation::Cw;
    else if (mode == QStringLiteral("rtty")) {
        configuration.modulation = TxModulation::Rtty;
        configuration.symbolRate = 45.45;
        configuration.deviationHz = 85.0;
    }
    else if (mode == QStringLiteral("dmr")) configuration.modulation = TxModulation::Dmr;
    else if (mode == QStringLiteral("ax25")) configuration.modulation = TxModulation::Nfm;
    else if (mode == QStringLiteral("audio") || mode == QStringLiteral("sstv")) {
        const QString voice = parser.value(QStringLiteral("voice-mode")).toLower();
        const QMap<QString, TxModulation> voices{
            {QStringLiteral("am"), TxModulation::Am}, {QStringLiteral("nfm"), TxModulation::Nfm},
            {QStringLiteral("wfm"), TxModulation::Wfm}, {QStringLiteral("dsb"), TxModulation::Dsb},
            {QStringLiteral("usb"), TxModulation::Usb}, {QStringLiteral("lsb"), TxModulation::Lsb}};
        if (!voices.contains(voice)) return fail(QStringLiteral("--voice-mode must be am, nfm, wfm, dsb, usb or lsb"));
        configuration.modulation = voices.value(voice);
    } else if (mode != QStringLiteral("lab")) return fail(QStringLiteral("TX module supports ft8, ft4, cw, rtty, ax25, dmr, lab, audio and sstv"));
    const bool mediaMode = mode == QStringLiteral("audio") || mode == QStringLiteral("sstv");
    if ((mode == QStringLiteral("dmr") || mode == QStringLiteral("lab") || mediaMode) != (args.size() == 2))
        return fail(QStringLiteral("DMR, lab, audio and SSTV use file options without MESSAGE; text modes require MESSAGE"));
    if (mode != QStringLiteral("dmr") && parser.isSet(QStringLiteral("pcm-file")))
        return fail(QStringLiteral("--pcm-file applies to DMR only"));
    if (mode != QStringLiteral("lab") && parser.isSet(QStringLiteral("lab-preset")))
        return fail(QStringLiteral("--lab-preset applies to lab only"));
    if (mode != QStringLiteral("audio") && parser.isSet(QStringLiteral("wav-file")))
        return fail(QStringLiteral("--wav-file applies to audio TX only"));
    if (mode != QStringLiteral("sstv") && (parser.isSet(QStringLiteral("image-file")) ||
                                             parser.isSet(QStringLiteral("sstv-mode"))))
        return fail(QStringLiteral("--image-file and --sstv-mode apply to SSTV only"));
    if (!mediaMode && (parser.isSet(QStringLiteral("voice-mode")) ||
                       parser.isSet(QStringLiteral("deviation")) ||
                       parser.isSet(QStringLiteral("signal-bandwidth"))))
        return fail(QStringLiteral("Voice modulation, deviation and signal bandwidth apply to audio/SSTV only"));
    if (mode != QStringLiteral("ax25") &&
        (parser.isSet(QStringLiteral("ax25-source")) ||
         parser.isSet(QStringLiteral("ax25-destination")) ||
         parser.isSet(QStringLiteral("ax25-path"))))
        return fail(QStringLiteral("--ax25-* options apply to AX.25 only"));
    const QString sink = parser.value(QStringLiteral("tx-sink")).toLower();
    const bool rfSink = sink == QStringLiteral("hackrf") || sink == QStringLiteral("uhd");
    if (sink != QStringLiteral("file") && sink != QStringLiteral("virtual") && !rfSink)
        return fail(QStringLiteral("Explicit --tx-sink file|virtual|hackrf|uhd is required"));
    if (sink != QStringLiteral("virtual") && parser.isSet(QStringLiteral("virtual-node-id")))
        return fail(QStringLiteral("--virtual-node-id applies only to virtual TX"));
    if (rfSink && !parser.isSet(QStringLiteral("rf-confirm")))
        return fail(QStringLiteral("RF output requires --rf-confirm"));
    if (!rfSink && parser.isSet(QStringLiteral("rf-confirm")))
        return fail(QStringLiteral("--rf-confirm applies only to RF sinks"));
    if (sink != QStringLiteral("uhd") &&
        (parser.isSet(QStringLiteral("tx-channel")) ||
         parser.isSet(QStringLiteral("tx-antenna")) ||
         parser.isSet(QStringLiteral("tx-fpga-image")) ||
         parser.isSet(QStringLiteral("tx-gain-db"))))
        return fail(QStringLiteral("--tx-channel, --tx-antenna, --tx-fpga-image and --tx-gain-db apply only to UHD"));
    bool okay = false;
    configuration.frequencyHz = number(parser.value(QStringLiteral("freq")), &okay);
    if (!okay || configuration.frequencyHz <= 0.0)
        return fail(QStringLiteral("--freq must be a positive RF center frequency"));
    const double rate = number(mode == QStringLiteral("dmr") && !parser.isSet(QStringLiteral("rate"))
        ? QStringLiteral("192000") : parser.value(QStringLiteral("rate")), &okay);
    const double maxRate = mode == QStringLiteral("lab") ? 1e9 :
        mediaMode ? 200000.0 : sink == QStringLiteral("uhd") ? 61440000.0 : 192000.0;
    if (!okay || rate < 8000.0 || rate > maxRate ||
        std::floor(rate) != rate)
        return fail(QStringLiteral("Invalid TX sample rate"));
    if (mediaMode && rate > 200000.0)
        return fail(QStringLiteral("Audio/SSTV generator rate is limited to 200 ksps"));
    configuration.sampleRate = static_cast<int>(rate);
    if (mode == QStringLiteral("ax25") &&
        (configuration.sampleRate < 12000 || configuration.sampleRate % 1200))
        return fail(QStringLiteral("AX.25 AFSK rate must be 12..192 ksps and divisible by 1200"));
    configuration.toneHz = number(parser.isSet(QStringLiteral("tx-tone"))
        ? parser.value(QStringLiteral("tx-tone"))
        : mode == QStringLiteral("rtty") ? QStringLiteral("2210") : QStringLiteral("1000"),
        &okay);
    if (!okay || configuration.toneHz < 0.0 || configuration.toneHz > rate * 0.4)
        return fail(QStringLiteral("--tx-tone must lie inside the baseband Nyquist range"));
    configuration.level = parser.value(QStringLiteral("tx-level")).toFloat(&okay);
    if (!okay || !std::isfinite(configuration.level) ||
        configuration.level <= 0.0f || configuration.level > 0.95f)
        return fail(QStringLiteral("--tx-level must be above 0 and at most 0.95"));
    if (mediaMode) {
        configuration.deviationHz = number(parser.value(QStringLiteral("deviation")), &okay);
        if (!okay || configuration.deviationHz < 100.0 || configuration.deviationHz > 100000.0)
            return fail(QStringLiteral("--deviation must be 100..100000 Hz"));
        configuration.signalBandwidthHz = number(parser.value(QStringLiteral("signal-bandwidth")), &okay);
        if (!okay || configuration.signalBandwidthHz < 0.0 ||
            configuration.signalBandwidthHz > configuration.sampleRate * 0.90)
            return fail(QStringLiteral("--signal-bandwidth must fit within 90% of --rate"));
        if (configuration.signalBandwidthHz > 0.0 &&
            (configuration.modulation == TxModulation::Nfm || configuration.modulation == TxModulation::Wfm) &&
            configuration.signalBandwidthHz * 0.5 -
                TransmitWaveformGenerator::effectiveFmDeviationHz(configuration) < 100.0)
            return fail(QStringLiteral("FM signal bandwidth must exceed twice the deviation plus 200 Hz"));
    }
    configuration.cwWpm = parser.value(QStringLiteral("cw-wpm")).toInt(&okay);
    if (!okay || configuration.cwWpm < 5 || configuration.cwWpm > 60)
        return fail(QStringLiteral("--cw-wpm must be 5..60"));
    TxGenerationResult generated;
    QJsonObject labMetadata;
    if (mode == QStringLiteral("audio") || mode == QStringLiteral("sstv")) {
        TxAudioMedia media;
        if (mode == QStringLiteral("audio")) {
            const QFileInfo input(parser.value(QStringLiteral("wav-file")));
            if (!input.isFile() || input.size() > 32 * 1024 * 1024)
                return fail(QStringLiteral("--wav-file must be a readable WAVE file of at most 32 MiB"));
            media = TransmitMediaGenerator::loadWav(input.absoluteFilePath(), configuration.sampleRate);
        } else {
            const QString sstvMode = parser.value(QStringLiteral("sstv-mode")).toLower();
            if (!QStringList{QStringLiteral("robot36"), QStringLiteral("robot72"),
                             QStringLiteral("martin-m1"), QStringLiteral("martin-m2")}.contains(sstvMode))
                return fail(QStringLiteral("Unknown SSTV mode"));
            if (configuration.sampleRate < 12000)
                return fail(QStringLiteral("SSTV --rate must be at least 12 ksps"));
            const QFileInfo input(parser.value(QStringLiteral("image-file")));
            if (!input.isFile() || input.size() > 32 * 1024 * 1024)
                return fail(QStringLiteral("--image-file must be a readable image of at most 32 MiB"));
            QImageReader reader(input.absoluteFilePath());
            reader.setAutoTransform(true);
            const QSize size = reader.size();
            if (!size.isValid() || static_cast<qint64>(size.width()) * size.height() > 50000000)
                return fail(QStringLiteral("Image dimensions are invalid or exceed 50 megapixels"));
            const QImage image = reader.read();
            if (image.isNull()) return fail(reader.errorString());
            const int height = sstvMode.startsWith(QStringLiteral("robot")) ? 240 : 256;
            QImage canvas(320, height, QImage::Format_RGB32);
            canvas.fill(Qt::black);
            const QImage fitted = image.scaled(canvas.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPainter painter(&canvas);
            painter.drawImage((canvas.width() - fitted.width()) / 2,
                              (canvas.height() - fitted.height()) / 2, fitted);
            painter.end();
            media = TransmitMediaGenerator::generateSstv(canvas, sstvMode, configuration.sampleRate);
        }
        if (!media.ok()) return fail(media.error);
        const double maximumSeconds = mode == QStringLiteral("audio") ? 60.0 : 180.0;
        if (static_cast<double>(media.samples.size()) / configuration.sampleRate > maximumSeconds)
            return fail(QStringLiteral("Media duration exceeds the CLI TX limit"));
        if (media.samples.size() > 20000000)
            return fail(QStringLiteral("Media would generate more than 20 million IQ samples"));
        generated.iq = TransmitWaveformGenerator::modulateAudio(configuration, media.samples);
        if (configuration.signalBandwidthHz > 0.0) {
            QVector<std::complex<float>> filtered;
            TxBandwidthLimiterState limiter;
            if (!TransmitWaveformGenerator::limitSignalBandwidth(configuration, generated.iq.constData(),
                    generated.iq.size(), &filtered, &limiter))
                return fail(QStringLiteral("Could not apply TX signal bandwidth"));
            generated.iq = std::move(filtered);
        }
        generated.durationSeconds = static_cast<double>(generated.iq.size()) / configuration.sampleRate;
        generated.summary = media.summary;
    } else if (mode == QStringLiteral("dmr")) {
        const double pcmRateValue = number(parser.value(QStringLiteral("pcm-rate")), &okay);
        if (!okay || pcmRateValue < 8000 || pcmRateValue > 192000 ||
            std::floor(pcmRateValue) != pcmRateValue)
            return fail(QStringLiteral("--pcm-rate must be a whole number from 8k to 192k"));
        const int pcmRate = static_cast<int>(pcmRateValue);
        QFile input(parser.value(QStringLiteral("pcm-file")));
        if (input.fileName().isEmpty() || !input.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("--pcm-file must name a readable raw PCM16LE file"));
        const qint64 minBytes = static_cast<qint64>(pcmRate) * 2;
        const qint64 maxBytes = minBytes * 60;
        if (input.size() < minBytes || input.size() > maxBytes || input.size() % 2)
            return fail(QStringLiteral("DMR PCM must contain 1..60 seconds of mono PCM16LE"));
        const QByteArray pcm = input.readAll();
        if (pcm.size() != input.size()) return fail(QStringLiteral("Cannot read DMR PCM input"));
        DmrTxParameters parameters;
        parameters.outputSampleRate = configuration.sampleRate;
        parameters.level = configuration.level;
        parameters.colorCode = parser.value(QStringLiteral("dmr-cc")).toInt(&okay);
        if (!okay) return fail(QStringLiteral("Invalid DMR color code"));
        parameters.timeslot = parser.value(QStringLiteral("dmr-slot")).toInt(&okay);
        if (!okay) return fail(QStringLiteral("Invalid DMR time slot"));
        parameters.sourceId = parser.value(QStringLiteral("dmr-source")).toUInt(&okay);
        if (!okay) return fail(QStringLiteral("Invalid DMR source ID"));
        parameters.destinationId = parser.value(QStringLiteral("dmr-target")).toUInt(&okay);
        if (!okay) return fail(QStringLiteral("Invalid DMR destination ID"));
        parameters.privateCall = parser.isSet(QStringLiteral("dmr-private"));
        DmrTransmitEncoder encoder;
        if (!encoder.configure(parameters, &generated.error)) return fail(generated.error);
        generated.iq = encoder.begin(&generated.error);
        if (!generated.error.isEmpty()) return fail(generated.error);
        constexpr int PcmChunk = 4096;
        const int inputSamples = pcm.size() / 2;
        const int outputSamples = static_cast<int>(
            (static_cast<qint64>(inputSamples) * configuration.sampleRate) / pcmRate);
        const auto *bytes = reinterpret_cast<const uchar *>(pcm.constData());
        const auto readSample = [bytes](int index) {
            return static_cast<qint16>(qFromLittleEndian<quint16>(bytes + index * 2)) / 32768.0f;
        };
        for (int offset = 0; offset < outputSamples; offset += PcmChunk) {
            const int count = (std::min)(PcmChunk, outputSamples - offset);
            QVector<float> samples;
            samples.reserve(count);
            for (int index = 0; index < count; ++index) {
                const double position = static_cast<double>(offset + index) * pcmRate /
                    configuration.sampleRate;
                const int left = static_cast<int>(position);
                const int right = (std::min)(inputSamples - 1, left + 1);
                const float first = readSample(left);
                samples.append(first + static_cast<float>(position - left) *
                    (readSample(right) - first));
            }
            generated.iq += encoder.pushAudio(samples, &generated.error);
            if (!generated.error.isEmpty()) return fail(generated.error);
        }
        generated.iq += encoder.finish(&generated.error);
        generated.durationSeconds = static_cast<double>(generated.iq.size()) / configuration.sampleRate;
    } else if (mode == QStringLiteral("ax25")) {
        if (!parser.isSet(QStringLiteral("ax25-source")))
            return fail(QStringLiteral("AX.25 requires --ax25-source CALLSIGN[-SSID]"));
        Ax25UiFrame frame;
        if (!Ax25Codec::parseAddress(parser.value(QStringLiteral("ax25-source")),
                                     &frame.source, &generated.error) ||
            !Ax25Codec::parseAddress(parser.value(QStringLiteral("ax25-destination")),
                                     &frame.destination, &generated.error)) return fail(generated.error);
        const QString path = parser.value(QStringLiteral("ax25-path"));
        if (!path.isEmpty()) {
            const QStringList parts = path.split(QLatin1Char(','), Qt::KeepEmptyParts);
            if (parts.size() > 8) return fail(QStringLiteral("AX.25 path allows at most eight digipeaters"));
            for (const QString &part : parts) {
                Ax25Address address;
                if (!Ax25Codec::parseAddress(part.trimmed(), &address, &generated.error))
                    return fail(generated.error);
                frame.digipeaters.append(address);
            }
        }
        frame.information = args[2].toUtf8();
        const QByteArray ax25 = Ax25Codec::encodeUiFrame(frame, false, &generated.error);
        if (ax25.isEmpty()) return fail(generated.error);
        const QVector<float> afsk = Ax25Afsk1200::modulate(ax25, configuration.sampleRate,
                                                            32, &generated.error);
        if (afsk.isEmpty()) return fail(generated.error);
        configuration.deviationHz = 3000;
        generated.iq = TransmitWaveformGenerator::modulateAudio(configuration, afsk);
        generated.durationSeconds = static_cast<double>(generated.iq.size()) /
                                    configuration.sampleRate;
        generated.summary = QStringLiteral("AX.25 AFSK1200 UI frame");
    } else if (mode == QStringLiteral("lab")) {
        SignalLabConfiguration lab;
        if (!loadLabPreset(parser.value(QStringLiteral("lab-preset")), &lab,
                           &labMetadata, &generated.error)) return fail(generated.error);
        if (parser.isSet(QStringLiteral("rate"))) lab.sampleRate = configuration.sampleRate;
        if (parser.isSet(QStringLiteral("tx-level"))) lab.amplitude = configuration.level;
        const SignalLabGenerationResult result = SignalLabGenerator::generate(lab);
        if (!result.ok()) return fail(result.error);
        configuration.sampleRate = result.sampleRate;
        generated.iq = result.iq;
        generated.summary = result.summary;
        generated.durationSeconds = result.durationSeconds;
    } else if (mode == QStringLiteral("ft4"))
        generated = generateFt4(configuration, args[2]);
    else generated = TransmitWaveformGenerator::generateText(configuration, args[2]);
    if (!generated.ok()) return fail(generated.error);

    if (sink == QStringLiteral("uhd")) {
        if (!parser.value(QStringLiteral("output")).isEmpty() ||
            parser.isSet(QStringLiteral("virtual-host")) ||
            parser.isSet(QStringLiteral("virtual-name")) ||
            parser.isSet(QStringLiteral("tx-vga-db")))
            return fail(QStringLiteral("UHD output cannot use file, virtual or HackRF VGA options"));
        if (generated.durationSeconds > 60.0 || generated.iq.size() > 32000000)
            return fail(QStringLiteral("UHD CLI TX is limited to 60 seconds and 32 million IQ samples per command"));
        bool valid = false;
        const int device = parser.value(QStringLiteral("tx-device")).toInt(&valid);
        if (!valid || device < 0 || device > 64)
            return fail(QStringLiteral("--tx-device must be 0..64"));
        const int channel = parser.value(QStringLiteral("tx-channel")).toInt(&valid);
        if (!valid || channel < 0 || channel > 7)
            return fail(QStringLiteral("--tx-channel must be 0..7"));
        const double hardwareRate = number(parser.value(QStringLiteral("device-rate")), &valid);
        if (!valid || hardwareRate < 100000 || hardwareRate > 61440000 ||
            std::floor(hardwareRate) != hardwareRate)
            return fail(QStringLiteral("--device-rate must be 100 kS/s..61.44 MS/s for UHD"));
        if (static_cast<double>(configuration.sampleRate) != hardwareRate)
            return fail(QStringLiteral("UHD TX currently requires --rate equal to --device-rate; no implicit resampling"));
        const double gain = number(parser.value(QStringLiteral("tx-gain-db")), &valid);
        if (!valid || !std::isfinite(gain) || gain < 0.0 || gain > 100.0)
            return fail(QStringLiteral("--tx-gain-db must be 0..100 dB (device may allow less)"));
        const double bandwidth = number(parser.value(QStringLiteral("tx-rf-bandwidth")), &valid);
        if (!valid || bandwidth < 0.0 || bandwidth > hardwareRate || !std::isfinite(bandwidth))
            return fail(QStringLiteral("--tx-rf-bandwidth must be 0..device-rate Hz"));
    }

    if (sink == QStringLiteral("hackrf")) {
        if (!parser.value(QStringLiteral("output")).isEmpty() ||
            parser.isSet(QStringLiteral("virtual-host")) ||
            parser.isSet(QStringLiteral("virtual-name")))
            return fail(QStringLiteral("RF output cannot also use file or virtual output options"));
        if (generated.durationSeconds > 60.0)
            return fail(QStringLiteral("CLI RF transmissions are limited to 60 seconds per command"));
        bool valid = false;
        configuration.deviceIndex = parser.value(QStringLiteral("tx-device")).toInt(&valid);
        if (!valid || configuration.deviceIndex < 0 || configuration.deviceIndex > 64)
            return fail(QStringLiteral("--tx-device must be 0..64"));
        const double deviceRate = number(parser.value(QStringLiteral("device-rate")), &valid);
        if (!valid || deviceRate < 2e6 || deviceRate > 20e6 || std::floor(deviceRate) != deviceRate)
            return fail(QStringLiteral("--device-rate must be 2..20 MS/s"));
        configuration.deviceSampleRate = static_cast<int>(deviceRate);
        configuration.txVgaGainDb = parser.value(QStringLiteral("tx-vga-db")).toInt(&valid);
        if (!valid || configuration.txVgaGainDb < 0 || configuration.txVgaGainDb > 47)
            return fail(QStringLiteral("--tx-vga-db must be 0..47"));
        const double bandwidth = number(parser.value(QStringLiteral("tx-rf-bandwidth")), &valid);
        if (!valid || bandwidth < 0 || bandwidth > 20e6 || std::floor(bandwidth) != bandwidth)
            return fail(QStringLiteral("--tx-rf-bandwidth must be 0..20 MHz"));
        configuration.bandwidthHz = static_cast<std::uint32_t>(bandwidth);
        configuration.rfAmpEnabled = false;
        if (configuration.sampleRate > 2000000)
            return fail(QStringLiteral("HackRF baseband IQ rate cannot exceed 2 MS/s"));
    }

    if (sink == QStringLiteral("file")) {
        if (QSysInfo::ByteOrder != QSysInfo::LittleEndian)
            return fail(QStringLiteral("CF32LE file output requires a little-endian host"));
        const QString path = parser.value(QStringLiteral("output"));
        if (path.isEmpty() || QFile::exists(path) || QFile::exists(path + QStringLiteral(".json")))
            return fail(QStringLiteral("--output must name a new CF32LE file and sidecar"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail(file.errorString());
        const qint64 bytes = static_cast<qint64>(generated.iq.size()) * sizeof(std::complex<float>);
        if (file.write(reinterpret_cast<const char *>(generated.iq.constData()), bytes) != bytes)
            return fail(file.errorString());
        file.close();
        QFile metadata(path + QStringLiteral(".json"));
        if (!metadata.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail(metadata.errorString());
        QJsonObject description{
            {QStringLiteral("format"), QStringLiteral("cf32le")},
            {QStringLiteral("input"), QStringLiteral("rf")},
            {QStringLiteral("generator"), QStringLiteral("ObriiCliTxModule")},
            {QStringLiteral("modulation"), mode},
            {QStringLiteral("centerFrequency"), configuration.frequencyHz},
            {QStringLiteral("sampleRate"), configuration.sampleRate},
            {QStringLiteral("complexSamples"), generated.iq.size()},
            {QStringLiteral("createdUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
        if (mode == QStringLiteral("lab")) description.insert(QStringLiteral("signalLabPreset"), labMetadata);
        if (mediaMode) {
            description.insert(QStringLiteral("voiceMode"), parser.value(QStringLiteral("voice-mode")));
            description.insert(QStringLiteral("deviationHz"), configuration.deviationHz);
            description.insert(QStringLiteral("signalBandwidthHz"), configuration.signalBandwidthHz);
            if (mode == QStringLiteral("sstv"))
                description.insert(QStringLiteral("sstvMode"), parser.value(QStringLiteral("sstv-mode")));
        }
        if (mode == QStringLiteral("ax25")) {
            description.insert(QStringLiteral("source"), parser.value(QStringLiteral("ax25-source")));
            description.insert(QStringLiteral("destination"), parser.value(QStringLiteral("ax25-destination")));
            description.insert(QStringLiteral("path"), parser.value(QStringLiteral("ax25-path")));
        }
        const QByteArray json = QJsonDocument(description).toJson(QJsonDocument::Indented);
        if (metadata.write(json) != json.size()) return fail(metadata.errorString());
        report({{QStringLiteral("ok"), true}, {QStringLiteral("event"), QStringLiteral("txComplete")},
                {QStringLiteral("sink"), sink}, {QStringLiteral("path"), QFileInfo(path).absoluteFilePath()},
                {QStringLiteral("samples"), generated.iq.size()},
                {QStringLiteral("durationSeconds"), generated.durationSeconds}});
        return 0;
    }

    if (!parser.value(QStringLiteral("output")).isEmpty())
        return fail(QStringLiteral("--output applies to the file sink only"));
    std::signal(SIGINT, interruptHandler);
    std::signal(SIGTERM, interruptHandler);
    if (rfSink) {
        if ((mode == QStringLiteral("ft8") || mode == QStringLiteral("ft4")) &&
            !parser.isSet(QStringLiteral("no-ft8-align")) &&
            !parser.isSet(QStringLiteral("no-slot-align"))) {
            const qint64 slotMs = mode == QStringLiteral("ft4") ? 7500 : 15000;
            const qint64 waitMs = (slotMs - QDateTime::currentMSecsSinceEpoch() % slotMs) % slotMs;
            report({{QStringLiteral("event"), QStringLiteral("txWaiting")},
                    {QStringLiteral("waitMs"), static_cast<double>(waitMs)}});
            QElapsedTimer wait;
            wait.start();
            while (wait.elapsed() < waitMs && !interrupted.load(std::memory_order_relaxed))
                QThread::msleep(20);
        }
        if (interrupted.load(std::memory_order_relaxed)) return fail(QStringLiteral("RF TX cancelled"));
        if (sink == QStringLiteral("uhd")) {
            bool valid = false;
            const int device = parser.value(QStringLiteral("tx-device")).toInt(&valid);
            const int channel = parser.value(QStringLiteral("tx-channel")).toInt(&valid);
            const double gain = number(parser.value(QStringLiteral("tx-gain-db")), &valid);
            const double bandwidth = number(parser.value(QStringLiteral("tx-rf-bandwidth")), &valid);
            UhdTxSession transmitter;
            QString error;
            if (!transmitter.open(device, channel, configuration.sampleRate,
                                  configuration.frequencyHz, gain, bandwidth,
                                  parser.value(QStringLiteral("tx-antenna")),
                                  parser.value(QStringLiteral("tx-fpga-image")), &error)) return fail(error);
            report({{QStringLiteral("event"), QStringLiteral("txStarted")},
                    {QStringLiteral("sink"), sink}, {QStringLiteral("frequencyHz"), configuration.frequencyHz},
                    {QStringLiteral("sampleRate"), configuration.sampleRate},
                    {QStringLiteral("channel"), channel}});
            constexpr int Chunk = 4096;
            bool sentAll = true;
            for (int offset = 0; offset < generated.iq.size(); offset += Chunk) {
                if (interrupted.load(std::memory_order_relaxed) ||
                    !transmitter.send(generated.iq.constData() + offset,
                                      static_cast<std::size_t>((std::min)(Chunk, generated.iq.size() - offset)),
                                      offset == 0, &error)) {
                    sentAll = false;
                    break;
                }
            }
            const bool ended = transmitter.finish(&error);
            transmitter.close();
            if (!sentAll || !ended) return fail(error.isEmpty() ? QStringLiteral("UHD TX interrupted") : error);
            report({{QStringLiteral("ok"), true}, {QStringLiteral("event"), QStringLiteral("txComplete")},
                    {QStringLiteral("sink"), sink}, {QStringLiteral("samples"), generated.iq.size()},
                    {QStringLiteral("durationSeconds"), generated.durationSeconds}});
            return 0;
        }
        HackRfTransmitterBackend transmitter;
        QString error;
        if (!transmitter.start(configuration, &error)) return fail(error);
        report({{QStringLiteral("event"), QStringLiteral("txStarted")},
                {QStringLiteral("sink"), sink},
                {QStringLiteral("frequencyHz"), configuration.frequencyHz},
                {QStringLiteral("deviceSampleRate"), configuration.deviceSampleRate}});
        if (!transmitter.writeIq(generated.iq.constData(), generated.iq.size(), &error)) {
            transmitter.stop();
            return fail(error);
        }
        const int deadlineMs = static_cast<int>(std::ceil(generated.durationSeconds * 1000.0)) + 5000;
        QElapsedTimer elapsed;
        elapsed.start();
        while (transmitter.bufferedSourceSamples() > 1 && elapsed.elapsed() < deadlineMs &&
               !interrupted.load(std::memory_order_relaxed))
            QThread::msleep(10);
        const bool finished = transmitter.bufferedSourceSamples() <= 1 &&
            !interrupted.load(std::memory_order_relaxed);
        const quint64 underruns = transmitter.sourceUnderrunSamples();
        const quint64 outputSamples = transmitter.outputSamples();
        transmitter.stop();
        if (!finished) return fail(QStringLiteral("RF TX was interrupted or timed out before its IQ queue drained"));
        report({{QStringLiteral("ok"), true}, {QStringLiteral("event"), QStringLiteral("txComplete")},
                {QStringLiteral("sink"), sink}, {QStringLiteral("sourceSamples"), generated.iq.size()},
                {QStringLiteral("hardwareSamples"), static_cast<double>(outputSamples)},
                {QStringLiteral("sourceUnderrunSamples"), static_cast<double>(underruns)},
                {QStringLiteral("durationSeconds"), generated.durationSeconds}});
        return 0;
    }
    VirtualSignalEndpoint endpoint;
    const QString host = parser.value(QStringLiteral("virtual-host"));
    if (!host.isEmpty()) {
        const int port = parser.value(QStringLiteral("virtual-port")).toInt(&okay);
        if (!okay || port < 1 || port > 65535) return fail(QStringLiteral("Invalid virtual RX port"));
        endpoint.mode = VirtualSignalTransportMode::Tcp;
        endpoint.host = host;
        endpoint.port = static_cast<quint16>(port);
    } else endpoint.localName = parser.value(QStringLiteral("virtual-name"));
    if (endpoint.mode == VirtualSignalTransportMode::LocalIpc && endpoint.localName.isEmpty())
        return fail(QStringLiteral("Local virtual channel name is empty"));
    if ((mode == QStringLiteral("ft8") || mode == QStringLiteral("ft4")) &&
        !parser.isSet(QStringLiteral("no-ft8-align")) &&
        !parser.isSet(QStringLiteral("no-slot-align"))) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 slotMs = mode == QStringLiteral("ft4") ? 7500 : 15000;
        const qint64 waitMs = (slotMs - now % slotMs) % slotMs;
        report({{QStringLiteral("event"), QStringLiteral("txWaiting")},
                {QStringLiteral("waitMs"), static_cast<double>(waitMs)}});
        if (waitMs > 0) QThread::msleep(static_cast<unsigned long>(waitMs));
    }
    VirtualTransmitterBackend transmitter;
    transmitter.setEndpoint(endpoint);
    const QString virtualNodeId = parser.value(QStringLiteral("virtual-node-id")).trimmed();
    if (!virtualNodeId.isEmpty() && !virtualSignalValidNodeId(virtualNodeId))
        return fail(QStringLiteral("Invalid virtual node ID (1-64 ASCII letters/digits/._-, excluding txN)"));
    transmitter.setNodeId(virtualNodeId);
    QString error;
    if (!transmitter.start(configuration, &error)) return fail(error);
    QElapsedTimer connectionWait;
    connectionWait.start();
    while (!transmitter.isConnected() && connectionWait.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
        transmitter.pollConnection();
    }
    if (!transmitter.isConnected()) {
        transmitter.stop();
        return fail(QStringLiteral("Virtual IQ receiver did not connect within 3 seconds"));
    }
    report({{QStringLiteral("event"), QStringLiteral("txStarted")},
            {QStringLiteral("sink"), sink}, {QStringLiteral("modulation"), mode}});
    QElapsedTimer elapsed;
    elapsed.start();
    constexpr int ChunkSamples = 4096;
    for (int offset = 0; offset < generated.iq.size(); offset += ChunkSamples) {
        const int count = (std::min)(ChunkSamples, generated.iq.size() - offset);
        if (!transmitter.writeIq(generated.iq.constData() + offset, count, &error)) {
            transmitter.stop();
            return fail(error);
        }
        const qint64 targetMs = static_cast<qint64>(
            (static_cast<double>(offset + count) / configuration.sampleRate) * 1000.0);
        while (elapsed.elapsed() + 20 < targetMs) {
            QCoreApplication::processEvents();
            QThread::msleep(static_cast<unsigned long>(
                (std::min)(20LL, targetMs - elapsed.elapsed() - 20)));
        }
    }
    if (!transmitter.drain(3000, &error)) {
        transmitter.stop();
        return fail(error);
    }
    transmitter.stop();
    report({{QStringLiteral("ok"), true}, {QStringLiteral("event"), QStringLiteral("txComplete")},
            {QStringLiteral("sink"), sink}, {QStringLiteral("samples"), generated.iq.size()},
            {QStringLiteral("durationSeconds"), generated.durationSeconds}});
    return 0;
}
