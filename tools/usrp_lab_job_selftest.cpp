#include "usrplabjob.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool passed, const char *message) {
    if (passed) return;
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}

int fixture(const QString &path) {
    const QDir dir(path);
    QFile file(dir.filePath("request.json"));
    if (!file.open(QIODevice::ReadOnly)) return 1;
    const auto request = QJsonDocument::fromJson(file.readAll()).object();
    const QString action = request.value("action").toString();
    if (action == QStringLiteral("umts-decode") &&
        (request.value("rate").toInt() != 7680000 || request.value("offset").toInt() != -1000 ||
         request.value("format").toString() != QStringLiteral("cs16") || request.value("start").toInt() != 10))
        return 1;
    QString status = QStringLiteral("PASS");
    if (action == QStringLiteral("gsm-demo")) {
        QElapsedTimer deadline;
        deadline.start();
        while (!dir.exists("cancel") && deadline.elapsed() < 5000) QThread::msleep(20);
        status = dir.exists("cancel") ? QStringLiteral("CANCELLED") : QStringLiteral("FAIL");
    }
    QFile result(dir.filePath("result.json"));
    if (!result.open(QIODevice::WriteOnly)) return 1;
    result.write(QJsonDocument(QJsonObject{{"schema", 1}, {"action", action}, {"status", status}}).toJson());
    result.close();
    return action == QStringLiteral("wifi-loopback") || status != QStringLiteral("PASS") ? 1 : 0;
}

QJsonObject execute(UsrpLabJob &job, const QJsonObject &request, bool cancel = false) {
    QEventLoop loop;
    QJsonObject result;
    auto connection = QObject::connect(&job, &UsrpLabJob::completed, &loop,
        [&](const QJsonObject &reply) { result = reply; loop.quit(); });
    const auto started = job.command(request);
    check(started.value("active").toBool(), "job must start asynchronously");
    check(!job.command({{"action", "contracts"}}).value("ok").toBool(), "job must reject a second start");
    if (cancel)
        check(job.command({{"action", "cancel"}}).value("cancelling").toBool(), "cancel marker must be written");
    QTimer::singleShot(8000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection);
    check(!result.isEmpty() && !job.status().value("active").toBool(), "job must finish and publish its result");
    return result;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 5 && app.arguments().value(1) == QStringLiteral("-I"))
        return fixture(app.arguments().last());
    QTemporaryDir root(QDir::temp().filePath("obrii usrp job XXXXXX"));
    check(root.isValid(), "temporary lab directory");
    QFile marker(root.filePath("app_job.py"));
    check(marker.open(QIODevice::WriteOnly), "local adapter marker");
    marker.close();
    qputenv("OBRII_USRP_LAB_ROOT", root.path().toUtf8());
    qputenv("OBRII_USRP_LAB_PYTHON", app.applicationFilePath().toUtf8());
    UsrpLabJob job;
    check(job.status().value("available").toBool(), "local lab discovered");
    check(!job.command({{"action", "gsm-decode"}, {"arfcn", 62}}).value("ok").toBool(), "missing input rejected");
    check(UsrpLabJob::arguments({"gsm-decode", "a b.cf32", "62", "3"}).value("input").toString() ==
        QStringLiteral("a b.cf32"), "argv preserves file paths containing spaces");
    QFile input(root.filePath("wide capture.cs16"));
    check(input.open(QIODevice::WriteOnly) && input.resize(7680000 / 50 * 4 + 40), "regular bounded UMTS test input");
    input.close();
    const auto umts = UsrpLabJob::arguments({"umts-decode", input.fileName(), "7680000", "-1000", "cs16", "10"});
    check(umts.value("input").toString() == input.fileName(), "UMTS argv preserves the IQ path");
    check(!job.command({{"action", "umts-decode"}, {"input", input.fileName()}, {"rate", 7680000},
        {"offset", 1350000}}).value("ok").toBool(), "UMTS rejects an offset outside the guard band");
    check(!job.command({{"action", "umts-decode"}, {"input", input.fileName()}, {"rate", 7680000},
        {"start", 10.5}}).value("ok").toBool(), "UMTS rejects a fractional start sample");
    check(execute(job, umts).value("ok").toBool(), "UMTS request reaches the worker");
    check(execute(job, {{"action", "contracts"}}).value("ok").toBool(), "verified worker exit succeeds");
    const auto failure = execute(job, {{"action", "wifi-loopback"}});
    check(!failure.value("ok").toBool() && failure.value("status") == QJsonValue("FAIL"),
        "nonzero worker exit cannot claim PASS");
    const auto cancelled = execute(job, {{"action", "gsm-demo"}}, true);
    check(cancelled.value("status") == QJsonValue("CANCELLED"), "cancel request reaches the worker");
    std::puts("USRP app job checks passed");
}
