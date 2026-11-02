#include "commanddispatcher.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QUuid>
#include <cmath>
#include <exception>

namespace {
QJsonObject schema(const QJsonObject &properties = {}, const QJsonArray &required = {}) {
    return {{"type", "object"}, {"properties", properties}, {"required", required},
            {"additionalProperties", false}};
}
QJsonObject number(double minimum, double maximum, bool integer = false) {
    return {{"type", integer ? "integer" : "number"}, {"minimum", minimum}, {"maximum", maximum}};
}
QJsonObject choice(const QJsonArray &values) { return {{"type", "string"}, {"enum", values}}; }
QString accessName(CommandDispatcher::Access access) {
    switch (access) {
    case CommandDispatcher::Access::Control: return QStringLiteral("control");
    case CommandDispatcher::Access::Files: return QStringLiteral("files");
    case CommandDispatcher::Access::Transmit: return QStringLiteral("transmit");
    default: return QStringLiteral("read");
    }
}
bool frequencyNumber(const QString &text, double &value) {
    static const QRegularExpression pattern(QStringLiteral(
        "^([+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?)([kKmMgG]?)$"));
    const auto match = pattern.match(text);
    if (!match.hasMatch()) return false;
    bool ok = false;
    value = match.captured(1).toDouble(&ok);
    const auto suffix = match.captured(2).toLower();
    if (suffix == QLatin1String("k")) value *= 1e3;
    else if (suffix == QLatin1String("m")) value *= 1e6;
    else if (suffix == QLatin1String("g")) value *= 1e9;
    return ok && std::isfinite(value);
}
}

CommandDispatcher::Context CommandDispatcher::Context::human() {
    Context context;
    context.origin = QStringLiteral("human");
    context.allowControl = context.allowFiles = true;
    return context;
}

