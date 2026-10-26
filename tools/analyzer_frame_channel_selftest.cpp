#include "analyzerframechannel.h"

#include <QCoreApplication>
#include <QTimer>

#include <cmath>
#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString channel = QStringLiteral("ObriiAnalyzerSelfTest-%1").arg(QCoreApplication::applicationPid());
    AnalyzerFramePublisher publisher;
    AnalyzerFrameClient client;
    if (!publisher.start(channel)) {
        std::cerr << publisher.errorString().toStdString() << '\n';
        return 1;
    }

    bool received = false;
    QObject::connect(&client, &AnalyzerFrameClient::connected, &app, [&]() {
        QTimer::singleShot(20, &app, [&]() {
            std::vector<float> frequencies(2048);
            std::vector<float> levels(2048, -120.0f);
            for (int i = 0; i < 2048; ++i) frequencies[static_cast<std::size_t>(i)] = 100.0e6f + i * 1000.0f;
            levels[1024] = -20.0f;
            publisher.publish(frequencies, levels, 1048576);
        });
    });
    QObject::connect(&client, &AnalyzerFrameClient::frameReady, &app,
                     [&](const AnalyzerSpectrumFrame &frame) {
        received = frame.levelsDb.size() == 2048 && frame.sourceFftLength == 1048576 &&
                   std::abs(frame.levelsDb[1024] + 20.0f) < 0.01f;
        app.quit();
    });
    QObject::connect(&client, &AnalyzerFrameClient::connectionError, &app,
                     [&](const QString &message) { std::cerr << message.toStdString() << '\n'; app.exit(2); });
    QTimer::singleShot(2000, &app, [&]() { app.exit(3); });
    client.connectToPublisher(channel);
    const int result = app.exec();
    if (result != 0 || !received) return result != 0 ? result : 4;
    std::cout << "analyzer frame channel self-test: ok\n";
    return 0;
}
