#include "commandconsoledialog.h"
#include "../core/commanddispatcher.h"
#include "../network/commandcontrolserver.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QHideEvent>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QTcpSocket>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <exception>
#include <utility>

namespace {
constexpr int ReplyLimit = 1024 * 1024;
constexpr int RequestTimeoutMs = 3000;
constexpr int HistoryLimit = 100;

bool sensitiveKey(QString key) {
    key.remove(QLatin1Char('_'));
    key.remove(QLatin1Char('-'));
    key = key.toLower();
    return key.contains(QStringLiteral("token")) || key.contains(QStringLiteral("password")) ||
           key.contains(QStringLiteral("secret")) || key == QLatin1String("authorization") ||
           key == QLatin1String("apikey") || key == QLatin1String("protocolprofiles") ||
           key.contains(QStringLiteral("psk")) || key.contains(QStringLiteral("privatekey")) ||
           key.contains(QStringLiteral("appkey")) || key.contains(QStringLiteral("networkkey")) ||
           key.contains(QStringLiteral("applicationkey")) || key.contains(QStringLiteral("nwkskey")) ||
           key.contains(QStringLiteral("appskey")) || key == QLatin1String("key");
}

QToolButton *tool(QWidget *parent, const char *name, const QIcon &icon, const QString &tip) {
    auto *button = new QToolButton(parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setIcon(icon);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setFixedSize(28, 28);
    return button;
}
}

CommandConsoleDialog::CommandConsoleDialog(CommandDispatcher *dispatcher, Translator translator,
                                           QWidget *parent)
    : QDialog(parent, Qt::Window), dispatcher_(dispatcher), translator_(std::move(translator)) {
    setObjectName(QStringLiteral("commandConsoleDialog"));
    setWindowTitle(text("command_console_title", "Command console"));
    setModal(false);
    resize(720, 450);
    setMinimumSize(450, 260);
    setStyleSheet(QStringLiteral(
        "QDialog#commandConsoleDialog, QDialog#commandConsoleAgentDialog { background:#202326; color:#e6e9eb; }"
        "QLabel, QCheckBox { color:#e6e9eb; }"
        "QLineEdit, QPlainTextEdit, QComboBox, QSpinBox { background:#151719; color:#e6e9eb;"
        " border:1px solid #4b5156; border-radius:3px; padding:3px; selection-background-color:#307d72; }"
        "QToolButton { background:#30353a; border:1px solid #50575d; border-radius:3px; }"
        "QToolButton:hover { background:#41484d; border-color:#68b9a8; }"
        "QToolButton:disabled, QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { color:#858b90; }"
        "QCheckBox:disabled { color:#858b90; }"
        "QComboBox QAbstractItemView { background:#202326; color:#e6e9eb; selection-background-color:#307d72; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(6);
    auto *top = new QHBoxLayout;
    target_ = new QComboBox(this);
    target_->setObjectName(QStringLiteral("commandConsoleTarget"));
    target_->addItem(text("command_console_local", "Local"));
    target_->addItem(text("command_console_remote", "Remote"));
    target_->setMinimumWidth(90);
    identity_ = new QLabel(this);
    identity_->setObjectName(QStringLiteral("commandConsoleInstance"));
    identity_->setTextFormat(Qt::PlainText);
    identity_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    identity_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identity_->setMinimumWidth(0);
    agentButton_ = tool(this, "commandConsoleAgentApi",
        style()->standardIcon(QStyle::SP_ComputerIcon), text("command_console_agent_api", "Agent API"));
    agentButton_->setEnabled(false);
    top->addWidget(target_);
    top->addWidget(identity_, 1);
    top->addWidget(agentButton_);
    root->addLayout(top);

    remoteFields_ = new QWidget(this);
    remoteFields_->setObjectName(QStringLiteral("commandConsoleRemoteFields"));
    auto *remote = new QGridLayout(remoteFields_);
    remote->setContentsMargins(0, 0, 0, 0);
    remote->setHorizontalSpacing(6);
    remote->setVerticalSpacing(4);
    host_ = new QLineEdit(QStringLiteral("127.0.0.1"), remoteFields_);
    host_->setObjectName(QStringLiteral("commandConsoleHost"));
    host_->setMaxLength(253);
    host_->setMinimumWidth(0);
    port_ = new QSpinBox(remoteFields_);
    port_->setObjectName(QStringLiteral("commandConsolePort"));
    port_->setRange(0, 65535);
    port_->setSpecialValueText(QStringLiteral("--"));
    port_->setFixedWidth(80);
    token_ = new QLineEdit(remoteFields_);
    token_->setObjectName(QStringLiteral("commandConsoleToken"));
    token_->setEchoMode(QLineEdit::Password);
    token_->setMaxLength(4096);
    token_->setMinimumWidth(0);
    remote->addWidget(new QLabel(text("command_console_host", "Host"), remoteFields_), 0, 0);
    remote->addWidget(host_, 0, 1);
    remote->addWidget(new QLabel(text("command_console_port", "Port"), remoteFields_), 0, 2);
    remote->addWidget(port_, 0, 3);
    remote->addWidget(new QLabel(text("command_console_token", "Token"), remoteFields_), 1, 0);
    remote->addWidget(token_, 1, 1, 1, 3);
    remote->setColumnStretch(1, 1);
    root->addWidget(remoteFields_);
    remoteFields_->hide();

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(5);
    catalog_ = new QComboBox(this);
    catalog_->setObjectName(QStringLiteral("commandConsoleCatalog"));
    catalog_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    catalog_->setMinimumContentsLength(8);
    catalog_->setMinimumWidth(70);
    metrics_ = new QCheckBox(text("command_console_metrics", "Metrics"), this);
    metrics_->setObjectName(QStringLiteral("commandConsoleMetrics"));
    help_ = tool(this, "commandConsoleHelp", style()->standardIcon(QStyle::SP_DialogHelpButton),
                 text("command_console_command_help", "Command catalog / selected command"));
    auto *clear = tool(this, "commandConsoleClear",
        QIcon::fromTheme(QStringLiteral("edit-clear"), style()->standardIcon(QStyle::SP_DialogResetButton)),
        text("command_console_clear", "Clear output"));
    auto *copy = tool(this, "commandConsoleCopy",
        QIcon::fromTheme(QStringLiteral("edit-copy"), style()->standardIcon(QStyle::SP_FileIcon)),
        text("command_console_copy", "Copy output / selection"));
    toolbar->addWidget(catalog_, 1);
    toolbar->addWidget(metrics_);
    toolbar->addWidget(help_);
    toolbar->addWidget(clear);
    toolbar->addWidget(copy);
    root->addLayout(toolbar);

    output_ = new QPlainTextEdit(this);
    output_->setObjectName(QStringLiteral("commandConsoleOutput"));
    output_->setReadOnly(true);
    output_->setUndoRedoEnabled(false);
    output_->setMaximumBlockCount(1000);
    output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output_->setMinimumHeight(40);
    root->addWidget(output_, 1);
    auto *entry = new QHBoxLayout;
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("commandConsoleInput"));
    input_->setMaxLength(32768);
    input_->setMinimumWidth(0);
    input_->setPlaceholderText(text("command_console_command", "Command"));
    input_->installEventFilter(this);
    send_ = tool(this, "commandConsoleSend", style()->standardIcon(QStyle::SP_ArrowRight),
                 text("command_console_send", "Send command"));
    entry->addWidget(input_, 1);
    entry->addWidget(send_);
    root->addLayout(entry);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("commandConsoleStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setMinimumWidth(0);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    status_->setFixedHeight(status_->fontMetrics().lineSpacing() + 2);
    root->addWidget(status_);

    pollTimer_ = new QTimer(this);
    pollTimer_->setObjectName(QStringLiteral("commandConsolePollTimer"));
    pollTimer_->setInterval(1000);
    requestTimer_ = new QTimer(this);
    requestTimer_->setObjectName(QStringLiteral("commandConsoleRequestTimer"));
    requestTimer_->setSingleShot(true);
    requestTimer_->setTimerType(Qt::PreciseTimer);
    requestTimer_->setInterval(RequestTimeoutMs);
    connect(send_, &QToolButton::clicked, this, [this] { executeLine(input_->text()); });
    connect(input_, &QLineEdit::returnPressed, this, [this] { executeLine(input_->text()); });
    connect(clear, &QToolButton::clicked, output_, &QPlainTextEdit::clear);
    connect(copy, &QToolButton::clicked, this, [this] {
        const auto selected = output_->textCursor().selectedText();
        QApplication::clipboard()->setText(redacted(selected.isEmpty() ? output_->toPlainText() : selected));
    });
    connect(help_, &QToolButton::clicked, this, &CommandConsoleDialog::showHelp);
    connect(agentButton_, &QToolButton::clicked, this, &CommandConsoleDialog::openAgentSettings);
    connect(catalog_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (index > 0) {
            input_->setText(catalog_->itemData(index).toJsonObject().value(QStringLiteral("name")).toString());
            input_->setFocus();
        }
    });
    connect(target_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        remoteFields_->setVisible(target_->currentIndex() == 1);
        metrics_->setChecked(false);
        refreshCatalog();
        setStatus(text("command_console_ready", "Ready"));
        updateControls();
    });
    const auto endpointChanged = [this] {
        if (target_->currentIndex() == 1 && !busy_) refreshCatalog();
    };
    connect(host_, &QLineEdit::textChanged, this, endpointChanged);
    connect(port_, QOverload<int>::of(&QSpinBox::valueChanged), this, endpointChanged);
    connect(token_, &QLineEdit::textChanged, this, endpointChanged);
    connect(metrics_, &QCheckBox::toggled, this, &CommandConsoleDialog::updatePolling);
    connect(pollTimer_, &QTimer::timeout, this, [this] {
        if (isVisible() && !busy_ && target_->currentIndex() == 0 && metricsSupported_)
            executeLine(QStringLiteral("metrics"), true);
    });
    connect(requestTimer_, &QTimer::timeout, this, [this] {
        failRemote("timeout", text("command_console_timeout", "Remote request timed out (3 s)"));
    });
    refreshCatalog();
    setStatus(text("command_console_ready", "Ready"));
    updateControls();
}

CommandConsoleDialog::~CommandConsoleDialog() {
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
    }
}

