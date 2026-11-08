#pragma once
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

inline QJsonObject cliCapabilities() {
    QJsonObject workers;
    for (const QString &name : {QStringLiteral("ObriiCliModule"), QStringLiteral("ObriiCliTxModule"),
         QStringLiteral("ObriiCliPacketModule"), QStringLiteral("ObriiCliImageModule"),
         QStringLiteral("ObriiCliM17Module"), QStringLiteral("ObriiCliSpectrumModule"),
         QStringLiteral("ObriiCliGnssModule"), QStringLiteral("ObriiCliView"), QStringLiteral("ObriiIqHub")}) {
        const QString path = QCoreApplication::applicationDirPath() + QLatin1Char('/') + name +
#ifdef _WIN32
            QStringLiteral(".exe");
#else
            QString();
#endif
        workers.insert(name, QFileInfo(path).isFile());
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("headless"), true}, {QStringLiteral("optionalWorkers"), workers},
        {QStringLiteral("backends"), QJsonArray{"fobos", "hackrf", "soapy", "uhd", "rtl", "rtl-tcp", "file", "virtual"}},
        {QStringLiteral("networkModes"), QJsonArray{"server", "channel-iq", "full-iq"}},
        {QStringLiteral("receiverLimit"), 8}, {QStringLiteral("markerLimit"), 10},
        {QStringLiteral("minimumFftBins"), 512}, {QStringLiteral("maximumFftBins"), 50000000},
        {QStringLiteral("browserVfos"), true}, {QStringLiteral("externalDspBranches"), true},
        {QStringLiteral("commands"), QJsonArray{"commands", "audit", "request", "agent", "status", "metrics", "tune", "listen", "gain", "demod", "bandwidth",
            "audio", "digital", "filters", "vfo", "multisdr", "graph", "scan", "trigger", "quality", "zero",
            "spectrum", "trace", "traces", "science", "tone", "measure", "preset", "experiment", "module", "web", "calibration", "gnss", "research", "record", "doctor"}},
        {QStringLiteral("asynchronousSpectrum"), true}, {QStringLiteral("runtimeRecording"), true},
        {QStringLiteral("commandDispatcher"), 1}, {QStringLiteral("optionalAgentApi"), true},
        {QStringLiteral("sharedCores"), QJsonObject{{QStringLiteral("fobosReceiver"), 1},
            {QStringLiteral("cpuFft"), 1}, {QStringLiteral("recordingFormats"), 1}}},
        {QStringLiteral("liveMicrophonePtt"), true}, {QStringLiteral("liveGnss"), true},
        {QStringLiteral("limitations"), QJsonArray{
            "Optional workers and external modem engines must be installed separately when absent",
            "Hardware support is not proof of RF interoperability or calibration",
            "FT4 and M17 RX are opt-in workers; RF interoperability still needs validation",
            "FT8 QSO automation and some specialized GUI tools are not CLI commands",
            "FPV/DMR specialized hunters and coherent multi-receiver IQ alignment are not CLI commands",
            "Interactive GUI layout and FPGA/Analyzer editor applications remain separate programs"}}};
}
