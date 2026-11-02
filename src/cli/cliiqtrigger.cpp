#include "cliiqtrigger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {
QJsonObject errorReply(const QString &message) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}
}

CliIqTrigger::~CliIqTrigger() {
    if (writer.valid()) writer.wait();
}

QJsonObject CliIqTrigger::status() const {
    const QString name = state == State::Unconfigured ? QStringLiteral("unconfigured") :
        state == State::Ready ? QStringLiteral("ready") :
        state == State::Armed ? QStringLiteral("armed") :
        state == State::Capturing ? QStringLiteral("capturing") : QStringLiteral("writing");
    return {{QStringLiteral("state"), name},
            {QStringLiteral("bandStartHz"), bandStartHz},
            {QStringLiteral("bandEndHz"), bandEndHz},
            {QStringLiteral("thresholdDbfs"), thresholdDbfs},
            {QStringLiteral("hysteresisDb"), hysteresisDb},
            {QStringLiteral("edge"), falling ? QStringLiteral("falling") : QStringLiteral("rising")},
            {QStringLiteral("preMs"), preMs}, {QStringLiteral("postMs"), postMs},
            {QStringLiteral("autoRearm"), autoRearm},
            {QStringLiteral("preBufferedSamples"), static_cast<double>(preBufferedSamples)},
            {QStringLiteral("postBufferedSamples"), static_cast<double>(postBufferedSamples)},
            {QStringLiteral("captures"), static_cast<double>(captures)}};
}

QJsonObject CliIqTrigger::configuration() const {
    if (state == State::Unconfigured) return {};
    return {{QStringLiteral("outputPrefix"), outputPrefix},
        {QStringLiteral("bandStartHz"), bandStartHz}, {QStringLiteral("bandEndHz"), bandEndHz},
        {QStringLiteral("thresholdDbfs"), thresholdDbfs},
        {QStringLiteral("hysteresisDb"), hysteresisDb},
        {QStringLiteral("edge"), falling ? QStringLiteral("falling") : QStringLiteral("rising")},
        {QStringLiteral("preMs"), preMs}, {QStringLiteral("postMs"), postMs},
        {QStringLiteral("autoRearm"), autoRearm}};
}

void CliIqTrigger::disarm() {
    if (state == State::Armed || state == State::Capturing) state = State::Ready;
    preBuffer.clear();
    captureBuffer.clear();
    preBufferedSamples = postBufferedSamples = 0;
    previousValid = false;
}

