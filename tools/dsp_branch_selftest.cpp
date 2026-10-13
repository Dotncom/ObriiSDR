#include "dspbranchengine.h"
#include "dspflowpanel.h"
#include "audiofilterchainwidget.h"
#include "iqbuffer.h"
#include "dspstreammanager.h"

#include <QApplication>
#include <QCheckBox>
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QGraphicsView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QTemporaryDir>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

bool runDspBranchAudioTests();

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
QJsonObject graph() {
    return {{"version", 8}, {"blocks", QJsonArray{
        QJsonObject{{"id", "source"}, {"type", "iq_source"}, {"x", 0}, {"y", 0}},
        QJsonObject{{"id", "ddc"}, {"type", "ddc_channelizer"}, {"x", 200}, {"y", 0},
            {"settings", QJsonObject{{"frequencyHz", 100100000.0}, {"bandwidthHz", 12500}, {"outputRateHz", 96000}, {"enabled", true}}}},
        QJsonObject{{"id", "fft"}, {"type", "branch_fft"}, {"x", 400}, {"y", 0},
            {"settings", QJsonObject{{"fftLength", 2048}, {"window", FFT_WINDOW_HANN}, {"refreshMs", 20}}}},
        QJsonObject{{"id", "display"}, {"type", "branch_spectrum"}, {"x", 620}, {"y", 0}},
        QJsonObject{{"id", "waterfall"}, {"type", "branch_waterfall"}, {"x", 620}, {"y", 240}}}},
        {"connections", QJsonArray{
            QJsonObject{{"source", "source"}, {"target", "ddc"}},
            QJsonObject{{"source", "ddc"}, {"target", "fft"}},
            QJsonObject{{"source", "fft"}, {"target", "display"}},
            QJsonObject{{"source", "fft"}, {"target", "waterfall"}}}}};
}
void testActiveBranchResources() {
    QJsonArray blocks, edges;
    const auto reset = [&] {
        blocks = QJsonArray{QJsonObject{{"id", "source"}, {"type", "iq_source"}}};
        edges = QJsonArray();
    };
    const auto addBranch = [&](const QString &id, bool enabled, int channels = 0, int taps = 16) {
        QString input = "source";
        if (channels) {
            input = id + "-pfb";
            blocks.append(QJsonObject{{"id", input}, {"type", "pfb_channelizer"},
                {"settings", QJsonObject{{"channels", channels}, {"tapsPerPhase", taps}}}});
            edges.append(QJsonObject{{"source", "source"}, {"target", input}});
        }
        blocks.append(QJsonObject{{"id", id}, {"type", "ddc_channelizer"},
            {"settings", QJsonObject{{"enabled", enabled}}}});
        edges.append(QJsonObject{{"source", input}, {"target", id}});
        blocks.append(QJsonObject{{"id", id + "-fft"}, {"type", "branch_fft"}});
        blocks.append(QJsonObject{{"id", id + "-view"}, {"type", "branch_spectrum"}});
        blocks.append(QJsonObject{{"id", id + "-scope"}, {"type", "oscilloscope"}});
        blocks.append(QJsonObject{{"id", id + "-demod"}, {"type", "branch_demodulator"}});
        blocks.append(QJsonObject{{"id", id + "-audio"}, {"type", "branch_audio_output"}});
        edges.append(QJsonObject{{"source", id}, {"target", id + "-fft"}});
        edges.append(QJsonObject{{"source", id + "-fft"}, {"target", id + "-view"}});
        edges.append(QJsonObject{{"source", id}, {"target", id + "-scope"}});
        edges.append(QJsonObject{{"source", id}, {"target", id + "-demod"}});
        edges.append(QJsonObject{{"source", id + "-demod"}, {"target", id + "-audio"}});
    };
    const auto compile = [&] { return compileDspGraph(QJsonObject{{"blocks", blocks}, {"connections", edges}}); };
    const auto requireInactiveRoutes = [](const DspGraphPlan &plan, const QString &id) {
        const auto branch = std::find_if(plan.branches.cbegin(), plan.branches.cend(),
            [&](const DspBranchConfig &b) { return b.id == id; });
        require(branch != plan.branches.cend() && !branch->enabled && branch->ffts.size() == 1 && branch->iqConsumer,
            "Inactive branch configuration was lost or activated");
        require(plan.spectrumRoutes.value(id + "-view") == id + "-fft" &&
                plan.researchRoutes.value(id + "-scope") == id &&
                plan.audioRoutes.value(id + "-demod") == id && plan.audioRoutes.value(id + "-audio") == id,
            "Inactive branch display/research/audio routes were lost");
    };
    const auto requireRejected = [](const DspGraphPlan &plan, const QString &error) {
        require(plan.errors.contains(error) && plan.branches.isEmpty() && plan.spectrumRoutes.isEmpty() &&
                plan.researchRoutes.isEmpty() && plan.audioRoutes.isEmpty() && plan.pfbChannels == 0 &&
                QJsonDocument::fromJson(plan.executionKey).object().value("branches").toArray().isEmpty(),
            "Invalid active branch resources did not clear the executable plan");
    };

    for (int active : {0, 1, DspMaxBranches, DspMaxBranches + 1}) {
        reset();
        // Disabled IDs sort both before and after the eight-active boundary.
        for (int i = 0; i < DspMaxBranches + 2; ++i)
            addBranch(QStringLiteral("ddc-%1").arg(i, 2, 10, QLatin1Char('0')), i > 0 && i <= active);
        const auto plan = compile();
        if (active > DspMaxBranches) {
            requireRejected(plan, "At most eight DDC branches can run");
        } else {
            require(plan.errors.isEmpty() && plan.branches.size() == DspMaxBranches + 2 &&
                    std::count_if(plan.branches.cbegin(), plan.branches.cend(),
                        [](const DspBranchConfig &b) { return b.enabled; }) == active,
                "Disabled DDCs consumed the active branch budget");
            requireInactiveRoutes(plan, "ddc-00");
            requireInactiveRoutes(plan, QStringLiteral("ddc-%1").arg(DspMaxBranches + 1, 2, 10, QLatin1Char('0')));
        }
    }

    const int channels[] = {8, 16, 64};
    const int taps[] = {8, 16, 32};
    for (int selected = -1; selected < 3; ++selected) {
        reset();
        for (int i = 0; i < 3; ++i)
            addBranch(QStringLiteral("pfb-ddc-%1").arg(i), i == selected, channels[i], taps[i]);
        const auto plan = compile();
        require(plan.errors.isEmpty() && plan.branches.size() == 3 &&
                plan.pfbChannels == (selected < 0 ? 0 : channels[selected]) &&
                plan.pfbTaps == (selected < 0 ? 16 : taps[selected]),
            "Disabled PFB branches selected, conflicted with or overwrote the active bank");
        for (int i = 0; i < 3; ++i)
            if (i != selected) requireInactiveRoutes(plan, QStringLiteral("pfb-ddc-%1").arg(i));
    }
    // Matching active banks remain valid; channel-count and tap-count conflicts still fail.
    for (int mismatch = 0; mismatch < 3; ++mismatch) {
        reset();
        addBranch("a-inactive", false, 8, 8);
        addBranch("m-active-1", true, 16, 16);
        addBranch("m-active-2", true, mismatch == 1 ? 32 : 16, mismatch == 2 ? 32 : 16);
        addBranch("z-inactive", false, 64, 32);
        const auto plan = compile();
        if (mismatch) {
            requireRejected(plan, "All active PFB branches must share the same bank settings");
        } else {
            require(plan.errors.isEmpty() && plan.branches.size() == 4 && plan.pfbChannels == 16 && plan.pfbTaps == 16,
                "Matching active PFB banks were rejected");
            requireInactiveRoutes(plan, "a-inactive");
            requireInactiveRoutes(plan, "z-inactive");
        }
    }
}
RadioSettings sourceSettings() {
    RadioSettings s;
    s.inputMode = INPUT_RF; s.sampleRate = 2048000.0;
    s.centerFrequency = 100000000.0; s.listeningFrequency = 100100000.0;
    return s;
}
void tone(const RadioSettings &s, std::size_t pairs, std::uint64_t start = 0) {
    std::vector<float> iq(pairs*2U);
    for (std::size_t i = 0; i < pairs; ++i) {
        const double phase = 6.28318530717958647692 * 101500.0 * double(start+i) / s.sampleRate;
        const double blocker = 6.28318530717958647692 * 400000.0 * double(start+i) / s.sampleRate;
        iq[i*2U] = float(0.5*std::cos(phase) + 0.2*std::cos(blocker));
        iq[i*2U+1U] = float(0.5*std::sin(phase) + 0.2*std::sin(blocker));
    }
    IqBuffer::BlockMetadata m;
    m.valid = true; m.centerFrequencyHz = s.centerFrequency;
    require(IqBuffer::publish(iq.data(), iq.size(), true, true, 0, &m), "Branch test publish failed");
}
DspBranchFrame waitFor(DspBranchEngine &engine, const QString &id, bool error = false) {
    for (int i = 0; i < 300; ++i) {
        for (auto &frame : engine.takeFrames()) if (frame.id == id &&
            (error ? !frame.error.isEmpty() : !frame.levels.empty())) return std::move(frame);
        QThread::msleep(5);
    }
    throw std::runtime_error("Branch worker produced no expected frame");
}
}

