#include "clifrequencyscanner.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QString path = directory.filePath(QStringLiteral("scan.json"));
    QFile preset(path);
    if (!preset.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 2;
    preset.write(R"({"centerHz":[100000000,101000000],"dwellMs":300,"holdMs":0,"thresholdDbfs":-60,"minSnrDb":10})");
    preset.close();

    CliFrequencyScanner scanner;
    if (!scanner.command({{QStringLiteral("operation"), QStringLiteral("load")},
                          {QStringLiteral("path"), path}}).value(QStringLiteral("ok")).toBool()) return 3;
    if (!scanner.command({{QStringLiteral("operation"), QStringLiteral("start")}})
             .value(QStringLiteral("ok")).toBool()) return 4;
    double center = 0.0;
    if (!scanner.takeTune(&center) || center != 100000000.0) return 5;
    scanner.tuned();
    QThread::msleep(120);
    const QJsonObject spectrum{{QStringLiteral("noiseFloorDbfs"), -90.0},
                               {QStringLiteral("peaks"), QJsonArray{QJsonObject{
                                   {QStringLiteral("frequencyHz"), 100010000.0},
                                   {QStringLiteral("levelDbfs"), -40.0}}}}};
    if (!scanner.observe(spectrum, center).isEmpty()) return 6;
    const QJsonObject hit = scanner.observe(spectrum, center);
    if (hit.value(QStringLiteral("event")) != QStringLiteral("scanHit") ||
        hit.value(QStringLiteral("signalFrequencyHz")).toDouble() != 100010000.0 ||
        scanner.status().value(QStringLiteral("state")) != QStringLiteral("locked")) return 7;
    if (scanner.takeTune(&center)) return 8;
    if (!scanner.command({{QStringLiteral("operation"), QStringLiteral("resume")}})
             .value(QStringLiteral("ok")).toBool()) return 9;
    if (!scanner.takeTune(&center) || center != 101000000.0) return 10;
    scanner.stop();
    if (scanner.running()) return 11;
    return 0;
}
