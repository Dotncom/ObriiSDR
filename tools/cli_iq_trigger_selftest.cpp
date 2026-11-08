#include "cliiqtrigger.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <vector>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QString presetPath = directory.filePath(QStringLiteral("trigger.json"));
    QFile preset(presetPath);
    if (!preset.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 2;
    preset.write(R"({"outputPrefix":"capture","bandStartHz":99990000,"bandEndHz":100010000,"thresholdDbfs":-60,"edge":"rising","preMs":100,"postMs":100})");
    preset.close();

    CliIqTrigger trigger;
    if (!trigger.command({{QStringLiteral("operation"), QStringLiteral("load")},
                          {QStringLiteral("path"), presetPath}}, 100e6, 48000)
             .value(QStringLiteral("ok")).toBool()) return 3;
    if (!trigger.command({{QStringLiteral("operation"), QStringLiteral("arm")}}, 100e6, 48000)
             .value(QStringLiteral("ok")).toBool()) return 4;
    std::vector<float> iq(1024 * 2, 0.1f);
    std::vector<float> low(1024, -100.0f), high(1024, -100.0f);
    high[512] = -30.0f;
    for (int i = 0; i < 5; ++i) trigger.submitIq(iq.data(), 1024);
    if (!trigger.observe(low, 100e6, 48000).isEmpty()) return 5;
    trigger.submitIq(iq.data(), 1024);
    if (trigger.observe(high, 100e6, 48000).value(QStringLiteral("event")) !=
        QStringLiteral("triggerFired")) return 6;
    for (int i = 0; i < 5; ++i) trigger.submitIq(iq.data(), 1024);
    QJsonObject saved;
    for (int i = 0; i < 200 && saved.isEmpty(); ++i) {
        saved = trigger.submitIq(nullptr, 0);
        if (saved.isEmpty()) QThread::msleep(10);
    }
    if (saved.value(QStringLiteral("event")) != QStringLiteral("triggerSaved")) return 7;
    const QString path = directory.filePath(QStringLiteral("capture_0001.cf32"));
    if (saved.value(QStringLiteral("path")) != path ||
        QFileInfo(path).size() != 9600 * 8 || !QFileInfo::exists(path + QStringLiteral(".json")))
        return 8;
    return 0;
}