QString CommandConsoleDialog::text(const char *key, const char *fallback) const {
    const QString value = QString::fromUtf8(fallback);
    return translator_ ? translator_(QString::fromLatin1(key), value) : value;
}

void CommandConsoleDialog::rememberSecret(const QString &secret) {
    if (secret.isEmpty() || secrets_.contains(secret)) return;
    secrets_.append(secret);
    while (secrets_.size() > 8) secrets_.removeFirst();
}

QString CommandConsoleDialog::redacted(QString value) const {
    QStringList secrets = secrets_;
    secrets.append(activeToken_);
    if (token_) secrets.append(token_->text());
    if (agentToken_) secrets.append(agentToken_->text());
    if (controlServer_) secrets.append(controlServer_->token());
    for (const auto &secret : secrets)
        if (!secret.isEmpty()) value.replace(secret, QStringLiteral("[redacted]"));
    return value;
}

QJsonValue CommandConsoleDialog::sanitized(const QJsonValue &value, int depth) const {
    if (depth >= 64) return QStringLiteral("[depth limit]");
    if (value.isObject()) {
        QJsonObject object;
        const auto original = value.toObject();
        for (auto it = original.begin(); it != original.end(); ++it)
            object.insert(redacted(it.key()), sensitiveKey(it.key()) ? QJsonValue(QStringLiteral("[redacted]")) : sanitized(it.value(), depth + 1));
        return object;
    }
    if (value.isArray()) {
        QJsonArray array;
        for (const auto &item : value.toArray()) array.append(sanitized(item, depth + 1));
        return array;
    }
    return value.isString() ? QJsonValue(redacted(value.toString())) : value;
}

