#include "remotevfoservice.h"
#include "remotewebserver.h"
#include "iqbuffer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void requireRemote(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

QJsonObject command(const QString &action,
                    const QString &vfoId,
                    double frequencyHz = 0.0) {
    QJsonObject result{{QStringLiteral("type"), QStringLiteral("remoteVfo")},
                       {QStringLiteral("action"), action},
                       {QStringLiteral("vfoId"), vfoId}};
    if (frequencyHz > 0.0) {
        result.insert(QStringLiteral("settings"),
                      QJsonObject{{QStringLiteral("frequencyHz"), frequencyHz},
                                  {QStringLiteral("bandwidthHz"), 12500.0},
                                  {QStringLiteral("audioEnabled"), false},
                                  {QStringLiteral("spectrumEnabled"), false}});
    }
    return result;
}

QJsonObject lastSession(const QList<QJsonObject> &frames, const QString &owner) {
    for (auto it = frames.crbegin(); it != frames.crend(); ++it) {
        if (it->value(QStringLiteral("_owner")).toString() == owner &&
            it->value(QStringLiteral("vfos")).isArray()) return *it;
    }
    return {};
}
}

bool runRemoteVfoServiceTests() {
    std::cerr << "Remote VFO self-test: constructing service\n" << std::flush;
    RemoteVfoService service;
    RadioSettings settings;
    settings.centerFrequency = 100000000.0;
    settings.listeningFrequency = 100000000.0;
    settings.sampleRate = 2000000.0;
    service.setSourceState(settings, false);
    std::cerr << "Remote VFO self-test: source configured\n" << std::flush;

    QList<QJsonObject> frames;
    QObject::connect(&service, &RemoteVfoService::jsonFrameReady,
                     [&frames](const QString &owner, QJsonObject frame) {
                         frame.insert(QStringLiteral("_owner"), owner);
                         frames.append(frame);
                     });

    service.openEndpoint(QStringLiteral("client-a"), QStringLiteral("A"));
    std::cerr << "Remote VFO self-test: first endpoint opened\n" << std::flush;
    service.handleCommand(QStringLiteral("client-a"),
                          command(QStringLiteral("create"), QStringLiteral("one"), 100100000.0));
    requireRemote(service.sessionCount() == 1 && service.activeVfoCount() == 1,
                  "Remote VFO did not create an isolated active branch");

    service.handleCommand(QStringLiteral("client-a"),
                          command(QStringLiteral("update"), QStringLiteral("one"), 100200000.0));
    service.handleCommand(QStringLiteral("client-a"), command(QStringLiteral("list"), QStringLiteral("one")));
    QJsonArray tuned = lastSession(frames, QStringLiteral("client-a"))
                           .value(QStringLiteral("vfos")).toArray();
    requireRemote(tuned.size() == 1 &&
                      std::abs(tuned.at(0).toObject().value(QStringLiteral("frequencyHz")).toDouble() -
                               100200000.0) < 0.5,
                  "Valid remote VFO tuning command did not change frequency");

    service.handleCommand(QStringLiteral("client-a"),
                          command(QStringLiteral("update"), QStringLiteral("one"), 110000000.0));
    requireRemote(!frames.isEmpty() &&
                      frames.back().value(QStringLiteral("action")).toString() == QStringLiteral("error"),
                  "Out-of-span remote VFO update was not rejected");
    service.handleCommand(QStringLiteral("client-a"), command(QStringLiteral("list"), QStringLiteral("one")));
    const QJsonArray retained = lastSession(frames, QStringLiteral("client-a"))
                                    .value(QStringLiteral("vfos")).toArray();
    requireRemote(retained.size() == 1 &&
                      std::abs(retained.at(0).toObject().value(QStringLiteral("frequencyHz")).toDouble() -
                               100200000.0) < 0.5,
                  "Rejected remote VFO update changed the previous valid state");

    service.handleCommand(QStringLiteral("client-a"),
                          command(QStringLiteral("create"), QStringLiteral("two"), 99900000.0));
    service.handleCommand(QStringLiteral("client-a"),
                          command(QStringLiteral("create"), QStringLiteral("three"), 100200000.0));
    requireRemote(service.activeVfoCount() == 2 &&
                      frames.back().value(QStringLiteral("action")).toString() == QStringLiteral("error"),
                  "Per-session remote VFO quota was not enforced");

    service.handleCommand(QStringLiteral("client-b"),
                          command(QStringLiteral("create"), QStringLiteral("one"), 100300000.0));
    requireRemote(service.sessionCount() == 2 && service.activeVfoCount() == 3,
                  "Second remote session did not receive an independent VFO");
    service.handleCommand(QStringLiteral("client-a"),
                          command(QStringLiteral("deactivate"), QStringLiteral("one")));
    requireRemote(service.activeVfoCount() == 2,
                  "Deactivating one remote VFO affected the wrong branch count");
    service.closeEndpoint(QStringLiteral("client-a"));
    requireRemote(service.sessionCount() == 1 && service.activeVfoCount() == 1,
                  "Closing a remote session did not release only its VFOs");
    service.closeEndpoint(QStringLiteral("client-b"));

    RemoteWebServer web(&service);
    requireRemote(web.start(QStringLiteral("127.0.0.1"), 0) && web.port() != 0,
                  "Remote VFO browser gateway did not bind to loopback");
    QTcpSocket pageSocket;
    pageSocket.connectToHost(QStringLiteral("127.0.0.1"), web.port());
    requireRemote(pageSocket.waitForConnected(2000), "Remote VFO browser page test did not connect");
    pageSocket.write("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    pageSocket.flush();
    QElapsedTimer wait;
    wait.start();
    while (pageSocket.bytesAvailable() == 0 && wait.elapsed() < 2000) {
        QCoreApplication::processEvents();
    }
    const QByteArray page = pageSocket.readAll();
    requireRemote(page.startsWith("HTTP/1.1 200") &&
                      page.contains("id=\"scale\"") && page.contains("id=\"captureTune\""),
                  "Remote VFO browser page lacks frequency scale or capture tuner");

    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), web.port());
    requireRemote(socket.waitForConnected(2000), "Remote VFO browser test did not connect");
    socket.write("GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                 "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    socket.flush();
    wait.start();
    while (socket.bytesAvailable() == 0 && wait.elapsed() < 2000) {
        QCoreApplication::processEvents();
    }
    const QByteArray response = socket.readAll();
    requireRemote(response.startsWith("HTTP/1.1 101") && service.sessionCount() == 1,
                  "Remote VFO WebSocket handshake did not create an isolated session");
    socket.disconnectFromHost();
    socket.waitForDisconnected(500);
    QCoreApplication::processEvents();
    web.stop();
    requireRemote(service.sessionCount() == 0,
                  "Remote VFO browser disconnect did not release its session");

    RemoteVfoService spectrumService;
    QJsonObject spectrumFrame;
    QObject::connect(&spectrumService, &RemoteVfoService::jsonFrameReady,
                     [&spectrumFrame](const QString &, const QJsonObject &frame) {
                         if (frame.value(QStringLiteral("type")).toString() == QStringLiteral("spectrum"))
                             spectrumFrame = frame;
                     });
    spectrumService.setSourceState(settings, true);
    QJsonObject activate = command(QStringLiteral("activate"), QStringLiteral("tone"), 100100000.0);
    QJsonObject toneSettings = activate.value(QStringLiteral("settings")).toObject();
    toneSettings.insert(QStringLiteral("fftLength"), 512);
    toneSettings.insert(QStringLiteral("refreshMs"), 40);
    toneSettings.insert(QStringLiteral("spectrumEnabled"), true);
    activate.insert(QStringLiteral("settings"), toneSettings);
    spectrumService.handleCommand(QStringLiteral("spectrum-client"), activate);

    IqBuffer::clear(7001);
    IqBuffer::setSampleRateEstimate(settings.sampleRate);
    constexpr int complexSamples = 262144;
    std::vector<float> iq(static_cast<std::size_t>(complexSamples) * 2U);
    const double step = 2.0 * 3.14159265358979323846 * 100000.0 / settings.sampleRate;
    for (int i = 0; i < complexSamples; ++i) {
        iq[static_cast<std::size_t>(2 * i)] = static_cast<float>(0.5 * std::cos(step * i));
        iq[static_cast<std::size_t>(2 * i + 1)] = static_cast<float>(0.5 * std::sin(step * i));
    }
    IqBuffer::BlockMetadata metadata;
    metadata.valid = true;
    metadata.centerFrequencyHz = settings.centerFrequency;
    metadata.epoch = 7001;
    requireRemote(IqBuffer::publish(iq.data(), iq.size(), false, true, 7001, &metadata),
                  "Could not publish synthetic IQ for remote VFO spectrum test");
    wait.restart();
    while (spectrumFrame.isEmpty() && wait.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    const QJsonArray magnitudes = spectrumFrame.value(QStringLiteral("magnitudes")).toArray();
    const QJsonArray frequencies = spectrumFrame.value(QStringLiteral("frequencies")).toArray();
    requireRemote(magnitudes.size() == 512 && frequencies.size() == 512 &&
                      spectrumFrame.value(QStringLiteral("magnitudeOrder")).toString() ==
                          QStringLiteral("fft-native"),
                  "Remote VFO did not publish the expected network spectrum format");
    int rawPeak = 0;
    for (int i = 1; i < magnitudes.size(); ++i)
        if (magnitudes.at(i).toDouble() > magnitudes.at(rawPeak).toDouble()) rawPeak = i;
    const int displayedPeak = (rawPeak + magnitudes.size() / 2) % magnitudes.size();
    requireRemote(std::abs(displayedPeak - magnitudes.size() / 2) <= 2 &&
                      std::abs(frequencies.at(displayedPeak).toDouble() - 100100000.0) < 500.0,
                  "Remote VFO network ordering shifted a centered signal away from its frequency");
    spectrumService.closeEndpoint(QStringLiteral("spectrum-client"));
    IqBuffer::clear();
    std::cerr << "Remote VFO self-test: checks complete\n" << std::flush;
    return true;
}
