#include "commandconsoledialog.h"
#include "commanddispatcher.h"
#include "commandcontrolserver.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPointer>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
int checks = 0;
QString snapshotMonoFamily;

void check(bool condition, const char *message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

bool visibleGlyphs(const QFont &font) {
    QImage image(240, 40, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    QPainter painter(&image);
    painter.setPen(Qt::white);
    painter.setFont(font);
    painter.drawText(image.rect(), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Aa09 \u041a\u043e\u043c\u0430\u043d\u0434\u0438"));
    painter.end();
    int pixels = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qRed(image.pixel(x, y)) > 40) ++pixels;
    return pixels > 40 && QFontMetrics(font).inFont(QChar(0x0407));
}

void prepareSnapshotFonts(QApplication &app) {
    if (QGuiApplication::platformName() != QLatin1String("offscreen")) return;
    const bool visibleBefore = visibleGlyphs(app.font());
    std::cout << "Offscreen snapshot fonts: families=" << QFontDatabase().families().size()
              << ", glyphs before=" << (visibleBefore ? "visible" : "blank") << '\n';
    if (!visibleBefore) {
#ifdef Q_OS_WIN
        // The Windows offscreen backend may expose no system fonts. Keep this
        // fallback inside the screenshot test, never the production dialog.
        const QDir fonts(QDir(qEnvironmentVariable("WINDIR", "C:/Windows")).filePath(QStringLiteral("Fonts")));
        for (const auto &filename : {QStringLiteral("segoeui.ttf"), QStringLiteral("arial.ttf")}) {
            const int id = QFontDatabase::addApplicationFont(fonts.filePath(filename));
            const auto families = QFontDatabase::applicationFontFamilies(id);
            if (families.isEmpty()) continue;
            app.setFont(QFont(families.first(), 9));
            if (visibleGlyphs(app.font())) break;
        }
        const int mono = QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("consola.ttf")));
        const auto families = QFontDatabase::applicationFontFamilies(mono);
        if (!families.isEmpty()) snapshotMonoFamily = families.first();
#endif
    }
    check(visibleGlyphs(app.font()), "offscreen fonts cannot render Latin/Ukrainian glyphs; set QT_QPA_FONTDIR");
    std::cout << "Offscreen snapshot font: " << app.font().family().toStdString() << "; raster probe passed\n";
}

template <typename T>
T *child(QObject &parent, const char *name) {
    auto *result = parent.findChild<T *>(QString::fromLatin1(name));
    check(result != nullptr, name);
    return result;
}

bool until(const std::function<bool()> &predicate, int timeout = 1500) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

void settle(int ms) {
    QElapsedTimer timer;
    timer.start();
    until([&timer, ms] { return timer.elapsed() >= ms; }, ms + 100);
}

void key(QLineEdit *input, int code) {
    QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier);
    QApplication::sendEvent(input, &event);
}

void send(CommandConsoleDialog &dialog, const QString &line) {
    child<QLineEdit>(dialog, "commandConsoleInput")->setText(line);
    child<QToolButton>(dialog, "commandConsoleSend")->click();
}

struct FakeServer {
    QTcpServer listener;
    QPointer<QTcpSocket> peer;
    QJsonObject request;
    int requests = 0;
    std::function<void(QTcpSocket *, const QJsonObject &)> handler;