QString CommandConsoleDialog::inputEcho(const QString &line, const QJsonObject &parsed) const {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(line.toUtf8(), &error);
    if (document.isObject())
        return redacted(QString::fromUtf8(QJsonDocument(sanitized(document.object()).toObject()).toJson(QJsonDocument::Compact)));
    if (line.trimmed().startsWith(QLatin1Char('{'))) return QStringLiteral("[invalid JSON]");
    const auto safe = sanitized(parsed).toObject();
    if (safe != parsed) return redacted(QString::fromUtf8(QJsonDocument(safe).toJson(QJsonDocument::Compact)));
    return redacted(line);
}

void CommandConsoleDialog::appendReply(const QJsonObject &reply) {
    const auto safe = sanitized(reply).toObject();
    const auto bytes = QJsonDocument(safe).toJson(QJsonDocument::Indented);
    output_->appendPlainText(redacted(QString::fromUtf8(bytes.constData(), qMin(bytes.size(), ReplyLimit))).left(ReplyLimit));
    if (bytes.size() > ReplyLimit) output_->appendPlainText(QStringLiteral("[output truncated]"));
    const auto instance = safe.value(QStringLiteral("instanceId")).toString();
    if (!instance.isEmpty()) setIdentity(instance);
    const bool okay = safe.value(QStringLiteral("ok")).toBool(true);
    const auto error = safe.value(QStringLiteral("error")).toString();
    setStatus(okay ? text("command_console_ready", "Ready") :
              (error.isEmpty() ? text("command_console_failed", "Command failed") : error), !okay);
}

