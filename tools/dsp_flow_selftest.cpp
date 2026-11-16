#include "dspflowpanel.h"
#include "multivfowidget.h"
#include "dspresearchwidget.h"
#include "signallabconstellation3d.h"
#include "MyWaterfallWidget.h"
#include "iqbuffer.h"
#include "dspcomputedialog.h"
#include "dspiqsourceregistry.h"
#include "dspboardqpskreceiver.h"
#include "../src/cli/clivirtualiqsource.h"
#include "virtualtransmitterbackend.h"
#include "audiofilterchainwidget.h"
#include "appsettingsutils.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsPathItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsView>
#include <QImage>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPointer>
#include <QSaveFile>
#include <QPushButton>
#include <QSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QThread>
#include <QTemporaryDir>
#include <QMouseEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUuid>

#include <iostream>
#include <array>
#include <cmath>
#include <random>
#include <stdexcept>
#ifdef _MSC_VER
#include <windows.h>
#include <dbghelp.h>
#endif

bool multiVfoPreviewHistorySelfTest();
bool dspBranchSelfTest();
bool dspBranchTeardownSelfTest();

// Keep profile import/recovery tests away from real per-user settings.
QString persistentSettingsFilePath() {
    static QTemporaryDir directory;
    if (!directory.isValid()) throw std::runtime_error("Temporary DSP settings directory failed");
    return directory.filePath(QStringLiteral("ObriiSDR.ini"));
}

struct DspFlowHistoryTestAccess {
    static void commit(DspFlowPanel &panel) { panel.commitBoardHistory(); }
    static QByteArray snapshot(const DspFlowPanel &panel) { return panel.captureBoardHistory(); }
    static void separateEdit(DspFlowPanel &panel) { panel.historyMergeTimer_.invalidate(); }
    static int count(const DspFlowPanel &panel) { return panel.boardHistory_.size(); }
    static std::shared_ptr<const DspBoardDecodedPayload> receivedPacket(const DspFlowPanel &panel, const QString &decoderId) {
        return panel.livePackets_.value(decoderId);
    }
    static qint64 bytes(const DspFlowPanel &panel) {
        qint64 bytes = 0;
        for (const auto &entry : panel.boardHistory_) bytes += entry.board.size() + entry.filters.size();
        return bytes;
    }
};

struct SignalLabConstellation3DTestAccess {
    static int points(const SignalLabConstellation3DWidget &widget) {
        int count = 0;
        for (const auto &primitive : widget.scene_) if (primitive.point) ++count;
        return count;
    }
    static int triangles(const SignalLabConstellation3DWidget &widget) {
        int count = 0;
        for (const auto &primitive : widget.scene_) if (primitive.surface) ++count;
        return count;
    }
};

namespace {
#ifdef _MSC_VER
LONG WINAPI reportCrash(EXCEPTION_POINTERS *exception) {
    const auto process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT context = *exception->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC = {context.Rip, 0, AddrModeFlat};
    frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
    frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
    std::cerr << "DSP test exception: " << std::hex << exception->ExceptionRecord->ExceptionCode << std::dec << '\n';
    for (int i = 0; i < 32; ++i) {
        char storage[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) std::cerr << symbol->Name;
        else std::cerr << "0x" << std::hex << frame.AddrPC.Offset << std::dec;
        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line); DWORD offset = 0;
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &offset, &line)) std::cerr << " " << line.FileName << ':' << line.LineNumber;
        std::cerr << '\n';
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
QString board(bool displays, int index = 0) {
    QJsonArray blocks{QJsonObject{{"id", "input"}, {"type", "receiver_source"}}};
    QJsonArray connections;
    if (displays) {
        blocks.append(QJsonObject{{"id", "spectrum"}, {"type", "workspace_spectrum"},
                                  {"width", 1200}, {"height", 200}});
        blocks.append(QJsonObject{{"id", "ruler"}, {"type", "workspace_ruler"},
                                  {"width", 1200}, {"height", 58}});
        blocks.append(QJsonObject{{"id", "channel"}, {"type", "vfo_channel"}, {"vfoIndex", index}});
        blocks.append(QJsonObject{{"id", "vfo"}, {"type", "vfo_spectrum"}, {"vfoIndex", 1}});
        connections.append(QJsonObject{{"source", "channel"}, {"target", "vfo"}});
    }
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"version", 7}, {"blocks", blocks},
                                                      {"connections", connections}}).toJson());
}

QJsonObject mainFftBoard(bool sourceConnected = true, bool viewsConnected = true,
                        const QString &sourceType = QStringLiteral("receiver_source")) {
    QJsonArray blocks{
        QJsonObject{{"id", "source"}, {"type", sourceType}},
        QJsonObject{{"id", "main"}, {"type", "fft"}, {"x", 200}},
        QJsonObject{{"id", "spectrum"}, {"type", "workspace_spectrum"}, {"x", 400}},
        QJsonObject{{"id", "waterfall"}, {"type", "workspace_waterfall"}, {"x", 400}, {"y", 250}},
        QJsonObject{{"id", "spectrum-settings"}, {"type", "spectrum_display"}, {"y", 150}},
        QJsonObject{{"id", "waterfall-settings"}, {"type", "waterfall_2d"}, {"y", 250}}};
    QJsonArray connections;
    if (sourceConnected) connections.append(QJsonObject{{"source", "source"}, {"target", "main"}});
    if (viewsConnected) {
        connections.append(QJsonObject{{"source", "main"}, {"target", "spectrum"}});
        connections.append(QJsonObject{{"source", "main"}, {"target", "waterfall"}});
    }
    connections.append(QJsonObject{{"source", "spectrum-settings"}, {"target", "spectrum"}});
    connections.append(QJsonObject{{"source", "waterfall-settings"}, {"target", "waterfall"}});
    return {{"version", 8}, {"blocks", blocks}, {"connections", connections}};
}

QString graphJson(const QJsonObject &graph) {
    return QString::fromUtf8(QJsonDocument(graph).toJson(QJsonDocument::Compact));
}

