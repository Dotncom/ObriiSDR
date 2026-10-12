#include "dmrcicdecimator.h"
#include "diagnosticlogging.h"
#include "dsdneobridge.h"
#include "dmrprivacyutils.h"
#include "wirelesspackettools.h"
#include "wirelesslabdialog.h"
#include "wirelesscaptureworker.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QDebug>
#include <QFontDatabase>
#include <QThread>
#include <QUdpSocket>
#include <QHostAddress>
#include <QDir>
#include <QTableWidget>
#include <QTabWidget>
#include <iostream>
#include <random>
#include <vector>
#include <csignal>
#include <cstring>
#include <functional>
#include <QTimer>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {
int failures = 0;
void check(bool condition, const char *name) {
    if (!condition) { qCritical() << "FAIL:" << name; ++failures; }
}
void testDsdDmrArguments() {
    for (int rate : {24000, 48000}) {
        const auto args = DsdNeoBridge::dmrProcessArguments(rate, 23457);
        check(args == QStringList({"-fs", "-i", "-", "-s", QString::number(rate),
                                   "-o", "udp:127.0.0.1:23457", "-mc"}),
              "Managed DSD uses the default slot-aware DMR decoder, not legacy mono");
    }
    const auto fallback = DsdNeoBridge::dmrProcessArguments(12345, 23456);
    check(fallback.value(fallback.indexOf("-s") + 1) == "24000",
          "DSD default input rate is supported PCM24k");
    const auto base = DsdNeoBridge::dmrProcessArguments(24000, 23456);
    check(normalizedDmrPrivacyMode(DMR_PRIVACY_HYTERA_BASIC) == DMR_PRIVACY_HYTERA_BASIC &&
          dmrPrivacyModeFromText(dmrPrivacyModeId(DMR_PRIVACY_HYTERA_BASIC)) == DMR_PRIVACY_HYTERA_BASIC,
          "Hytera Basic mode survives settings round trip");
    check(dmrPrivacyModeFromText("hytera-full") == DMR_PRIVACY_NONE,
          "Hytera Full is not misrepresented as Basic");
    for (int size : {10, 32, 64}) {
        const QString key(size, QLatin1Char('0'));
        check(isValidHyteraBasicKey(key), "All-zero Basic key retains explicit length");
        const auto args = DsdNeoBridge::privacyProcessArguments(base, "hytera-bp", key, true);
        QString cliKey = args.actual.value(args.actual.indexOf("-H") + 1);
        check(args.privacyError.isEmpty() && args.actual.count("-H") == 1 &&
              args.actual.count("-4") == 1 && !args.actual.contains("-1") &&
              !args.actual.contains("--dmr-force-algid"),
              "Basic uses Hytera key and Basic enforcement, never an AES ALG override");
        check(cliKey.count(' ') == (size - 1) / 16 && cliKey.remove(' ') == key,
              "Hytera word grouping preserves the complete key in a single CLI argument");
        check(!args.display.join(' ').contains(key) && args.display.join(' ').contains("redacted"),
              "Hytera command display masks key material");
    }
    for (const QString &bad : {QString("1234"), QString(64, 'G'), QString(66, 'A')}) {
        check(!isValidHyteraBasicKey(bad), "Invalid Basic key rejected without truncation");
        const auto args = DsdNeoBridge::privacyProcessArguments(base, "hytera-bp", bad, true);
        check(!args.privacyError.isEmpty() && !args.actual.contains("-H") && !args.actual.contains("-4"),
              "Invalid Basic key does not arm decryption");
    }
    check(isValidHyteraBasicKey("0x01:02:03:04:05") &&
          normalizedDmrPrivacyKeyHex("0x01:02:03:04:05") == "0102030405",
          "Key prefix and separators handled without adding an extra zero");
    const auto arc4 = DsdNeoBridge::privacyProcessArguments(base, "arc4", "0102030405", true);
    const auto aes = DsdNeoBridge::privacyProcessArguments(base, "aes256", QString(64, 'A'), true);
    check(arc4.actual == base + QStringList({"-1", "0102030405"}) &&
          aes.actual == base + QStringList({"-H", QString(64, 'A')}),
          "Working ARC4 and AES CLI arguments remain unchanged");
    const auto disabled = DsdNeoBridge::privacyProcessArguments(
        base + QStringList({"-4", "-H", "0102030405"}), "hytera-bp", "0102030405", false);
    check(disabled.actual == base && disabled.privacyMode == "none",
          "Disable removes key and Basic enforcement, including stale arguments");
}
QByteArray pcm16(const std::vector<int> &values) {
    QByteArray bytes;
    for (int value : values) {
        const auto word = static_cast<quint16>(value);
        bytes.append(static_cast<char>(word & 255));
        bytes.append(static_cast<char>(word >> 8));
    }
    return bytes;
}
bool waitFor(QApplication &app, const std::function<bool()> &ready, int timeoutMs = 2500) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < timeoutMs) {
        app.processEvents();
        QThread::msleep(2);
    }
    return ready();
}
template <typename T> class Reference {
public:
    explicit Reference(int r) : factor(r) {
        for (auto &b : delay) b.resize(r);
    }
    bool push(std::complex<T> input, std::complex<T> &output) {
        for (int s = 0; s < 4; ++s) {
            const auto old = delay[s][index];
            delay[s][index] = input;
            sums[s] += input - old;
            input = sums[s] * (T(1) / T(factor));
        }
        index = (index + 1) % factor;
        average += input;
        if (++count != factor) return false;
        output = average * (T(1) / T(factor));
        count = 0; average = {};
        return true;
    }
    int factor, index = 0, count = 0;
    std::array<std::vector<std::complex<T>>, 4> delay;
    std::array<std::complex<T>, 4> sums = {};
    std::complex<T> average = {};
};
void testDecimator() {
    std::mt19937 random(45);
    std::uniform_real_distribution<float> distribution(-0.6f, 0.6f);
    for (int factor : {1, 2, 13, 65, 128, 260, 520}) {
        Reference<double> reference(factor);
        DmrCicDecimator actual;
        actual.configure(factor);
        double worst = 0;
        int count = 0;
        for (int n = 0; n < 300000; ++n) {
            const std::complex<float> input(distribution(random), distribution(random));
            std::complex<double> expected;
            std::complex<float> output;
            const bool ready = reference.push(input, expected);
            check(ready == actual.push(input, output), "decimator cadence");
            if (ready) {
                worst = (std::max)(worst, std::abs(expected - std::complex<double>(output)));
                ++count;
            }
            // Same-factor configure calls must preserve state across arbitrary chunks.
            if (n % 997 == 0) actual.configure(factor);
        }
        check(worst < 0.00002, "decimator response equals old five-box filter");
        check(count == 300000 / factor, "decimator sample count");
        qInfo() << "CIC R=" << factor << "maxError=" << worst;
    }
    DmrCicDecimator constant;
    constant.configure(65);
    std::complex<float> value;
    for (int n = 0; n < 4000000; ++n) constant.push({0.125f, -0.25f}, value);
    check(std::abs(value - std::complex<float>(0.125f, -0.25f)) < 0.00001f, "CIC no long-run drift");
    for (int factor : {65, 128, 260, 520, 1300}) {
        DmrCicDecimator weak; weak.configure(factor);
        for (int n = 0; n < factor * 12; ++n) weak.push({0.000001f, -0.000001f}, value);
        check(std::abs(value - std::complex<float>(0.000001f, -0.000001f)) < 0.0000001f,
              "weak DMR input preserved at large decimation factors");
    }
    std::vector<std::complex<float>> input(3000000);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = {float(std::sin(i * 0.01) * 0.1), float(std::cos(i * 0.01) * 0.1)};
    Reference<float> reference(65);
    QElapsedTimer timer; timer.start();
    double sum = 0;
    for (auto sample : input) {
        std::complex<float> out;
        if (reference.push(sample, out)) sum += out.real();
    }
    const auto legacyNs = timer.nsecsElapsed();
    DmrCicDecimator fast; fast.configure(65); timer.restart();
    for (auto sample : input) {
        std::complex<float> out;
        if (fast.push(sample, out)) sum += out.real();
    }
    const auto fastNs = timer.nsecsElapsed();
    qInfo() << "CIC benchmark: referenceMs=" << legacyNs / 1.0e6
            << "optimizedMs=" << fastNs / 1.0e6
            << "speedup=" << double(legacyNs) / fastNs << "checksum=" << sum;
}
void testPackets() {
    QStringList fields = Wireless::tsharkFields();
    for (int i = 0; i < fields.size(); ++i) fields[i] = QString::number(i + 1);
    fields[7] = QStringLiteral("quoted \"SSID\"\\t value");
    QStringList quoted;
    for (QString field : fields) {
        field.replace(QStringLiteral("\""), QStringLiteral("\"\""));
        quoted << QLatin1Char('"') + field + QLatin1Char('"');
    }
    QStringList output;
    check(Wireless::parsePacketLine(quoted.join(QLatin1Char('\t')).toUtf8(), output) && output == fields,
          "TShark quoted TSV, doubled quotes, escaped controls");
    check(!Wireless::parsePacketLine(QByteArray("\"unterminated"), output), "reject malformed packet line");
    check(!Wireless::parsePacketLine(QByteArray(70000, 'x'), output), "packet line size bound");
    check(Wireless::wifiChannel(2484) == 14 && Wireless::wifiChannel(2412) == 1 &&
          Wireless::wifiChannel(5180) == 36 && Wireless::wifiChannel(5955) == 1 &&
          Wireless::wifiChannel(1000) == -1, "Wi-Fi channels");
    Wireless::BackendCapabilities c;
    Wireless::IqStreamDescriptor s;
    QString error;
    s.sampleRateHz = 50e6; s.usableBandwidthHz = 40e6;
    check(!Wireless::validateIqStream(c, s, 20e6, error), "packet-only backend rejects IQ");
    c.receivesIq = true;
    check(Wireless::validateIqStream(c, s, 20e6, error), "wideband backend accepts supported IQ");
    check(!Wireless::validateIqStream(c, s, 80e6, error), "80 MHz requires sufficient usable bandwidth");
    Wireless::PacketTxRequest r; r.frame = "test";
    check(!Wireless::validateTransmission(c, s, r, error), "receive-only TX rejected");
    c.transmitsPackets = true; c.timedTransmit = true;
    s.clockId = QStringLiteral("clock"); s.epoch = 8; s.continuous = true;
    r.timed = true; r.clockId = s.clockId; r.epoch = 7;
    check(!Wireless::validateTransmission(c, s, r, error), "stale epoch TX rejected");
    r.epoch = 8;
    check(Wireless::validateTransmission(c, s, r, error), "matching timed TX descriptor");
    r.simultaneousRxOnSameDevice = true;
    check(!Wireless::validateTransmission(c, s, r, error), "half-duplex concurrent RX rejected");
    c.fullDuplex = true;
    check(Wireless::validateTransmission(c, s, r, error), "confirmed full-duplex concurrent RX allowed");
}
void testCaptureWorker(QApplication &app) {
    auto *worker = new WirelessCaptureWorker;
    QThread thread;
    worker->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    thread.start();
    QMetaObject::invokeMethod(worker, [worker]() {
        worker->start(QCoreApplication::applicationFilePath(), {QStringLiteral("--fake-tshark")});
    }, Qt::QueuedConnection);
    QElapsedTimer elapsed; elapsed.start();
    quint64 total = 0, lost = 0;
    while (elapsed.elapsed() < 4000 && total + lost < 6000) {
        QThread::msleep(50);
        quint64 drops = 0;
        const auto rows = worker->takePackets(drops);
        check(rows.size() <= 2000, "worker queue bound");
        for (const auto &row : rows) check(row[7] == QStringLiteral("test SSID"), "worker packet parsing");
        total += rows.size(); lost += drops;
        app.processEvents();
    }
    check(total + lost == 6000, "worker stdout framing and overflow accounting");
    QMetaObject::invokeMethod(worker, [worker]() {
        worker->stop(); QThread::currentThread()->quit();
    }, Qt::QueuedConnection);
    check(thread.wait(3000), "packet worker shutdown");
    qInfo() << "Packet worker: delivered=" << total << "boundedDrops=" << lost;
}
void testDsdTransport() {
    QUdpSocket sink;
    check(sink.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)), "local UDP bind");
    auto *bridge = new DsdNeoBridge;
    QThread thread;
    bridge->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, bridge, &QObject::deleteLater);
    thread.start();
    QMetaObject::invokeMethod(bridge, [bridge, port = sink.localPort()]() {
        bridge->configureInputServer(true, port);
        bridge->configureUdpOutput(false, 23456, 2);
        bridge->setEnabled(true);
    }, Qt::BlockingQueuedConnection);
    std::vector<QByteArray> expected;
    for (int n = 0; n < 9; ++n) {
        std::vector<int> samples(480, 5000);
        samples[0] = n % 2 ? 32000 : 18000;
        samples[1] = -samples[0];
        const QByteArray frame = pcm16(samples);
        if (n >= 3) expected.push_back(frame);
        bridge->submitInputPcm(frame, 24000);
        QThread::msleep(10);
    }
    // GUI event loop is deliberately not pumped; transport must still run.
    int packets = 0;
    while (sink.hasPendingDatagrams()) {
        QByteArray packet; packet.resize(int(sink.pendingDatagramSize()));
        sink.readDatagram(packet.data(), packet.size());
        check(packet.size() == 960, "DSD 20 ms PCM16 packet");
        check(packets < int(expected.size()) && packet == expected[packets],
              "DSD symbol amplitudes unchanged by per-frame noise peaks");
        ++packets;
    }
    check(packets == 6, "DSD transport independent of GUI, three warmup frames");
    QMetaObject::invokeMethod(bridge, [bridge]() {
        bridge->setEnabled(false); QThread::currentThread()->quit();
    }, Qt::QueuedConnection);
    check(thread.wait(3000), "DSD worker shutdown");
}
QByteArray receiveDsdAudio(QApplication &app, const std::vector<QByteArray> &packets,
                         int channels = 1) {
    QUdpSocket reserve;
    check(reserve.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)), "audio port allocation");
    const auto port = reserve.localPort();
    reserve.close();
    DsdNeoBridge bridge;
    bridge.configureInputServer(false, 7355);
    bridge.configureUdpOutput(true, port, channels);
    QByteArray result;
    int received = 0;
    QObject::connect(&bridge, &DsdNeoBridge::decodedPcmReady, &bridge,
                     [&](const QByteArray &audio) { result += audio; ++received; });
    bridge.setEnabled(true);
    QUdpSocket sender;
    for (const auto &packet : packets) {
        check(sender.writeDatagram(packet, QHostAddress::LocalHost, port) == packet.size(),
              "synthetic decoded audio datagram");
        check(waitFor(app, [&]() { return received > 0; }), "decoded audio received");
        --received;
    }
    bridge.setEnabled(false);
    return result;
}
void testDsdAudioContinuity(QApplication &app) {
    std::vector<int> values(641);
    for (int i = 0; i < int(values.size()); ++i)
        values[i] = int(std::lround(18000 * std::sin(i * 0.37)));
    const QByteArray input = pcm16(values);
    const auto whole = receiveDsdAudio(app, {input});
    std::vector<QByteArray> chunks;
    for (int pos = 0; pos < input.size(); pos += 74) chunks.push_back(input.mid(pos, 74));
    const auto split = receiveDsdAudio(app, chunks);
    check(whole == split, "DSD resampling independent of UDP packet boundaries");
    std::vector<int> reference;
    int previous = values.front();
    for (int current : values) {
        for (int step = 1; step <= 6; ++step)
            reference.push_back((previous * (6 - step) + current * step) / 6);
        previous = current;
    }
    check(whole == pcm16(reference), "continuous 8k to 48k linear interpolation");
    check(whole.size() == input.size() * 6, "decoded PCM sample clock preserved");
    check(receiveDsdAudio(app, {pcm16({-9000, 9000})}, 2) == pcm16(std::vector<int>(6, 0)),
          "stereo channels averaged before resampling");
}
void testDsdProcessRestart() {
    auto *bridge = new DsdNeoBridge;
    QThread thread;
    std::mutex statusMutex;
    QStringList statuses;
    std::atomic<int> heartbeats{0};
    QObject::connect(bridge, &DsdNeoBridge::statusChanged, bridge,
                     [&](const QString &status) {
        std::lock_guard<std::mutex> lock(statusMutex);
        statuses.append(status);
    }, Qt::DirectConnection);
    const auto hasStatus = [&](const QString &text) {
        std::lock_guard<std::mutex> lock(statusMutex);
        for (const QString &status : statuses) if (status.contains(text)) return true;
        return false;
    };
    bridge->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, bridge, &QObject::deleteLater);
    thread.start();
    QMetaObject::invokeMethod(bridge, [bridge, &heartbeats]() {
        bridge->configureInputServer(false, 7355);
        bridge->configureUdpOutput(false, 23456, 1);
        bridge->configurePrivacy(QStringLiteral("arc4"), 1, QStringLiteral("1122334455"), true);
        bridge->configureProcess(true, QCoreApplication::applicationFilePath(),
                                 {QStringLiteral("--fake-dsd"), QStringLiteral("initial")});
        auto *heartbeat = new QTimer(bridge);
        QObject::connect(heartbeat, &QTimer::timeout, bridge, [&heartbeats]() { ++heartbeats; });
        heartbeat->start(20);
        bridge->setEnabled(true);
    }, Qt::BlockingQueuedConnection);
    QElapsedTimer startup; startup.start();
    while (!hasStatus("fake-dsd ready initial") && startup.elapsed() < 3000) QThread::msleep(10);
    check(hasStatus("fake-dsd ready initial"), "fake DSD process started");
    QElapsedTimer configure; configure.start();
    QMetaObject::invokeMethod(bridge, [bridge]() {
        bridge->configurePrivacy(QStringLiteral("arc4"), 2, QStringLiteral("A1B2C3D4E5"), true);
        bridge->configureProcess(true, QCoreApplication::applicationFilePath(),
                                 {QStringLiteral("--fake-dsd"), QStringLiteral("superseded")});
        bridge->configureProcess(true, QCoreApplication::applicationFilePath(),
                                 {QStringLiteral("--fake-dsd"), QStringLiteral("latest")});
    }, Qt::BlockingQueuedConnection);
    const auto configMs = configure.elapsed();
    check(configMs < 120, "DSD reconfiguration does not block transport thread");
    const int before = heartbeats.load();
    QThread::msleep(180);
    check(heartbeats.load() - before >= 3, "bridge event loop responsive during process termination");
    startup.restart();
    while (!hasStatus("fake-dsd ready latest") && startup.elapsed() < 3000) QThread::msleep(10);
    check(hasStatus("fake-dsd ready latest"), "restart uses latest process configuration");
    check(!hasStatus("fake-dsd ready superseded"), "intermediate configuration not launched");
    check(hasStatus("decoder material: <redacted>"), "backend key dump masked");
    check(!hasStatus("1122334455") && !hasStatus("A1B2C3D4E5"),
          "old and new keys redacted while restarting");
    QMetaObject::invokeMethod(bridge, [bridge]() {
        bridge->setEnabled(false);
    }, Qt::BlockingQueuedConnection);
    QThread::msleep(650);
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        int launches = 0;
        for (const QString &status : statuses) if (status.startsWith("Starting DSD-neo:")) ++launches;
        check(launches == 2, "disabled bridge does not restart child process");
    }
    QMetaObject::invokeMethod(bridge, []() { QThread::currentThread()->quit(); }, Qt::QueuedConnection);
    check(thread.wait(3000), "restarting bridge shutdown");
    std::cout << "DSD restart configureMs=" << configMs << "\n";
}
void testDsdStdinRecovery(){
    auto *bridge=new DsdNeoBridge;
    QThread thread;std::mutex mutex;QStringList statuses;
    QObject::connect(bridge,&DsdNeoBridge::statusChanged,bridge,[&](const QString &s){
        std::lock_guard<std::mutex> guard(mutex);statuses.append(s);
    },Qt::DirectConnection);
    const auto count=[&](const QString &text){
        std::lock_guard<std::mutex> guard(mutex);int n=0;for(const auto &s:statuses)if(s.contains(text))++n;return n;
    };
    const auto wait=[&](const QString &text,int expected,int limit){
        QElapsedTimer timer;timer.start();while(count(text)<expected&&timer.elapsed()<limit)QThread::msleep(10);
        return count(text)>=expected;
    };
    bridge->moveToThread(&thread);QObject::connect(&thread,&QThread::finished,bridge,&QObject::deleteLater);thread.start();
    QMetaObject::invokeMethod(bridge,[bridge](){
        bridge->configureUdpOutput(false,23456,1);
        bridge->configureProcess(true,QCoreApplication::applicationFilePath(),{"--fake-dsd-stdin","-i","-","-s","24000"});
        bridge->setEnabled(true);
    },Qt::BlockingQueuedConnection);
    check(wait("stdin ready",1,3000),"stdin decoder starts");
    bridge->submitInputPcm(pcm16(std::vector<int>(480,999)),48000);
    for(int i=0;i<9;++i){bridge->submitInputPcm(pcm16(std::vector<int>(480,100+i)),24000);QThread::msleep(25);}
    check(wait("stdin frame 8 value 108",1,3000),"stdin preserves all frames, including first frame, in order");
    check(count("stdin frame 0 value 100")==1&&count("stdin corrupt")==0,"Wrong-rate frame rejected without shifting PCM sequence");
    QMetaObject::invokeMethod(bridge,[bridge](){bridge->findChild<QProcess*>()->kill();},Qt::BlockingQueuedConnection);
    check(wait("stdin ready",2,3500),"Unexpected child crash automatically recovers on transport thread");
    check(count("restart backoff 1000 ms")==1&&count("restart backoff 2000 ms")==0,"Crash backoff is armed once, not twice for error and finished");
    QMetaObject::invokeMethod(bridge,[bridge](){bridge->findChild<QProcess*>()->kill();},Qt::BlockingQueuedConnection);
    check(wait("stdin ready",3,4500),"Repeated child crash recovers");
    check(count("restart backoff 2000 ms")==1,"Repeated crashes increase backoff instead of restarting in a tight loop");
    QMetaObject::invokeMethod(bridge,[bridge](){
        bridge->sendInputPcm(QByteArray(48000, char(0)),24000);
    },Qt::BlockingQueuedConnection);
    check(wait("input stalled",1,1000)&&wait("stdin ready",4,1500),"Oversized stdin backlog is bounded and decoder recovers");
    QMetaObject::invokeMethod(bridge,[bridge](){
        for(int i=0;i<40;++i)bridge->submitInputPcm(pcm16(std::vector<int>(480,200+i)),24000);
    },Qt::BlockingQueuedConnection);
    check(wait("PCM discontinuity",1,1000)&&wait("stdin ready",5,1500),"Bridge queue discontinuity resets decoder instead of splicing missing samples");
    QMetaObject::invokeMethod(bridge,[bridge](){bridge->setEnabled(false);},Qt::BlockingQueuedConnection);
    QThread::msleep(700);check(count("stdin ready")==5,"Disabled stdin bridge does not relaunch decoder");
    QMetaObject::invokeMethod(bridge,[](){QThread::currentThread()->quit();},Qt::QueuedConnection);
    check(thread.wait(3000),"Crashed/restarted stdin bridge destroys timers on owning thread");
}
}
int main(int argc, char **argv) {
    if(argc>1&&std::strcmp(argv[1],"--fake-dsd-stdin")==0){
#ifdef Q_OS_WIN
        _setmode(_fileno(stdin),_O_BINARY);
#endif
        std::cout<<"stdin ready"<<std::endl;
        for(int frame=0;frame<9;++frame){
            char pcm[960];if(!std::cin.read(pcm,sizeof(pcm)))return 1;
            const int value=int(static_cast<unsigned char>(pcm[0]))|(int(static_cast<unsigned char>(pcm[1]))<<8);
            bool intact=true;for(int i=0;i<480;++i)if(pcm[2*i]!=pcm[0]||pcm[2*i+1]!=pcm[1])intact=false;
            std::cout<<(intact?"stdin frame ":"stdin corrupt ")<<frame<<" value "<<value<<std::endl;
        }
        QThread::msleep(12000);return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--fake-dsd") == 0) {
        std::signal(SIGTERM, SIG_IGN);
        std::cout << "fake-dsd ready " << argv[2] << std::endl;
        const QByteArray key = qgetenv("FOBOS_DMR_PRIVACY_KEY_HEX");
        for (int n = 0; n < 160; ++n) {
            if (!key.isEmpty()) std::cout << "decoder material: " << key.constData() << std::endl;
            QThread::msleep(50);
        }
        return 0;
    }
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--fake-tshark"))) {
        for (int i = 1; i <= 6000; ++i) {
            QStringList fields = Wireless::tsharkFields();
            for (QString &field : fields) field.clear();
            fields[0] = QString::number(i); fields[1] = QStringLiteral("100.25");
            fields[7] = QStringLiteral("test SSID");
            const QByteArray line = fields.join(QLatin1Char('\t')).toUtf8();
            std::cout.write(line.constData(), line.size()); std::cout << '\n';
            if (i % 37 == 0) std::cout.flush();
        }
        return 0;
    }
    testDecimator();
    testPackets();
    testDsdDmrArguments();
    testDsdTransport();
    setFobosVerboseLoggingEnabled(true);
    testDsdTransport();
    setFobosVerboseLoggingEnabled(false);
    testDsdAudioContinuity(app);
    testDsdProcessRestart();
    testDsdStdinRecovery();
    testCaptureWorker(app);
    if (app.arguments().contains(QStringLiteral("--render"))) {
#ifdef Q_OS_WIN
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
        app.setFont(QFont(QStringLiteral("Segoe UI"), 9));
#endif
        for (bool uk : {false, true}) {
            WirelessLabDialog dialog(uk);
            dialog.resize(1050, 640); dialog.show(); app.processEvents();
            std::vector<float> f(4096), levels(4096);
            for (int i = 0; i < 4096; ++i) {
                f[i] = float(2412e6 - 25e6 + i * 50e6 / 4096);
                levels[i] = -90 + 20 * std::sin(i * 0.005);
            }
            dialog.appendSpectrum(f, levels, 50e6);
            QImage image(dialog.size(), QImage::Format_ARGB32);
            dialog.render(&image);
            check(image.save(QDir::current().filePath(uk ? QStringLiteral("wireless-uk.png")
                                                       : QStringLiteral("wireless-en.png"))), "render");
            dialog.findChild<QTabWidget *>()->setCurrentIndex(2);
            app.processEvents();
            dialog.render(&image);
            check(image.save(QDir::current().filePath(uk ? QStringLiteral("wireless-rf-uk.png")
                                                       : QStringLiteral("wireless-rf-en.png"))), "RF render");
        }
    }
    qInfo() << "Wireless/DMR selftest failures=" << failures;
    std::cout << "Wireless/DMR selftest failures=" << failures << std::endl;
    return failures ? 1 : 0;
}