bool dspBranchSelfTest() {
    testActiveBranchResources();
    require(runDspBranchAudioTests(), "DSP branch audio tests failed");
    require(dspLinkKind("iq_source", "ddc_channelizer") == DspLinkKind::Iq, "Raw IQ port type wrong");
    require(dspLinkKind("ddc_channelizer", "branch_fft") == DspLinkKind::Iq, "DDC IQ port type wrong");
    require(dspLinkKind("branch_fft", "branch_spectrum") == DspLinkKind::Spectrum, "FFT port type wrong");
    require(dspLinkKind("branch_fft", "ddc_channelizer") == DspLinkKind::Invalid, "Stream cycle accepted");
    require(dspLinkKind("channel_filter", "demodulator") == DspLinkKind::Documentation, "Legacy pipeline falsely executable");
    auto layout = graph();
    auto plan = compileDspGraph(layout);
    require(plan.errors.empty() && plan.branches.size() == 1 && plan.spectrumRoutes.size() == 2, "Branch plan failed");
    auto moved = layout;
    auto blocks = moved["blocks"].toArray();
    auto first = blocks[0].toObject(); first["x"] = 999; blocks[0] = first; moved["blocks"] = blocks;
    require(compileDspGraph(moved).executionKey == plan.executionKey, "Layout changes reconfigure DSP");
    auto invalid = layout;
    auto edges = invalid["connections"].toArray();
    edges.append(QJsonObject{{"source", "fft"}, {"target", "ddc"}}); invalid["connections"] = edges;
    require(!compileDspGraph(invalid).errors.empty() && compileDspGraph(invalid).branches.empty(), "Invalid plan executed");

    AudioFilterChainWidget filters;
    filters.ensureStage("owned-filter", AudioFilterKind::Gain, false);
    auto stages = decodeAudioFilterChain(filters.configurationJson());
    require(stages.size() == 1 && !stages[0].enabled, "Disconnected new filter activated");
    require(filters.openStageEditor("owned-filter"), "Filter editor unavailable");
    auto *enabled = filters.findChild<QCheckBox *>("audioFilterEnabled");
    require(enabled, "Filter editor has no enable control");
    enabled->setChecked(true);
    require(decodeAudioFilterChain(filters.configurationJson())[0].enabled, "Filter enable did not reach the real chain");
    require(filters.removeStageById("owned-filter") && decodeAudioFilterChain(filters.configurationJson()).empty(), "Deleted filter remains in the chain");

    const auto s = sourceSettings();
    IqBuffer::clear(10001); IqBuffer::setSampleRateEstimate(s.sampleRate);
    DspBranchEngine engine;
    engine.configure(plan, s, true);
    tone(s, 262144);
    const auto queued = IqBuffer::queuedBlocks();
    auto frame = waitFor(engine, "fft");
    const auto peak = std::max_element(frame.levels.begin(), frame.levels.end());
    const auto index = std::size_t(peak-frame.levels.begin());
    require(frame.sampleRateHz == 96000.0 && std::abs(frame.frequencies[index]-100101500.0) < 70.0,
        "DDC frequency or output rate wrong");
    require(*peak > -12.0 && *peak < -4.0 && frame.binWidthHz == 96000.0/2048 && frame.rbwHz > frame.binWidthHz,
        "Branch amplitude/window/RBW wrong");
    require(IqBuffer::queuedBlocks() == queued, "DDC stole samples from the audio queue");
    std::cout << "DDC tone: peak=" << *peak << " dBFS, bin=" << frame.binWidthHz << " Hz, D=" << frame.decimation << '\n';

    // An integer D followed by a fractional conversion must use the exact requested D.
    blocks = layout["blocks"].toArray();
    auto ddc = blocks[1].toObject(); auto settings = ddc["settings"].toObject();
    settings["decimation"] = 15; ddc["settings"] = settings; blocks[1] = ddc; layout["blocks"] = blocks;
    plan = compileDspGraph(layout); engine.configure(plan, s, true);
    frame = waitFor(engine, "fft");
    require(frame.decimation == 15 && frame.sampleRateHz == 96000.0, "Manual D changed output duration/rate");
    tone(s, 1200000, 262144);
    frame = waitFor(engine, "fft");
    require(frame.gaps > 0, "Over-budget IQ discontinuity not reported");
    IqBuffer::clear(10002); IqBuffer::setSampleRateEstimate(s.sampleRate);
    tone(s, 262144);
    frame = waitFor(engine, "fft");
    require(frame.error.isEmpty() && frame.sampleRateHz == 96000.0, "New source epoch did not recover the branch");
    settings["decimation"] = 4096; ddc["settings"] = settings; blocks[1] = ddc; layout["blocks"] = blocks;
    engine.configure(compileDspGraph(layout), s, true);
    require(!waitFor(engine, "ddc", true).error.isEmpty(), "Unsafe decimation not rejected");
    engine.configure(plan, s, false);
    tone(s, 32768, 262144); QThread::msleep(30);
    require(engine.takeFrames().empty(), "Disabled branch still produced frames");
    auto simplified = s; simplified.simplifiedAudioChannelizer = true;
    engine.configure(plan, simplified, true);
    tone(s, 65536, 294912); const auto simpleQueue = IqBuffer::queuedBlocks();
    QThread::msleep(100);
    require(engine.takeFrames().empty() && IqBuffer::queuedBlocks() == simpleQueue,
            "Simplified mode executed a branch or consumed audio IQ");

    DspComputeServer server; DspStreamEndpoint endpoint;
    endpoint.enabled = true; endpoint.token = QString(64, 'b');
    require(server.start("127.0.0.1", 0, endpoint.token), "Board compute worker rejected start");
    for (int i = 0; i < 100 && !server.port(); ++i) QThread::msleep(10);
    endpoint.port = server.port(); require(endpoint.port, "Board compute worker did not bind");
    auto remotePlan = plan; remotePlan.branches[0].external = true; remotePlan.executionKey += "remote-test";
    engine.setExternalEndpoint(endpoint); engine.configure(remotePlan, s, true);
    IqBuffer::clear(10003); IqBuffer::setSampleRateEstimate(s.sampleRate); tone(s, 262144);
    frame = waitFor(engine, "fft");
    require(frame.sampleRateHz == 96000 && *std::max_element(frame.levels.begin(), frame.levels.end()) > -12,
            "Remote branch did not deliver a calibrated spectrum");
    engine.configure(plan, s, false); QThread::msleep(30); server.stop();

    DspFlowPanel panel;
    panel.setExternalEndpoint(endpoint);
    require(panel.setConfigurationJson(QString::fromUtf8(QJsonDocument(graph()).toJson())), "Typed board load failed");
    panel.resize(1200, 650); panel.setBranchSourceContext(s, true); panel.show();
    IqBuffer::setSampleRateEstimate(s.sampleRate*0.83);
    tone(s, 262144, 294912);
    for (int i = 0; i < 60 && !panel.branchStatus("fft").contains("Fs "); ++i) { QApplication::processEvents(); QThread::msleep(10); }
    require(panel.branchStatus("fft").contains("Fs "), "DDC frames did not reach the DSP board");
    for (int n = 0; n < 8; ++n) {
        tone(s, 65536, 557056U+std::uint64_t(n)*65536U);
        QThread::msleep(35); QApplication::processEvents();
    }
    const auto image = panel.grab().toImage();
    require(!image.isNull() && image.width() == panel.width(), "Branch board render is blank");
    QWidget *preview = nullptr;
    for (auto *widget : QApplication::allWidgets())
        if (widget->objectName() == QStringLiteral("dspPreview_display")) { preview = widget; break; }
    require(preview, "Independent spectrum widget missing");
    const auto rendered = preview->grab().toImage();
    int signalPixels = 0;
    for (int y = 22; y < rendered.height()-18; ++y) for (int x = 6; x < rendered.width()-6; ++x) {
        const auto color = rendered.pixelColor(x, y);
        if (color.green() > 120 && color.green() > color.red()*1.3 && color.green() > color.blue()*1.2) ++signalPixels;
    }
    require(signalPixels > 20, "Spectrum canvas has no rendered signal pixels");
    if (qEnvironmentVariableIsSet("OBRII_DSP_RENDER_TEST")) {
        std::cout << "Saving rendered DSP canvas" << std::endl;
        require(image.save(QStringLiteral("build/dsp-branch-preview.png")), "Cannot save rendered DSP canvas");
    }
    panel.hide(); QApplication::processEvents();
    QTemporaryDir recording;
    require(recording.isValid() && panel.startBranchRecording("ddc", recording.path()), "Cannot arm independent branch recording");
    for (int i = 0; i < 20; ++i) {
        tone(s, 65536, 1081344U+std::uint64_t(i)*65536U);
        QThread::msleep(25); QApplication::processEvents();
    }
    require(!QDir(recording.path()).entryList({"*.cf32"}, QDir::Files).empty(), "Hidden armed branch did not record");
    require(!panel.configurationJson().contains(endpoint.token) && !panel.configurationJson().contains(recording.path()),
            "DSP layout persisted session credentials or recording arming");
    panel.setBranchSourceContext(simplified, true); QApplication::processEvents();
    require(!panel.branchRecordingRequested("ddc"), "Simplified mode left branch recording armed");
    require(!panel.setConfigurationJson(QString::fromUtf8(QJsonDocument(invalid).toJson())), "Invalid board replaced a valid running graph");

    DspFlowPanel lifecycle;
    const auto filterLayout = QJsonObject{{"blocks", QJsonArray{QJsonObject{{"id", "owned"}, {"type", "filter_gain"}}}}, {"connections", QJsonArray{}}};
    require(lifecycle.setConfigurationJson(QString::fromUtf8(QJsonDocument(filterLayout).toJson())), "Filter board load failed");
    int removed = 0;
    QObject::connect(&lifecycle, &DspFlowPanel::blockRemoved, [&removed](const QString &type, const QString &id) { if (type == "filter_gain" && id == "owned") ++removed; });
    require(lifecycle.setConfigurationJson(QString::fromUtf8(QJsonDocument(filterLayout).toJson())) && removed == 0, "Reload removed a retained filter");
    require(lifecycle.setConfigurationJson(QString::fromUtf8(QJsonDocument(graph()).toJson())) && removed == 1, "Layout replacement did not release the filter");
    IqBuffer::clear();
    std::cout << "Typed DSP graph, independent DDC/FFT, filter lifecycle and board delivery tests passed" << std::endl;
    return true;
}

bool dspBranchTeardownSelfTest() {
    for (int i = 0; i < 32; ++i) {
        DspFlowPanel panel;
        require(panel.setConfigurationJson(QString::fromUtf8(QJsonDocument(graph()).toJson())), "Teardown graph load failed");
    }
    std::cout << "DSP connected-node teardown: 32 cycles passed\n";
    return true;
}