    FakeServer() {
        check(listener.listen(QHostAddress::LocalHost, 0), "fake listener failed");
        QObject::connect(&listener, &QTcpServer::newConnection, &listener, [this] {
            while (listener.hasPendingConnections()) {
                auto *socket = listener.nextPendingConnection();
                peer = socket;
                auto input = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, &listener, [this, socket, input] {
                    input->append(socket->readAll());
                    const int newline = input->indexOf('\n');
                    if (newline < 0) return;
                    request = QJsonDocument::fromJson(input->left(newline)).object();
                    input->remove(0, newline + 1);
                    ++requests;
                    if (handler) handler(socket, request);
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    void reply(const QJsonObject &object) {
        check(peer != nullptr, "fake peer missing");
        peer->write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
    }
};

void localAndHistory(const QString &snapshotDir) {
    CommandDispatcher dispatcher(QStringLiteral("console-test"));
    int statuses = 0, metricCalls = 0;
    double tuned = 0;
    bool correctThread = true;
    dispatcher.addStandard(QStringLiteral("status"), [&](const QJsonObject &) {
        ++statuses;
        correctThread = QThread::currentThread() == qApp->thread();
        return QJsonObject{{QStringLiteral("receiver"), QStringLiteral("synthetic")}};
    });
    dispatcher.addStandard(QStringLiteral("metrics"), [&](const QJsonObject &) {
        ++metricCalls;
        return QJsonObject{{QStringLiteral("frames"), metricCalls}};
    });
    dispatcher.addStandard(QStringLiteral("tune"), [&](const QJsonObject &args) {
        tuned = args.value(QStringLiteral("frequencyHz")).toDouble();
        return QJsonObject{{QStringLiteral("frequencyHz"), tuned}};
    });
    CommandDispatcher::Spec echo;
    echo.name = QStringLiteral("echo");
    echo.inputSchema = {{QStringLiteral("type"), QStringLiteral("object")},
                        {QStringLiteral("additionalProperties"), true}};
    dispatcher.add(echo, [](const QJsonObject &args) { return args; });
    int translations = 0;
    CommandConsoleDialog dialog(&dispatcher, [&translations](const QString &name, const QString &fallback) {
        if (name.startsWith(QStringLiteral("command_console_"))) ++translations;
        return fallback;
    });
    dialog.show();
    settle(20);
    auto *input = child<QLineEdit>(dialog, "commandConsoleInput");
    auto *output = child<QPlainTextEdit>(dialog, "commandConsoleOutput");
    auto *metrics = child<QCheckBox>(dialog, "commandConsoleMetrics");
    check(!dialog.isModal() && translations > 0, "dialog modality/translation contract failed");
    check(child<QLabel>(dialog, "commandConsoleInstance")->toolTip().contains(dispatcher.instanceId()), "local identity missing");
    input->setText(QStringLiteral("status"));
    key(input, Qt::Key_Return);
    check(statuses == 1 && correctThread && output->toPlainText().contains(QStringLiteral("synthetic")), "local status did not use owning dispatcher");
    send(dialog, QStringLiteral("tune 100M"));
    check(tuned == 100e6, "human control context or shared frequency parser failed");
    input->setText(QStringLiteral("draft"));
    key(input, Qt::Key_Up);
    check(input->text() == QLatin1String("tune 100M"), "history up failed");
    key(input, Qt::Key_Up);
    check(input->text() == QLatin1String("status"), "history older command failed");
    key(input, Qt::Key_Down);
    key(input, Qt::Key_Down);
    check(input->text() == QLatin1String("draft"), "history draft restore failed");
    send(dialog, QStringLiteral("{\"command\":\"echo\",\"args\":{\"token\":\"typed-token-528\",\"array\":[{\"password\":\"typed-password-528\",\"secret\":\"typed-secret-528\"}]}}"));
    for (const auto &secret : {"typed-token-528", "typed-password-528", "typed-secret-528"})
        check(!output->toPlainText().contains(QString::fromLatin1(secret)), "secret leaked in input echo or local response");
    key(input, Qt::Key_Up);
    check(input->text() == QLatin1String("tune 100M"), "sensitive JSON entered history");
    metrics->setChecked(true);
    check(until([&metricCalls] { return metricCalls > 0; }, 1600), "visible metrics did not poll");
    dialog.hide();
    const int hiddenCalls = metricCalls;
    settle(1100);
    check(metricCalls == hiddenCalls && !child<QTimer>(dialog, "commandConsolePollTimer")->isActive(), "hidden console kept polling");
    dialog.show();
    metrics->setChecked(false);
    auto *catalog = child<QComboBox>(dialog, "commandConsoleCatalog");
    const int statusIndex = catalog->findText(QStringLiteral("status"));
    check(statusIndex > 0, "local catalog missing status");
    catalog->setCurrentIndex(statusIndex);
    child<QToolButton>(dialog, "commandConsoleHelp")->click();
    check(output->toPlainText().contains(QStringLiteral("inputSchema")), "selected command schema missing");
    child<QToolButton>(dialog, "commandConsoleCopy")->click();
    check(QApplication::clipboard()->text().contains(QStringLiteral("inputSchema")), "copy output failed");
    if (!snapshotDir.isEmpty()) {
        if (!snapshotMonoFamily.isEmpty()) output->setFont(QFont(snapshotMonoFamily, 9));
        check(visibleGlyphs(output->font()), "local terminal screenshot glyphs are blank");
        dialog.resize(720, 450);
        settle(20);
        check(dialog.grab().save(snapshotDir + QStringLiteral("/console-local.png")), "local snapshot failed");
    }
    for (int i = 0; i < 1200; ++i) output->appendPlainText(QString::number(i));
    check(output->maximumBlockCount() == 1000 && output->document()->blockCount() <= 1000, "terminal block limit failed");
    child<QToolButton>(dialog, "commandConsoleClear")->click();
    check(output->toPlainText().isEmpty(), "clear output failed");

    CommandDispatcher unsupported(QStringLiteral("without-metrics"));
    CommandConsoleDialog noMetrics(&unsupported);
    check(!child<QCheckBox>(noMetrics, "commandConsoleMetrics")->isEnabled(), "unsupported metrics were enabled");
}

void remoteRequests(const QString &snapshotDir) {
    CommandDispatcher dispatcher(QStringLiteral("remote-test"));
    int localExecutions = 0;
    dispatcher.addStandard(QStringLiteral("status"), [&](const QJsonObject &) {
        ++localExecutions;
        return QJsonObject{};
    });
    FakeServer server;
    CommandConsoleDialog dialog(&dispatcher);
    dialog.show();
    auto *target = child<QComboBox>(dialog, "commandConsoleTarget");
    target->setCurrentIndex(1);
    child<QSpinBox>(dialog, "commandConsolePort")->setValue(server.listener.serverPort());
    const QString token = QStringLiteral("remote-token-9472");
    child<QLineEdit>(dialog, "commandConsoleToken")->setText(token);
    auto *output = child<QPlainTextEdit>(dialog, "commandConsoleOutput");
    auto *sendButton = child<QToolButton>(dialog, "commandConsoleSend");
    auto *metrics = child<QCheckBox>(dialog, "commandConsoleMetrics");
    check(!metrics->isEnabled() && child<QLineEdit>(dialog, "commandConsoleToken")->echoMode() == QLineEdit::Password, "remote credential/metrics controls failed");
    send(dialog, QStringLiteral("status"));
    check(!sendButton->isEnabled() && !target->isEnabled(), "remote request was not single flight");
    check(until([&server] { return server.requests == 1; }), "remote request not received");
    check(server.request.value(QStringLiteral("token")).toString() == token &&
          server.request.value(QStringLiteral("request")).toObject().value(QStringLiteral("command")) == QLatin1String("status"), "remote token/request envelope incorrect");
    sendButton->click();
    settle(20);
    check(server.requests == 1 && localExecutions == 0, "remote request executed twice or locally");
    const QJsonObject response{{QStringLiteral("ok"), true}, {QStringLiteral("instanceId"), QStringLiteral("remote-instance-7")},
        {QStringLiteral("token"), token}, {QStringLiteral("echo"), token},
        {QStringLiteral("nested"), QJsonArray{QJsonObject{{QStringLiteral("password"), QStringLiteral("remote-password-819")},
            {QStringLiteral("secret"), QStringLiteral("remote-secret-819")}}}}};
    const auto wire = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
    server.peer->write(wire.left(7));
    settle(40);
    check(!sendButton->isEnabled(), "partial JSON line completed prematurely");
    server.peer->write(wire.mid(7));
    check(until([sendButton] { return sendButton->isEnabled(); }), "fragmented remote response did not complete");
    check(child<QLabel>(dialog, "commandConsoleInstance")->toolTip().contains(QStringLiteral("remote-instance-7")), "remote identity missing");
    for (const auto &secret : {token, QStringLiteral("remote-password-819"), QStringLiteral("remote-secret-819")})
        check(!output->toPlainText().contains(secret), "remote secret leaked in output");
    child<QToolButton>(dialog, "commandConsoleCopy")->click();
    check(!QApplication::clipboard()->text().contains(token), "copy exposed authentication token");

    server.handler = [](QTcpSocket *socket, const QJsonObject &) {
        const QJsonObject custom{{QStringLiteral("name"), QStringLiteral("remote.only")},
            {QStringLiteral("description"), QStringLiteral("Remote-only synthetic command")},
            {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("properties"), QJsonObject{{QStringLiteral("number"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}}}}},
            {QStringLiteral("positional"), QJsonArray{QStringLiteral("number")}}};
        socket->write(QJsonDocument(QJsonObject{{QStringLiteral("ok"), true},
            {QStringLiteral("commands"), QJsonArray{custom}}}).toJson(QJsonDocument::Compact) + '\n');
    };
    child<QToolButton>(dialog, "commandConsoleHelp")->click();
    auto *catalog = child<QComboBox>(dialog, "commandConsoleCatalog");
    check(until([catalog] { return catalog->findText(QStringLiteral("remote.only")) > 0; }), "remote catalog not applied");
    server.handler = [](QTcpSocket *socket, const QJsonObject &) { socket->write("{\"ok\":true}\n"); };
    send(dialog, QStringLiteral("remote.only 42"));
    check(until([sendButton] { return sendButton->isEnabled(); }), "remote-only command did not complete");
    const auto remoteEnvelope = server.request.value(QStringLiteral("request")).toObject();
    check(remoteEnvelope.value(QStringLiteral("command")) == QLatin1String("remote.only") &&
        remoteEnvelope.value(QStringLiteral("args")).toObject().value(QStringLiteral("number")).toInt() == 42 &&
        localExecutions == 0, "remote catalog did not use shared parser");
    child<QLineEdit>(dialog, "commandConsoleHost")->setText(QStringLiteral("localhost"));
    check(catalog->count() == 1 && child<QLabel>(dialog, "commandConsoleInstance")->toolTip().endsWith(QStringLiteral("--")),
          "endpoint edit retained stale remote catalog or identity");
    child<QLineEdit>(dialog, "commandConsoleHost")->setText(QStringLiteral("127.0.0.1"));

    server.handler = [](QTcpSocket *socket, const QJsonObject &) { socket->write("not-json\n"); };
    send(dialog, QStringLiteral("status"));
    check(until([sendButton] { return sendButton->isEnabled(); }) && output->toPlainText().contains(QStringLiteral("\"errorCode\": \"json\"")), "invalid JSON reply was not rejected");
    server.handler = [](QTcpSocket *socket, const QJsonObject &) {
        socket->write("{\"ok\":true}");
        socket->disconnectFromHost();
    };
    send(dialog, QStringLiteral("status"));
    check(until([sendButton] { return sendButton->isEnabled(); }) && output->toPlainText().contains(QStringLiteral("\"errorCode\": \"framing\"")), "missing reply newline was accepted");
    server.handler = [](QTcpSocket *socket, const QJsonObject &) { socket->write(QByteArray(1024 * 1024 + 32, 'x')); };
    send(dialog, QStringLiteral("status"));
    check(until([sendButton] { return sendButton->isEnabled(); }) && output->toPlainText().contains(QStringLiteral("reply_limit")), "oversize reply was accepted");

    server.handler = {};
    int beats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(25);
    QObject::connect(&heartbeat, &QTimer::timeout, &dialog, [&beats] { ++beats; });
    heartbeat.start();
    QElapsedTimer elapsed;
    elapsed.start();
    send(dialog, QStringLiteral("status"));
    check(until([sendButton] { return sendButton->isEnabled(); }, 3800), "remote timeout failed");
    check(elapsed.elapsed() >= 2800 && beats > 30 && output->toPlainText().contains(QStringLiteral("\"errorCode\": \"timeout\"")), "remote timeout blocked GUI or used wrong deadline");
    heartbeat.stop();
    send(dialog, QStringLiteral("status"));
    dialog.hide();
    check(!child<QTimer>(dialog, "commandConsoleRequestTimer")->isActive() && sendButton->isEnabled(), "hidden dialog did not cancel pending request");
    dialog.show();
    if (!snapshotDir.isEmpty()) {
        if (!snapshotMonoFamily.isEmpty()) output->setFont(QFont(snapshotMonoFamily, 9));
        check(visibleGlyphs(output->font()), "remote terminal screenshot glyphs are blank");
        dialog.resize(450, 260);
        settle(30);
        check(dialog.size() == QSize(450, 260), "compact remote layout exceeded minimum size");
        check(dialog.grab().save(snapshotDir + QStringLiteral("/console-remote-compact.png")), "remote snapshot failed");
    }
}

void agentSettings(const QString &snapshotDir) {
    CommandDispatcher dispatcher(QStringLiteral("agent-settings-test"));
    auto server = std::make_unique<CommandControlServer>(&dispatcher);
    CommandConsoleDialog dialog(&dispatcher);
    dialog.setControlServer(server.get());
    dialog.show();
    child<QToolButton>(dialog, "commandConsoleAgentApi")->click();
    auto *settings = child<QDialog>(dialog, "commandConsoleAgentDialog");
    auto *control = child<QCheckBox>(dialog, "commandConsoleAgentAllowControl");
    check(settings->isVisible() && !settings->isModal() && !control->isChecked() && !server->isListening(), "Agent API initial policy failed");
    auto *port = child<QSpinBox>(dialog, "commandConsoleAgentPort");
    check(port->minimum() == 0 && port->maximum() == 65535, "Agent API port range failed");
    port->setValue(0);
    child<QToolButton>(dialog, "commandConsoleAgentStart")->click();
    check(server->isListening() && server->port() > 0 && !server->allowControl(), "Agent API start or read-only default failed");
    auto *token = child<QLineEdit>(dialog, "commandConsoleAgentToken");
    check(token->isReadOnly() && token->echoMode() == QLineEdit::Password && token->text() == server->token(), "Agent API token field failed");
    auto *address = child<QLineEdit>(dialog, "commandConsoleAgentAddress");
    check(address->isReadOnly() && address->text() == QStringLiteral("127.0.0.1:%1").arg(server->port()), "Agent API bound address failed");
    child<QToolButton>(dialog, "commandConsoleAgentCopyToken")->click();
    check(QApplication::clipboard()->text() == server->token(), "Agent API explicit token copy failed");
    check(!child<QPlainTextEdit>(dialog, "commandConsoleOutput")->toPlainText().contains(server->token()), "Agent API token logged");
    if (!snapshotDir.isEmpty()) {
        settle(20);
        check(settings->grab().save(snapshotDir + QStringLiteral("/console-agent-api.png")), "Agent API snapshot failed");
    }
    control->setChecked(true);
    check(server->allowControl(), "Agent API control opt-in failed");
    dialog.hide();
    check(server->isListening() && !settings->isVisible(), "hiding console stopped owned server or left settings open");
    dialog.show();
    child<QToolButton>(dialog, "commandConsoleAgentApi")->click();
    child<QToolButton>(dialog, "commandConsoleAgentStop")->click();
    check(!server->isListening() && token->text().isEmpty(), "Agent API stop did not clear token field");
    server.reset();
    check(!child<QToolButton>(dialog, "commandConsoleAgentApi")->isEnabled(), "borrowed server destruction left controls enabled");
}
}

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    try {
        QString snapshots;
        const auto arguments = app.arguments();
        const int index = arguments.indexOf(QStringLiteral("--snapshot-dir"));
        if (index >= 0 && index + 1 < arguments.size()) {
            snapshots = QDir(arguments.at(index + 1)).absolutePath();
            check(QDir().mkpath(snapshots), "snapshot directory unavailable");
            prepareSnapshotFonts(app);
        }
        localAndHistory(snapshots);
        remoteRequests(snapshots);
        agentSettings(snapshots);
        std::cout << "Command console passed: " << checks << " checks (local human dispatch, history, "
            "visibility polling, remote framing/deadline/limits, redaction, Agent API ownership)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Command console failed: " << error.what() << '\n';
        return 1;
    }
}
