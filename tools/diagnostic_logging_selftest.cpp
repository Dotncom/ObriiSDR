#include "appdiagnostics.h"
#include "diagnosticlogging.h"

#include <QCoreApplication>
#include <QDebug>
#include <QProcess>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--child"))) {
        setFobosVerboseLoggingEnabled(args.contains(QStringLiteral("--verbose")));
        installDiagnosticLogger();
        qDebug() << "debug-test-marker";
        qInfo() << "info-test-marker";
        qWarning() << "warning-test-marker";
        qCritical() << "critical-test-marker";
        return 0;
    }
    for (bool verbose : {false, true}) {
        QProcess process;
        QStringList childArgs{QStringLiteral("--child")};
        if (verbose) {
            childArgs.append(QStringLiteral("--verbose"));
        }
        process.start(QCoreApplication::applicationFilePath(), childArgs);
        if (!process.waitForStarted(5000) || !process.waitForFinished(10000) ||
            process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            std::fprintf(stderr, "Diagnostic logging child failed\n");
            return 1;
        }
        const QByteArray output = process.readAllStandardError();
        if (output.contains("debug-test-marker") != verbose ||
            output.contains("info-test-marker") != verbose ||
            !output.contains("warning-test-marker") ||
            !output.contains("critical-test-marker") ||
            (!verbose && output.contains("[Log]"))) {
            std::fprintf(stderr, "Diagnostic console filtering failed\n");
            return 1;
        }
    }
    std::printf("Diagnostic logging self-test: PASS (quiet/verbose, warnings/errors)\n");
    return 0;
}
