#include "climultisdr.h"

#include "dspiqsourceregistry.h"

#include <QDateTime>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace {
QJsonObject errorReply(const QString &message) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

bool sameHardware(const QString &a, const QString &b) {
    if (a.startsWith(QStringLiteral("fobos-")) && b.startsWith(QStringLiteral("fobos-"))) return true;
    return a == b;
}

}

CliMultiSdr::CliMultiSdr(QString backend, int index, QObject *parent)
    : QObject(parent), primaryBackend(std::move(backend)), primaryDeviceIndex(index) {
    mosaic.setMaximumOutputBins(4096);
    mosaic.setMaximumFrameAgeMs(2000);
}

CliMultiSdr::~CliMultiSdr() {
    for (auto &entry : sourceSlots) {
        entry.second->receiver->stop();
        DspIqSourceRegistry::instance().clear(DspSourceIdentity::normalized(
            entry.second->settings.receiverId, entry.second->settings.channelId));
    }
}

bool CliMultiSdr::duplicatesHardware(const QString &backend, int index, int excludingId) const {
    if (index == primaryDeviceIndex && sameHardware(backend, primaryBackend)) return true;
    for (const auto &entry : sourceSlots)
        if (entry.first != excludingId && entry.second->settings.nativeIndex == index &&
            sameHardware(backend, entry.second->settings.backendId)) return true;
    return false;
}

bool CliMultiSdr::hasRunningAuxiliary() const {
    for (const auto &entry : sourceSlots)
        if (entry.second->receiver->isRunning()) return true;
    return false;
}

void CliMultiSdr::rebuildMosaic() {
    mosaic.clear();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (hasPrimaryFrame && now - primaryFrame.utcMs <= 2000)
        mosaic.updateFrame(1, primaryFrame);
    for (const auto &entry : sourceSlots)
        if (entry.second->hasFrame && entry.second->receiver->isRunning() &&
            now - entry.second->frame.utcMs <= 2000)
            mosaic.updateFrame(entry.first, entry.second->frame);
}

void CliMultiSdr::updatePrimarySpectrum(const std::vector<float> &levels, double centerHz,
                                        double sampleRateHz) {
    if (levels.size() < 2 || !std::isfinite(centerHz) || !std::isfinite(sampleRateHz) ||
        sampleRateHz <= 0.0) return;
    primaryFrame.utcMs = QDateTime::currentMSecsSinceEpoch();
    primaryFrame.centerHz = centerHz;
    primaryFrame.minimumHz = centerHz - sampleRateHz * 0.5;
    primaryFrame.maximumHz = centerHz + sampleRateHz * 0.5;
    primaryFrame.sourceFftLength = static_cast<int>(levels.size());
    primaryFrame.levelsDb = levels;
    hasPrimaryFrame = true;
    mosaic.updateFrame(1, primaryFrame);
}

void CliMultiSdr::clearPrimarySpectrum() {
    hasPrimaryFrame = false;
    primaryFrame = {};
    rebuildMosaic();
}

bool CliMultiSdr::hasSource(const DspSourceIdentity &source, double *centerHz, double *rateHz) const {
    if (source.channelId != QStringLiteral("rx0")) return false;
    for (const auto &entry : sourceSlots) {
        if (entry.second->settings.receiverId != source.receiverId) continue;
        if (centerHz) *centerHz = entry.second->settings.centerHz;
        if (rateHz) *rateHz = entry.second->settings.sampleRateHz;
        return true;
    }
    return false;
}

QJsonObject CliMultiSdr::frameReply(int id, const AnalyzerSpectrumFrame &frame) const {
    QJsonArray levels;
    for (float value : frame.levelsDb)
        levels.append(std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null));
    return {{QStringLiteral("ok"), true}, {QStringLiteral("id"), id},
            {QStringLiteral("utcMs"), frame.utcMs},
            {QStringLiteral("centerHz"), frame.centerHz},
            {QStringLiteral("minimumHz"), frame.minimumHz},
            {QStringLiteral("maximumHz"), frame.maximumHz},
            {QStringLiteral("fftLength"), frame.sourceFftLength},
            {QStringLiteral("levelsDbfs"), levels}};
}