void CommandConsoleDialog::setStatus(const QString &message, bool error) {
    const auto safe = redacted(message);
    status_->setToolTip(safe);
    status_->setText(status_->fontMetrics().elidedText(safe, Qt::ElideRight, status_->width()));
    status_->setStyleSheet(error ? QStringLiteral("color:#f19b93;") : QStringLiteral("color:#a6c9b2;"));
}

void CommandConsoleDialog::setIdentity(const QString &identity) {
    identityText_ = text("command_console_instance", "Instance") + QStringLiteral(": ") + redacted(identity);
    identity_->setToolTip(identityText_);
    identity_->setText(identity_->fontMetrics().elidedText(identityText_, Qt::ElideMiddle, identity_->width()));
}

void CommandConsoleDialog::updatePolling() {
    const bool run = isVisible() && !busy_ && target_->currentIndex() == 0 &&
        metricsSupported_ && metrics_->isChecked();
    if (run && !pollTimer_->isActive()) pollTimer_->start();
    else if (!run) pollTimer_->stop();
}

void CommandConsoleDialog::updateControls() {
    const bool usable = dispatcher_ != nullptr;
    target_->setEnabled(!busy_);
    remoteFields_->setEnabled(!busy_);
    input_->setEnabled(usable && !busy_);
    send_->setEnabled(usable && !busy_);
    catalog_->setEnabled(usable && !busy_);
    help_->setEnabled(usable && !busy_);
    metrics_->setEnabled(usable && target_->currentIndex() == 0 && metricsSupported_);
    if (!metrics_->isEnabled()) metrics_->setChecked(false);
    updatePolling();
}

void CommandConsoleDialog::applyCatalog(const QJsonObject &reply) {
    const QSignalBlocker blocker(catalog_);
    catalog_->clear();
    catalog_->addItem(text("command_console_commands", "Commands"));
    metricsSupported_ = false;
    for (const auto &item : reply.value(QStringLiteral("commands")).toArray()) {
        const auto spec = item.toObject();
        const auto name = spec.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) continue;
        catalog_->addItem(name, spec);
        catalog_->setItemData(catalog_->count() - 1, spec.value(QStringLiteral("description")).toString(), Qt::ToolTipRole);
        if (name == QLatin1String("metrics")) metricsSupported_ = true;
    }
    updateControls();
}

void CommandConsoleDialog::refreshCatalog(bool display) {
    if (target_->currentIndex() == 1) {
        applyCatalog({});
        setIdentity(QStringLiteral("--"));
        if (display) sendRemote({{QStringLiteral("command"), QStringLiteral("commands")},
                                 {QStringLiteral("args"), QJsonObject{}}});
    } else if (dispatcher_) {
        const auto reply = dispatcher_->catalog(CommandDispatcher::Context::human());
        applyCatalog(sanitized(reply).toObject());
        setIdentity(dispatcher_->instanceId());
        if (display) appendReply(reply);
    } else {
        applyCatalog({});
        setIdentity(QStringLiteral("--"));
    }
}

