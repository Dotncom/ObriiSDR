#include "clilaunchscript.h"

#include <QHostAddress>
#include <QStringList>

#include <cmath>

bool validateCliLaunchSettings(const CliLaunchSettings &settings, QString *error) {
    const auto reject = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (settings.api != QStringLiteral("standard") && settings.api != QStringLiteral("agile"))
        return reject(QStringLiteral("Only Fobos Standard or Agile is supported by ObriiCli."));
    if (settings.inputMode != 0)
        return reject(QStringLiteral("The CLI network server currently supports only the Fobos RF input."));
    if (settings.deviceIndex < 0 || settings.lna < 0 || settings.lna > 2 ||
        settings.vga < 0 || settings.vga > 15)
        return reject(QStringLiteral("The selected Fobos device or gain is outside the CLI range."));
    if (!QStringList{QStringLiteral("am"), QStringLiteral("nfm"), QStringLiteral("wfm"),
                     QStringLiteral("usb"), QStringLiteral("lsb"), QStringLiteral("cw")}
             .contains(settings.modulation))
        return reject(QStringLiteral("This demodulation mode is not yet available in ObriiCli."));
    if (!std::isfinite(settings.centerHz) || settings.centerHz < 50e6 ||
        !std::isfinite(settings.sampleRateHz) || settings.sampleRateHz <= 0 ||
        !std::isfinite(settings.listeningHz) || settings.listeningHz <= 0 ||
        !std::isfinite(settings.bandwidthHz) || settings.bandwidthHz <= 0 ||
        std::abs(settings.listeningHz - settings.centerHz) + settings.bandwidthHz / 2 >=
            settings.sampleRateHz / 2)
        return reject(QStringLiteral("Frequency, sample rate or audio channel is invalid for CLI streaming."));
    if (settings.fftLength < 512 || settings.fftLength > 50000000 || settings.networkPort < 1 || settings.networkPort > 65535)
        return reject(QStringLiteral("FFT length or network port is invalid."));
    QHostAddress address;
    if (!address.setAddress(settings.bindAddress))
        return reject(QStringLiteral("Enter a numeric bind address, for example 127.0.0.1 or 0.0.0.0."));
    return true;
}

QString buildCliLaunchScript(const CliLaunchSettings &settings, CliScriptPlatform platform) {
    QString error;
    if (!validateCliLaunchSettings(settings, &error)) return {};
    const int fft = settings.fftLength;
    QStringList arguments = {
        QStringLiteral("run"), QStringLiteral("--serve"),
        QStringLiteral("--api"), settings.api,
        QStringLiteral("--device"), QString::number(settings.deviceIndex),
        QStringLiteral("--input"), QStringLiteral("rf"),
        QStringLiteral("--freq"), QString::number(settings.centerHz, 'f', 0),
        QStringLiteral("--listen"), QString::number(settings.listeningHz, 'f', 0),
        QStringLiteral("--rate"), QString::number(settings.sampleRateHz, 'f', 0),
        QStringLiteral("--lna"), QString::number(settings.lna),
        QStringLiteral("--vga"), QString::number(settings.vga),
        QStringLiteral("--mode"), settings.modulation,
        QStringLiteral("--bandwidth"), QString::number(settings.bandwidthHz, 'f', 0),
        QStringLiteral("--fft"), QString::number(fft),
        QStringLiteral("--bind"), settings.bindAddress,
        QStringLiteral("--network-port"), QString::number(settings.networkPort)};
    if (!settings.audioEnabled) arguments.append(QStringLiteral("--no-audio"));
    QStringList quoted;
    quoted.reserve(arguments.size());
    for (QString argument : arguments) {
        argument.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        quoted.push_back(QLatin1Char('\'') + argument + QLatin1Char('\''));
    }
    const QString note = settings.fftLength != fft
        ? QStringLiteral("# GUI FFT %1 is not supported by the CLI server; using %2.\n")
              .arg(settings.fftLength).arg(fft)
        : QString();
    if (platform == CliScriptPlatform::Bash) {
        return QStringLiteral("#!/usr/bin/env bash\nset -euo pipefail\n"
                              "# Generated from Obrii SDR receiver settings. Fobos RF and basic audio only.\n"
                              "# Close Obrii SDR on the receiver host before running this script.\n"
                              "# Network control has no authentication; use only on a trusted LAN or VPN.\n") +
               note +
               QStringLiteral("script_dir=$(cd -- \"$(dirname -- \"${BASH_SOURCE[0]}\")\" && pwd)\n"
                              "cli=\"$script_dir/ObriiCli\"\n"
                              "if [[ ! -x \"$cli\" ]]; then cli=\"$script_dir/build/linux-cli/ObriiCli\"; fi\n"
                              "if [[ ! -x \"$cli\" ]]; then echo 'ObriiCli not found beside this script' >&2; exit 1; fi\n"
                              "exec \"$cli\" ") + quoted.join(QLatin1Char(' ')) + QLatin1Char('\n');
    }
    return QStringLiteral("# Generated from Obrii SDR receiver settings. Fobos RF and basic audio only.\n"
                          "# Close Obrii SDR on the receiver host before running this script.\n"
                          "# Network control has no authentication; use only on a trusted LAN or VPN.\n") +
           note +
           QStringLiteral("$cli = Join-Path $PSScriptRoot 'ObriiCli.exe'\n"
                          "if (-not (Test-Path -LiteralPath $cli -PathType Leaf)) {\n"
                          "    $cli = Join-Path $PSScriptRoot 'build\\runtime\\ObriiCli.exe'\n"
                          "}\n"
                          "if (-not (Test-Path -LiteralPath $cli -PathType Leaf)) { throw 'ObriiCli.exe not found beside this script' }\n"
                          "& $cli ") + quoted.join(QLatin1Char(' ')) + QStringLiteral("\nexit $LASTEXITCODE\n");
}