CommandDispatcher::CommandDispatcher(const QString &frontend)
    : instanceId_(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      frontend_(frontend), thread_(QThread::currentThread()) {
    addStandard(QStringLiteral("commands"), [this](const QJsonObject &) { return QJsonObject{}; });
    addStandard(QStringLiteral("audit"), [this](const QJsonObject &) { return QJsonObject{}; });
}

QJsonObject CommandDispatcher::failure(const QString &code, const QString &message) {
    return {{"ok", false}, {"errorCode", code}, {"error", message}};
}

CommandDispatcher::Spec CommandDispatcher::standardSpec(const QString &name) {
    Spec spec; spec.name = name; spec.inputSchema = schema();
    if (name == QLatin1String("commands")) {
        spec.description = QStringLiteral("List supported commands, schemas and session identity");
        spec.inputSchema = schema({{"name", QJsonObject{{"type", "string"}, {"maxLength", 64}}}});
        spec.positional = QStringList{QStringLiteral("name")};
    } else if (name == QLatin1String("audit")) {
        spec.description = QStringLiteral("Read bounded command outcomes; no arguments, keys or raw logs");
        spec.inputSchema = schema({{"after", QJsonObject{{"type", "string"}, {"pattern", "^[0-9]{1,20}$"}}},
                                  {"limit", number(1, 100, true)}});
        spec.positional = QStringList{QStringLiteral("after"), QStringLiteral("limit")};
    } else if (name == QLatin1String("status") || name == QLatin1String("metrics") ||
               name == QLatin1String("capabilities") || name == QLatin1String("spectrum")) {
        spec.description = QStringLiteral("Read current %1 without opening hardware or enabling analysis").arg(name);
    } else if (name == QLatin1String("trace")) {
        spec.description = QStringLiteral("Read a page of the existing exact spectrum; no new FFT");
        spec.inputSchema = schema({{"source", choice({"live", "max", "min", "avg", "detected", "p50", "p90", "p99"})},
            {"offset", number(0, 50000000, true)}, {"limit", number(1, 1024, true)}});
        spec.positional = QStringList{QStringLiteral("source"), QStringLiteral("offset"), QStringLiteral("limit")};
    } else if (name == QLatin1String("start") || name == QLatin1String("stop")) {
        spec.access = Access::Control;
        spec.description = QStringLiteral("%1 the existing receiver session (never RF TX)").arg(name);
    } else if (name == QLatin1String("tune") || name == QLatin1String("listen")) {
        spec.access = Access::Control;
        spec.description = name == QLatin1String("tune") ? QStringLiteral("Set receiver center in Hz")
                                                         : QStringLiteral("Set listening frequency in Hz");
        spec.inputSchema = schema({{"frequencyHz", number(0, 1e12)}}, {"frequencyHz"});
        spec.positional = QStringList{QStringLiteral("frequencyHz")};
    } else if (name == QLatin1String("demod")) {
        spec.access = Access::Control;
        spec.description = QStringLiteral("Select analog demodulation through the existing frontend");
        spec.inputSchema = schema({{"mode", choice({"am", "nfm", "wfm", "usb", "lsb", "cw"})}}, {"mode"});
        spec.positional = QStringList{QStringLiteral("mode")};
    } else if (name == QLatin1String("bandwidth")) {
        spec.access = Access::Control;
        spec.description = QStringLiteral("Set audio channel bandwidth in Hz");
        spec.inputSchema = schema({{"bandwidthHz", number(1, 1e9)}}, {"bandwidthHz"});
        spec.positional = QStringList{QStringLiteral("bandwidthHz")};
    } else if (name == QLatin1String("audio")) {
        spec.access = Access::Control;
        spec.description = QStringLiteral("Enable or disable the frontend audio output");
        spec.inputSchema = schema({{"enabled", QJsonObject{{"type", "boolean"}}}}, {"enabled"});
        spec.positional = QStringList{QStringLiteral("enabled")};
    } else if (name == QLatin1String("gain")) {
        spec.access = Access::Control;
        spec.description = QStringLiteral("Set backend LNA/VGA gain indices; hardware limits still apply");
        spec.inputSchema = schema({{"lna", number(0, 255, true)}, {"vga", number(0, 255, true)}}, {"lna", "vga"});
        spec.positional = QStringList{QStringLiteral("lna"), QStringLiteral("vga")};
    } else spec.name.clear();
    return spec;
}

bool CommandDispatcher::add(const Spec &spec, Handler handler) {
    if (QThread::currentThread() != thread_ || executing_ || !handler || commands_.contains(spec.name) ||
        !QRegularExpression(QStringLiteral("^[a-z][a-z0-9._-]{0,63}$")).match(spec.name).hasMatch()) return false;
    commands_.insert(spec.name, {spec, std::move(handler)});
    return true;
}

bool CommandDispatcher::addStandard(const QString &name, Handler handler) {
    const auto spec = standardSpec(name);
    return !spec.name.isEmpty() && add(spec, std::move(handler));
}

bool CommandDispatcher::allowed(Access access, const Context &context) const {
    return access == Access::Read || (access == Access::Control && context.allowControl) ||
        (access == Access::Files && context.allowFiles) || (access == Access::Transmit && context.allowTransmit);
}

QString CommandDispatcher::validate(const QJsonObject &arguments, const QJsonObject &definition) const {
    const auto properties = definition.value("properties").toObject();
    for (const auto &field : definition.value("required").toArray())
        if (!arguments.contains(field.toString())) return QStringLiteral("Missing argument: %1").arg(field.toString());
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (!properties.contains(it.key())) {
            if (!definition.value("additionalProperties").toBool(false))
                return QStringLiteral("Unknown argument: %1").arg(it.key());
            continue;
        }
        const auto rule = properties.value(it.key()).toObject();
        const auto type = rule.value("type").toString();
        const auto value = it.value();
        bool valid = true;
        if (type == QLatin1String("number") || type == QLatin1String("integer")) {
            const double n = value.toDouble();
            valid = value.isDouble() && std::isfinite(n) &&
                (type != QLatin1String("integer") || std::floor(n) == n) &&
                (!rule.contains("minimum") || n >= rule.value("minimum").toDouble()) &&
                (!rule.contains("maximum") || n <= rule.value("maximum").toDouble());
        } else if (type == QLatin1String("boolean")) valid = value.isBool();
        else if (type == QLatin1String("string")) {
            valid = value.isString() && value.toString().size() <= rule.value("maxLength").toInt(4096);
            if (valid && rule.contains("pattern"))
                valid = QRegularExpression(rule.value("pattern").toString()).match(value.toString()).hasMatch();
        } else if (type == QLatin1String("object")) valid = value.isObject();
        else if (type == QLatin1String("array")) valid = value.isArray();
        if (valid && rule.contains("enum")) valid = rule.value("enum").toArray().contains(value);
        if (!valid) return QStringLiteral("Invalid argument: %1").arg(it.key());
    }
    return {};
}