void CommandConsoleDialog::showHelp() {
    if (busy_) return;
    if (catalog_->currentIndex() > 0) appendReply(catalog_->currentData().toJsonObject());
    else refreshCatalog(true);
}

void CommandConsoleDialog::executeLine(const QString &raw, bool poll) {
    const auto line = raw.trimmed();
    if (busy_ || line.isEmpty()) return;
    if (!dispatcher_) {
        appendReply(CommandDispatcher::failure(QStringLiteral("unavailable"), text("command_console_unavailable", "Dispatcher unavailable")));
        return;
    }
    const auto parsed = target_->currentIndex() == 1 ? parseRemoteText(line) : dispatcher_->parseText(line);
    if (!poll) {
        const auto echo = inputEcho(line, parsed);
        output_->appendPlainText(QStringLiteral("> ") + echo);
        if (echo == line && (history_.isEmpty() || history_.last() != line)) {
            history_.append(line);
            while (history_.size() > HistoryLimit) history_.removeFirst();
        }
        historyIndex_ = history_.size();
        historyDraft_.clear();
        input_->clear();
    }
    if (target_->currentIndex() == 1) {
        if (parsed.value(QStringLiteral("ok")) == QJsonValue(false)) appendReply(parsed);
        else sendRemote(parsed);
        return;
    }
    busy_ = true;
    updateControls();
    try {
        auto context = poll ? CommandDispatcher::Context{} : CommandDispatcher::Context::human();
        if (poll) context.origin = QStringLiteral("console-metrics");
        const auto reply = dispatcher_->executeText(line, context);
        if (!poll && reply.value("command") == QLatin1String("mesh.service") && reply.value("ok").toBool() && !reply.value("token").toString().isEmpty()) {
            const auto secret = reply.value("token").toString();
            rememberSecret(secret);
            const auto pairing = reply.value("pairingSecret").toString();if(!pairing.isEmpty())rememberSecret(pairing);
            auto dialog = new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setAttribute(Qt::WA_DontShowOnScreen,testAttribute(Qt::WA_DontShowOnScreen));dialog->setObjectName("meshNodeStartupConnection");
            dialog->setWindowTitle(text("command_console_mesh_connection", "Mesh node connection"));
            auto layout = new QVBoxLayout(dialog);
            auto address = new QLabel(QStringLiteral("%1:%2").arg(reply.value("bind").toString()).arg(reply.value("port").toInt()),dialog);
            address->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(address);
            auto field = new QLineEdit(secret,dialog);field->setReadOnly(true);field->setEchoMode(QLineEdit::Password);field->setObjectName("meshNodeStartupToken");layout->addWidget(field);
            if(!pairing.isEmpty()){
                layout->addWidget(new QLabel(text("command_console_mesh_pair_code","Pairing code (LAN registration)"),dialog));
                auto code=new QLineEdit(pairing,dialog);code->setReadOnly(true);code->setObjectName("meshNodePairingCode");layout->addWidget(code);
            }
            auto copy = new QPushButton(text("command_console_copy_token", "Copy session token"),dialog);layout->addWidget(copy);
            copy->setObjectName("meshNodeStartupCopy");
            connect(copy,&QPushButton::clicked,dialog,[secret]{QApplication::clipboard()->setText(secret);});
            dialog->resize(430,pairing.isEmpty()?130:220);dialog->show();
        }
        appendReply(reply);
        if (reply.value(QStringLiteral("commands")).isArray()) refreshCatalog();
        if (poll && !reply.value(QStringLiteral("ok")).toBool(true) &&
            (reply.value(QStringLiteral("errorCode")).toString() == QLatin1String("unsupported_command") ||
             reply.value(QStringLiteral("errorCode")).toString() == QLatin1String("unknown_command"))) {
            metricsSupported_ = false;
            metrics_->setChecked(false);
        }
    } catch (const std::exception &) {
        appendReply(CommandDispatcher::failure(QStringLiteral("exception"), text("command_console_exception", "Command execution failed")));
    } catch (...) {
        appendReply(CommandDispatcher::failure(QStringLiteral("exception"), text("command_console_exception", "Command execution failed")));
    }
    busy_ = false;
    updateControls();
    if (!poll) input_->setFocus();
}