void testBoardSignalPath() {
    QJsonArray blocks{
        QJsonObject{{"id", "carrier"}, {"type", "lab_carrier"}, {"x", 0}, {"y", 0}},
        QJsonObject{{"id", "text"}, {"type", "lab_text"}, {"x", 0}, {"y", 120},
                    {"settings", QJsonObject{{"text", "Obrii"}}}},
        QJsonObject{{"id", "mod"}, {"type", "lab_modulator"}, {"x", 330},
                    {"settings", QJsonObject{{"mode", "bpsk"}}}},
        QJsonObject{{"id", "iq"}, {"type", "lab_iq"}, {"x", 660}},
        QJsonObject{{"id", "tx"}, {"type", "lab_tx"}, {"x", 660}, {"y", 220}},
        QJsonObject{{"id", "ddc"}, {"type", "ddc_channelizer"}, {"x", 990},
                    {"settings", QJsonObject{{"enabled", true}, {"outputRateHz", 24000}}}},
        QJsonObject{{"id", "fft"}, {"type", "branch_fft"}, {"x", 1200}},
        QJsonObject{{"id", "spectrum"}, {"type", "branch_spectrum"}, {"x", 1410}}};
    QJsonArray edges;
    const auto wire = [&](const char *from, const char *to) {
        edges.append(QJsonObject{{"source", from}, {"target", to}});
    };
    wire("carrier", "mod");
    wire("text", "mod");
    wire("mod", "iq");
    wire("iq", "tx");
    wire("iq", "ddc");
    wire("ddc", "fft");
    wire("fft", "spectrum");
    QJsonObject graph{{"version", 8}, {"blocks", blocks}, {"connections", edges}};

    DspFlowPanel panel;
    panel.resize(1200, 650);
    require(panel.setConfigurationJson(graphJson(graph)), "Laboratory board load failed");
    panel.show();
    QApplication::processEvents();
    const auto restored = QJsonDocument::fromJson(panel.configurationJson().toUtf8()).object();
    const auto request = resolveDspBoardSignal(restored, "iq");
    require(request.error.isEmpty() && !request.payload.isEmpty(), "Board lost either the carrier or payload wire");
    require(resolveDspBoardSignal(restored, "tx") == request, "TX did not resolve the same I/Q chain");
    const auto generated = generateDspBoardSignal(request);
    require(generated.ok() && generated.iq.size() == 48000, "Board text modulation failed");
    require(panel.setBlockSettings("text", {{"text", "Other"}}), "Inline payload settings were rejected");
    const auto edited = QJsonDocument::fromJson(panel.configurationJson().toUtf8()).object();
    const auto different = generateDspBoardSignal(resolveDspBoardSignal(edited, "iq"));
    require(different.ok() && different.iq != generated.iq, "Changing payload did not change the signal");
    const auto source = dspSignalSource("iq");
    const auto plan = compileDspGraph(edited);
    require(plan.errors.isEmpty() && plan.branches.size() == 1 && plan.branches[0].source == source,
            "DDC was not bound to the board I/Q source");
    auto *view = panel.findChild<QGraphicsView *>();
    require(view != nullptr, "Laboratory board view missing");
    view->fitInView(view->scene()->itemsBoundingRect().adjusted(-25, -25, 25, 25), Qt::KeepAspectRatio);
    QPushButton *run = nullptr;
    QPushButton *stop = nullptr;
    QPushButton *prepareTx = nullptr;
    for (auto *item : view->scene()->items()) {
        auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(item);
        if (!proxy || !proxy->widget()) continue;
        for (auto *button : proxy->widget()->findChildren<QPushButton *>()) {
            if (button->text() == "Run I/Q") run = button;
            if (button->text() == "Stop") stop = button;
            if (button->text() == "Prepare transmission") prepareTx = button;
        }
    }
    require(run && stop && prepareTx, "Inline I/Q controls missing");
    run->click();
    DspIqSourceSnapshot snapshot;
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 2000) {
        QApplication::processEvents();
        if (DspIqSourceRegistry::instance().snapshotSince(source, 0, 0, 4096, snapshot) &&
            panel.branchStatus("fft").startsWith("Fs ")) break;
        QThread::msleep(5);
    }
    require(!snapshot.iq.empty() && snapshot.sampleRateHz == 48000 && snapshot.centerHz == 100e6,
            "Board did not publish I/Q without a running receiver");
    require(panel.branchStatus("fft").startsWith("Fs "), "Laboratory signal did not reach the branch FFT");
    stop->click();
    require(!DspIqSourceRegistry::instance().snapshotSince(source, 0, 0, 4096, snapshot),
            "Stopping the board source left stale I/Q in the registry");
    bool txPrepared = false;
    QObject::connect(&panel, &DspFlowPanel::laboratoryTransmissionRequested, &panel,
                     [&](const SignalLabGenerationResult &result, double center) {
        txPrepared = result.iq == different.iq && center == 100e6;
    });
    prepareTx->click();
    deadline.restart();
    while (!txPrepared && deadline.elapsed() < 2000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(txPrepared, "TX output did not prepare the graph's samples and RF center");

    QTemporaryDir media;
    require(media.isValid(), "Unable to create board test media directory");
    const auto writeMedia = [&](const QString &name, const QByteArray &bytes) {
        QFile file(media.filePath(name));
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Unable to write board test media");
        return file.fileName();
    };
    const QString wav = writeMedia("tone.wav", QByteArray::fromHex(
        "524946462c00000057415645666d74201000000001000100401f0000803e000002001000"
        "646174610800000000000040000000c0"));
    auto audioRequest = request;
    audioRequest.modulator["settings"] = QJsonObject{{"mode", "am"}};
    audioRequest.payload = QJsonObject{{"type", "lab_audio"}, {"settings", QJsonObject{{"path", wav}}}};
    const auto audio = generateDspBoardSignal(audioRequest);
    require(audio.ok() && audio.iq.size() == 24 && audio.iq[6].real() > audio.iq[0].real(),
            "PCM16 WAV did not drive AM amplitude");
    const QString cf32 = writeMedia("sample.cf32", QByteArray::fromHex("0000003f000080be"));
    auto fileRequest = request;
    fileRequest.modulator = QJsonObject{{"type", "lab_cf32"}, {"settings", QJsonObject{{"path", cf32}}}};
    const auto imported = generateDspBoardSignal(fileRequest);
    require(imported.ok() && imported.iq.size() == 1 && imported.iq.front() == std::complex<float>(0.5f, -0.25f),
            "CF32LE import did not preserve the complex sample");

    auto packetRequest = request;
    packetRequest.modulator["settings"] = QJsonObject{{"mode", "packet_qpsk"}};
    packetRequest.carrier["settings"] = QJsonObject{{"offset", 4300}, {"amplitude", 0.3}};
    const QString message = QString::fromUtf8("Привіт, DSP!");
    packetRequest.payload["settings"] = QJsonObject{{"text", message}};
    auto packetSignal = generateDspBoardSignal(packetRequest);
    const auto packetText = decodeDspBoardPacket(packetSignal);
    require(packetText.ok() && packetText.type == DspBoardPayloadType::Text &&
            packetText.bytes == message.toUtf8(), "QPSK packet did not recover UTF-8 from shifted I/Q");
    auto corrupted = packetSignal;
    const int corruptStart = 116 * corrupted.samplesPerSymbol;
    for (int i = corruptStart; i < corruptStart + 8 * corrupted.samplesPerSymbol; ++i)
        corrupted.iq[i] = -corrupted.iq[i];
    require(decodeDspBoardPacket(corrupted).error == QStringLiteral("Packet integrity check failed"),
            "Decoder accepted a packet with corrupted samples");
    packetSignal.iq.resize(52 * packetSignal.samplesPerSymbol);
    require(!decodeDspBoardPacket(packetSignal).ok() && !decodeDspBoardPacket(generated).ok(),
            "Decoder accepted truncated or unframed I/Q");

    QBuffer imageBuffer;
    QImage image(2, 1, QImage::Format_RGB32);
    image.fill(Qt::red);
    require(imageBuffer.open(QIODevice::WriteOnly) && image.save(&imageBuffer, "PNG"), "Cannot create packet test image");
    const QByteArray imageBytes = imageBuffer.data();
    const QString imagePath = writeMedia("pixel.png", imageBytes);
    packetRequest.payload = QJsonObject{{"type", "lab_image"}, {"settings", QJsonObject{{"path", imagePath}}}};
    const auto packetImage = decodeDspBoardPacket(generateDspBoardSignal(packetRequest));
    require(packetImage.ok() && packetImage.type == DspBoardPayloadType::Image &&
            packetImage.fileName == "pixel.png" && packetImage.bytes == imageBytes,
            "QPSK packet did not recover original image bytes and name");
    QByteArray binary;
    for (int i = 0; i < 12000; ++i) binary.append(char(i % 256));
    const QString binaryPath = writeMedia("payload.bin", binary);
    packetRequest.iq["settings"] = QJsonObject{{"rate", 192000}, {"duration", 1}};
    packetRequest.modulator["settings"] = QJsonObject{{"mode", "packet_qpsk"}, {"symbols", 64000}};
    packetRequest.payload = QJsonObject{{"type", "lab_file"}, {"settings", QJsonObject{{"path", binaryPath}}}};
    const auto fileBurst = generateDspBoardSignal(packetRequest);
    const auto packetFile = decodeDspBoardPacket(fileBurst);
    require(packetFile.ok() && packetFile.type == DspBoardPayloadType::File &&
            packetFile.fileName == "payload.bin" && packetFile.bytes == binary,
            "QPSK packet did not recover the complete binary file");

    auto rxRequest = request;
    rxRequest.modulator["settings"] = QJsonObject{{"mode", "packet_qpsk"}, {"symbols", 1200}};
    rxRequest.payload["settings"] = QJsonObject{{"text", message}};
    const auto burst = generateDspBoardSignal(rxRequest);
    require(burst.ok(), "Could not generate live QPSK test burst");
    DspBoardQpskReceiver receiver;
    double receivedRate = 9600;
    constexpr double Tau = 6.2831853071795864769;
    std::mt19937 noise(199);
    std::normal_distribution<float> gaussian(0, 0.029f);
    std::vector<float> received;
    const auto append = [&](int count, double offset, const SignalLabGenerationResult *signal) {
        for (int i = 0; i < count; ++i) {
            std::complex<float> sample{};
            if (signal) {
                const double position = (i - 0.47) * signal->sampleRate / receivedRate * 1.0008;
                if (position >= 0 && position + 1 < signal->iq.size()) {
                    const int index = int(position);
                    sample = signal->iq[index] + float(position - index) * (signal->iq[index + 1] - signal->iq[index]);
                    sample *= std::polar(0.6f, float(1.3 + Tau * offset * i / receivedRate));
                }
            }
            received.push_back(sample.real() + gaussian(noise));
            received.push_back(sample.imag() + gaussian(noise));
        }
    };
    append(381, 0, nullptr);
    append(2800, 145, &burst);
    append(219, 0, nullptr);
    append(2800, -420, &burst);
    QVector<DspBoardDecodedPayload> livePackets;
    for (std::size_t i = 0; i < received.size() / 2; i += 137) {
        const auto count = std::min<std::size_t>(137, received.size() / 2 - i);
        livePackets += receiver.feed(received.data() + i * 2, count, receivedRate, 1200);
    }
    require(livePackets.size() == 2 && livePackets[0].bytes == message.toUtf8() &&
            livePackets[1].bytes == message.toUtf8(), "Streaming QPSK acquisition or tracking failed");

    const QByteArray driftFile = binary.left(2048);
    const QString driftPath = writeMedia("clock.bin", driftFile);
    rxRequest.payload = QJsonObject{{"type", "lab_file"}, {"settings", QJsonObject{{"path", driftPath}}}};
    rxRequest.iq["settings"] = QJsonObject{{"duration", 8}};
    const auto longBurst = generateDspBoardSignal(rxRequest);
    require(longBurst.ok(), "Could not generate long QPSK tracking test");
    received.clear();
    append(913, 0, nullptr);
    append(75000, 212, &longBurst);
    receiver.reset();
    livePackets.clear();
    for (std::size_t i = 0; i < received.size() / 2; i += 1021) {
        const auto count = std::min<std::size_t>(1021, received.size() / 2 - i);
        livePackets += receiver.feed(received.data() + i * 2, count, receivedRate, 1200);
    }
    require(livePackets.size() == 1 && livePackets[0].bytes == driftFile,
            "Live decoder lost symbol timing during a long packet");

    receivedRate = 192000;
    received.clear();
    append(187, 0, nullptr);
    append(156000, -6000, &fileBurst);
    receiver.reset();
    livePackets.clear();
    for (std::size_t i = 0; i < received.size() / 2; i += 1093) {
        const auto count = std::min<std::size_t>(1093, received.size() / 2 - i);
        livePackets += receiver.feed(received.data() + i * 2, count, receivedRate, 64000);
    }
    require(livePackets.size() == 1 && livePackets[0].bytes == binary,
            "Live decoder did not recover the high-rate file at three samples per symbol");

    blocks = {blocks[0], blocks[1], blocks[2], blocks[3]};
    blocks.append(QJsonObject{{"id", "decode"}, {"type", "lab_packet_decoder"}, {"x", 990}});
    blocks.append(QJsonObject{{"id", "received"}, {"type", "lab_received"}, {"x", 1320}});
    edges = {edges[0], edges[1], edges[2]};
    wire("iq", "decode");
    wire("decode", "received");
    graph["blocks"] = blocks;
    graph["connections"] = edges;
    require(panel.setConfigurationJson(graphJson(graph)), "Packet decoder graph did not load");
    require(panel.setBlockSettings("mod", {{"mode", "packet_qpsk"}}) &&
            panel.setBlockSettings("text", {{"text", message}}), "Cannot configure board packet source");
    run = nullptr;
    QPlainTextEdit *receivedText = nullptr;
    QPushButton *save = nullptr;
    for (auto *item : view->scene()->items()) {
        auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(item);
        if (!proxy || !proxy->widget()) continue;
        if (auto *preview = proxy->widget()->findChild<QPlainTextEdit *>("dspReceivedText"))
            receivedText = preview;
        for (auto *button : proxy->widget()->findChildren<QPushButton *>()) {
            if (button->text() == "Run I/Q") run = button;
            if (button->text() == QString::fromUtf8("Save received data…")) save = button;
        }
    }
    require(run && receivedText && save && !save->isEnabled(), "Packet output controls did not start idle");
    run->click();
    deadline.restart();
    while (receivedText->toPlainText() != message && deadline.elapsed() < 2000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(receivedText->toPlainText() == message && save->isEnabled(),
            "Wired packet did not reach the board output without a physical receiver");
    require(panel.setBlockSettings("text", {{"text", "Changed"}}) && receivedText->toPlainText().isEmpty() &&
            !save->isEnabled(), "Editing the packet source left a stale receive result");

    auto imageBlock = blocks[1].toObject();
    imageBlock["type"] = "lab_image";
    imageBlock["settings"] = QJsonObject{{"path", imagePath}};
    blocks[1] = imageBlock;
    graph["blocks"] = blocks;
    require(panel.setConfigurationJson(graphJson(graph)) &&
            panel.setBlockSettings("mod", {{"mode", "packet_qpsk"}}), "Cannot configure board image source");
    run = nullptr;
    QLabel *preview = nullptr;
    for (auto *item : view->scene()->items()) {
        auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(item);
        if (!proxy || !proxy->widget()) continue;
        if (auto *imageLabel = proxy->widget()->findChild<QLabel *>("dspReceivedImage"))
            preview = imageLabel;
        for (auto *button : proxy->widget()->findChildren<QPushButton *>())
            if (button->text() == "Run I/Q") run = button;
    }
    require(run && preview && preview->pixmap(Qt::ReturnByValue).isNull(), "Image output did not start empty");
    run->click();
    deadline.restart();
    while (preview->pixmap(Qt::ReturnByValue).isNull() && deadline.elapsed() < 2000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    const auto thumbnail = preview->pixmap(Qt::ReturnByValue).toImage();
    require(!thumbnail.isNull() && thumbnail.pixelColor(0, 0) == QColor(Qt::red),
            "Wired image was not rendered from the received bytes");

    const QJsonArray receiverBlocks{
        QJsonObject{{"id", "rx"}, {"type", "receiver_source"}},
        QJsonObject{{"id", "ddc"}, {"type", "ddc_channelizer"}, {"x", 220},
            {"settings", QJsonObject{{"enabled", true}, {"frequencyHz", 100010000},
                {"bandwidthHz", 4500}, {"outputRateHz", 12000}, {"firTaps", 128}}}},
        QJsonObject{{"id", "decode"}, {"type", "lab_packet_decoder"}, {"x", 440}},
        QJsonObject{{"id", "received"}, {"type", "lab_received"}, {"x", 770}}};
    edges = {};
    wire("rx", "ddc");
    wire("ddc", "decode");
    wire("decode", "received");
    graph["blocks"] = receiverBlocks;
    graph["connections"] = edges;
    require(panel.setConfigurationJson(graphJson(graph)), "Cannot configure receiver-to-QPSK graph");
    receivedText = nullptr;
    for (auto *item : view->scene()->items()) {
        auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(item);
        if (!proxy || !proxy->widget()) continue;
        if (auto *output = proxy->widget()->findChild<QPlainTextEdit *>("dspReceivedText")) receivedText = output;
    }
    require(receivedText, "Live QPSK output widget is missing");
    RadioSettings hardware;
    hardware.inputMode = INPUT_RF;
    hardware.centerFrequency = hardware.listeningFrequency = 100000000;
    hardware.sampleRate = burst.sampleRate;
    IqBuffer::clear(51000);
    IqBuffer::setSampleRateEstimate(hardware.sampleRate);
    panel.setBranchSourceContext(hardware, true);
    deadline.restart();
    while (deadline.elapsed() < 150) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    VirtualSignalEndpoint endpoint;
    endpoint.localName = QStringLiteral("obrii-qpsk-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    CliVirtualIqSource virtualReceiver;
    VirtualTransmitterBackend virtualTransmitter;
    virtualTransmitter.setEndpoint(endpoint);
    QString transportError;
    require(virtualReceiver.start(endpoint, quint32(hardware.sampleRate), qint64(hardware.centerFrequency),
                                  4 * 1024 * 1024, &transportError), qPrintable(transportError));
    TxConfiguration txConfiguration;
    txConfiguration.sampleRate = int(hardware.sampleRate);
    txConfiguration.frequencyHz = hardware.centerFrequency;
    require(virtualTransmitter.start(txConfiguration, &transportError), qPrintable(transportError));
    IqBuffer::BlockMetadata metadata;
    metadata.valid = true;
    metadata.centerFrequencyHz = hardware.centerFrequency;
    unsigned int receivedSamples = 0;
    bool published = true;
    virtualReceiver.onDataAvailable = [&] {
        std::array<float, 4096> iq{};
        while (const auto count = virtualReceiver.read(iq.data(), unsigned(iq.size() / 2))) {
            if (virtualReceiver.takeDiscontinuity()) IqBuffer::clear();
            published &= IqBuffer::publish(iq.data(), count * 2, false, true, 0, &metadata);
            receivedSamples += count;
        }
    };
    const auto transmit = [&](const SignalLabGenerationResult &signal, int prefix, double offset) {
        require(signal.ok(), qPrintable(signal.error));
        QVector<std::complex<float>> wideIq(prefix + signal.iq.size());
        for (int i = 0; i < signal.iq.size(); ++i) {
            const float phase = float(0.9 + Tau * (10000 + offset) * i / hardware.sampleRate);
            wideIq[prefix + i] = signal.iq[i] * std::polar(1.0f, phase);
        }
        constexpr int chunkSamples = 2048;
        for (int i = 0; i < wideIq.size(); i += chunkSamples) {
            const int count = std::min(chunkSamples, wideIq.size() - i);
            require(virtualTransmitter.writeIq(wideIq.constData() + i, count, &transportError), qPrintable(transportError));
            QApplication::processEvents();
            QThread::msleep(3);
        }
        require(virtualTransmitter.drain(1000, &transportError), qPrintable(transportError));
        return wideIq.size();
    };
    const QString benchDirectory = qEnvironmentVariable("OBRII_QPSK_BENCH_DIR");
    const auto readFixture = [](const QString &path) {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "Cannot open virtual TX fixture");
        return file.readAll();
    };
    const QString expectedText = benchDirectory.isEmpty() ? message :
        QString::fromUtf8(readFixture(QDir(benchDirectory).filePath("tx-text.txt")));
    rxRequest.iq["settings"] = QJsonObject{{"duration", 1}};
    rxRequest.payload = QJsonObject{{"type", "lab_text"}, {"settings", QJsonObject{{"text", expectedText}}}};
    const int textSamples = transmit(generateDspBoardSignal(rxRequest), 1800, 145);
    deadline.restart();
    while (receivedText->toPlainText() != expectedText && deadline.elapsed() < 2500) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(receivedText->toPlainText() == expectedText, "Virtual receiver/DDC graph did not deliver the QPSK text");
    const QByteArray recoveredText = receivedText->toPlainText().toUtf8();

    const QString testFile = benchDirectory.isEmpty() ? writeMedia("loopback.bin", binary.left(96)) :
        QDir(benchDirectory).filePath("tx-payload.bin");
    const QByteArray expected = readFixture(testFile);
    rxRequest.payload = QJsonObject{{"type", "lab_file"}, {"settings", QJsonObject{{"path", testFile}}}};
    const auto filePacket = generateDspBoardSignal(rxRequest);
    const int fileSamples = transmit(filePacket, 2173, -205);
    std::shared_ptr<const DspBoardDecodedPayload> receivedFile;
    deadline.restart();
    while (deadline.elapsed() < 2500) {
        QApplication::processEvents();
        receivedFile = DspFlowHistoryTestAccess::receivedPacket(panel, "decode");
        if (receivedFile && receivedFile->ok() && receivedFile->type == DspBoardPayloadType::File) break;
        QThread::msleep(5);
    }
    const QString expectedName = benchDirectory.isEmpty() ? QStringLiteral("loopback.bin") : QStringLiteral("tx-payload.bin");
    require(receivedFile && receivedFile->ok() && receivedFile->type == DspBoardPayloadType::File &&
            receivedFile->bytes == expected && receivedFile->fileName == expectedName,
            "Virtual receiver/DDC graph did not recover the file byte for byte");
    const auto transport = virtualReceiver.status();
    require(published && receivedSamples == unsigned(textSamples + fileSamples) &&
            transport.value("droppedSamples").toInt() == 0 && transport.value("rejected").toInt() == 0,
            "Virtual transport dropped or rejected QPSK samples");
    if (!benchDirectory.isEmpty()) {
        const auto saveRecovered = [&](const QString &name, const QByteArray &bytes) {
            QSaveFile output(QDir(benchDirectory).filePath(name));
            require(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit(),
                    "Could not save the data recovered by the board");
        };
        saveRecovered("rx-text.txt", recoveredText);
        saveRecovered("rx-payload.bin", receivedFile->bytes);
    }
    std::cout << "Virtual QPSK: " << transport.value("frames").toInt() << " frames, "
              << receivedSamples << " I/Q samples, text verified, file " << receivedFile->bytes.size()
              << " bytes SHA-256 " << QCryptographicHash::hash(receivedFile->bytes, QCryptographicHash::Sha256).toHex().constData()
              << '\n';
    virtualTransmitter.stop();
    panel.setBranchSourceContext(hardware, false);
    panel.close();
    IqBuffer::clear();
}

QJsonObject mainFftAliasBoard(bool bindingIds = false) {
    auto graph = mainFftBoard(true, false);
    auto blocks = graph["blocks"].toArray();
    if (bindingIds) {
        for (int i = 0; i < blocks.size(); ++i) {
            auto block = blocks[i].toObject();
            const auto id = block["id"].toString();
            if (id.startsWith("spectrum") || id.startsWith("waterfall"))
                block["settings"] = QJsonObject{{"bindingId", id.startsWith("spectrum") ? "S" : "W"}};
            blocks[i] = block;
        }
    }
    graph["blocks"] = blocks;
    auto edges = graph["connections"].toArray();
    if (bindingIds) edges = QJsonArray{edges.first()};
    edges.append(QJsonObject{{"source", "main"}, {"target", "spectrum-settings"}});
    edges.append(QJsonObject{{"source", "main"}, {"target", "waterfall-settings"}});
    graph["connections"] = edges;
    return graph;
}

void testMainFftAliases() {
    for (const auto *type : {"spectrum_display", "waterfall_2d", "waterfall_3d"})
        require(dspLinkKind("fft", type) == DspLinkKind::Spectrum, "FFT settings alias is not executable");
    for (bool bindingIds : {false, true}) {
        auto graph = mainFftAliasBoard(bindingIds);
        auto blocks = graph["blocks"].toArray();
        auto extraView = blocks[2].toObject();
        extraView["id"] = "spectrum-copy";
        blocks.append(extraView);
        graph["blocks"] = blocks;
        if (!bindingIds) {
            auto edges = graph["connections"].toArray();
            edges.append(QJsonObject{{"source", "spectrum-settings"}, {"target", "spectrum-copy"}});
            graph["connections"] = edges;
        }
        const auto plan = compileDspGraph(graph);
        require(plan.errors.isEmpty() && plan.mainSpectrumRoutes.size() == 3 &&
                plan.mainDisplayControllers.value("spectrum-copy") == "spectrum-settings" &&
                plan.mainSpectrumRoutes.value("spectrum-copy") == "main" &&
                plan.explicitMainSpectrumViews.size() == 3, "Settings alias fanout failed");
        require(plan.branches.isEmpty() && plan.executionKey == compileDspGraph(mainFftBoard()).executionKey,
                "Settings alias created/restarted branch processing");
    }
    auto graph = mainFftAliasBoard(true);
    auto blocks = graph["blocks"].toArray();
    blocks.append(QJsonObject{{"id", "waterfall-3d"}, {"type", "waterfall_3d"},
                             {"settings", QJsonObject{{"bindingId", "W"}}}});
    graph["blocks"] = blocks;
    auto plan = compileDspGraph(graph);
    require(plan.errors.isEmpty() && plan.mainSpectrumRoutes.value("waterfall") == "main" &&
            plan.mainDisplayControllers.value("waterfall") == "waterfall-3d",
            "Mixed 2D/3D binding broke existing settings selection or shared FFT");
    auto edges = graph["connections"].toArray();
    edges.append(QJsonObject{{"source", "main"}, {"target", "waterfall-3d"}});
    graph["connections"] = edges;
    require(compileDspGraph(graph).errors.isEmpty(), "Same FFT through two controllers was rejected");
    auto legacy = graph;
    legacy["connections"] = QJsonArray{};
    plan = compileDspGraph(legacy);
    require(plan.errors.isEmpty() && plan.explicitMainSpectrumViews.isEmpty(),
            "Mixed legacy controllers disabled implicit display feed");
    auto invalid = graph;
    blocks.append(QJsonObject{{"id", "other-fft"}, {"type", "fft"}});
    invalid["blocks"] = blocks;
    edges.removeLast();
    edges.append(QJsonObject{{"source", "source"}, {"target", "other-fft"}});
    edges.append(QJsonObject{{"source", "other-fft"}, {"target", "waterfall-3d"}});
    invalid["connections"] = edges;
    plan = compileDspGraph(invalid);
    require(!plan.errors.isEmpty() && plan.mainSpectrumRoutes.isEmpty() &&
            plan.explicitMainSpectrumViews.contains("waterfall"), "Conflicting alias sources remained active");
    invalid = mainFftAliasBoard();
    edges = invalid["connections"].toArray();
    edges.append(QJsonObject{{"source", "main"}, {"target", "spectrum-settings"}});
    invalid["connections"] = edges;
    require(!compileDspGraph(invalid).errors.isEmpty(), "Duplicate alias IQ input accepted");
    invalid = mainFftAliasBoard();
    edges = invalid["connections"].toArray();
    edges.append(QJsonObject{{"source", "spectrum-settings"}, {"target", "spectrum"}});
    invalid["connections"] = edges;
    require(!compileDspGraph(invalid).errors.isEmpty(), "Duplicate display settings edge accepted");
}

void testMainFftAliasDelivery() {
    DspFlowPanel panel;
    panel.show();
    const auto frames = [&]() {
        QApplication::processEvents();
        QThread::msleep(25);
        return panel.shouldUpdateWorkspaceSpectrum();
    };
    for (bool bindingIds : {false, true}) {
        require(panel.setConfigurationJson(graphJson(mainFftAliasBoard(bindingIds))), "Alias board load failed");
        require(frames(), "Complete alias route did not admit frames");
        auto persisted = QJsonDocument::fromJson(panel.configurationJson().toUtf8()).object();
        for (const auto &value : persisted["blocks"].toArray()) {
            auto block = value.toObject();
            if (block["type"] == "workspace_spectrum" || block["type"] == "workspace_waterfall")
                require(block["settings"].toObject()["mainFftRouteExplicit"].toBool(), "Explicit admission was not persisted");
        }
        for (int cut = 0; cut < 3; ++cut) {
            auto disconnected = persisted;
            QJsonArray remaining;
            for (const auto &value : persisted["connections"].toArray()) {
                const auto source = value.toObject()["source"].toString();
                if ((cut == 0 && source == "source") || (cut == 1 && source == "main") ||
                    (cut == 2 && source.endsWith("-settings"))) continue;
                remaining.append(value);
            }
            disconnected["connections"] = remaining;
            if (cut == 2 && bindingIds) {
                auto blocks = disconnected["blocks"].toArray();
                for (int i = 0; i < blocks.size(); ++i) {
                    auto block = blocks[i].toObject();
                    auto settings = block["settings"].toObject();
                    settings.remove("bindingId");
                    block["settings"] = settings;
                    blocks[i] = block;
                }
                disconnected["blocks"] = blocks;
            }
            require(panel.setConfigurationJson(graphJson(disconnected)) && !frames(),
                    "Disconnected alias silently reverted to legacy frames");
        }
        require(panel.setConfigurationJson(graphJson(persisted)), "Alias restore failed");
        auto *scene = panel.findChild<QGraphicsView *>()->scene();
        for (auto *item : scene->items())
            if (dynamic_cast<QGraphicsPathItem *>(item) && item->flags().testFlag(QGraphicsItem::ItemIsSelectable))
                item->setSelected(true);
        QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(scene, &remove);
        DspFlowHistoryTestAccess::commit(panel);
        require(!frames(), "Deleted alias route continued frame delivery");
        panel.undoBoard();
        require(frames(), "Undo did not recover alias delivery");
        panel.redoBoard();
        require(!frames(), "Redo did not remove alias delivery");
    }
    require(panel.setConfigurationJson(graphJson(mainFftBoard(false, false))) && frames(),
            "Unwired legacy views lost their automatic feed");
    panel.hide();
}

void testExperimentLayoutImport() {
    struct ProfilesGuard {
        QSettings settings{persistentSettingsFilePath(), QSettings::IniFormat};
        QMap<QString, QVariant> saved;
        ProfilesGuard() {
            settings.beginGroup("dspFlow/profiles");
            for (const auto &key : settings.allKeys()) saved.insert(key, settings.value(key));
            settings.endGroup();
        }
        ~ProfilesGuard() {
            settings.remove("dspFlow/profiles");
            settings.beginGroup("dspFlow/profiles");
            for (auto it = saved.cbegin(); it != saved.cend(); ++it) settings.setValue(it.key(), it.value());
            settings.endGroup(); settings.sync();
        }
    } guard;
    DspFlowPanel panel;
    require(panel.setConfigurationJson(graphJson(mainFftBoard())), "Import starting layout failed");
    const auto before = DspFlowHistoryTestAccess::snapshot(panel);
    int rfCommands = 0;
    QObject::connect(&panel, &DspFlowPanel::controlTriggered, [&]() { ++rfCommands; });
    QString error;
    const bool importedOk = panel.importExperimentLayout(graphJson(mainFftAliasBoard()), &error);
    if (!importedOk || !error.isEmpty())
        throw std::runtime_error(QStringLiteral("Experiment layout import failed: %1 [settings: %2]")
            .arg(error, persistentSettingsFilePath()).toStdString());
    QComboBox *profiles = nullptr;
    int backupIndex = -1;
    for (auto *combo : panel.findChildren<QComboBox *>()) {
        const int index = combo->findData("Before experiment import");
        if (index >= 0) { profiles = combo; backupIndex = index; break; }
    }
    require(profiles && backupIndex > 0 && profiles->currentIndex() == 0,
            "Backup is not a visible ordinary profile, or imported layout is mislabeled");
    const auto imported = DspFlowHistoryTestAccess::snapshot(panel);
    auto invalid = mainFftAliasBoard();
    auto blocks = invalid["blocks"].toArray();
    auto bad = blocks[0].toObject(); bad["x"] = 100001; blocks[0] = bad; invalid["blocks"] = blocks;
    require(!panel.importExperimentLayout(graphJson(invalid), &error) && !error.isEmpty() &&
            DspFlowHistoryTestAccess::snapshot(panel) == imported, "Invalid import changed the board");
    profiles->setCurrentIndex(backupIndex);
    QMetaObject::invokeMethod(profiles, "activated", Qt::DirectConnection, Q_ARG(int, backupIndex));
    require(DspFlowHistoryTestAccess::snapshot(panel) == before, "Backup profile did not recover the original layout");
    const QJsonObject empty{{"version", 8}, {"blocks", QJsonArray{}}, {"connections", QJsonArray{}}};
    require(panel.setConfigurationJson(graphJson(empty)), "Explicit empty board cannot be loaded");
    if (!panel.importExperimentLayout(graphJson(mainFftAliasBoard()), &error))
        throw std::runtime_error(QStringLiteral("Empty board backup failed: %1").arg(error).toStdString());
    const int updatedIndex = profiles->findData("Before experiment import");
    profiles->setCurrentIndex(updatedIndex);
    QMetaObject::invokeMethod(profiles, "activated", Qt::DirectConnection, Q_ARG(int, updatedIndex));
    require(QJsonDocument::fromJson(panel.configurationJson().toUtf8()).object()["blocks"].toArray().isEmpty(),
            "Updated backup did not restore an empty board");
    require(rfCommands == 0, "Layout import/recovery issued an RF control command");
}

void testBoardHistory() {
    DspFlowPanel panel;
    require(panel.setConfigurationJson(graphJson(mainFftBoard())), "History fixture load failed");
    auto *view = panel.findChild<QGraphicsView *>();
    auto *scene = view->scene();
    const auto snapshot = [&]() { return DspFlowHistoryTestAccess::snapshot(panel); };
    const auto commit = [&]() { DspFlowHistoryTestAccess::commit(panel); };
    const auto initial = snapshot();
    int controls = 0;
    QObject::connect(&panel, &DspFlowPanel::controlTriggered, [&]() { ++controls; });
    require(!panel.canUndoBoard() && !panel.canRedoBoard(), "Loaded board retained unrelated history");
    auto *undo = panel.findChild<QToolButton *>("dspUndo");
    auto *redo = panel.findChild<QToolButton *>("dspRedo");
    require(undo && redo && !undo->isEnabled() && !redo->isEnabled(), "History buttons missing or initially enabled");

    panel.addDdcBranch();
    commit();
    const auto withBranch = snapshot();
    require(withBranch != initial && DspFlowHistoryTestAccess::count(panel) == 2, "Compound DDC addition was not one history step");
    undo->click();
    require(snapshot() == initial && panel.canRedoBoard(), "Undo addition did not preserve IDs/links/settings");
    redo->click();
    require(snapshot() == withBranch, "Redo addition changed branch IDs or connections");

    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    for (auto *item : scene->items()) if (item->flags().testFlag(QGraphicsItem::ItemIsSelectable)) item->setSelected(true);
    QApplication::sendEvent(scene, &remove);
    commit();
    require(QJsonDocument::fromJson(snapshot()).object()["blocks"].toArray().isEmpty(), "Delete-all history fixture failed");
    panel.undoBoard();
    require(snapshot() == withBranch, "Undo delete-all did not restore group and links");
    panel.redoBoard();
    require(QJsonDocument::fromJson(snapshot()).object()["blocks"].toArray().isEmpty(), "Redo cannot restore an empty board");
    panel.undoBoard();

    const auto originalSettings = panel.blockSettings("spectrum-settings");
    for (int i = 0; i < 20; ++i) {
        auto settings = originalSettings;
        settings.insert("minimumDbfs", -160 + i);
        require(panel.setBlockSettings("spectrum-settings", settings), "History settings fixture failed");
        commit();
    }
    require(!panel.canRedoBoard(), "A new edit did not discard redo history");
    panel.undoBoard();
    require(panel.blockSettings("spectrum-settings") == originalSettings, "Continuous settings were not coalesced");
    panel.redoBoard();
    require(panel.blockSettings("spectrum-settings")["minimumDbfs"].toInt() == -141, "Redo lost final coalesced value");
    require(controls == 0, "Board history invoked live run/enable controls");

    // Each release records the completed drag, not its intermediate positions.
    scene->clearSelection();
    QGraphicsItem *moved = nullptr;
    for (auto *item : scene->items()) if (item->flags().testFlag(QGraphicsItem::ItemIsSelectable) &&
        !dynamic_cast<QGraphicsPathItem *>(item)) { moved = item; break; }
    require(moved != nullptr, "No block available for history drag test");
    const auto beforeMove = snapshot();
    for (int i = 0; i < 20; ++i) moved->moveBy(1, 2);
    QGraphicsSceneMouseEvent released(QEvent::GraphicsSceneMouseRelease);
    released.setButton(Qt::LeftButton);
    QApplication::sendEvent(scene, &released);
    commit();
    const auto afterMove = snapshot();
    panel.undoBoard();
    require(snapshot() == beforeMove, "Undo drag lost placement");
    panel.redoBoard();
    require(snapshot() == afterMove, "Redo drag lost placement");

    QComboBox *sectors = nullptr;
    for (auto *combo : panel.findChildren<QComboBox *>()) if (combo->findData("3x1") >= 0) { sectors = combo; break; }
    require(sectors, "Sector selector missing");
    const auto beforeSectors = snapshot();
    sectors->setCurrentIndex(sectors->findData("3x1"));
    commit();
    const auto afterSectors = snapshot();
    panel.undoBoard();
    require(snapshot() == beforeSectors, "Undo sector split changed layout");
    panel.redoBoard();
    require(snapshot() == afterSectors && panel.workspaceSectorCount() == 3, "Redo sector split failed");

    // Widget-local shortcuts work on the canvas, not in embedded text editors.
    const auto beforeShortcut = snapshot();
    panel.addDdcBranch(); commit();
    const auto afterShortcut = snapshot();
    QKeyEvent undoKey(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    QApplication::sendEvent(view, &undoKey);
    require(snapshot() == beforeShortcut, "Canvas Ctrl+Z failed");
    QKeyEvent redoKey(QEvent::KeyPress, Qt::Key_Y, Qt::ControlModifier);
    QApplication::sendEvent(view, &redoKey);
    require(snapshot() == afterShortcut, "Canvas Ctrl+Y failed");
    panel.undoBoard();
    QKeyEvent shiftRedo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QApplication::sendEvent(view, &shiftRedo);
    require(snapshot() == afterShortcut, "Canvas Ctrl+Shift+Z failed");
    QLineEdit edit;
    edit.setText("a"); edit.insert("b");
    QApplication::sendEvent(&edit, &undoKey);
    require(edit.text() == "a" && snapshot() == afterShortcut, "Text undo changed board history");

    require(panel.setConfigurationJson(graphJson(mainFftBoard())), "History limit fixture failed");
    for (int i = 0; i < 110; ++i) {
        DspFlowHistoryTestAccess::separateEdit(panel);
        panel.setBlockSettings("spectrum-settings", {{"minimumDbfs", -i}}); commit();
    }
    require(DspFlowHistoryTestAccess::count(panel) == 101, "Board history exceeded 100 undo steps");
    const QString payload(256 * 1024, QLatin1Char('x'));
    for (int i = 0; i < 75; ++i) {
        DspFlowHistoryTestAccess::separateEdit(panel);
        panel.setBlockSettings("spectrum-settings", {{"minimumDbfs", -i}, {"historyTestPayload", payload}}); commit();
    }
    require(DspFlowHistoryTestAccess::bytes(panel) <= 16 * 1024 * 1024 && panel.canUndoBoard(), "Board history memory is unbounded");
    require(!panel.configurationJson().contains("boardHistory"), "Undo history leaked into persisted board");
    std::cout << "DSP board history: group/empty delete, IDs/links, coalescing, drag/sectors, shortcuts and bounds passed\n";
}

void testBoardFilterHistory() {
    AudioFilterChainWidget chain;
    auto first = defaultAudioFilterStage(AudioFilterKind::Gain);
    first.id = "unrelated"; first.gainDb = 3;
    auto owned = defaultAudioFilterStage(AudioFilterKind::Notch);
    owned.id = "owned-filter"; owned.enabled = false; owned.frequencyHz = 1234; owned.q = 4.7;
    chain.setConfigurationJson(encodeAudioFilterChain({first, owned}));
    DspFlowPanel panel;
    QJsonObject graph{{"version", 8}, {"blocks", QJsonArray{
        QJsonObject{{"id", "source"}, {"type", "receiver_source"}},
        QJsonObject{{"id", owned.id}, {"type", "filter_notch"}, {"x", 300}}}}, {"connections", QJsonArray{}}};
    require(panel.setConfigurationJson(graphJson(graph)), "Filter history graph load failed");
    panel.setMainAudioFilterChainWidget(&chain);
    QObject::connect(&panel, &DspFlowPanel::blockRemoved, &chain, [&](const QString &, const QString &id) { chain.removeStageById(id); });
    auto *scene = panel.findChild<QGraphicsView *>()->scene();
    for (auto *item : scene->items()) if (item->flags().testFlag(QGraphicsItem::ItemIsSelectable) && item->scenePos().x() > 200) item->setSelected(true);
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(scene, &remove);
    DspFlowHistoryTestAccess::commit(panel);
    require(decodeAudioFilterChain(chain.configurationJson()).size() == 1, "Deleted filter stage stayed active");
    first.gainDb = 11;
    chain.setConfigurationJson(encodeAudioFilterChain({first}));
    panel.undoBoard();
    auto stages = decodeAudioFilterChain(chain.configurationJson());
    require(stages.size() == 2 && stages[0].gainDb == 11 && stages[1].id == owned.id &&
        !stages[1].enabled && stages[1].frequencyHz == 1234 && stages[1].q == 4.7,
        "Filter undo lost original parameters/order or overwrote unrelated live changes");
    panel.redoBoard();
    require(decodeAudioFilterChain(chain.configurationJson()).size() == 1, "Filter redo failed to remove stage");
    chain.setConfigurationJson(encodeAudioFilterChain({first, owned}));
    panel.undoBoard();
    require(decodeAudioFilterChain(chain.configurationJson()).size() == 2, "Filter undo duplicated an existing ID");
    std::cout << "DSP board history: main filter membership/parameters/order and same-ID preservation passed\n";
}

void testSelectionDeletion() {
    DspFlowPanel panel;
    panel.show();
    auto *view = panel.findChild<QGraphicsView *>();
    require(view && view->scene(), "Board scene unavailable for deletion test");
    auto *scene = view->scene();
    const auto selectItems = [scene](bool blocks, bool edges) {
        scene->clearSelection();
        for (QGraphicsItem *item : scene->items()) {
            if (!item->flags().testFlag(QGraphicsItem::ItemIsSelectable)) continue;
            item->setSelected(dynamic_cast<QGraphicsPathItem *>(item) ? edges : blocks);
        }
    };
    const auto graph = [&panel]() {
        return QJsonDocument::fromJson(panel.configurationJson().toUtf8()).object();
    };
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    for (int cycle = 0; cycle < 32; ++cycle) {
        require(panel.setConfigurationJson(graphJson(mainFftBoard())), "Deletion graph load failed");
        QApplication::processEvents();
        panel.updateWorkspaceSpectrum({100, 101, 102, 103}, {-120, -100, -40, -90}, 101.5, 102, 4, 1, 7);
        selectItems(true, true);
        require(scene->selectedItems().size() == 11, "Deletion must select six blocks and five edges");
        QKeyEvent remove(QEvent::KeyPress, cycle % 2 ? Qt::Key_Backspace : Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(scene, &remove);
        const auto remaining = QJsonDocument::fromJson(panel.configurationJson().toUtf8()).object();
        require(remaining["blocks"].toArray().isEmpty() && remaining["connections"].toArray().isEmpty(),
                "Group deletion left blocks or dangling connections");
        QApplication::processEvents();
        panel.updateWorkspaceSpectrum({100, 101, 102, 103}, {-120, -100, -40, -90}, 101.5, 102, 4, 1, 7);
        QApplication::sendEvent(scene, &remove);
    }
    // Removing only connections must leave all endpoint widgets alive.
    require(panel.setConfigurationJson(graphJson(mainFftBoard())), "Edge deletion graph load failed");
    selectItems(false, true);
    require(scene->selectedItems().size() == 5, "Edge-only selection is wrong");
    QApplication::sendEvent(scene, &remove);
    require(graph()["blocks"].toArray().size() == 6 && graph()["connections"].toArray().isEmpty(),
            "Edge-only deletion removed blocks");
    selectItems(true, false);
    QApplication::sendEvent(scene, &remove);
    require(graph()["blocks"].toArray().isEmpty(), "Block-only deletion failed");

    // A partial deletion must not recreate or remove the surviving components.
    require(panel.setConfigurationJson(graphJson(mainFftBoard())), "Partial deletion graph load failed");
    QSet<QGraphicsItem *> survivors;
    for (QGraphicsItem *item : scene->items())
        if (item->flags().testFlag(QGraphicsItem::ItemIsSelectable) && !dynamic_cast<QGraphicsPathItem *>(item))
            survivors.insert(item);
    require(survivors.size() == 6, "Partial deletion block count is wrong");
    QGraphicsItem *victim = *survivors.begin();
    survivors.remove(victim);
    victim->setSelected(true);
    QApplication::sendEvent(scene, &remove);
    require(graph()["blocks"].toArray().size() == 5, "Partial deletion removed unrelated blocks");
    const auto liveItems = scene->items();
    for (QGraphicsItem *item : survivors) require(liveItems.contains(item), "Deletion recreated a surviving block");
    require(compileDspGraph(graph()).errors.isEmpty(), "Partial deletion left a dangling graph reference");

    // Delete/reset while the mouse is still holding a block or drawing a connection.
    const QJsonObject single{{"version", 8}, {"blocks", QJsonArray{QJsonObject{{"id", "source"}, {"type", "receiver_source"}}}},
                            {"connections", QJsonArray{}}};
    for (int gesture = 0; gesture < 2; ++gesture) for (int reset = 0; reset < 2; ++reset) {
        require(panel.setConfigurationJson(graphJson(single)), "Interaction graph load failed");
        selectItems(true, false);
        require(scene->selectedItems().size() == 1, "Interaction test needs exactly one source");
        auto *source = scene->selectedItems().front();
        const auto position = source->mapToScene(gesture == 0 ? QPointF(172, 34) : QPointF(86, 20));
        const int count = scene->items().size();
        QGraphicsSceneMouseEvent press(QEvent::GraphicsSceneMousePress);
        press.setButton(Qt::LeftButton); press.setButtons(Qt::LeftButton);
        press.setScenePos(position); press.setButtonDownScenePos(Qt::LeftButton, position);
        QApplication::sendEvent(scene, &press);
        if (gesture == 0) require(scene->items().size() == count + 1, "Pending connection was not started");
        if (reset) require(panel.setConfigurationJson(graphJson(single)), "Reset during interaction failed");
        else QApplication::sendEvent(scene, &remove);
        QGraphicsSceneMouseEvent move(QEvent::GraphicsSceneMouseMove);
        move.setButtons(Qt::LeftButton); move.setScenePos(position + QPointF(30, 30));
        QApplication::sendEvent(scene, &move);
        QGraphicsSceneMouseEvent release(QEvent::GraphicsSceneMouseRelease);
        release.setButton(Qt::LeftButton); release.setScenePos(position + QPointF(30, 30));
        QApplication::sendEvent(scene, &release);
        require(graph()["blocks"].toArray().size() == reset && graph()["connections"].toArray().isEmpty(),
                "Late mouse event changed the graph after delete/reset");
        QApplication::processEvents();
    }

    panel.setWorkspaceMode(true);
    require(panel.setConfigurationJson(graphJson(mainFftBoard())), "Docked deletion graph load failed");
    QPointer<QDialog> settings = new QDialog;
    settings->setWindowTitle("Deletion test settings");
    require(panel.dockSettingsDialog("spectrum-settings", settings), "Cannot dock deletion test settings");
    selectItems(true, true);
    QApplication::sendEvent(scene, &remove);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(settings.isNull() && graph()["blocks"].toArray().isEmpty(), "Docked settings outlived their deleted block");
    QApplication::processEvents();
    std::cout << "DSP deletion: 32 mixed selections, edge-only, partial, pending gestures and docked settings passed\n";
}

void testExplicitDspRateGeometry() {
    const auto graph = [](const QString &sourceType, const QJsonObject &sourceSettings,
                          const QJsonObject &ddcSettings) {
        return QJsonObject{
            {"blocks", QJsonArray{
                QJsonObject{{"id", "source"}, {"type", sourceType}, {"settings", sourceSettings}},
                QJsonObject{{"id", "ddc"}, {"type", "ddc_channelizer"}, {"settings", ddcSettings}},
                QJsonObject{{"id", "good"}, {"type", "ddc_channelizer"},
                            {"settings", QJsonObject{{"enabled", true}, {"outputRateHz", 16000}}}}}},
            {"connections", QJsonArray{
                QJsonObject{{"source", "source"}, {"target", "ddc"}},
                QJsonObject{{"source", "source"}, {"target", "good"}}}}};
    };
    const QJsonObject lab{{"rate", 48000}};
    const QJsonObject excessive{{"enabled", true}, {"outputRateHz", 96000}};
    const auto conflict = compileDspGraph(graph("lab_iq", lab, excessive));
    require(conflict.errors.isEmpty() && conflict.branchInputErrors.size() == 1 &&
            conflict.branchInputErrors.value("ddc").contains("48000") && conflict.branches.size() == 2 &&
            !conflict.branches[0].enabled && conflict.branches[1].enabled,
            "An impossible Lab IQ rate must disable only the affected DDC");
    const auto decimation = compileDspGraph(graph("lab_iq", lab,
        {{"enabled", true}, {"outputRateHz", 12001}, {"decimation", 4}}));
    require(decimation.branchInputErrors.value("ddc").contains("12000") && !decimation.branches[0].enabled,
            "Manual decimation rate limit was ignored");
    const auto boundary = compileDspGraph(graph("lab_iq", lab,
        {{"enabled", true}, {"outputRateHz", 12000}, {"decimation", 4}}));
    require(boundary.branchInputErrors.isEmpty() && boundary.branches[0].enabled,
            "Valid manual decimation was rejected at the exact rate limit");
    for (const auto &legacy : {graph("lab_iq", {}, excessive),
                               graph("lab_iq", lab, {{"enabled", true}}),
                               graph("receiver_source", lab, excessive), graph("network_input", lab, excessive)}) {
        const auto plan = compileDspGraph(legacy);
        require(plan.errors.isEmpty() && plan.branchInputErrors.isEmpty() &&
                plan.branches.size() == 2 && plan.branches[0].enabled,
                "Unspecified or runtime input geometry was treated as an incompatible link");
    }
}

void testMainFftGraph() {
    require(dspLinkKind("receiver_source", "fft") == DspLinkKind::Iq &&
            dspLinkKind("iq_source", "fft") == DspLinkKind::Iq, "Main FFT input is not typed IQ");
    require(dspLinkKind("fft", "workspace_spectrum") == DspLinkKind::Spectrum &&
            dspLinkKind("fft", "workspace_waterfall") == DspLinkKind::Spectrum,
            "Main FFT output is not typed spectrum");
    require(dspLinkKind("channel_filter", "fft") == DspLinkKind::Documentation &&
            dspLinkKind("channel_filter", "demodulator") == DspLinkKind::Documentation,
            "Legacy diagram link became executable");
    for (const auto &source : {QStringLiteral("receiver_source"), QStringLiteral("iq_source")}) {
        const auto plan = compileDspGraph(mainFftBoard(true, true, source));
        require(plan.errors.isEmpty() && plan.mainFftSources.size() == 1 &&
                plan.mainFftSources.value("main") == "source", "Main FFT source mapping failed");
        require(plan.mainSpectrumRoutes.size() == 2 && plan.mainSpectrumRoutes.value("spectrum") == "main" &&
                plan.mainSpectrumRoutes.value("waterfall") == "main", "Main FFT display mapping failed");
        require(plan.branches.isEmpty() && plan.spectrumRoutes.isEmpty() && plan.researchRoutes.isEmpty(),
                "Main FFT bindings created DDC workers/routes");
    }
    const auto incomplete = compileDspGraph(mainFftBoard(false, true));
    require(incomplete.errors.isEmpty() && incomplete.mainFftSources.isEmpty() &&
            incomplete.mainSpectrumRoutes.isEmpty(), "Disconnected main FFT still has executable routes");
    const auto unwired = compileDspGraph(mainFftBoard(true, false));
    require(unwired.errors.isEmpty() && unwired.mainFftSources.size() == 1 && unwired.mainSpectrumRoutes.isEmpty(),
            "Compiler invented routes for unwired legacy displays");
    const auto documentary = compileDspGraph(mainFftBoard(true, true, "channel_filter"));
    require(documentary.errors.isEmpty() && documentary.mainFftSources.isEmpty() &&
            documentary.mainSpectrumRoutes.isEmpty(), "Unsupported legacy source activated main FFT route");

    auto combined = mainFftBoard();
    auto blocks = combined["blocks"].toArray();
    blocks.append(QJsonObject{{"id", "ddc"}, {"type", "ddc_channelizer"},
                             {"settings", QJsonObject{{"enabled", true}}}});
    blocks.append(QJsonObject{{"id", "branch"}, {"type", "branch_fft"}});
    blocks.append(QJsonObject{{"id", "branch-view"}, {"type", "branch_spectrum"}});
    combined["blocks"] = blocks;
    const QJsonArray branchEdges{
        QJsonObject{{"source", "source"}, {"target", "ddc"}},
        QJsonObject{{"source", "ddc"}, {"target", "branch"}},
        QJsonObject{{"source", "branch"}, {"target", "branch-view"}}};
    auto edges = combined["connections"].toArray();
    for (const auto &edge : branchEdges) edges.append(edge);
    combined["connections"] = edges;
    const auto combinedPlan = compileDspGraph(combined);
    auto ddcOnly = combined;
    ddcOnly["connections"] = branchEdges;
    const auto ddcPlan = compileDspGraph(ddcOnly);
    require(combinedPlan.errors.isEmpty() && combinedPlan.branches.size() == 1 &&
            combinedPlan.spectrumRoutes.value("branch-view") == "branch" &&
            combinedPlan.mainSpectrumRoutes.size() == 2 &&
            combinedPlan.executionKey == ddcPlan.executionKey && combinedPlan.spectrumRoutes == ddcPlan.spectrumRoutes,
            "Main FFT bindings changed DDC execution or routes");

    const auto requireInvalid = [](const QJsonObject &graph) {
        const auto plan = compileDspGraph(graph);
        require(!plan.errors.isEmpty() && plan.mainFftSources.isEmpty() && plan.mainSpectrumRoutes.isEmpty() &&
                plan.branches.isEmpty() && plan.spectrumRoutes.isEmpty(), "Invalid main FFT graph remained executable");
    };
    for (const auto &edge : QJsonArray{
             QJsonObject{{"source", "main"}, {"target", "ddc"}},
             QJsonObject{{"source", "branch"}, {"target", "spectrum"}},
             QJsonObject{{"source", "missing"}, {"target", "main"}},
             QJsonObject{{"source", "main"}, {"target", "main"}}}) {
        auto invalid = combined;
        auto invalidEdges = edges;
        invalidEdges.append(edge);
        invalid["connections"] = invalidEdges;
        requireInvalid(invalid);
    }
    auto multiple = combined;
    blocks.append(QJsonObject{{"id", "other-source"}, {"type", "iq_source"}});
    multiple["blocks"] = blocks;
    edges.append(QJsonObject{{"source", "other-source"}, {"target", "main"}});
    multiple["connections"] = edges;
    requireInvalid(multiple);
    multiple = combined;
    blocks = combined["blocks"].toArray();
    blocks.append(QJsonObject{{"id", "other-main"}, {"type", "fft"}});
    multiple["blocks"] = blocks;
    edges = combined["connections"].toArray();
    edges.append(QJsonObject{{"source", "source"}, {"target", "other-main"}});
    edges.append(QJsonObject{{"source", "other-main"}, {"target", "spectrum"}});
    multiple["connections"] = edges;
    requireInvalid(multiple);
}

void testMainFftBoardRouting() {
    DspFlowPanel panel;
    const auto load = [&panel](const QJsonObject &graph, bool framesExpected) {
        require(panel.setConfigurationJson(graphJson(graph)), "Main FFT board load failed");
        panel.show();
        QApplication::processEvents();
        // Permit the normal 16 ms display throttle between route-cache transitions.
        QThread::msleep(25);
        require(panel.shouldUpdateWorkspaceSpectrum() == framesExpected, "Main FFT board frame routing mismatch");
        require(panel.boundWorkspaceSettingsBlockId("spectrum") == "spectrum-settings" &&
                panel.boundWorkspaceSettingsBlockId("waterfall") == "waterfall-settings",
                "Main FFT signal wiring removed a display settings binding");
        panel.updateWorkspaceSpectrum({100, 101, 102, 103}, {-120, -100, -40, -90}, 101.5, 102, 4, 1, 7);
    };
    load(mainFftBoard(), true);
    require(compileDspGraph(QJsonDocument::fromJson(panel.configurationJson().toUtf8()).object())
                .mainSpectrumRoutes.size() == 2, "Board loading dropped FFT-to-view output links");
    require(panel.bindWorkspaceSettingsBlock("spectrum-settings", "spectrum"), "Settings rebinding failed");
    require(compileDspGraph(QJsonDocument::fromJson(panel.configurationJson().toUtf8()).object())
                .mainSpectrumRoutes.size() == 2, "Settings rebinding removed main FFT signal links");
    auto reverseOrder = mainFftBoard();
    QJsonArray reversed;
    const auto connections = reverseOrder["connections"].toArray();
    for (int i = connections.size()-1; i >= 0; --i) reversed.append(connections[i]);
    reverseOrder["connections"] = reversed;
    load(reverseOrder, true);
    load(mainFftBoard(false, true), false);
    load(mainFftBoard(true, true, "channel_filter"), false);
    load(mainFftBoard(false, false), true);
    load(mainFftBoard(), true);
    auto invalid = mainFftBoard();
    auto invalidEdges = invalid["connections"].toArray();
    invalidEdges.append(QJsonObject{{"source", "missing"}, {"target", "main"}});
    invalid["connections"] = invalidEdges;
    const QString before = panel.configurationJson();
    require(!panel.setConfigurationJson(graphJson(invalid)) && panel.configurationJson() == before,
            "Rejected main FFT configuration replaced the live board");
    panel.hide();
}

void testSpectrumComputePreferences() {
    DspFlowPanel panel;
    require(panel.setConfigurationJson(graphJson(mainFftBoard())), "Preference test board load failed");
    require(panel.spectrumComputeOptions() == SpectrumComputeOptions{}, "Unexpected default main FFT preferences");
    int preferenceSignals = 0, endpointSignals = 0, statusSignals = 0;
    QObject::connect(&panel, &DspFlowPanel::spectrumComputeOptionsChanged, &panel, [&]() { ++preferenceSignals; });
    QObject::connect(&panel, &DspFlowPanel::externalEndpointChanged, &panel, [&]() { ++endpointSignals; });
    QObject::connect(&panel, &DspFlowPanel::spectrumComputeStatusChanged, &panel, [&](const QString &) { ++statusSignals; });
    const auto beforeEndpoint = panel.configurationJson();
    DspStreamEndpoint endpoint;
    endpoint.enabled = true; endpoint.address = QStringLiteral("192.0.2.47"); endpoint.port = 17358;
    endpoint.token = QStringLiteral("selftest-session-token-never-persist-0123456789");
    panel.setExternalEndpoint(endpoint);
    panel.setExternalEndpoint(endpoint);
    panel.setSpectrumComputeStatus(QStringLiteral("runtime-status-not-a-preference"));
    panel.setSpectrumComputeStatus(QStringLiteral("runtime-status-not-a-preference"));
    require(endpointSignals == 1 && statusSignals == 1 && panel.externalEndpoint() == endpoint &&
            panel.configurationJson() == beforeEndpoint, "Endpoint, permission or runtime status entered board JSON");

    SpectrumComputeOptions options;
    options.mode = SpectrumComputeMode::Continuous; options.backend = 2;
    options.iqFormat = DspIqFormat::BlockFloat16Le;
    options.uploadLimitMBps = 321; options.fallbackToLocal = true;
    panel.setSpectrumComputeOptions(options);
    panel.setSpectrumComputeOptions(options);
    require(preferenceSignals == 1 && panel.spectrumComputeOptions() == options, "Preference setter/signal contract failed");
    const auto saved = panel.configurationJson();
    auto object = QJsonDocument::fromJson(saved.toUtf8()).object();
    require(object["spectrumCompute"].toObject() == QJsonObject{{"mode", 2}, {"backend", 2}, {"iqFormat", 3},
                {"uploadLimitMBps", 321}, {"fallbackToLocal", true}}, "Unexpected persisted main FFT fields");
    require(!saved.contains(endpoint.token) && !saved.contains(endpoint.address) &&
            !saved.contains("runtime-status-not-a-preference"), "Session-only values serialized");
    endpoint.enabled = false; endpoint.token.clear();
    panel.setExternalEndpoint(endpoint);
    require(panel.configurationJson() == saved, "Transfer permission affected serialized preferences");

    DspFlowPanel restored;
    int restoreSignals = 0;
    QObject::connect(&restored, &DspFlowPanel::spectrumComputeOptionsChanged, &restored, [&]() { ++restoreSignals; });
    require(restored.setConfigurationJson(saved) && restored.spectrumComputeOptions() == options &&
            restored.externalEndpoint() == DspStreamEndpoint{} && restored.spectrumComputeStatus().isEmpty() && restoreSignals == 1,
            "Preference restore lost options or restored credentials/permission/status");
    object["externalEndpoint"] = QJsonObject{{"enabled", true}, {"token", "injected-session-token"}, {"address", "192.0.2.99"}};
    object["spectrumCompute"] = QJsonObject{{"mode", 99}, {"backend", -1}, {"uploadLimitMBps", 9000},
                                          {"token", "injected-main-token"}, {"enabled", true}};
    require(restored.setConfigurationJson(graphJson(object)), "Preference validation rejected valid board geometry");
    auto normalized = SpectrumComputeOptions{};
    normalized.uploadLimitMBps = 2000;
    require(restored.spectrumComputeOptions() == normalized && restored.externalEndpoint() == DspStreamEndpoint{} &&
            !restored.configurationJson().contains("injected-") && !restored.configurationJson().contains("externalEndpoint"),
            "Untrusted preference data granted transfer or bypassed normalization");
    const auto cleanPreferences = QJsonDocument::fromJson(restored.configurationJson().toUtf8()).object()["spectrumCompute"].toObject();
    require(cleanPreferences.size() == 5 && !cleanPreferences.contains("enabled") && restoreSignals == 2,
            "Preference restore retained permission fields or missed its change signal");
    normalized.uploadLimitMBps = -1;
    restored.setSpectrumComputeOptions(normalized);
    require(restored.spectrumComputeOptions().uploadLimitMBps == 1, "Upload budget lower bound was not enforced");
    object.remove("spectrumCompute"); object.remove("externalEndpoint");
    require(restored.setConfigurationJson(graphJson(object)) && restored.spectrumComputeOptions() == SpectrumComputeOptions{} && restoreSignals == 4,
            "Legacy layout did not restore safe main FFT defaults");

    QString closedConfiguration;
    QObject::connect(&panel, &DspFlowPanel::configurationChanged, &panel,
                     [&](const QString &json) { closedConfiguration = json; });
    panel.show(); panel.close();
    require(closedConfiguration == panel.configurationJson() &&
            QJsonDocument::fromJson(closedConfiguration.toUtf8()).object()["spectrumCompute"] ==
                QJsonDocument::fromJson(saved.toUtf8()).object()["spectrumCompute"],
            "Board close discarded main FFT preferences");
}

void testBoard() {
    DspFlowPanel panel;
    require(panel.setConfigurationJson(board(true)), "board loading failed");
    panel.show();
    QApplication::processEvents();
    require(panel.shouldUpdateWorkspaceSpectrum(), "workspace frame consumers missing");
    require(!panel.shouldUpdateWorkspaceSpectrum(), "workspace throttle failed");
    require(panel.shouldUpdateMultiVfoSpectrum(), "VFO frame consumers missing");
    require(panel.workspaceSpectrumTargetBins() >= 512, "DSP display width is invalid");
    panel.showMinimized();
    QApplication::processEvents();
    require(!panel.shouldUpdateWorkspaceSpectrum() && !panel.shouldUpdateMultiVfoSpectrum(),
            "minimized board still requests frames");
    require(panel.workspaceSpectrumTargetBins() == 0, "minimized board still requests display width");
    panel.showNormal();
    QApplication::processEvents();
    const std::vector<float> frequencies{100, 101, 102, 103};
    const std::vector<float> levels{-120, -100, -40, -90};
    for (int repeat = 0; repeat < 20; ++repeat) {
        require(panel.setConfigurationJson(board(true, repeat % 2)), "board reload failed");
        require(panel.vfoIndexForBlock("vfo") == repeat % 2, "connected VFO index failed");
        panel.updateWorkspaceSpectrum(frequencies, levels, 101.5, 102, 4, 1, 7);
        panel.updateMultiVfoSpectrum(R"({"channels":[{"frequencyHz":101,"bandwidthHz":2},{"frequencyHz":102,"bandwidthHz":2}]})",
                                    frequencies, levels);
        require(panel.assignVfoIndexToBlock("channel", 1), "VFO assignment failed");
        panel.updateMultiVfoSpectrum(R"({"channels":[{"frequencyHz":101,"bandwidthHz":2},{"frequencyHz":102,"bandwidthHz":2}]})",
                                    frequencies, levels);
        // Remove all frame consumers after populating the route cache.
        require(panel.setConfigurationJson(board(false)), "consumer removal failed");
        QThread::msleep(35);
        require(!panel.shouldUpdateWorkspaceSpectrum(), "removed workspace consumers retained");
        require(!panel.shouldUpdateMultiVfoSpectrum(), "removed VFO consumers retained");
        panel.updateWorkspaceSpectrum(frequencies, levels, 101.5, 102, 4, 1, 7);
        panel.updateMultiVfoSpectrum("{}", frequencies, levels);
    }
    panel.hide();
    require(!panel.shouldUpdateWorkspaceSpectrum() && !panel.shouldUpdateMultiVfoSpectrum(),
            "hidden board still requests frames");
    require(panel.workspaceSpectrumTargetBins() == 0, "hidden board still requests display width");
}

void testLazyWorkspaceWaterfall() {
    DspFlowPanel panel;
    const QJsonObject graph{{"version", 8}, {"blocks", QJsonArray{
        QJsonObject{{"id", "receiver"}, {"type", "receiver_source"}},
        QJsonObject{{"id", "waterfall"}, {"type", "workspace_waterfall"}}}}};
    require(panel.setConfigurationJson(graphJson(graph)), "Workspace waterfall fixture failed");
    require(panel.findChildren<MyWaterfallWidget *>().isEmpty(),
            "2D workspace waterfall allocated an OpenGL widget");
    panel.setWorkspaceVisualizationSettings({{"displayMode", 1}});
    require(panel.findChildren<MyWaterfallWidget *>().size() == 1,
            "3D workspace waterfall did not allocate its renderer");
}

void testComputeDialog() {
    DspStreamEndpoint endpoint;
    DspComputeDialog dialog(endpoint, true);
    dialog.setAttribute(Qt::WA_DeleteOnClose, false);
    dialog.show(); QApplication::processEvents();
    require(!dialog.endpoint().enabled && dialog.endpoint().token.isEmpty(), "Compute dialog enabled transfer by default");
    auto *controls = dialog.findChild<SpectrumComputeControls *>();
    require(controls, "Manager has no shared main FFT controls");
    auto *mode = controls->findChild<QComboBox *>("mainFftComputeMode");
    auto *backend = controls->findChild<QComboBox *>("mainFftBackend");
    auto *iqFormat = controls->findChild<QComboBox *>("mainFftIqFormat");
    auto *upload = controls->findChild<QSpinBox *>("mainFftUploadLimitMBps");
    auto *fallback = controls->findChild<QCheckBox *>("mainFftFallbackToLocal");
    auto *apply = controls->findChild<QPushButton *>("mainFftApply");
    auto *status = controls->findChild<QLabel *>("mainFftComputeStatus");
    auto *memory = dialog.findChild<QSpinBox *>("mainFftMemoryMiB");
    require(mode && backend && iqFormat && upload && fallback && apply && status && memory, "Main FFT controls are incomplete");
    require(controls->options() == SpectrumComputeOptions{} && !upload->isEnabled() && !fallback->isEnabled(),
            "Local defaults unexpectedly arm remote FFT controls");
    require(mode->count() == 3 && backend->count() == 3 && iqFormat->count() == 5 && upload->minimum() == 1 && upload->maximum() == 2000 &&
            memory->minimum() == 64 && memory->maximum() == 8192 && memory->value() == 512 && memory->isEnabled(),
            "Main FFT control ranges/defaults changed");
    int applySignals = 0, optionSignals = 0, endpointSignals = 0;
    QObject::connect(controls, &SpectrumComputeControls::applyRequested, &dialog, [&]() { ++applySignals; });
    QObject::connect(&dialog, &DspComputeDialog::spectrumComputeOptionsChanged, &dialog, [&]() { ++optionSignals; });
    QObject::connect(&dialog, &DspComputeDialog::endpointChanged, &dialog, [&]() { ++endpointSignals; });
    mode->setCurrentIndex(mode->findData(int(SpectrumComputeMode::Windows)));
    backend->setCurrentIndex(backend->findData(0));
    iqFormat->setCurrentIndex(iqFormat->findData(int(DspIqFormat::Cs16Le)));
    upload->setValue(234); fallback->setChecked(true);
    SpectrumComputeOptions draft;
    draft.mode = SpectrumComputeMode::Windows; draft.backend = 0; draft.iqFormat = DspIqFormat::Cs16Le;
    draft.uploadLimitMBps = 234; draft.fallbackToLocal = true;
    require(controls->options() == draft && dialog.spectrumComputeOptions() == SpectrumComputeOptions{} &&
            applySignals == 0 && optionSignals == 0 && upload->isEnabled() && fallback->isEnabled(),
            "Draft FFT edits committed before Apply");
    apply->click();
    require(dialog.spectrumComputeOptions() == draft && applySignals == 1 && optionSignals == 1 && endpointSignals == 0,
            "Main FFT Apply did not commit exactly once or changed endpoint");
    apply->click();
    require(applySignals == 2 && optionSignals == 1, "Unchanged Apply emitted an option change");
    draft.mode = SpectrumComputeMode::Continuous; draft.backend = 2;
    dialog.setSpectrumComputeOptions(draft);
    dialog.setSpectrumComputeStatus(QStringLiteral("<worker-status>"));
    endpoint.enabled = true; endpoint.token = QString(64, 's');
    dialog.setEndpoint(endpoint);
    require(dialog.spectrumComputeOptions() == draft && controls->options() == draft && optionSignals == 1 &&
            endpointSignals == 0 && dialog.endpoint() == endpoint && status->text() == "<worker-status>" &&
            status->textFormat() == Qt::PlainText, "Dialog setters emitted Apply or failed to sync controls/status");
    SpectrumComputeControls blockControls(false);
    blockControls.setOptions(draft);
    require(blockControls.options() == controls->options(), "Shared block and manager controls disagree");
    const auto image = dialog.grab().toImage();
    require(!image.isNull(), "Compute dialog render is blank");
    if (qEnvironmentVariableIsSet("OBRII_DSP_RENDER_TEST")) {
        require(image.save("build/dsp-stream-manager-preview.png"), "Cannot save compute dialog preview");
        auto *tabs = dialog.findChild<QTabWidget *>();
        require(tabs && tabs->count() == 5, "Compute manager tabs missing");
        tabs->setCurrentIndex(1);
        dialog.setSpectrumComputeStatus(QStringLiteral("Remote continuous FFT | upload 64 MB/s | download 12 MB/s | DSP 20 ms | latency 30 ms | gaps 0 | overrun 0 samples"));
        QApplication::processEvents();
        require(dialog.grab().save("build/dsp-main-fft-preview.png"), "Cannot save main FFT controls preview");
    }
    DspComputeServer server;
    const QString testToken(64, 'p');
    require(server.start("127.0.0.1", 0, testToken), "Cannot start probe UI test worker");
    for (int i = 0; i < 100 && !server.port(); ++i) QThread::msleep(10);
    require(server.port(), "Probe UI test worker did not bind");
    dialog.findChild<QLineEdit *>("dspRemoteAddress")->setText("127.0.0.1");
    dialog.findChild<QSpinBox *>("dspRemotePort")->setValue(server.port());
    dialog.findChild<QLineEdit *>("dspRemoteToken")->setText(testToken);
    dialog.setProbeContext(8000000, 524288, 100, 25);
    auto *testRate = dialog.findChild<QDoubleSpinBox *>("dspProbeRateMSps");
    auto *testLength = dialog.findChild<QSpinBox *>("dspProbeFftLength");
    auto *testInterval = dialog.findChild<QSpinBox *>("dspProbeIntervalMs");
    auto *testOverlap = dialog.findChild<QSpinBox *>("dspProbeOverlap");
    auto *testStart = dialog.findChild<QPushButton *>("dspProbeStart");
    auto *testStatus = dialog.findChild<QLabel *>("dspProbeStatus");
    require(testRate && testLength && testInterval && testOverlap && testStart && testStatus &&
            testRate->value() == 8 && testLength->value() == 524288 && testInterval->value() == 100 && testOverlap->value() == 25,
            "Receiver context did not reach connection test");
    const auto savedEndpoint = dialog.endpoint();
    const auto savedOptions = dialog.spectrumComputeOptions();
    testRate->setValue(50);
    dialog.setProbeContext(8000000, 524288, 100, 25);
    require(testRate->value() == 50, "Unchanged context overwrote manual connection estimate");
    testStart->click();
    require(!testStart->isEnabled(), "Connection test did not start asynchronously");
    QElapsedTimer probeTimer; probeTimer.start();
    while (!testStart->isEnabled() && probeTimer.elapsed() < 15000) { QApplication::processEvents(); QThread::msleep(10); }
    require(testStatus->text() == QString::fromUtf8("Тест завершено"), "Connection test UI did not complete");
    require(dialog.endpoint() == savedEndpoint && dialog.spectrumComputeOptions() == savedOptions && endpointSignals == 0 && optionSignals == 1,
            "Connection test applied drafts or enabled real IQ transfer");
    require(dialog.findChild<QLabel *>("dspProbeTarget")->text().contains("loopback") &&
            dialog.findChild<QLabel *>("dspProbeEstimate")->text().contains("200.00"), "Probe result lacks loopback warning or traffic estimate");
    auto *tabs = dialog.findChild<QTabWidget *>();
    auto *probeTab = dialog.findChild<QScrollArea *>("dspConnectionProbeTab");
    require(probeTab && tabs->indexOf(probeTab) >= 0, "Connection test tab missing");
    tabs->setCurrentWidget(probeTab);
    dialog.resize(640, 480); QApplication::processEvents();
    require(probeTab->horizontalScrollBar()->maximum() == 0, "Connection test does not fit a compact window");
    if (qEnvironmentVariableIsSet("OBRII_DSP_RENDER_TEST")) {
        dialog.resize(820, 840); QApplication::processEvents();
        require(dialog.grab().save("build/dsp-connection-test-preview.png"), "Cannot save connection test preview");
    }
    server.stop();
    dialog.hide();
}

void testPanelComputeDialog() {
    DspFlowPanel panel;
    SpectrumComputeOptions options;
    options.mode = SpectrumComputeMode::Windows; options.backend = 0; options.uploadLimitMBps = 222;
    panel.setSpectrumComputeOptions(options);
    panel.setSpectrumComputeStatus(QStringLiteral("panel-runtime-status"));
    panel.showComputeDialog();
    auto *dialog = panel.findChild<DspComputeDialog *>();
    require(dialog, "Panel did not open the compute manager");
    panel.showComputeDialog();
    require(panel.findChildren<DspComputeDialog *>().size() == 1, "Panel opened duplicate compute managers");
    auto *controls = dialog->findChild<SpectrumComputeControls *>();
    require(controls && controls->options() == options, "Panel options did not reach manager");
    auto *status = controls->findChild<QLabel *>("mainFftComputeStatus");
    auto *apply = controls->findChild<QPushButton *>("mainFftApply");
    require(status && apply && status->text() == panel.spectrumComputeStatus(), "Panel status did not reach manager");
    auto *memory = dialog->findChild<QSpinBox *>("mainFftMemoryMiB");
    require(memory, "Worker memory control missing");
    auto *worker = memory->parentWidget();
    const auto consent = worker->findChildren<QCheckBox *>();
    require(consent.size() == 1 && !consent.front()->isChecked(), "Worker consent unexpectedly granted by default");
    QString workerToken;
    for (auto *field : worker->findChildren<QLineEdit *>())
        if (field->isReadOnly() && field->echoMode() == QLineEdit::Password) workerToken = field->text();
    require(!workerToken.isEmpty(), "Worker session token missing");
    const auto beforeWorkerConsent = panel.configurationJson();
    // Do not click Start; armed worker permission must stay out of saved layouts.
    consent.front()->setChecked(true);
    memory->setValue(1024);
    require(panel.configurationJson() == beforeWorkerConsent && !panel.configurationJson().contains(workerToken),
            "Worker token/consent/memory entered board JSON");
    consent.front()->setChecked(false);
    int changes = 0;
    QObject::connect(&panel, &DspFlowPanel::spectrumComputeOptionsChanged, &panel, [&]() { ++changes; });
    options.mode = SpectrumComputeMode::Continuous; options.backend = 2; options.fallbackToLocal = true;
    controls->setOptions(options);
    require(panel.spectrumComputeOptions().mode == SpectrumComputeMode::Windows && changes == 0,
            "Manager draft changed panel state");
    apply->click();
    require(panel.spectrumComputeOptions() == options && changes == 1, "Manager Apply did not reach panel API");
    options.backend = 1;
    panel.setSpectrumComputeOptions(options);
    panel.setSpectrumComputeStatus(QStringLiteral("updated-runtime-status"));
    require(controls->options() == options && changes == 2 && status->text() == panel.spectrumComputeStatus(),
            "Panel setter did not synchronize an open manager");
    QPointer<DspComputeDialog> oldDialog(dialog);
    dialog->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(oldDialog.isNull(), "Closed manager did not follow delete-on-close lifecycle");
    panel.showComputeDialog();
    dialog = panel.findChild<DspComputeDialog *>();
    require(dialog && dialog->spectrumComputeOptions() == options, "Reopened manager lost committed preferences");
    dialog->hide();
}

void testVfoContext() {
    MultiVfoWidget widget;
    require(widget.setConfigurationJson(R"({"enabled":true,"channels":[{"frequencyHz":100000000,"bandwidthHz":12500}]})"),
            "VFO config load failed");
    int configurationSignals = 0;
    QObject::connect(&widget, &MultiVfoWidget::configurationChanged, [&configurationSignals]() { ++configurationSignals; });
    const auto before = widget.configurationJson();
    for (int i = 0; i < 100; ++i)
        widget.setReceiverContext(100000000, 50000000, 100000000, 12500, 1);
    require(configurationSignals == 0 && widget.configurationJson() == before, "context updates changed VFO configuration");
    widget.setReceiverContext(110000000, 10000000, 109000000, 200000, 7);
    require(widget.configurationJson() == before, "retuning modified channel configuration");
}

void testResearch3D() {
    DspResearchWidget widget(DspResearchWidget::Mode::Constellation);
    widget.resize(640,440);
    widget.setConnected(true, true);
    widget.setSettings({{"constellation3D", true}, {"symbolRate", 4800}, {"samples", 4096}});
    auto *child = widget.findChild<QWidget *>(QStringLiteral("dspResearchConstellation3D"));
    require(child && !child->isHidden() && !widget.testAttribute(Qt::WA_TransparentForMouseEvents),
            "DSP 3D constellation is missing or cannot receive camera events");
    widget.show();
    IqBuffer::clear();
    IqBuffer::setSampleRateEstimate(48000);
    std::vector<float> iq(8192);
    for (int index = 0; index < 4096; ++index) {
        iq[2*index] = float(0.00001*std::cos(index*0.1));
        iq[2*index+1] = float(0.00001*std::sin(index*0.1));
    }
    require(IqBuffer::publish(iq.data(), iq.size(), false), "Failed to publish a research test snapshot");
    QThread::msleep(60);
    QApplication::processEvents();
    auto *threeD = static_cast<SignalLabConstellation3DWidget *>(child);
    widget.grab();
    require(SignalLabConstellation3DTestAccess::points(*threeD) > 2500, "Live IQ did not reach the DSP 3D constellation");
    widget.setSettings({{"constellation3D", true}, {"constellationSurface", true}});
    widget.grab();
    require(SignalLabConstellation3DTestAccess::triangles(*threeD) == 4998,
            "DSP radial surface setting did not reach the shared renderer");
    const auto camera = threeD->cameraState();
    QMouseEvent press(QEvent::MouseButtonPress, {200,180}, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QMouseEvent move(QEvent::MouseMove, {240,210}, Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, {240,210}, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
    QApplication::sendEvent(child, &press);
    QApplication::sendEvent(child, &move);
    QApplication::sendEvent(child, &release);
    require(threeD->cameraState() != camera, "DSP camera does not rotate");
    require(threeD->cameraState().value("pitch").toDouble() > camera.value("pitch").toDouble(),
            "DSP vertical camera drag still has the old direction");
    widget.setSettings({{"constellation3D", false}});
    require(child->isHidden() && widget.testAttribute(Qt::WA_TransparentForMouseEvents), "DSP did not return to its 2D view");
    widget.setSettings({{"constellation3D", true}});
    require(threeD->cameraState() != camera, "DSP mode changes reset the camera");
    widget.setConnected(false, true);
    widget.grab();
    require(SignalLabConstellation3DTestAccess::points(*threeD) == 0, "Disconnected DSP still shows old IQ");
    widget.hide();
    IqBuffer::clear();
}
}

bool runRemoteVfoServiceTests();
int runRemoteVfoSmokeServer(QCoreApplication &app);

int main(int argc, char **argv) {
#ifdef _MSC_VER
    SetUnhandledExceptionFilter(reportCrash);
#endif
    if (qEnvironmentVariableIsSet("OBRII_REMOTE_VFO_SELFTEST")) {
        // The VFO broker also serves headless clients; it must not need a GUI,
        // a display driver or the platform font database to validate its protocol.
        QCoreApplication core(argc, argv);
        QStandardPaths::setTestModeEnabled(true);
        try {
            if (qEnvironmentVariableIsSet("OBRII_REMOTE_VFO_SMOKE_SERVER"))
                return runRemoteVfoSmokeServer(core);
            require(runRemoteVfoServiceTests(), "Remote VFO session tests failed");
            std::cout << "Remote VFO session isolation and quota tests passed\n";
            return 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (qEnvironmentVariableIsSet("OBRII_DSP_BOARD_SETTINGS"))
        std::cerr << "Saved DSP board: initializing Qt\n" << std::flush;
    QApplication app(argc, argv);
    if (qEnvironmentVariableIsSet("OBRII_DSP_BOARD_SETTINGS"))
        std::cerr << "Saved DSP board: Qt initialized\n" << std::flush;
#ifdef _WIN32
    if (qEnvironmentVariableIsSet("OBRII_DSP_RENDER_TEST")) {
        const int font = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/arial.ttf"));
        const auto families = QFontDatabase::applicationFontFamilies(font);
        if (!families.isEmpty()) app.setFont(QFont(families.front(), 10));
    }
#endif
    QStandardPaths::setTestModeEnabled(true);
    QApplication::setApplicationName("ObriiDspFlowSelfTest");
    try {
        if (qEnvironmentVariableIsSet("OBRII_DSP_BOARD_SETTINGS")) {
            QSettings settings(qEnvironmentVariable("OBRII_DSP_BOARD_SETTINGS"), QSettings::IniFormat);
            const QString boardJson = settings.value(QStringLiteral("dspFlow/configuration")).toString();
            require(!boardJson.isEmpty(), "No saved DSP board in settings");
            std::cerr << "Saved DSP board: constructing panel\n" << std::flush;
            DspFlowPanel panel;
            std::cerr << "Saved DSP board: loading configuration\n" << std::flush;
            require(panel.setConfigurationJson(boardJson), "Saved DSP board did not load");
            std::cerr << "Saved DSP board: showing panel\n" << std::flush;
            panel.show();
            for (int i = 0; i < 100; ++i) {
                QApplication::processEvents();
                QThread::msleep(10);
            }
            std::cout << "Saved DSP board opened successfully\n";
            return 0;
        }
        testBoardHistory();
        testBoardFilterHistory();
        testSelectionDeletion();
        testBoard();
        testLazyWorkspaceWaterfall();
        testExplicitDspRateGeometry();
        testMainFftGraph();
        testBoardSignalPath();
        testMainFftBoardRouting();
        testMainFftAliases();
        testMainFftAliasDelivery();
        testExperimentLayoutImport();
        testSpectrumComputePreferences();
        testComputeDialog();
        testPanelComputeDialog();
        testVfoContext();
        testResearch3D();
        require(dspBranchSelfTest(), "DSP branch tests failed");
        require(runRemoteVfoServiceTests(), "Remote VFO session tests failed");
        require(dspBranchTeardownSelfTest(), "DSP teardown tests failed");
        require(multiVfoPreviewHistorySelfTest(), "VFO level changes cleared or reordered waterfall history");
        std::cout << "DSP frame routing, main FFT bindings/preferences/controls, cache lifetime, throttling and VFO context tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
