#ifndef OBRII_CLI_LAUNCH_SCRIPT_H
#define OBRII_CLI_LAUNCH_SCRIPT_H

#include <QString>

struct CliLaunchSettings {
    QString api;
    QString modulation;
    QString bindAddress;
    int deviceIndex = 0;
    int inputMode = 0;
    int lna = 0;
    int vga = 0;
    int fftLength = 4096;
    int networkPort = 21090;
    bool audioEnabled = true;
    double centerHz = 100e6;
    double listeningHz = 100e6;
    double sampleRateHz = 8e6;
    double bandwidthHz = 12500;
};

enum class CliScriptPlatform { Bash, PowerShell };

bool validateCliLaunchSettings(const CliLaunchSettings &settings, QString *error);
QString buildCliLaunchScript(const CliLaunchSettings &settings, CliScriptPlatform platform);

#endif