QJsonObject CommandDispatcher::catalog(const Context &context) const {
    QJsonArray entries;
    for (const auto &entry : commands_) {
        if (!allowed(entry.spec.access, context)) continue;
        entries.append(QJsonObject{{"name", entry.spec.name}, {"description", entry.spec.description},
            {"access", accessName(entry.spec.access)}, {"inputSchema", entry.spec.inputSchema},
            {"positional", QJsonArray::fromStringList(entry.spec.positional)}});
    }
    return {{"ok", true}, {"schema", "obrii.command.catalog"}, {"schemaVersion", 1},
        {"instanceId", instanceId_}, {"frontend", frontend_}, {"commands", entries},
        {"transport", "obrii-jsonl-v1"}, {"mcp", false}, {"scope", "current-instance"}};
}

QJsonObject CommandDispatcher::finish(QJsonObject reply, const QString &command, const QJsonValue &id,
                                     const Context &context, qint64 durationUs) {
    if (!reply.contains("ok")) reply.insert("ok", true);
    reply.insert("schemaVersion", 1);
    reply.insert("instanceId", instanceId_);
    reply.insert("command", command);
    reply.insert("id", id);
    reply.insert("durationUs", static_cast<double>(durationUs));
    ++sequence_;
    events_.append(QJsonObject{{"sequence", QString::number(sequence_)},
        {"utcMs", QDateTime::currentMSecsSinceEpoch()}, {"command", command.left(64)},
        {"origin", context.origin.left(48)}, {"ok", reply.value("ok")},
        {"durationUs", static_cast<double>(durationUs)}});
    if (events_.size() > 256) events_.removeFirst();
    return reply;
}

QJsonObject CommandDispatcher::execute(const QJsonObject &request, const Context &context) {
    if (QThread::currentThread() != thread_)
        return failure(QStringLiteral("wrong_thread"), QStringLiteral("Dispatch on the owning event thread"));
    const QString name = request.value("command").toString();
    const auto id = request.value("id").isUndefined()
        ? QJsonValue(QUuid::createUuid().toString(QUuid::WithoutBraces)) : request.value("id");
    QElapsedTimer timer; timer.start();
    const auto done = [&](QJsonObject reply) { return finish(reply, name, id, context, timer.nsecsElapsed() / 1000); };
    if (executing_) return failure(QStringLiteral("busy"), QStringLiteral("A command is already executing"));
    QScopedValueRollback<bool> guard(executing_, true);
    if (QJsonDocument(request).toJson(QJsonDocument::Compact).size() > 65536)
        return done(failure("request_too_large", "Command request exceeds 64 KiB"));
    for (auto it = request.begin(); it != request.end(); ++it)
        if (it.key() != QLatin1String("command") && it.key() != QLatin1String("args") &&
            it.key() != QLatin1String("id") && it.key() != QLatin1String("targetInstanceId"))
            return done(failure("invalid_request", "Unknown request field"));
    if (!id.isString() && !id.isDouble()) return done(failure("invalid_request", "id must be a string or number"));
    if (id.isString() && id.toString().size() > 128) return done(failure("invalid_request", "id is too long"));
    if (request.contains("targetInstanceId") &&
        request.value("targetInstanceId").toString() != instanceId_)
        return done(failure("wrong_instance", "Target instance does not match this session"));
    const auto found = commands_.constFind(name);
    if (found == commands_.constEnd()) return done(failure("unsupported_command", "Command is not supported by this frontend"));
    if (!allowed(found->spec.access, context)) return done(failure("permission_denied", "Command is not allowed for this caller"));
    if (request.contains("args") && !request.value("args").isObject())
        return done(failure("invalid_arguments", "args must be an object"));
    const auto arguments = request.value("args").toObject();
    const QString error = validate(arguments, found->spec.inputSchema);
    if (!error.isEmpty()) return done(failure("invalid_arguments", error));
    if (name == QLatin1String("commands")) {
        auto result = catalog(context);
        if (arguments.contains("name")) {
            QJsonArray selected;
            for (const auto &entry : result.value("commands").toArray())
                if (entry.toObject().value("name") == arguments.value("name")) selected.append(entry);
            if (selected.isEmpty()) return done(failure("unsupported_command", "Command is unavailable to this caller"));
            result.insert("commands", selected);
        }
        return done(result);
    }
    if (name == QLatin1String("audit")) {
        const quint64 after = arguments.value("after").toString().toULongLong();
        const int limit = arguments.value("limit").toInt(50);
        QJsonArray selected;
        for (const auto &event : events_)
            if (event.toObject().value("sequence").toString().toULongLong() > after && selected.size() < limit)
                selected.append(event);
        return done({{"ok", true}, {"events", selected}, {"latestSequence", QString::number(sequence_)},
            {"capacity", 256}, {"argumentsStored", false}});
    }
    try { return done(found->handler(arguments)); }
    catch (const std::exception &) { return done(failure("handler_failed", "Command handler failed")); }
    catch (...) { return done(failure("handler_failed", "Command handler failed")); }
}