QJsonObject CliIqTrigger::command(const QJsonObject &request, double centerHz, double sampleRateHz) {
    const QString operation = request.value(QStringLiteral("operation")).toString();
    if (operation == QStringLiteral("status"))
        return {{QStringLiteral("ok"), true}, {QStringLiteral("trigger"), status()}};
    if (operation == QStringLiteral("disarm")) {
        disarm();
        return {{QStringLiteral("ok"), true}, {QStringLiteral("trigger"), status()}};
    }
    if (operation == QStringLiteral("load") || operation == QStringLiteral("restoreConfiguration")) {
        if (state == State::Writing) return errorReply(QStringLiteral("Wait for the current capture to finish"));
        QJsonObject config;
        QString basePath;
        if (operation == QStringLiteral("load")) {
            QFile file(request.value(QStringLiteral("path")).toString());
            if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
                return errorReply(QStringLiteral("Cannot read trigger preset or it exceeds 64 KiB"));
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return errorReply(QStringLiteral("Trigger preset must be a JSON object"));
            config = document.object();
            basePath = file.fileName();
        } else config = request.value(QStringLiteral("configuration")).toObject();
        const QString prefix = config.value(QStringLiteral("outputPrefix")).toString();
        const double start = config.value(QStringLiteral("bandStartHz")).toDouble(-1.0);
        const double end = config.value(QStringLiteral("bandEndHz")).toDouble(-1.0);
        const double threshold = config.value(QStringLiteral("thresholdDbfs")).toDouble(-60.0);
        const double hysteresis = config.value(QStringLiteral("hysteresisDb")).toDouble(3.0);
        const QString edge = config.value(QStringLiteral("edge")).toString(QStringLiteral("rising"));
        const int pre = config.value(QStringLiteral("preMs")).toInt(100);
        const int post = config.value(QStringLiteral("postMs")).toInt(100);
        if (prefix.isEmpty() || !std::isfinite(start) || !std::isfinite(end) || start <= 0.0 || end <= start ||
            !std::isfinite(threshold) || threshold < -160.0 || threshold > 0.0 ||
            !std::isfinite(hysteresis) || hysteresis < 0.0 || hysteresis > 30.0 ||
            (edge != QStringLiteral("rising") && edge != QStringLiteral("falling")) ||
            pre < 0 || post < 0 || pre + post == 0 || pre > 10000 || post > 10000)
            return errorReply(QStringLiteral("Invalid trigger preset"));
        disarm();
        if (operation == QStringLiteral("restoreConfiguration") && !QFileInfo(prefix).isAbsolute())
            return errorReply(QStringLiteral("Restored trigger output prefix must be absolute"));
        outputPrefix = QFileInfo(prefix).isAbsolute() ? prefix :
            QFileInfo(basePath).dir().filePath(prefix);
        bandStartHz = start;
        bandEndHz = end;
        thresholdDbfs = threshold;
        hysteresisDb = hysteresis;
        falling = edge == QStringLiteral("falling");
        preMs = pre;
        postMs = post;
        autoRearm = config.value(QStringLiteral("autoRearm")).toBool(false);
        state = State::Ready;
    } else if (operation == QStringLiteral("arm")) {
        if (state != State::Ready && state != State::Armed)
            return errorReply(QStringLiteral("Load a trigger preset and finish any capture first"));
        if (!std::isfinite(centerHz) || !std::isfinite(sampleRateHz) || sampleRateHz <= 0.0 ||
            bandStartHz < centerHz - sampleRateHz * 0.5 ||
            bandEndHz > centerHz + sampleRateHz * 0.5)
            return errorReply(QStringLiteral("Trigger band is outside the current IQ span"));
        const double totalBytes = sampleRateHz * (preMs + postMs) * 0.001 * 8.0;
        if (totalBytes > 64.0 * 1024.0 * 1024.0)
            return errorReply(QStringLiteral("Pre/post IQ buffer exceeds 64 MiB; shorten capture or reduce sample rate"));
        disarm();
        sourceCenterHz = centerHz;
        sourceRateHz = sampleRateHz;
        preTargetSamples = static_cast<quint64>(std::ceil(sampleRateHz * preMs * 0.001));
        postTargetSamples = static_cast<quint64>(std::ceil(sampleRateHz * postMs * 0.001));
        state = State::Armed;
    } else return errorReply(QStringLiteral("Unknown trigger operation"));
    return {{QStringLiteral("ok"), true}, {QStringLiteral("trigger"), status()}};
}

QJsonObject CliIqTrigger::submitIq(const float *iq, int samples) {
    if (state == State::Writing && writer.valid() &&
        writer.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const WriteResult result = writer.get();
        state = autoRearm && result.error.isEmpty() ? State::Armed : State::Ready;
        previousValid = false;
        if (!result.error.isEmpty())
            return {{QStringLiteral("event"), QStringLiteral("triggerError")},
                    {QStringLiteral("error"), result.error}};
        return {{QStringLiteral("event"), QStringLiteral("triggerSaved")},
                {QStringLiteral("path"), result.path},
                {QStringLiteral("samples"), static_cast<double>(result.samples)}};
    }
    if (!iq || samples <= 0 || (state != State::Armed && state != State::Capturing)) return {};
    const qint64 bytes = static_cast<qint64>(samples) * 2 * sizeof(float);
    if (state == State::Armed) {
        if (preTargetSamples == 0) return {};
        preBuffer.emplace_back(reinterpret_cast<const char *>(iq), static_cast<int>(bytes));
        preBufferedSamples += static_cast<quint64>(samples);
        while (preBufferedSamples > preTargetSamples && !preBuffer.empty()) {
            QByteArray &front = preBuffer.front();
            const quint64 excess = preBufferedSamples - preTargetSamples;
            const quint64 frontSamples = static_cast<quint64>(front.size()) / 8;
            const quint64 remove = (std::min)(excess, frontSamples);
            if (remove == frontSamples) preBuffer.pop_front();
            else front.remove(0, static_cast<int>(remove * 8));
            preBufferedSamples -= remove;
        }
        return {};
    }
    const quint64 remaining = postTargetSamples - postBufferedSamples;
    const quint64 accepted = (std::min)(remaining, static_cast<quint64>(samples));
    if (accepted > 0) {
        captureBuffer.emplace_back(reinterpret_cast<const char *>(iq), static_cast<int>(accepted * 8));
        postBufferedSamples += accepted;
    }
    if (postBufferedSamples >= postTargetSamples) startWrite();
    return {};
}

