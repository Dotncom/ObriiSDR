#ifndef OBRII_CLI_COMMAND_ADAPTER_H
#define OBRII_CLI_COMMAND_ADAPTER_H

#include "../core/commanddispatcher.h"
#include "../core/radiosettings.h"
#include "../network/commandcontrolserver.h"
#include "../mesh/meshnodecommands.h"

inline QJsonObject cliAgentCommand(CommandControlServer &server, const QJsonObject &input) {
    const auto operation = input.value("operation").toString();
    QJsonObject reply;
    if (operation == QLatin1String("start")) {
        if (server.isListening()) return CommandDispatcher::failure("already_started", "Stop the agent API before restarting it");
        const auto portValue = input.value("port");
        const double port = portValue.isUndefined() ? 0 : portValue.toDouble(-1);
        if (port < 0 || port > 65535 || std::floor(port) != port)
            return CommandDispatcher::failure("invalid_arguments", "Agent port must be 0..65535");
        QString error;
        if (!server.start(static_cast<quint16>(port), &error)) return CommandDispatcher::failure("listen_failed", error);
        reply = server.status();
        reply.insert("token", server.token()); // Only the explicit trusted local start reply.
    } else if (operation == QLatin1String("stop")) { server.stop(); reply = server.status(); }
    else if (operation == QLatin1String("control") && input.value("enabled").isBool()) {
        server.setAllowControl(input.value("enabled").toBool()); reply = server.status();
    } else if (operation == QLatin1String("status")) reply = server.status();
    else return CommandDispatcher::failure("invalid_arguments", "Unknown agent operation");
    reply.insert("ok", true);
    return reply;
}

inline void registerCliCommandHandlers(CommandDispatcher &dispatcher,
                                        const CommandDispatcher::Handler &legacyHandler,
                                        bool includeTrace) {
    for (const QString name : {QStringLiteral("status"), QStringLiteral("metrics"), QStringLiteral("capabilities"),
            QStringLiteral("spectrum"), QStringLiteral("start"), QStringLiteral("stop"), QStringLiteral("tune"),
            QStringLiteral("listen"), QStringLiteral("gain"), QStringLiteral("demod"),
            QStringLiteral("bandwidth"), QStringLiteral("audio"), QStringLiteral("trace")}) {
        if (name == QLatin1String("trace") && !includeTrace) continue;
        dispatcher.addStandard(name, [legacyHandler, name](const QJsonObject &arguments) {
            QJsonObject input = arguments; input.insert("action", name);
            if (name == QLatin1String("trace")) {
                input.insert("count", arguments.value("limit").toInt(512)); input.remove("limit");
            } else if (name == QLatin1String("demod")) {
                const QMap<QString, int> modes{{"am", MOD_AM}, {"nfm", MOD_NFM}, {"wfm", MOD_WFM},
                    {"usb", MOD_USB}, {"lsb", MOD_LSB}, {"cw", MOD_CW}};
                input.insert("modulationType", modes.value(arguments.value("mode").toString(), -1)); input.remove("mode");
            }
            return legacyHandler(input);
        });
    }
    if (includeTrace) {
        CommandDispatcher::Spec preview;
        preview.name = QStringLiteral("cli.preview");
        preview.description = QStringLiteral("Lease the existing exact FFT for an optional display; ttlMs=0 cancels");
        preview.access = CommandDispatcher::Access::Control;
        preview.inputSchema = {{"type", "object"}, {"properties", QJsonObject{
            {"ttlMs", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 3000}}},
            {"intervalMs", QJsonObject{{"type", "integer"}, {"minimum", 100}, {"maximum", 2000}}}}},
            {"required", QJsonArray{"ttlMs"}}, {"additionalProperties", false}};
        preview.positional = QStringList{QStringLiteral("ttlMs"), QStringLiteral("intervalMs")};
        dispatcher.add(preview, [legacyHandler](const QJsonObject &args) {
            auto input = args; input.insert("action", "preview"); return legacyHandler(input);
        });
    }
    CommandDispatcher::Spec legacy;
    legacy.name = QStringLiteral("cli.legacy");
    legacy.description = QStringLiteral("Trusted local CLI compatibility adapter; unavailable to agent callers");
    legacy.access = CommandDispatcher::Access::Files;
    legacy.inputSchema = {{"type", "object"}, {"properties", QJsonObject{{"request", QJsonObject{{"type", "object"}}}}},
        {"required", QJsonArray{"request"}}, {"additionalProperties", false}};
    dispatcher.add(legacy, [legacyHandler](const QJsonObject &a) {
        const auto result = legacyHandler(a.value("request").toObject());
        QJsonObject reply{{"ok", result.value("ok").toBool(true)}, {"legacyResult", result}};
        if (!reply.value("ok").toBool()) {
            reply.insert("errorCode", result.value("errorCode").toString(QStringLiteral("legacy_failed")));
            reply.insert("error", result.value("error").toString(QStringLiteral("Legacy command failed")));
        }
        return reply;
    });
}

inline QJsonObject cliCommandWireReply(const QJsonObject &reply, bool legacyWire) {
    // Original action-based clients own fields such as id/schemaVersion. Only
    // typed requests receive dispatcher metadata around their legacy result.
    const auto result = reply.value("legacyResult");
    return legacyWire && result.isObject() ? result.toObject() : reply;
}

#endif