QJsonObject CommandConsoleDialog::parseRemoteText(const QString &line) const {
    if (line.startsWith(QLatin1Char('{'))) return dispatcher_->parseText(line);
    const auto name = line.simplified().section(QLatin1Char(' '), 0, 0).toLower();
    CommandDispatcher parser(QStringLiteral("console-remote-parser"));
    auto spec = CommandDispatcher::standardSpec(name);
    for (int index = 1; index < catalog_->count(); ++index) {
        const auto entry = catalog_->itemData(index).toJsonObject();
        if (entry.value(QStringLiteral("name")).toString() != name) continue;
        spec.name = name;
        spec.description = entry.value(QStringLiteral("description")).toString();
        spec.inputSchema = entry.value(QStringLiteral("inputSchema")).toObject();
        spec.positional.clear();
        for (const auto &field : entry.value(QStringLiteral("positional")).toArray())
            spec.positional.append(field.toString());
        break;
    }
    // This temporary dispatcher is a parser only; no handler is executed locally.
    if (!spec.name.isEmpty()) parser.add(spec, [](const QJsonObject &) { return QJsonObject{}; });
    return parser.parseText(line);
}

void CommandConsoleDialog::sendRemote(const QJsonObject &request) {
    if (busy_) return;
    if (host_->text().trimmed().isEmpty() || port_->value() == 0) {
        appendReply(CommandDispatcher::failure(QStringLiteral("target"), text("command_console_target_required", "Remote host and port are required")));
        return;
    }
    activeToken_ = token_->text();
    remoteCatalog_ = request.value(QStringLiteral("command")).toString() == QLatin1String("commands") &&
        !request.value(QStringLiteral("args")).toObject().contains(QStringLiteral("name"));
    rememberSecret(activeToken_);
    const QByteArray wire = QJsonDocument(QJsonObject{{QStringLiteral("token"), activeToken_},
        {QStringLiteral("request"), request}}).toJson(QJsonDocument::Compact) + '\n';
    incoming_.clear();
    busy_ = true;
    updateControls();
    setStatus(text("command_console_connecting", "Connecting"));
    auto *socket = new QTcpSocket(this);
    socket_ = socket;
    socket->setReadBufferSize(ReplyLimit + 1);
    connect(socket, &QTcpSocket::connected, this, [this, socket, wire] {
        if (socket_ != socket) return;
        if (socket->write(wire) != wire.size()) {
            failRemote("write", text("command_console_write_failed", "Remote request write failed"));
            return;
        }
        setStatus(text("command_console_waiting", "Waiting for reply"));
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readRemote(socket); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (socket_ != socket) return;
        readRemote(socket);
        if (socket_ == socket) failRemote("framing", text("command_console_incomplete", "Remote peer closed before a complete JSON line"));
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError error) {
        if (socket_ != socket || error == QAbstractSocket::RemoteHostClosedError) return;
        failRemote("network", text("command_console_network_failed", "Remote connection failed") + QStringLiteral(": ") + socket->errorString());
    });
    requestTimer_->start();
    socket->connectToHost(host_->text().trimmed(), quint16(port_->value()));
}

