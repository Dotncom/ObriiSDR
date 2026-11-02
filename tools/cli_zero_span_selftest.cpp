#include "clizerospan.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>

#include <cmath>
#include <vector>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    CliZeroSpan zero;
    const QJsonObject started = zero.command(
        {{QStringLiteral("operation"), QStringLiteral("start")},
         {QStringLiteral("startHz"), 99.0}, {QStringLiteral("endHz"), 100.0},
         {QStringLiteral("maxPoints"), 2}}, 100.0, 4.0);
    if (!started.value(QStringLiteral("ok")).toBool() || !zero.running()) return 1;
    const std::vector<float> levels{-90.0f, -20.0f, -10.0f, -80.0f};
    if (!zero.observe(levels, 100.0, 4.0).isEmpty()) return 2;
    const QJsonObject done = zero.observe(levels, 100.0, 4.0);
    if (done.value(QStringLiteral("event")) != QStringLiteral("zeroSpanComplete") || zero.running()) return 3;
    const QJsonObject read = zero.command({{QStringLiteral("operation"), QStringLiteral("read")}}, 0, 0);
    const QJsonArray points = read.value(QStringLiteral("points")).toArray();
    if (points.size() != 2 ||
        std::abs(points[0].toObject().value(QStringLiteral("meanPowerDbfs")).toDouble() + 12.59637) > 0.01 ||
        points[1].toObject().value(QStringLiteral("elapsedMs")).toDouble() <
            points[0].toObject().value(QStringLiteral("elapsedMs")).toDouble()) return 4;
    QTemporaryDir directory;
    if (!directory.isValid()) return 5;
    const QString path = directory.filePath(QStringLiteral("zero.csv"));
    const QJsonObject saved = zero.command(
        {{QStringLiteral("operation"), QStringLiteral("export")}, {QStringLiteral("path"), path}}, 0, 0);
    QFile file(path);
    if (!saved.value(QStringLiteral("ok")).toBool() || !file.open(QIODevice::ReadOnly) ||
        file.readAll().count('\n') != 3) return 6;
    if (zero.command({{QStringLiteral("operation"), QStringLiteral("export")},
                      {QStringLiteral("path"), path}}, 0, 0).value(QStringLiteral("ok")).toBool()) return 7;
    return 0;
}