QJsonObject CliMultiSdr::status(double center, double rate, bool primaryRunning) const {
    QJsonArray sources;
    sources.append(QJsonObject{{QStringLiteral("id"), 1}, {QStringLiteral("receiverId"), QStringLiteral("main")},
        {QStringLiteral("channelId"), QStringLiteral("rx0")}, {QStringLiteral("backend"), primaryBackend},
        {QStringLiteral("deviceIndex"), primaryDeviceIndex}, {QStringLiteral("centerHz"), center},
        {QStringLiteral("sampleRateHz"), rate}, {QStringLiteral("running"), primaryRunning},
        {QStringLiteral("frameUtcMs"), hasPrimaryFrame ? QJsonValue(static_cast<double>(primaryFrame.utcMs)) : QJsonValue()}});
    for (const auto &entry : sourceSlots) {
        const Slot &slot = *entry.second;
        sources.append(QJsonObject{{QStringLiteral("id"), entry.first},
            {QStringLiteral("receiverId"), slot.settings.receiverId},
            {QStringLiteral("channelId"), slot.settings.channelId},
            {QStringLiteral("backend"), slot.settings.backendId},
            {QStringLiteral("deviceIndex"), slot.settings.nativeIndex},
            {QStringLiteral("centerHz"), slot.settings.centerHz},
            {QStringLiteral("sampleRateHz"), slot.settings.sampleRateHz},
            {QStringLiteral("fftLength"), slot.settings.fftLength},
            {QStringLiteral("running"), slot.receiver->isRunning()},
            {QStringLiteral("frameUtcMs"), slot.hasFrame ? QJsonValue(static_cast<double>(slot.frame.utcMs)) : QJsonValue()},
            {QStringLiteral("error"), slot.error}});
    }
    return {{QStringLiteral("sources"), sources}, {QStringLiteral("count"), sources.size()}};
}