QJsonObject CommandDispatcher::parseText(const QString &text) const {
    if (text.toUtf8().size() > 65536) return failure("request_too_large", "Command exceeds 64 KiB");
    const QString line = text.trimmed();
    if (line.startsWith(QLatin1Char('{'))) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return failure("invalid_json", "Expected a command JSON object");
        return document.object();
    }
    const auto words = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (words.isEmpty()) return failure("empty_command", "Enter a command");
    const QString name = words[0].toLower() == QLatin1String("help") ? QStringLiteral("commands") : words[0].toLower();
    const auto entry = commands_.constFind(name);
    if (entry == commands_.constEnd()) return failure("unsupported_command", "Use commands to list supported commands");
    if (words.size() - 1 > entry->spec.positional.size()) return failure("invalid_arguments", "Too many arguments");
    const auto properties = entry->spec.inputSchema.value("properties").toObject();
    QJsonObject arguments;
    for (int i = 1; i < words.size(); ++i) {
        const auto field = entry->spec.positional[i - 1];
        const auto type = properties.value(field).toObject().value("type").toString();
        const auto value = words[i];
        if (type == QLatin1String("number") || type == QLatin1String("integer")) {
            double n = 0;
            if (!frequencyNumber(value, n)) return failure("invalid_arguments", QStringLiteral("Invalid number: %1").arg(field));
            arguments.insert(field, n);
        } else if (type == QLatin1String("boolean")) {
            const auto lower = value.toLower();
            if (lower != QLatin1String("on") && lower != QLatin1String("off") &&
                lower != QLatin1String("true") && lower != QLatin1String("false"))
                return failure("invalid_arguments", "Use on/off or true/false");
            arguments.insert(field, lower == QLatin1String("on") || lower == QLatin1String("true"));
        } else {
            const auto rule = properties.value(field).toObject();
            const auto choices = rule.value("enum").toArray();
            arguments.insert(field, name == QLatin1String("commands") || choices.contains(value.toLower())
                ? value.toLower() : value);
        }
    }
    return {{"command", name}, {"args", arguments}};
}

QJsonObject CommandDispatcher::executeText(const QString &text, const Context &context) {
    if (QThread::currentThread() != thread_)
        return failure(QStringLiteral("wrong_thread"), QStringLiteral("Dispatch on the owning event thread"));
    const auto request = parseText(text);
    if (request.value("ok").isBool() && !request.value("ok").toBool()) return request;
    return execute(request, context);
}
