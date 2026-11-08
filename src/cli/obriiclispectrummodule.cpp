#include "climoduleprotocol.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
bool readExact(char *target, std::size_t bytes) {
    std::size_t offset = 0;
    while (offset < bytes) {
        const std::size_t count = std::fread(target + offset, 1, bytes - offset, stdin);
        if (!count) return false;
        offset += count;
    }
    return true;
}

void send(const QJsonObject &object) {
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
    std::fflush(stdout);
}

bool number(const QJsonObject &object, const char *name, double fallback, double *out) {
    const auto value = object.value(QLatin1String(name));
    if (value.isUndefined()) { *out = fallback; return true; }
    if (!value.isDouble() || !std::isfinite(value.toDouble())) return false;
    *out = value.toDouble();
    return true;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3 || QByteArray(argv[1]) != "rx") {
        std::fprintf(stderr, "Usage: ObriiCliSpectrumModule rx PRESET.json\n");
        return 2;
    }
    QFile preset(QString::fromLocal8Bit(argv[2]));
    if (!preset.open(QIODevice::ReadOnly) || preset.size() > 65536) {
        std::fprintf(stderr, "Cannot read monitor preset\n");
        return 2;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(preset.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return 2;
    const QJsonObject config = document.object();
    double minHz = 0, maxHz = 1e12, thresholdDbfs = 0, hysteresisDb = 3, holdoffMs = 1000;
    if (!number(config, "minHz", 0, &minHz) || !number(config, "maxHz", 1e12, &maxHz) ||
        !number(config, "thresholdDbfs", -80, &thresholdDbfs) ||
        !number(config, "hysteresisDb", 3, &hysteresisDb) ||
        !number(config, "holdoffMs", 1000, &holdoffMs) ||
        minHz < 0 || maxHz <= minHz || maxHz > 1e12 ||
        thresholdDbfs < -200 || thresholdDbfs > 20 ||
        hysteresisDb < 0 || hysteresisDb > 100 || holdoffMs < 0 || holdoffMs > 600000 ||
        config.size() > 5) {
        std::fprintf(stderr, "Invalid monitor preset\n");
        return 2;
    }
    for (auto it = config.begin(); it != config.end(); ++it)
        if (it.key() != QLatin1String("minHz") && it.key() != QLatin1String("maxHz") &&
            it.key() != QLatin1String("thresholdDbfs") && it.key() != QLatin1String("hysteresisDb") &&
            it.key() != QLatin1String("holdoffMs")) return 2;
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) < 0 ||
        _setmode(_fileno(stdout), _O_BINARY) < 0) return 2;
#endif
    QElapsedTimer timer;
    timer.start();
    qint64 lastEventMs = -600001;
    bool above = false;
    send({{QStringLiteral("event"), QStringLiteral("status")},
          {QStringLiteral("module"), QStringLiteral("monitor")},
          {QStringLiteral("text"), QStringLiteral("Spectrum threshold monitor ready")}});
    for (;;) {
        uchar header[12];
        if (!readExact(reinterpret_cast<char *>(header), sizeof(header))) break;
        const quint32 kind = qFromLittleEndian<quint32>(header);
        const quint32 size = qFromLittleEndian<quint32>(header + 8);
        if (size > CliModuleProtocol::MaximumPayloadBytes) return 2;
        QByteArray payload(static_cast<int>(size), Qt::Uninitialized);
        if (size && !readExact(payload.data(), size)) return 1;
        if (kind == CliModuleProtocol::Reset) { above = false; continue; }
        if (kind == CliModuleProtocol::Configure) continue;
        if (kind != CliModuleProtocol::SpectrumF32 || size < 20 || (size - 16) % 4) return 2;
        double centerHz = 0, sampleRateHz = 0;
        std::memcpy(&centerHz, payload.constData(), 8);
        std::memcpy(&sampleRateHz, payload.constData() + 8, 8);
        if (!std::isfinite(centerHz) || !std::isfinite(sampleRateHz) || sampleRateHz <= 0) continue;
        const std::size_t bins = (size - 16) / 4;
        const double startHz = centerHz - sampleRateHz * 0.5;
        float peak = -200;
        double peakHz = 0;
        bool found = false;
        for (std::size_t i = 0; i < bins; ++i) {
            const double hz = startHz + (i + 0.5) * sampleRateHz / bins;
            if (hz < minHz || hz > maxHz) continue;
            float level;
            std::memcpy(&level, payload.constData() + 16 + i * 4, 4);
            if (std::isfinite(level)) {
                found = true;
                if (level > peak) { peak = level; peakHz = hz; }
            }
        }
        if (!found) { above = false; continue; }
        if (above && peak < thresholdDbfs - hysteresisDb) above = false;
        if (!above && peak >= thresholdDbfs && timer.elapsed() - lastEventMs >= holdoffMs) {
            above = true;
            lastEventMs = timer.elapsed();
            send({{QStringLiteral("event"), QStringLiteral("spectrumThreshold")},
                  {QStringLiteral("module"), QStringLiteral("monitor")},
                  {QStringLiteral("utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                  {QStringLiteral("peakFrequencyHz"), peakHz},
                  {QStringLiteral("peakDbfs"), peak},
                  {QStringLiteral("thresholdDbfs"), thresholdDbfs}});
        }
    }
    return std::feof(stdin) ? 0 : 1;
}
