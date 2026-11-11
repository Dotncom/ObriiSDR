#include "clinetworkbridge.h"
#include "displayspectrumcodec.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QThread>

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
bool waitFor(const std::function<bool()> &condition, int timeoutMs = 2000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return condition();
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) return 1;
    const quint16 port = probe.serverPort();
    probe.close();
    CliNetworkBridge server(&app), client(&app);
    CliNetworkBridge::ServerCallbacks callbacks;
    bool running = true;
    callbacks.isRunning = [&] { return running; };
    callbacks.setRunning = [&](bool requested, QString *) {
        running = requested;
        return true;
    };
    callbacks.settings = [] { return QJsonObject{{QStringLiteral("centerFrequency"), 100e6},
                                                  {QStringLiteral("listeningFrequency"), 100e6},
                                                  {QStringLiteral("sampleRate"), 8e6}}; };
    callbacks.receivers = [] { return QJsonArray{QJsonObject{{QStringLiteral("deviceIndex"), 0},
                                                               {QStringLiteral("label"), QStringLiteral("test")}}}; };
    if (!server.startServer(QStringLiteral("127.0.0.1"), port, std::move(callbacks))) return 2;
    int channelFrames = 0, fullFrames = 0, audioFrames = 0;
    int uhdSpectrumFrames = 0;
    QByteArray receivedAudio;
    client.onClientIq = [&](const QJsonObject &frame, const QByteArray &payload) {
        if (frame.value(QStringLiteral("channelized")).toBool()) {
            if (frame.value(QStringLiteral("sampleFormat")).toString() ==
                    QStringLiteral("channel_iq_s16le") && payload.size() > 0 &&
                payload.size() == frame.value(QStringLiteral("sampleCount")).toInt() * 4)
                ++channelFrames;
        } else if (frame.value(QStringLiteral("sampleFormat")).toString() ==
                       QStringLiteral("iq_s8_interleaved") && payload.size() == 65536 * 2) {
            ++fullFrames;
        }
    };
    client.onClientAudio = [&](const QByteArray &pcm) {
        ++audioFrames;
        receivedAudio += pcm;
    };
    client.onClientUhdRxSpectrum = [&](const QJsonObject &frame, const QByteArray &payload) {
        const int count = frame.value("fftLength").toInt();
        std::vector<float> levels;
        if (frame.value("channel").toInt(-1) == 1 && count == 512 &&
            frame.value("sourceFftLength").toInt() == 65537 &&
            frame.value("centerFrequency").toDouble() == 100e6 &&
            frame.value("sampleRate").toDouble() == 8e6 &&
            DisplaySpectrumCodec::decodeDb10(payload, count, false, &levels) &&
            levels[256] == -7.0f) ++uhdSpectrumFrames;
    };
    client.connectClient(QStringLiteral("127.0.0.1"), port);
    if (!waitFor([&] {
        return server.peerCount() == 1 && client.ready() &&
               client.clientState().value("receiverRunning") == QJsonValue(true);
    })) return 3;
    if (!client.sendClientControl(QStringLiteral("stop")) ||
        !waitFor([&] { return client.clientState().value("receiverRunning") == QJsonValue(false); }))
        return 17;
    bool startAck = false;
    client.onClientState = [&](const QJsonObject &state) {
        if (state.value("action") == QJsonValue("settingsAck"))
            startAck = state.value("ok").toBool() && state.value("receiverRunning").toBool();
    };
    if (!client.sendClientControl(QStringLiteral("start")) ||
        !waitFor([&] { return startAck && client.clientState().value("receiverRunning").toBool(); }))
        return 18;

    RadioSettings settings;
    settings.sampleRate = 8e6;
    settings.centerFrequency = 100e6;
    settings.listeningFrequency = 100e6;
    settings.bandwidth = 12500;
    settings.modulationType = MOD_NFM;
    std::vector<float> iq(65536 * 2);
    for (std::size_t n = 0; n < iq.size() / 2; ++n) {
        iq[n * 2] = 0.02f * std::cos(2.0 * 3.141592653589793 * n / 32.0);
        iq[n * 2 + 1] = 0.02f * std::sin(2.0 * 3.141592653589793 * n / 32.0);
    }
    if (!server.setServerProcessingMode(1)) return 4;
    server.sendIq(iq.data(), 65536, settings);
    if (!waitFor([&] { return channelFrames > 0; })) return 5;
    if (!server.setServerProcessingMode(2)) return 6;
    server.sendIq(iq.data(), 65536, settings);
    if (!waitFor([&] { return fullFrames > 0; })) return 7;
    if (!server.setServerProcessingMode(0)) return 8;
    QByteArray expectedAudio;
    for (int i = 0; i < 120; ++i) {
        const QByteArray fragment(32, static_cast<char>(i));
        server.sendAudio(fragment);
        expectedAudio += fragment;
    }
    if (!waitFor([&] { return audioFrames >= 2; }) ||
        audioFrames != 2 || receivedAudio != expectedAudio) return 9;
    int spectrumFrames = 0;
    QJsonObject latest;
    client.onClientSpectrum = [&](const QJsonObject &frame) { latest = frame; ++spectrumFrames; };
    constexpr int length = 65537;
    std::vector<float> full(length, -120.0f), reduced(512);
    full[length / 2] = -7;
    for (int i = 0; i < 512; ++i) {
        const int first = static_cast<int>(static_cast<qint64>(i) * length / 512);
        const int last = static_cast<int>(static_cast<qint64>(i + 1) * length / 512);
        reduced[i] = *std::max_element(full.begin() + first, full.begin() + last);
    }
    server.sendSpectrum(full, 100e6, 8e6, 100e6, 12500, MOD_NFM, INPUT_RF);
    if (!waitFor([&] { return spectrumFrames == 1; })) return 10;
    const auto reference = latest;
    server.sendSpectrum(reduced, 100e6, 8e6, 100e6, 12500, MOD_NFM, INPUT_RF, length);
    if (!waitFor([&] { return spectrumFrames == 2; })) return 11;
    if (latest.value("sourceFftLength").toInt() != length || latest.value("magnitudes") != reference.value("magnitudes") ||
        latest.value("frequencies") != reference.value("frequencies")) return 12;
    const double binHz = 8e6 / length;
    const double expectedFirst = 100e6 - (length / 2) * binHz + (length / 512 - 1) * 0.5 * binHz;
    if (std::abs(latest.value("frequencies").toArray()[0].toDouble() - expectedFirst) > 1e-6) return 13;
    const auto native = latest.value("magnitudes").toArray();
    if (latest.value("magnitudeOrder") != QJsonValue("fft-native") || native.size() != 512 ||
        native[0].toDouble() != -7 || native[256].toDouble() != -120) return 14;
    std::vector<float> small{-90, -80, -7, -70, -100};
    server.sendSpectrum(small, 100e6, 8e6, 100e6, 12500, MOD_NFM, INPUT_RF);
    if (!waitFor([&] { return spectrumFrames == 3; })) return 15;
    const auto oddNative = latest.value("magnitudes").toArray();
    for (int i = 0; i < 5; ++i)
        if (oddNative[(i + 3) % 5].toDouble() != small[static_cast<std::size_t>(i)]) return 16;

    CliNetworkBridge legacy(&app);
    legacy.connectClient(QStringLiteral("127.0.0.1"), port);
    if (!waitFor([&] { return legacy.ready() && server.peerCount() == 2 && server.hasUhdRxSpectrumPeers(); }))
        return 19;
    int legacyUhdFrames = 0, legacySpectrumFrames = 0;
    // Install the observer after the legacy peer has sent its capabilities.
    legacy.onClientUhdRxSpectrum = [&](const QJsonObject &, const QByteArray &) { ++legacyUhdFrames; };
    legacy.onClientSpectrum = [&](const QJsonObject &) { ++legacySpectrumFrames; };
    server.sendUhdRxSpectrum(1, full, 100e6, 8e6, length);
    server.sendSpectrum(small, 100e6, 8e6, 100e6, 12500, MOD_NFM, INPUT_RF);
    if (!waitFor([&] { return uhdSpectrumFrames == 1 && legacySpectrumFrames == 1; }) ||
        legacyUhdFrames != 0) return 20;
    std::puts("CLI network bridge self-test passed");
    return 0;
}
