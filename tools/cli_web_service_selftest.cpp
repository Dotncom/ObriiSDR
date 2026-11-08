#include "cliwebservice.h"
#include "iqbuffer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
void require(bool okay, const char *message) {
    if (!okay) throw std::runtime_error(message);
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 4000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
    return condition();
}

QByteArray http(quint16 port, const QByteArray &path) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    require(waitFor([&] { return socket.state() == QAbstractSocket::ConnectedState; }),
            "HTTP connect failed");
    socket.write("GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    QByteArray response;
    require(waitFor([&] {
        response += socket.readAll();
        return socket.state() == QAbstractSocket::UnconnectedState;
    }), "HTTP response timed out");
    return response;
}

QJsonObject statusHttp(quint16 port) {
    const QByteArray response = http(port, "/api/status");
    require(response.startsWith("HTTP/1.1 200"), "Status endpoint is not healthy");
    QJsonParseError error;
    const QJsonDocument json = QJsonDocument::fromJson(response.mid(response.indexOf("\r\n\r\n") + 4), &error);
    require(error.error == QJsonParseError::NoError && json.isObject(), "Invalid HTTP status JSON");
    return json.object();
}

// Minimal test client for the production gateway, without QtWebSockets.
struct Browser {
    QTcpSocket socket;
    QByteArray pending;
    QList<QJsonObject> messages;
    int pcmFrames = 0;

    void poll() {
        pending += socket.readAll();
        while (pending.size() >= 2) {
            const auto byte = [&](int i) { return static_cast<quint8>(pending.at(i)); };
            const quint8 opcode = byte(0) & 0x0f;
            require(!(byte(1) & 0x80), "Server frames must not be masked");
            quint64 length = byte(1) & 0x7f;
            int offset = 2;
            if (length == 126) {
                if (pending.size() < 4) return;
                length = (quint64(byte(2)) << 8) | byte(3);
                offset = 4;
            } else if (length == 127) {
                if (pending.size() < 10) return;
                length = 0;
                for (int i = 2; i < 10; ++i) length = (length << 8) | byte(i);
                offset = 10;
            }
            require(length <= 4 * 1024 * 1024, "Oversized server frame");
            if (quint64(pending.size()) < quint64(offset) + length) return;
            const QByteArray payload = pending.mid(offset, int(length));
            pending.remove(0, offset + int(length));
            if (opcode == 1) {
                const QJsonDocument json = QJsonDocument::fromJson(payload);
                require(json.isObject(), "Invalid WebSocket JSON");
                messages.append(json.object());
            } else if (opcode == 2) {
                require(!payload.isEmpty() && payload.size() % 2 == 0, "Invalid PCM payload");
                ++pcmFrames;
            }
        }
    }

