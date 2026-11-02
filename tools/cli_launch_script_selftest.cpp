#include "clilaunchscript.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    CliLaunchSettings settings;
    settings.api = QStringLiteral("standard");
    settings.modulation = QStringLiteral("wfm");
    settings.bindAddress = QStringLiteral("0.0.0.0");
    settings.deviceIndex = 1;
    settings.centerHz = 101700000;
    settings.listeningHz = 101700000;
    settings.sampleRateHz = 8000000;
    settings.bandwidthHz = 200000;
    settings.fftLength = 131072;
    settings.vga = 3;
    settings.audioEnabled = false;
    QString error;
    const QString bash = buildCliLaunchScript(settings, CliScriptPlatform::Bash);
    const QString powerShell = buildCliLaunchScript(settings, CliScriptPlatform::PowerShell);
    if (!validateCliLaunchSettings(settings, &error) ||
        !bash.startsWith(QStringLiteral("#!/usr/bin/env bash")) ||
        !bash.contains(QStringLiteral("'--freq' '101700000'")) ||
        !bash.contains(QStringLiteral("'--fft' '131072'")) ||
        !bash.contains(QStringLiteral("'--bind' '0.0.0.0'")) ||
        !bash.contains(QStringLiteral("'--no-audio'")) ||
        !powerShell.contains(QStringLiteral("& $cli 'run' '--serve'")) ||
        !powerShell.contains(QStringLiteral("'--device' '1'"))) {
        std::fprintf(stderr, "CLI launch script validation failed\n");
        return 1;
    }
    settings.bindAddress = QStringLiteral("0.0.0.0; rm -rf /");
    if (validateCliLaunchSettings(settings, &error) ||
        !buildCliLaunchScript(settings, CliScriptPlatform::Bash).isEmpty()) {
        std::fprintf(stderr, "CLI launch script accepted an unsafe bind address\n");
        return 1;
    }
    std::puts("CLI launch script self-test passed");
    return 0;
}