void CommandConsoleDialog::readRemote(QTcpSocket *socket) {
    if (socket_ != socket) return;
    incoming_.append(socket->read(qMin<qint64>(socket->bytesAvailable(), ReplyLimit + 1 - incoming_.size())));
    const int newline = incoming_.indexOf('\n');
    if (newline >= 0) {
        if (newline > ReplyLimit) {
            failRemote("reply_limit", text("command_console_reply_limit", "Remote reply exceeds 1 MiB"));
            return;
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(incoming_.left(newline), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            failRemote("json", text("command_console_invalid_reply", "Remote reply is not a JSON object"));
        else finishRemote(document.object());
    } else if (incoming_.size() > ReplyLimit) {
        failRemote("reply_limit", text("command_console_reply_limit", "Remote reply exceeds 1 MiB"));
    }
}

void CommandConsoleDialog::finishRemote(const QJsonObject &reply) {
    if (!socket_) return;
    requestTimer_->stop();
    auto *socket = socket_;
    socket_ = nullptr;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    incoming_.clear();
    appendReply(reply);
    if (remoteCatalog_ && reply.value(QStringLiteral("commands")).isArray()) applyCatalog(sanitized(reply).toObject());
    activeToken_.clear();
    busy_ = false;
    updateControls();
    if (isVisible()) input_->setFocus();
}

void CommandConsoleDialog::failRemote(const char *code, const QString &message) {
    if (socket_) finishRemote(CommandDispatcher::failure(QString::fromLatin1(code), message));
}

bool CommandConsoleDialog::eventFilter(QObject *watched, QEvent *event) {
    if (watched == input_ && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (!busy_ && (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
            if (history_.isEmpty()) return true;
            if (historyIndex_ == history_.size()) historyDraft_ = input_->text();
            historyIndex_ = qBound(0, historyIndex_ + (key->key() == Qt::Key_Up ? -1 : 1), history_.size());
            input_->setText(historyIndex_ == history_.size() ? historyDraft_ : history_.at(historyIndex_));
            return true;
        }
    }
    if (watched == agentDialog_ && event->type() == QEvent::Show) refreshAgentSettings();
    return QDialog::eventFilter(watched, event);
}

void CommandConsoleDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    if (target_->currentIndex() == 0) refreshCatalog();
    updatePolling();
    input_->setFocus();
}

void CommandConsoleDialog::hideEvent(QHideEvent *event) {
    pollTimer_->stop();
    if (agentDialog_) agentDialog_->hide();
    if (socket_) failRemote("cancelled", text("command_console_cancelled", "Remote request cancelled"));
    QDialog::hideEvent(event);
}

void CommandConsoleDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (identity_) identity_->setText(identity_->fontMetrics().elidedText(identityText_, Qt::ElideMiddle, identity_->width()));
    if (status_) status_->setText(status_->fontMetrics().elidedText(status_->toolTip(), Qt::ElideRight, status_->width()));
}

void CommandConsoleDialog::setControlServer(CommandControlServer *server) {
    if (controlServer_) disconnect(controlServer_, nullptr, this, nullptr);
    controlServer_ = server;
    agentButton_->setEnabled(server != nullptr);
    if (server) {
        rememberSecret(server->token());
        connect(server, &CommandControlServer::statusChanged, this, [this] {
            if (agentDialog_ && agentDialog_->isVisible()) refreshAgentSettings();
        });
        connect(server, &QObject::destroyed, this, [this] {
            controlServer_ = nullptr;
            agentButton_->setEnabled(false);
            refreshAgentSettings();
        });
    }
    refreshAgentSettings();
}

void CommandConsoleDialog::openAgentSettings() {
    if (!controlServer_) return;
    if (!agentDialog_) {
        agentDialog_ = new QDialog(this, Qt::Window);
        agentDialog_->setObjectName(QStringLiteral("commandConsoleAgentDialog"));
        agentDialog_->setWindowTitle(text("command_console_agent_api", "Agent API"));
        agentDialog_->setModal(false);
        agentDialog_->resize(420, 240);
        agentDialog_->setMinimumWidth(340);
        agentDialog_->installEventFilter(this);
        auto *form = new QFormLayout(agentDialog_);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        agentPort_ = new QSpinBox(agentDialog_);
        agentPort_->setObjectName(QStringLiteral("commandConsoleAgentPort"));
        agentPort_->setRange(0, 65535);
        agentPort_->setSpecialValueText(text("command_console_auto_port", "Auto"));
        agentControl_ = new QCheckBox(text("command_console_allow_control", "Allow receiver control"), agentDialog_);
        agentControl_->setObjectName(QStringLiteral("commandConsoleAgentAllowControl"));
        agentControl_->setChecked(false);
        agentAddress_ = new QLineEdit(agentDialog_);
        agentAddress_->setObjectName(QStringLiteral("commandConsoleAgentAddress"));
        agentAddress_->setReadOnly(true);
        agentAddress_->setMinimumWidth(0);
        auto *tokenRow = new QWidget(agentDialog_);
        auto *tokenLayout = new QHBoxLayout(tokenRow);
        tokenLayout->setContentsMargins(0, 0, 0, 0);
        agentToken_ = new QLineEdit(tokenRow);
        agentToken_->setObjectName(QStringLiteral("commandConsoleAgentToken"));
        agentToken_->setReadOnly(true);
        agentToken_->setEchoMode(QLineEdit::Password);
        agentToken_->setMinimumWidth(0);
        agentCopy_ = tool(tokenRow, "commandConsoleAgentCopyToken",
            QIcon::fromTheme(QStringLiteral("edit-copy"), style()->standardIcon(QStyle::SP_FileIcon)),
            text("command_console_copy_token", "Copy session token"));
        tokenLayout->addWidget(agentToken_, 1);
        tokenLayout->addWidget(agentCopy_);
        agentStatus_ = new QLabel(agentDialog_);
        agentStatus_->setObjectName(QStringLiteral("commandConsoleAgentStatus"));
        agentStatus_->setTextFormat(Qt::PlainText);
        agentStatus_->setWordWrap(true);
        auto *actions = new QWidget(agentDialog_);
        auto *actionLayout = new QHBoxLayout(actions);
        actionLayout->setContentsMargins(0, 0, 0, 0);
        agentStart_ = tool(actions, "commandConsoleAgentStart", style()->standardIcon(QStyle::SP_MediaPlay),
            text("command_console_start_api", "Start loopback Agent API"));
        agentStop_ = tool(actions, "commandConsoleAgentStop", style()->standardIcon(QStyle::SP_MediaStop),
            text("command_console_stop_api", "Stop Agent API"));
        actionLayout->addWidget(agentStart_);
        actionLayout->addWidget(agentStop_);
        actionLayout->addStretch(1);
        form->addRow(text("command_console_port", "Port"), agentPort_);
        form->addRow(agentControl_);
        form->addRow(text("command_console_bound_address", "Bound address"), agentAddress_);
        form->addRow(text("command_console_token", "Token"), tokenRow);
        form->addRow(agentStatus_);
        form->addRow(actions);
        connect(agentStart_, &QToolButton::clicked, this, [this] {
            if (!controlServer_) return;
            QString error;
            if (!controlServer_->start(quint16(agentPort_->value()), &error)) {
                agentStatus_->setText(redacted(error));
                return;
            }
            refreshAgentSettings();
        });
        connect(agentStop_, &QToolButton::clicked, this, [this] {
            if (controlServer_) controlServer_->stop();
            refreshAgentSettings();
        });
        connect(agentControl_, &QCheckBox::toggled, this, [this](bool enabled) {
            if (controlServer_) controlServer_->setAllowControl(enabled);
        });
        connect(agentCopy_, &QToolButton::clicked, this, [this] {
            if (controlServer_ && controlServer_->isListening())
                QApplication::clipboard()->setText(controlServer_->token());
        });
    }
    agentDialog_->show();
    refreshAgentSettings();
    agentDialog_->raise();
    agentDialog_->activateWindow();
}

void CommandConsoleDialog::refreshAgentSettings() {
    if (!agentDialog_ || !agentDialog_->isVisible()) return;
    const bool available = controlServer_ != nullptr;
    const bool listening = available && controlServer_->isListening();
    agentStart_->setEnabled(available && !listening);
    agentStop_->setEnabled(listening);
    agentPort_->setEnabled(available && !listening);
    agentControl_->setEnabled(available);
    agentCopy_->setEnabled(listening);
    const QSignalBlocker blocker(agentControl_);
    agentControl_->setChecked(available && controlServer_->allowControl());
    if (listening) agentPort_->setValue(controlServer_->port());
    agentAddress_->setText(listening ? QStringLiteral("127.0.0.1:%1").arg(controlServer_->port()) : QStringLiteral("--"));
    const auto token = available ? controlServer_->token() : QString();
    rememberSecret(agentToken_->text());
    rememberSecret(token);
    agentToken_->setText(token);
    agentStatus_->setText(!available ? text("command_console_api_unavailable", "Agent API unavailable") :
        listening ? text("command_console_listening", "Listening on loopback") : text("command_console_stopped", "Stopped"));
}