    void open(quint16 port) {
        socket.connectToHost(QHostAddress::LocalHost, port);
        require(waitFor([&] { return socket.state() == QAbstractSocket::ConnectedState; }), "WS connect failed");
        socket.write("GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
        require(waitFor([&] {
            pending += socket.readAll();
            return pending.contains("\r\n\r\n");
        }), "WS upgrade timed out");
        const int end = pending.indexOf("\r\n\r\n") + 4;
        const QByteArray header = pending.left(end);
        require(header.startsWith("HTTP/1.1 101") &&
                header.contains("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "WS upgrade failed");
        pending.remove(0, end);
        require(waitFor([&] { poll(); return !messages.isEmpty(); }), "WS session missing");
    }

    void send(const QJsonObject &command) {
        const QByteArray payload = QJsonDocument(command).toJson(QJsonDocument::Compact);
        require(payload.size() <= 65535, "Test frame too large");
        QByteArray frame(1, char(0x81));
        if (payload.size() < 126) frame.append(char(0x80 | payload.size()));
        else {
            frame.append(char(0xfe));
            frame.append(char((payload.size() >> 8) & 0xff));
            frame.append(char(payload.size() & 0xff));
        }
        const QByteArray mask = QByteArray::fromHex("12345678");
        frame += mask;
        for (int i = 0; i < payload.size(); ++i) frame.append(payload.at(i) ^ mask.at(i & 3));
        require(socket.write(frame) == frame.size(), "WS command write failed");
    }

    QJsonObject exchange(const QJsonObject &command, const QString &action) {
        poll();
        const int first = messages.size();
        send(command);
        QJsonObject found;
        require(waitFor([&] {
            poll();
            for (int i = first; i < messages.size(); ++i) {
                if (messages.at(i).value("action").toString() == action) {
                    found = messages.at(i);
                    return true;
                }
            }
            return false;
        }), "WS command response timed out");
        return found;
    }
};

QJsonObject vfo(const char *action, double frequency = 100100000.0) {
    return {{"action", QString::fromLatin1(action)}, {"vfoId", "main"},
            {"settings", QJsonObject{{"frequencyHz", frequency}, {"bandwidthHz", 12500},
                                     {"fftLength", 512}, {"refreshMs", 40},
                                     {"audioEnabled", true}, {"spectrumEnabled", true}}}};
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        CliWebService web;
        RadioSettings settings;
        settings.centerFrequency = settings.listeningFrequency = 100000000.0;
        settings.sampleRate = 2000000.0;
        web.setSourceState(settings, true);
        IqBuffer::clear(7901);
        auto &registry = DspIqSourceRegistry::instance();
        const DspSourceIdentity aux = DspSourceIdentity::normalized("aux", "rx0");
        std::vector<float> iq(262144 * 2);
        constexpr double tau = 6.283185307179586;
        for (std::size_t n = 0; n < iq.size() / 2; ++n) {
            const double t = double(n) / settings.sampleRate;
            const double phase = tau * 100000.0 * t + std::sin(tau * 1000.0 * t);
            iq[n * 2] = float(0.5 * std::cos(phase));
            iq[n * 2 + 1] = float(0.5 * std::sin(phase));
        }
        const auto before = IqBuffer::stats();
        const auto registryBefore = registry.stats();
        require(!web.needsIq() && !web.isListening() && web.port() == 0, "Construction enabled service");
        require(!web.submitIq(iq.data(), iq.size() / 2) &&
                !web.submitSourceIq(aux, iq.data(), iq.size() / 2, settings.sampleRate, settings.centerFrequency),
                "Disabled service published IQ");
        require(IqBuffer::stats().totalFloatCount == before.totalFloatCount &&
                registry.stats().ignoredBlocks == registryBefore.ignoredBlocks,
                "Disabled service touched shared buffers");
        require(!web.start("not-an-address", 0) && !web.isListening(), "Invalid address was accepted");
        require(!web.command({{"action", "start"}, {"port", 1.5}}).value("ok").toBool(),
                "Fractional port accepted");
        QTcpServer occupied;
        require(occupied.listen(QHostAddress::LocalHost, 0), "Port reservation failed");
        require(!web.start("127.0.0.1", occupied.serverPort()) && !web.isListening(),
                "Occupied port accepted");
        require(web.command({{"action", "start"}, {"port", 0}}).value("ok").toBool(), "Start failed");
        const quint16 port = web.port();
        require(port && !web.needsIq() && !web.submitIq(iq.data(), iq.size() / 2),
                "Listener with no VFO published IQ");
        require(statusHttp(port).value("activeVfos").toInt(-1) == 0, "Initial status incorrect");
        const QByteArray page = http(port, "/");
        require(page.startsWith("HTTP/1.1 200") && page.contains("id=\"spectrum\"") &&
                page.contains("id=\"captureTune\"") && page.contains("/ws"), "Browser assets missing");
        require(http(port, "/index.html") == page, "Index route differs");
        require(http(port, "/health").startsWith("HTTP/1.1 404"), "Unexpected health route");

        Browser a, b;
        a.open(port);
        b.open(port);
        require(a.messages.front().value("sessionId") != b.messages.front().value("sessionId"),
                "Browser sessions are not isolated");
        require(web.start("127.0.0.1", 0) && web.port() == port &&
                statusHttp(port).value("sessions").toInt() == 2, "Repeated start destroyed sessions");
        a.exchange(vfo("activate"), "updated");
        b.exchange(vfo("activate", 99900000.0), "updated");
        require(web.needsIq() && statusHttp(port).value("activeVfos").toInt() == 2,
                "VFO activation failed");
        QJsonObject extra = vfo("create");
        extra.insert("vfoId", "second");
        a.exchange(extra, "updated");
        extra.insert("vfoId", "third");
        a.exchange(extra, "error");
        a.exchange({{"action", "delete"}, {"vfoId", "second"}}, "updated");
        require(statusHttp(port).value("activeVfos").toInt() == 2, "Per-session VFO quota failed");
        const QJsonObject error = a.exchange(vfo("update", 110000000.0), "error");
        require(error.value("requestAction").toString() == "update", "Out-of-span update accepted");
        const QJsonArray retained = a.exchange({{"action", "list"}}, "session").value("vfos").toArray();
        require(retained.size() == 1 && retained.at(0).toObject().value("frequencyHz").toDouble() == 100100000.0,
                "Rejected update altered VFO");

        const auto preIq = IqBuffer::stats();
        require(!web.submitIq(iq.data(), iq.size() / 2, true) &&
                !web.submitIq(nullptr, 1) && !web.submitIq(iq.data(), std::numeric_limits<std::size_t>::max()),
                "Duplicate or invalid block accepted");
        require(IqBuffer::stats().totalFloatCount == preIq.totalFloatCount, "Duplicate publication occurred");
        require(!web.submitIq(iq.data(), iq.size() / 2, false, 7902), "Stale epoch accepted");
        require(web.submitIq(iq.data(), iq.size() / 2, false, 7901), "Raw IQ publish failed");
        require(IqBuffer::stats().totalFloatCount == preIq.totalFloatCount + iq.size() &&
                IqBuffer::queuedBlocks() == 0, "IQ publication count or queue incorrect");
        require(waitFor([&] {
            a.poll();
            bool spectrum = false;
            for (const QJsonObject &frame : a.messages) {
                if (frame.value("type").toString() != "spectrum") continue;
                require(frame.value("vfoId").toString() == "main" &&
                        frame.value("centerFrequency").toDouble() == 100100000.0 &&
                        frame.value("magnitudes").toArray().size() == 512 &&
                        frame.value("magnitudeOrder").toString() == "fft-native", "Invalid VFO FFT");
                spectrum = true;
            }
            return spectrum && a.pcmFrames > 0;
        }), "Production VFO did not deliver spectrum and PCM over WS");

        QJsonObject auxiliary = vfo("update");
        QJsonObject auxSettings = auxiliary.value("settings").toObject();
        auxSettings.insert("receiverId", "aux");
        auxiliary.insert("settings", auxSettings);
        b.exchange(auxiliary, "updated");
        const auto auxBefore = registry.stats();
        require(!web.submitSourceIq(aux, iq.data(), iq.size() / 2, settings.sampleRate,
                                    settings.centerFrequency, true), "Duplicate auxiliary IQ accepted");
        require(web.submitSourceIq(aux, iq.data(), iq.size() / 2, settings.sampleRate,
                                   settings.centerFrequency), "Subscribed auxiliary IQ rejected");
        require(registry.stats().publishedBlocks == auxBefore.publishedBlocks + 1, "Auxiliary IQ duplicated");
        require(waitFor([&] {
            b.poll();
            for (const QJsonObject &frame : b.messages)
                if (frame.value("type").toString() == "spectrum" &&
                    frame.value("centerFrequency").toDouble() == 100100000.0) return true;
            return false;
        }), "Auxiliary registry IQ did not reach browser VFO");

        QJsonObject native;
        QObject::connect(&web, &CliWebService::jsonFrameReady, &app,
                         [&](const QString &peer, const QJsonObject &frame) {
            require(peer == "web-1", "Native peer ID routing changed");
            native = frame;
        });
        require(!web.handleRemoteCommand("web-1", {{"type", "control"}, {"action", "stop"}}),
                "Receiver control reached VFO broker");
        require(web.handleRemoteCommand("web-1", {{"type", "remoteVfo"}, {"action", "open"}}) &&
                native.value("role").toString() == "listener", "Native session hook failed");
        web.closeRemoteEndpoint("web-1");
        require(statusHttp(port).value("sessions").toInt() == 2, "Native peer collided with browser owner");
        web.setSourceState(settings, false);
        require(!web.needsIq() && !web.submitIq(iq.data(), iq.size() / 2) &&
                !web.submitSourceIq(aux, iq.data(), iq.size() / 2, settings.sampleRate, settings.centerFrequency) &&
                !statusHttp(port).value("receiverRunning").toBool(), "Stopped receiver still publishes IQ");
        web.setSourceState(settings, true);
        a.exchange(vfo("deactivate"), "updated");
        b.socket.abort();
        require(waitFor([&] { return web.status().value("activeVfos").toInt() == 0; }),
                "Disconnect did not release VFO");
        require(!web.needsIq() && registry.stats().subscribedSources == registryBefore.subscribedSources,
                "Last VFO retained IQ work/subscriptions");
        const auto afterIq = IqBuffer::stats();
        web.command({{"action", "stop"}});
        web.stop();
        require(!web.isListening() && !web.needsIq() && web.port() == 0 &&
                web.status().value("sessions").toInt() == 0 &&
                IqBuffer::stats().totalFloatCount == afterIq.totalFloatCount,
                "Stop retained service or cleared shared IQ");
        require(web.start("127.0.0.1", port), "Port not released after stop");
        require(!web.start("invalid-bind", 0) && web.port() == port,
                "Invalid address destroyed the live listener");
        Browser restarted;
        restarted.open(web.port());
        restarted.exchange(vfo("activate"), "updated");
        web.stop();
        require(!web.needsIq(), "Active service shutdown failed");
        IqBuffer::clear();
        std::cout << "CLI web service self-test passed (HTTP, two WS sessions, FFT/PCM, IQ gating, native hooks, lifecycle)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "CLI web service self-test failed: " << error.what() << '\n';
        return 1;
    }
}
