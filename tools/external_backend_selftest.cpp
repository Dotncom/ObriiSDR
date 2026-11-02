#include "gnsssdrstream.h"
#include "gnsssdrdecimator.h"
#include "gnsssdrpvtdialog.h"
#include "lrptdecodedialog.h"
#include "iqbuffer.h"
#include "samplefileutils.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLayout>
#include <QDir>
#include <QHostAddress>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
int checks = 0;
void require(bool okay, const char *message) { ++checks; if (!okay) throw std::runtime_error(message); }
void pump(int milliseconds) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < milliseconds) { QApplication::processEvents(); QThread::msleep(1); }
}
void streamTest(GnssSdr::Input in, const QByteArray &expected) {
    GnssSdrStream worker(in); QTcpSocket iq, nmea; QLocalSocket pipe; QIODevice *iqDevice = nullptr; int sentences = 0;
    QObject::connect(&worker, &GnssSdrStream::listening, &iq, [&](const QString &iqSource, quint16 nmeaPort) {
#ifdef Q_OS_WIN
        pipe.connectToServer(iqSource); iqDevice = &pipe;
        const bool iqConnected = pipe.waitForConnected(1000);
#else
        iq.connectToHost(QHostAddress::LocalHost, iqSource.section(':', -1).toUShort()); iqDevice = &iq;
        const bool iqConnected = iq.waitForConnected(1000);
#endif
        nmea.connectToHost(QHostAddress::LocalHost, nmeaPort);
        require(iqConnected && nmea.waitForConnected(1000), "Loopback stream connect failed");
        nmea.write("$GNRMC,test*00\r\n"); nmea.flush();
    });
    QObject::connect(&worker, &GnssSdrStream::nmeaLine, &iq, [&](const QString &line) { if (line == "$GNRMC,test*00") ++sentences; });
    worker.start(); pump(100);
    if (in.live) {
        std::vector<float> floats(expected.size() / 2);
        for (std::size_t i = 0; i < floats.size(); ++i) floats[i] = qFromLittleEndian<qint16>(expected.constData() + i * 2) / 32767.0f;
        IqBuffer::BlockMetadata metadata; metadata.valid = true; metadata.centerFrequencyHz = in.centerHz;
        require(IqBuffer::publish(floats.data(), floats.size(), true, true, 0, &metadata), "Live test publication failed");
    }
    QByteArray got; QElapsedTimer timeout; timeout.start();
    while (got.size() < expected.size() && timeout.elapsed() < 2500) { pump(10); got += iqDevice->readAll(); }
    if (got != expected)
        std::cerr << "IQ mismatch: got " << got.size() << " bytes, expected " << expected.size()
                  << ", got=" << got.toHex().constData() << ", expected=" << expected.toHex().constData() << '\n';
    require(got == expected, "IQ stream order / sign / quantization changed");
    pump(30); require(sentences == 1, "NMEA loopback not delivered");
    worker.requestStop(); require(worker.wait(3000), "Worker cancellation timed out");
}
void pocketPipeSmoke() {
#ifdef Q_OS_WIN
    const QString executable = qEnvironmentVariable("OBRII_POCKETSDR_EXE");
    if (executable.isEmpty()) return;
    QLocalServer server;
    require(server.listen(QStringLiteral("obrii-gnss-test-%1")
                              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))),
            "PocketSDR named-pipe listen failed");
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= 0x08000000;
    });
    process.start(executable, {"-sig", "L1CA", "-prn", "1-8", "-fmt", "CS16",
                               "-f", "4", "-fo", "1575.42", "-ti", "0", server.fullServerName()});
    require(process.waitForStarted(5000), "PocketSDR process did not start");
    require(server.waitForNewConnection(5000), "PocketSDR did not open the Qt named pipe");
    std::unique_ptr<QLocalSocket> pipe(server.nextPendingConnection());
    require(bool(pipe), "PocketSDR named-pipe connection missing");
    const QByteArray block(512 * 1024, '\0');
    QElapsedTimer clock; clock.start();
    for (int i = 0; i < 32; ++i) {
        require(pipe->write(block) == block.size(), "PocketSDR pipe write failed");
        while (pipe->bytesToWrite() > 1024 * 1024)
            require(pipe->waitForBytesWritten(5000), "PocketSDR pipe stalled");
    }
    while (pipe->bytesToWrite() > 0)
        require(pipe->waitForBytesWritten(5000), "PocketSDR pipe drain stalled");
    require(clock.elapsed() < 5000, "PocketSDR pipe slower than the 4 MS/s live stream");
    process.kill(); process.waitForFinished(5000);