QJsonObject CliMultiSdr::command(const QJsonObject &request, double primaryCenter,
                                 double primaryRate, bool primaryRunning) {
    const QString operation = request.value(QStringLiteral("operation")).toString();
    if (operation == QStringLiteral("status"))
        return {{QStringLiteral("ok"), true}, {QStringLiteral("multisdr"), status(primaryCenter, primaryRate, primaryRunning)}};
    if (operation == QStringLiteral("devices")) {
        QJsonArray devices;
        for (const AnalyzerReceiverDevice &device : AnalyzerReceiver::enumerateDevices()) {
            QJsonArray rates;
            for (double rate : device.sampleRatesHz) rates.append(rate);
            devices.append(QJsonObject{{QStringLiteral("backend"), device.backendId},
                {QStringLiteral("label"), device.label}, {QStringLiteral("deviceIndex"), device.nativeIndex},
                {QStringLiteral("sampleRatesHz"), rates},
                {QStringLiteral("minimumFrequencyHz"), device.minimumFrequencyHz},
                {QStringLiteral("maximumFrequencyHz"), device.maximumFrequencyHz}});
        }
        return {{QStringLiteral("ok"), true}, {QStringLiteral("devices"), devices}};
    }
    if (operation == QStringLiteral("mosaic")) {
        rebuildMosaic();
        const auto result = mosaic.build();
        if (!result) return errorReply(QStringLiteral("No current Multi-SDR spectrum frames"));
        QJsonObject reply = frameReply(0, result->frame);
        QJsonArray contributors, stale;
        for (int id : result->contributingSources) contributors.append(id);
        for (int id : result->staleSources) stale.append(id);
        reply.insert(QStringLiteral("contributingReceivers"), contributors);
        reply.insert(QStringLiteral("staleReceivers"), stale);
        reply.insert(QStringLiteral("uncoveredBins"), result->uncoveredBins);
        reply.insert(QStringLiteral("overlapBins"), result->overlapBins);
        return reply;
    }
    const int id = request.value(QStringLiteral("id")).toInt(-1);
    if (id < 2 || id > 8) return errorReply(QStringLiteral("Multi-SDR receiver ID must be 2..8; RX1 is primary"));
    if (operation == QStringLiteral("add")) {
        if (sourceSlots.count(id)) return errorReply(QStringLiteral("This receiver ID is already configured"));
        const QString backend = request.value(QStringLiteral("backend")).toString();
        if (!QStringList{QStringLiteral("fobos-standard"), QStringLiteral("fobos-agile"),
            QStringLiteral("hackrf"), QStringLiteral("rtl-sdr"), QStringLiteral("soapy-sdr")}.contains(backend))
            return errorReply(QStringLiteral("Supported auxiliary backends: fobos-standard, fobos-agile, hackrf, rtl-sdr, soapy-sdr"));
        const int index = request.value(QStringLiteral("deviceIndex")).toInt(-1);
        const double center = request.value(QStringLiteral("centerHz")).toDouble();
        const double rate = request.value(QStringLiteral("sampleRateHz")).toDouble();
        const int fft = request.value(QStringLiteral("fftLength")).toInt(4096);
        if (index < 0 || index > 64 || !std::isfinite(center) || center <= 0.0 ||
            !std::isfinite(rate) || rate < 100000.0 || rate > 61440000.0 ||
            fft < 512 || fft > 65536 || (fft & (fft - 1)) != 0)
            return errorReply(QStringLiteral("Invalid device, center, rate or power-of-two FFT (512..65536)"));
        if (duplicatesHardware(backend, index))
            return errorReply(QStringLiteral("The same physical receiver is already configured"));
        auto slot = std::make_unique<Slot>();
        slot->settings.backendId = backend;
        slot->settings.nativeIndex = index;
        slot->settings.centerHz = center;
        slot->settings.sampleRateHz = rate;
        slot->settings.fftLength = fft;
        slot->settings.receiverId = QStringLiteral("multi-rx-%1").arg(id);
        slot->receiver = std::make_unique<AnalyzerReceiver>();
        QObject::connect(slot->receiver.get(), &AnalyzerReceiver::frameReady, this,
            [this, id](const AnalyzerSpectrumFrame &frame) {
                auto found = sourceSlots.find(id);
                if (found == sourceSlots.end() || !found->second->receiver->isRunning()) return;
                found->second->frame = frame;
                found->second->hasFrame = true;
                mosaic.updateFrame(id, frame);
            });
        QObject::connect(slot->receiver.get(), &AnalyzerReceiver::receiverError, this,
            [this, id](const QString &message) {
                auto found = sourceSlots.find(id);
                if (found != sourceSlots.end()) found->second->error = message;
            });
        sourceSlots.emplace(id, std::move(slot));
    } else {
        const auto found = sourceSlots.find(id);
        if (found == sourceSlots.end()) return errorReply(QStringLiteral("Multi-SDR receiver is not configured"));
        Slot &slot = *found->second;
        if (operation == QStringLiteral("start")) {
            if (slot.receiver->isRunning()) return errorReply(QStringLiteral("Receiver is already running"));
            slot.error.clear();
            slot.hasFrame = false;
            if (!slot.receiver->start(slot.settings)) return errorReply(slot.receiver->errorString());
        } else if (operation == QStringLiteral("stop")) {
            slot.receiver->stop();
            slot.hasFrame = false;
            DspIqSourceRegistry::instance().clear(DspSourceIdentity::normalized(slot.settings.receiverId, slot.settings.channelId));
            rebuildMosaic();
        } else if (operation == QStringLiteral("remove")) {
            slot.receiver->stop();
            DspIqSourceRegistry::instance().clear(DspSourceIdentity::normalized(slot.settings.receiverId, slot.settings.channelId));
            sourceSlots.erase(found);
            rebuildMosaic();
        } else if (operation == QStringLiteral("tune")) {
            const double center = request.value(QStringLiteral("centerHz")).toDouble();
            if (!std::isfinite(center) || center <= 0.0) return errorReply(QStringLiteral("Invalid center frequency"));
            if (slot.receiver->isRunning() && !slot.receiver->retune(center))
                return errorReply(slot.receiver->errorString());
            slot.settings.centerHz = center;
            slot.hasFrame = false;
            rebuildMosaic();
        } else if (operation == QStringLiteral("spectrum")) {
            if (!slot.hasFrame || !slot.receiver->isRunning())
                return errorReply(QStringLiteral("No current spectrum frame for this receiver"));
            return frameReply(id, slot.frame);
        } else return errorReply(QStringLiteral("Unknown Multi-SDR operation"));
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("multisdr"), status(primaryCenter, primaryRate, primaryRunning)}};
}
