#include "clifrequencyscanner.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>

namespace {
QJsonObject errorReply(const QString &message) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}
}

QJsonObject CliFrequencyScanner::status() const {
    const QString name = state == State::Stopped ? QStringLiteral("stopped") :
        state == State::Scanning ? QStringLiteral("scanning") : QStringLiteral("locked");
    return {{QStringLiteral("state"), name},
            {QStringLiteral("frequencies"), static_cast<int>(centers.size())},
            {QStringLiteral("currentIndex"), index},
            {QStringLiteral("dwellMs"), dwellMs},
            {QStringLiteral("holdMs"), holdMs},
            {QStringLiteral("thresholdDbfs"), thresholdDbfs},
            {QStringLiteral("minSnrDb"), minSnrDb},
            {QStringLiteral("visits"), static_cast<double>(visits)},
            {QStringLiteral("hits"), static_cast<double>(hits)},
            {QStringLiteral("lastHit"), lastHit}};
}

QJsonObject CliFrequencyScanner::configuration() const {
    if (centers.empty()) return {};
    QJsonArray frequencies;
    for (double hz : centers) frequencies.append(hz);
    return {{QStringLiteral("centerHz"), frequencies}, {QStringLiteral("dwellMs"), dwellMs},
        {QStringLiteral("holdMs"), holdMs}, {QStringLiteral("thresholdDbfs"), thresholdDbfs},
        {QStringLiteral("minSnrDb"), minSnrDb}};
}

void CliFrequencyScanner::stop() {
    state = State::Stopped;
    tunePending = false;
    confirmed = 0;
}

QJsonObject CliFrequencyScanner::command(const QJsonObject &request) {
    const QString operation = request.value(QStringLiteral("operation")).toString();
    if (operation == QStringLiteral("status"))
        return {{QStringLiteral("ok"), true}, {QStringLiteral("scan"), status()}};
    if (operation == QStringLiteral("stop")) {
        stop();
        return {{QStringLiteral("ok"), true}, {QStringLiteral("scan"), status()}};
    }
    if (operation == QStringLiteral("load") || operation == QStringLiteral("restoreConfiguration")) {
        QJsonObject config;
        if (operation == QStringLiteral("load")) {
            QFile file(request.value(QStringLiteral("path")).toString());
            if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
                return errorReply(QStringLiteral("Cannot read scan preset or file exceeds 64 KiB"));
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return errorReply(QStringLiteral("Scan preset must be a JSON object"));
            config = document.object();
        } else config = request.value(QStringLiteral("configuration")).toObject();
        const QJsonArray frequencies = config.value(QStringLiteral("centerHz")).toArray();
        const int dwell = config.value(QStringLiteral("dwellMs")).toInt(500);
        const int hold = config.value(QStringLiteral("holdMs")).toInt(0);
        const double threshold = config.value(QStringLiteral("thresholdDbfs")).toDouble(-70.0);
        const double snr = config.value(QStringLiteral("minSnrDb")).toDouble(8.0);
        if (frequencies.isEmpty() || frequencies.size() > 256 || dwell < 250 || dwell > 60000 ||
            hold < 0 || hold > 3600000 || !std::isfinite(threshold) || threshold < -160.0 || threshold > 0.0 ||
            !std::isfinite(snr) || snr < 0.0 || snr > 80.0)
            return errorReply(QStringLiteral("Invalid scan preset limits"));
        std::vector<double> values;
        values.reserve(frequencies.size());
        for (const QJsonValue &value : frequencies) {
            const double frequency = value.toDouble(-1.0);
            if (!std::isfinite(frequency) || frequency <= 0.0)
                return errorReply(QStringLiteral("Invalid center frequency in scan preset"));
            values.push_back(frequency);
        }
        stop();
        centers = std::move(values);
        dwellMs = dwell;
        holdMs = hold;
        thresholdDbfs = threshold;
        minSnrDb = snr;
        index = 0;
        visits = hits = 0;
        lastHit = {};
    } else if (operation == QStringLiteral("start")) {
        if (centers.empty()) return errorReply(QStringLiteral("Load a scan preset first"));
        state = State::Scanning;
        index = 0;
        tunePending = true;
        confirmed = 0;
    } else if (operation == QStringLiteral("resume")) {
        if (state != State::Locked) return errorReply(QStringLiteral("Scanner is not locked"));
        state = State::Scanning;
        index = (index + 1) % static_cast<int>(centers.size());
        tunePending = true;
        confirmed = 0;
    } else return errorReply(QStringLiteral("Unknown scan operation"));
    return {{QStringLiteral("ok"), true}, {QStringLiteral("scan"), status()}};
}

bool CliFrequencyScanner::takeTune(double *frequencyHz) {
    if (state == State::Stopped || centers.empty()) return false;
    if (state == State::Locked) {
        if (!holdMs || !timer.isValid() || timer.elapsed() < holdMs) return false;
        state = State::Scanning;
        index = (index + 1) % static_cast<int>(centers.size());
        tunePending = true;
    }
    if (!tunePending && timer.isValid() && timer.elapsed() >= dwellMs) {
        index = (index + 1) % static_cast<int>(centers.size());
        tunePending = true;
    }
    if (!tunePending) return false;
    tunePending = false;
    *frequencyHz = centers[static_cast<std::size_t>(index)];
    return true;
}

void CliFrequencyScanner::tuned() {
    timer.start();
    ++visits;
    confirmed = 0;
}

QJsonObject CliFrequencyScanner::observe(const QJsonObject &spectrum, double centerHz) {
    if (state != State::Scanning || !timer.isValid() || timer.elapsed() < 100 ||
        centers.empty() || std::abs(centerHz - centers[static_cast<std::size_t>(index)]) > 0.5)
        return {};
    const double noise = spectrum.value(QStringLiteral("noiseFloorDbfs")).toDouble(0.0);
    for (const QJsonValue &value : spectrum.value(QStringLiteral("peaks")).toArray()) {
        const QJsonObject peak = value.toObject();
        const double level = peak.value(QStringLiteral("levelDbfs")).toDouble(-200.0);
        if (level < thresholdDbfs || level - noise < minSnrDb) continue;
        if (++confirmed < 2) return {};
        state = State::Locked;
        ++hits;
        timer.restart();
        lastHit = {{QStringLiteral("event"), QStringLiteral("scanHit")},
                   {QStringLiteral("utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                   {QStringLiteral("centerFrequencyHz"), centerHz},
                   {QStringLiteral("signalFrequencyHz"), peak.value(QStringLiteral("frequencyHz"))},
                   {QStringLiteral("levelDbfs"), level},
                   {QStringLiteral("noiseFloorDbfs"), noise},
                   {QStringLiteral("index"), index}};
        return lastHit;
    }
    confirmed = 0;
    return {};
}
