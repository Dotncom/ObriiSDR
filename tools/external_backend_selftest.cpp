#include "gnsssdrstream.h"
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtEndian>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0;
void require(bool okay, const char *message) { ++checks; if (!okay) throw std::runtime_error(message); }
void pump(int milliseconds) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < milliseconds) { QApplication::processEvents(); QThread::msleep(1); }
}
void streamTest(GnssSdr::Input in, const QByteArray &expected) {
    GnssSdrStream worker(in); QTcpSocket iq, nmea; int sentences = 0;
    QObject::connect(&worker, &GnssSdrStream::listening, &iq, [&](quint16 iqPort, quint16 nmeaPort) {
        iq.connectToHost(QHostAddress::LocalHost, iqPort); nmea.connectToHost(QHostAddress::LocalHost, nmeaPort);
        require(iq.waitForConnected(1000) && nmea.waitForConnected(1000), "Loopback stream connect failed");
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
    while (got.size() < expected.size() && timeout.elapsed() < 2500) { pump(10); got += iq.readAll(); }
    require(got == expected, "IQ stream order / sign / quantization changed");
    pump(30); require(sentences == 1, "NMEA loopback not delivered");
    worker.requestStop(); require(worker.wait(3000), "Worker cancellation timed out");
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