QJsonObject CliIqTrigger::observe(const std::vector<float> &levels, double centerHz, double sampleRateHz) {
    if (state != State::Armed || levels.empty() ||
        std::abs(centerHz - sourceCenterHz) > 0.5 || std::abs(sampleRateHz - sourceRateHz) > 0.5)
        return {};
    const double firstHz = centerHz - sampleRateHz * 0.5;
    const double binHz = sampleRateHz / levels.size();
    const int first = (std::max)(0, static_cast<int>(std::ceil((bandStartHz - firstHz) / binHz)));
    const int last = (std::min)(static_cast<int>(levels.size()) - 1,
        static_cast<int>(std::floor((bandEndHz - firstHz) / binHz)));
    if (last < first) return {};
    auto peak = std::max_element(levels.begin() + first, levels.begin() + last + 1);
    const double level = *peak;
    const bool above = previousValid && previousAbove
        ? level >= thresholdDbfs - hysteresisDb
        : level >= thresholdDbfs;
    const bool crossed = previousValid && (falling ? previousAbove && !above : !previousAbove && above);
    previousAbove = above;
    previousValid = true;
    if (!crossed) return {};
    triggerUtcMs = QDateTime::currentMSecsSinceEpoch();
    triggerFrequencyHz = firstHz + (peak - levels.begin()) * binHz;
    triggerLevelDbfs = level;
    captureBuffer.reserve(preBuffer.size() + 16);
    for (QByteArray &block : preBuffer) captureBuffer.push_back(std::move(block));
    preBuffer.clear();
    state = State::Capturing;
    if (postTargetSamples == 0) startWrite();
    return {{QStringLiteral("event"), QStringLiteral("triggerFired")},
            {QStringLiteral("approximateUtc"), QDateTime::fromMSecsSinceEpoch(triggerUtcMs, Qt::UTC)
                .toString(Qt::ISODateWithMs)},
            {QStringLiteral("frequencyHz"), triggerFrequencyHz},
            {QStringLiteral("levelDbfs"), triggerLevelDbfs}};
}

void CliIqTrigger::startWrite() {
    state = State::Writing;
    const QString path = QStringLiteral("%1_%2.cf32").arg(outputPrefix)
        .arg(++captures, 4, 10, QLatin1Char('0'));
    const double rate = sourceRateHz;
    const double center = sourceCenterHz;
    const qint64 utcMs = triggerUtcMs;
    const double frequency = triggerFrequencyHz;
    const double level = triggerLevelDbfs;
    const quint64 preSamples = preBufferedSamples;
    const quint64 postSamples = postBufferedSamples;
    std::vector<QByteArray> blocks = std::move(captureBuffer);
    captureBuffer.clear();
    preBufferedSamples = postBufferedSamples = 0;
    writer = std::async(std::launch::async,
        [path, rate, center, utcMs, frequency, level, preSamples, postSamples,
         blocks = std::move(blocks)]() mutable -> WriteResult {
            QFile file(path);
            QFile metadata(path + QStringLiteral(".json"));
            if (QFile::exists(path) || QFile::exists(metadata.fileName()) ||
                !file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                return {path, QStringLiteral("Cannot create unique trigger IQ file"), 0};
            quint64 written = 0;
            for (const QByteArray &block : blocks) {
                if (file.write(block) != block.size()) {
                    file.close();
                    return {path, QStringLiteral("Trigger IQ write failed"), written};
                }
                written += static_cast<quint64>(block.size()) / 8;
            }
            if (!file.flush()) return {path, QStringLiteral("Trigger IQ flush failed"), written};
            file.close();
            if (!metadata.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                return {path, QStringLiteral("Trigger IQ metadata create failed"), written};
            const QJsonObject sidecar{{QStringLiteral("format"), QStringLiteral("cf32le")},
                {QStringLiteral("input"), QStringLiteral("rf")},
                {QStringLiteral("sampleRate"), rate},
                {QStringLiteral("centerFrequency"), center},
                {QStringLiteral("preSamples"), static_cast<double>(preSamples)},
                {QStringLiteral("postSamples"), static_cast<double>(postSamples)},
                {QStringLiteral("triggerFrequencyHz"), frequency},
                {QStringLiteral("triggerLevelDbfs"), level},
                {QStringLiteral("approximateTriggerUtc"),
                    QDateTime::fromMSecsSinceEpoch(utcMs, Qt::UTC).toString(Qt::ISODateWithMs)},
                {QStringLiteral("timebase"), QStringLiteral("host-clock-estimate-not-hardware-timestamp")}};
            const QByteArray json = QJsonDocument(sidecar).toJson(QJsonDocument::Indented);
            if (metadata.write(json) != json.size() || !metadata.flush())
                return {path, QStringLiteral("Trigger IQ metadata write failed"), written};
            return {path, {}, written};
        });
}
