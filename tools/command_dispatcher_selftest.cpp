#include "commanddispatcher.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace {
int checks = 0;
void check(bool value) { ++checks; if (!value) { std::fprintf(stderr, "check %d failed\n", checks); std::exit(1); } }
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    CommandDispatcher dispatcher(QStringLiteral("test"));
    int calls = 0;
    dispatcher.addStandard("status", [&](const QJsonObject &) { ++calls; return QJsonObject{{"ok", true}, {"running", false}}; });
    dispatcher.addStandard("tune", [&](const QJsonObject &a) { ++calls; return QJsonObject{{"frequencyHz", a.value("frequencyHz")}}; });
    dispatcher.addStandard("gain", [&](const QJsonObject &a) { ++calls; return a; });
    dispatcher.addStandard("audio", [&](const QJsonObject &a) { ++calls; return a; });
    dispatcher.addStandard("trace", [&](const QJsonObject &a) { ++calls; return a; });
    CommandDispatcher::Spec literal{"echo", "Preserve literal string arguments", CommandDispatcher::Access::Read,
        QJsonObject{{"properties", QJsonObject{{"value", QJsonObject{{"type", "string"}}}}}}, {"value"}};
    dispatcher.add(literal, [](const QJsonObject &a) { return a; });
    auto restricted = CommandDispatcher::Spec{"files", "write files", CommandDispatcher::Access::Files,
        QJsonObject{{"properties", QJsonObject{}}}, {}};
    dispatcher.add(restricted, [&](const QJsonObject &) { ++calls; return QJsonObject{}; });
    restricted.name = QStringLiteral("tx"); restricted.access = CommandDispatcher::Access::Transmit;
    dispatcher.add(restricted, [&](const QJsonObject &) { ++calls; return QJsonObject{}; });
    restricted.name = QStringLiteral("error"); restricted.access = CommandDispatcher::Access::Read;
    dispatcher.add(restricted, [](const QJsonObject &) -> QJsonObject { throw std::runtime_error("private-token"); });
    restricted.name = QStringLiteral("nested");
    dispatcher.add(restricted, [&](const QJsonObject &) { return dispatcher.execute({{"command", "status"}}, {}); });
    check(calls == 0);
    const auto human = CommandDispatcher::Context::human();
    const auto catalog = dispatcher.catalog({});
    check(catalog.value("instanceId").toString() == dispatcher.instanceId());
    for (const auto &entry : catalog.value("commands").toArray())
        check(entry.toObject().value("access").toString() == QLatin1String("read"));
    check(!dispatcher.executeText("tune 100M", {}).value("ok").toBool() && calls == 0);
    auto reply = dispatcher.executeText("tune 100.25M", human);
    check(reply.value("ok").toBool() && reply.value("frequencyHz").toDouble() == 100250000 && calls == 1);
    check(reply.value("instanceId").toString() == dispatcher.instanceId() && reply.value("id").isString());
    check(!dispatcher.executeText("tune nan", human).value("ok").toBool());
    check(!dispatcher.executeText("tune -1", human).value("ok").toBool());
    check(!dispatcher.executeText("gain 1.5 2", human).value("ok").toBool());
    check(!dispatcher.executeText("status; quit", human).value("ok").toBool());
    check(!dispatcher.executeText("status | cmd", human).value("ok").toBool());
    check(!dispatcher.executeText("trace live 0 1025", human).value("ok").toBool());
    check(!dispatcher.executeText("trace bogus 0 10", human).value("ok").toBool());
    check(dispatcher.executeText("TRACE LIVE 0 10", human).value("source") == QJsonValue("live"));
    check(dispatcher.executeText("echo KeepMyCase", human).value("value") == QJsonValue("KeepMyCase"));
    check(dispatcher.executeText("audio off", human).value("enabled") == QJsonValue(false));
    check(!dispatcher.executeText("audio 2", human).value("ok").toBool());
    check(!dispatcher.execute({{"command", "tune"}, {"args", QJsonObject{{"frequencyHz", "100M"}}}}, human).value("ok").toBool());
    check(!dispatcher.execute({{"command", "tune"}, {"args", QJsonObject{{"frequencyHz", 100e6}, {"shell", "test"}}}}, human).value("ok").toBool());
    check(!dispatcher.execute({{"command", "status"}, {"allowControl", true}}, {}).value("ok").toBool());
    check(!dispatcher.execute({{"command", "status"}, {"args", false}}, {}).value("ok").toBool());
    check(!dispatcher.execute({{"command", "status"}, {"targetInstanceId", "other"}}, {}).value("ok").toBool());
    check(dispatcher.execute({{"command", "status"}, {"targetInstanceId", dispatcher.instanceId()}, {"id", "request-4"}}, {}).value("id") == QJsonValue("request-4"));
    check(!dispatcher.executeText("files", {}).value("ok").toBool());
    check(dispatcher.executeText("files", human).value("ok").toBool());
    check(!dispatcher.executeText("tx", human).value("ok").toBool());
    check(!dispatcher.executeText("nested", human).value("ok").toBool());
    reply = dispatcher.executeText("error", human);
    check(!reply.value("ok").toBool() && !QJsonDocument(reply).toJson().contains("private-token"));
    check(dispatcher.executeText("help tune", human).value("commands").toArray().size() == 1);
    check(dispatcher.executeText("help tune", {}).value("ok") == QJsonValue(false));
    for (int i = 0; i < 300; ++i) check(dispatcher.executeText("status", {}).value("ok").toBool());
    reply = dispatcher.executeText("audit 0 100", {});
    check(reply.value("events").toArray().size() == 100 && reply.value("capacity").toInt() == 256);
    for (const auto &event : reply.value("events").toArray()) check(!event.toObject().contains("args"));
    QJsonObject threaded;
    std::thread worker([&] { threaded = dispatcher.executeText("status", {}); }); worker.join();
    check(threaded.value("errorCode").toString() == QLatin1String("wrong_thread"));
    check(CommandDispatcher("other").instanceId() != dispatcher.instanceId());
    std::printf("Command dispatcher passed: %d checks (schemas, permissions, parser, routing, audit, reentry, thread affinity)\n", checks);
}