#endif
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        float input[]{1, -1, 2, -2, std::numeric_limits<float>::quiet_NaN(), 0.5f};
        const auto data = GnssSdr::floatToCs16(input, 6);
        require(qFromLittleEndian<qint16>(data.constData()) == 32767, "Positive sign");
        require(qFromLittleEndian<qint16>(data.constData()+2) == -32767, "Negative sign");
        require(qFromLittleEndian<qint16>(data.constData()+4) == 32767, "Upper clipping");
        require(qFromLittleEndian<qint16>(data.constData()+8) == 0, "NaN handling");
        GnssSdr::Input in; in.sampleRateHz = 4e6; in.centerHz = 1575.42e6;
        QString error;
        const auto args = GnssSdr::trackingArguments(in, {"GPS", "QZSS"}, 1234, 1235, &error);
        require(error.isEmpty() && args.contains("CS16"), "Valid tracking arguments");
        require(args.last() == "127.0.0.1:1234" && args.contains("127.0.0.1:1235"), "Backend cannot accidentally open hardware");
        require(GnssSdr::trackingArguments(in, {"GLONASS"}, 1, 2, &error).isEmpty() && !error.isEmpty(), "Out-of-band system must be rejected");
        in.centerHz = 1602e6; in.sampleRateHz = 12e6;
        require(GnssSdr::trackingArguments(in, {"GLONASS"}, 1, 2, &error).contains("-7,-6,-5,-4,-3,-2,-1,0,1,2,3,4,5,6"), "GLONASS FCNs");
        in.centerHz = 1575.42e6; in.sampleRateHz = 4e6;
        require(GnssSdr::trackingArguments(in, {"Galileo"}, 1, 2, &error).isEmpty(), "Galileo requires its full acquisition band");
        in.sampleRateHz = 50e6;
        require(GnssSdr::trackingArguments(in, {"GPS"}, 1, 2, &error).isEmpty(), "No silent rate conversion");
        require(GnssSdr::configureTrackingRate(in, {"GPS"}, &error) && in.decimation == 16,
                "50 MS/s GPS should use a 3.125 MS/s GNSS stream");
        require(GnssSdr::trackingArguments(in, {"GPS"}, 1, 2, &error).contains("3.125"),
                "PocketSDR must receive the decimated rate");
        in.sampleRateHz = 16e6;
        require(GnssSdr::configureTrackingRate(in, {"GPS"}, &error) && in.decimation == 4,
                "16 MS/s GPS should use a 4 MS/s GNSS stream");
        in.sampleRateHz = 8e6;
        require(GnssSdr::configureTrackingRate(in, {"GPS"}, &error) && in.decimation == 2,
                "8 MS/s GPS should use a 4 MS/s GNSS stream");
        in.sampleRateHz = 2.5e6;
        require(GnssSdr::configureTrackingRate(in, {"GPS"}, &error) && in.decimation == 1,
                "An already supported low IQ rate must not be rejected");
        in.sampleRateHz = 8e6;
        in.centerHz = 1500e6;
        require(!GnssSdr::configureTrackingRate(in, {"GPS"}, &error) && error.contains("1575.420"),
                "Out-of-band GPS must identify the center frequency, not blame sample rate");
        in.centerHz = 1575.42e6;
        GnssSdrDecimator decimator(4);
        std::vector<float> constantIq(128, 0.25f), firstHalf, secondHalf;
        decimator.process(constantIq.data(), 64, firstHalf);
        decimator.process(constantIq.data() + 64, 64, secondHalf);
        require(firstHalf.size() == 16 && secondHalf.size() == 16,
                "GNSS decimator must emit one complex sample per four input samples");
        for (float sample : secondHalf)
            require(std::abs(sample - 0.25f) < 0.0001f, "GNSS filter must preserve DC across IQ blocks");
        GnssSdrDecimator aliasFilter(4);
        std::vector<float> outOfBand(256);
        for (std::size_t i = 0; i < outOfBand.size() / 2; ++i)
            outOfBand[2 * i] = (i % 2 == 0) ? 0.5f : -0.5f;
        std::vector<float> filtered;
        aliasFilter.process(outOfBand.data(), outOfBand.size(), filtered);
        for (std::size_t i = 16; i < filtered.size(); i += 2)
            require(std::abs(filtered[i]) < 0.001f, "GNSS filter must reject an out-of-band tone before decimation");
        in.decimation = 1;
        in.sampleRateHz = 4e6;
        QTemporaryDir temporary; require(temporary.isValid(), "Temporary directory");
        in.live = false; in.path = temporary.filePath("capture.cs16"); in.format = "cs16";
        QFile raw(in.path); require(raw.open(QIODevice::WriteOnly) && raw.write(data) == data.size(), "Write fixture"); raw.close();
        require(GnssSdr::readInputMetadata(in, &error), "Read raw metadata"); streamTest(in, data);
        in.path = temporary.filePath("capture.wav");
        QFile wave(in.path); wave.open(QIODevice::WriteOnly); wave.write(fixedPcm16StereoWavHeader(4000000, data.size())); wave.write(data); wave.close();
        require(GnssSdr::readInputMetadata(in, &error) && in.dataOffset >= 44 && in.sampleRateHz == 4e6 && in.dataBytes == static_cast<quint64>(data.size()), "Stereo IQ WAV metadata");
        streamTest(in, data);
        in.live = true; in.centerHz = 1575.42e6; IqBuffer::clear(777);
        streamTest(in, data);
        require(IqBuffer::queuedBlocks() == 1, "GNSS must not steal audio queue blocks");
        std::vector<float> audio; require(IqBuffer::popBlock(audio) && audio.size() == 6, "Audio queue remains readable");
        pocketPipeSmoke();
        QFile translations(QCoreApplication::applicationDirPath() + "/../../../translations.json");
        if (!translations.open(QIODevice::ReadOnly)) translations.setFileName("translations.json");
        if (!translations.isOpen()) translations.open(QIODevice::ReadOnly);
        const auto language = QJsonDocument::fromJson(translations.readAll()).object().value("uk").toObject();
        const auto text = [language](const QString &key, const QString &fallback) { return language.value(key).toString(fallback); };
        GnssSdrPvtDialog gnss(text); gnss.ensurePolished(); gnss.layout()->activate(); pump(30);
        require(gnss.grab().save(temporary.filePath("gnss.png")), "GNSS dialog renders"); gnss.close();
        LrptDecodeDialog lrpt(text); lrpt.ensurePolished(); lrpt.layout()->activate(); pump(30);
        require(lrpt.grab().save(temporary.filePath("lrpt.png")), "LRPT dialog renders"); lrpt.close();
        const QString preview = qEnvironmentVariable("OBRII_BACKEND_PREVIEW_DIR");
        if (!preview.isEmpty()) { QDir().mkpath(preview); gnss.grab().save(preview + "/gnss.png"); lrpt.grab().save(preview + "/lrpt.png"); }
        std::cout << checks << " checks, 0 failures\n"; return 0;
    } catch (const std::exception &exception) { std::cerr << exception.what() << '\n'; return 1; }
}
