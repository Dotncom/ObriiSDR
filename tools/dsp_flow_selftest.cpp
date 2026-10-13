#include "dspflowpanel.h"
#include "multivfowidget.h"
#include "dspresearchwidget.h"
#include "signallabconstellation3d.h"
#include "iqbuffer.h"
#include "dspcomputedialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsPathItem>
#include <QGraphicsView>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QThread>
#include <QMouseEvent>

#include <iostream>
#include <cmath>
#include <stdexcept>
#ifdef _MSC_VER
#include <windows.h>
#include <dbghelp.h>
#endif

bool multiVfoPreviewHistorySelfTest();
bool dspBranchSelfTest();
bool dspBranchTeardownSelfTest();

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
    options.uploadLimitMBps = 321; options.fallbackToLocal = true;
    panel.setSpectrumComputeOptions(options);
    panel.setSpectrumComputeOptions(options);
    require(preferenceSignals == 1 && panel.spectrumComputeOptions() == options, "Preference setter/signal contract failed");
    const auto saved = panel.configurationJson();
    auto object = QJsonDocument::fromJson(saved.toUtf8()).object();
    require(object["spectrumCompute"].toObject() == QJsonObject{{"mode", 2}, {"backend", 2},
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
    require(cleanPreferences.size() == 4 && !cleanPreferences.contains("enabled") && restoreSignals == 2,
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
    auto *upload = controls->findChild<QSpinBox *>("mainFftUploadLimitMBps");
    auto *fallback = controls->findChild<QCheckBox *>("mainFftFallbackToLocal");
    auto *apply = controls->findChild<QPushButton *>("mainFftApply");
    auto *status = controls->findChild<QLabel *>("mainFftComputeStatus");
    auto *memory = dialog.findChild<QSpinBox *>("mainFftMemoryMiB");
    require(mode && backend && upload && fallback && apply && status && memory, "Main FFT controls are incomplete");
    require(controls->options() == SpectrumComputeOptions{} && !upload->isEnabled() && !fallback->isEnabled(),
            "Local defaults unexpectedly arm remote FFT controls");
    require(mode->count() == 3 && backend->count() == 3 && upload->minimum() == 1 && upload->maximum() == 2000 &&
            memory->minimum() == 64 && memory->maximum() == 8192 && memory->value() == 512 && memory->isEnabled(),
            "Main FFT control ranges/defaults changed");
    int applySignals = 0, optionSignals = 0, endpointSignals = 0;
    QObject::connect(controls, &SpectrumComputeControls::applyRequested, &dialog, [&]() { ++applySignals; });
    QObject::connect(&dialog, &DspComputeDialog::spectrumComputeOptionsChanged, &dialog, [&]() { ++optionSignals; });
    QObject::connect(&dialog, &DspComputeDialog::endpointChanged, &dialog, [&]() { ++endpointSignals; });
    mode->setCurrentIndex(mode->findData(int(SpectrumComputeMode::Windows)));
    backend->setCurrentIndex(backend->findData(0)); upload->setValue(234); fallback->setChecked(true);
    SpectrumComputeOptions draft;
    draft.mode = SpectrumComputeMode::Windows; draft.backend = 0; draft.uploadLimitMBps = 234; draft.fallbackToLocal = true;
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
        require(tabs && tabs->count() == 4, "Compute manager tabs missing");
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
            dialog.findChild<QLabel *>("dspProbeEstimate")->text().contains("400.00"), "Probe result lacks loopback warning or traffic estimate");
    auto *tabs = dialog.findChild<QTabWidget *>();
    tabs->setCurrentIndex(2);
    dialog.resize(640, 480); QApplication::processEvents();
    auto *scroll = qobject_cast<QScrollArea *>(tabs->currentWidget());
    require(scroll && scroll->horizontalScrollBar()->maximum() == 0, "Connection test does not fit a compact window");
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

int main(int argc, char **argv) {
#ifdef _MSC_VER
    SetUnhandledExceptionFilter(reportCrash);
#endif
    QApplication app(argc, argv);
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
        testSelectionDeletion();
        testBoard();
        testMainFftGraph();
        testMainFftBoardRouting();
        testSpectrumComputePreferences();
        testComputeDialog();
        testPanelComputeDialog();
        testVfoContext();
        testResearch3D();
        require(dspBranchSelfTest(), "DSP branch tests failed");
        require(dspBranchTeardownSelfTest(), "DSP teardown tests failed");
        require(multiVfoPreviewHistorySelfTest(), "VFO level changes cleared or reordered waterfall history");
        std::cout << "DSP frame routing, main FFT bindings/preferences/controls, cache lifetime, throttling and VFO context tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
