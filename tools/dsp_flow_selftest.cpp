#include "dspflowpanel.h"
#include "multivfowidget.h"
#include "dspresearchwidget.h"
#include "signallabconstellation3d.h"
#include "iqbuffer.h"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTableWidget>
#include <QThread>
#include <QMouseEvent>

#include <iostream>
#include <cmath>
#include <stdexcept>

bool multiVfoPreviewHistorySelfTest();

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

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QApplication::setApplicationName("ObriiDspFlowSelfTest");
    try {
        testBoard();
        testVfoContext();
        testResearch3D();
        require(multiVfoPreviewHistorySelfTest(), "VFO level changes cleared or reordered waterfall history");
        std::cout << "DSP frame routing, cache lifetime, throttling and VFO context tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
