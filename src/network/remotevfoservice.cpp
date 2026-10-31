#include "remotevfoservice.h"

#include "dspbranchaudiooutput.h"
#include "dspbranchengine.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {
QString safeId(QString value) {
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (!c.isLetterOrNumber() && c != QLatin1Char('-') && c != QLatin1Char('_')) {
            value[i] = QLatin1Char('_');
        }
    }
    return value.left(80);
}
double finiteNumber(const QJsonObject &object,
                    const QString &key,
                    double fallback,
                    double minimum,
                    double maximum) {
    const double value = object.value(key).toDouble(fallback);
    return std::isfinite(value) ? (std::clamp)(value, minimum, maximum) : fallback;
}

double branchRateForBandwidth(double bandwidthHz) {
    const double required = (std::max)(48000.0, bandwidthHz * 2.5);
    const double rates[] = {48000.0, 96000.0, 192000.0, 384000.0, 768000.0, 1536000.0};
    for (double rate : rates) if (rate >= required) return rate;
    return 2000000.0;
}
}

struct RemoteVfoService::Vfo {
    QString id;
    DspSourceIdentity source;
    double frequencyHz = 0.0;
    double bandwidthHz = 12500.0;
    double lowPassHz = 3000.0;
    double squelchDb = -160.0;
    double volume = 0.7;
    int modulation = MOD_NFM;
    int fftLength = 2048;
    int refreshMs = 100;
    bool audioEnabled = true;
    bool spectrumEnabled = true;
    bool active = false;
    QString lastError;
    QString branchId;
    QString fftId;
    QString audioId;
};

struct RemoteVfoService::Session {
    QString ownerId;
    QString label;
    QString sessionId;
    QString role = QStringLiteral("listener");
    QHash<QString, Vfo> vfos;
};

RemoteVfoService::RemoteVfoService(QObject *parent)
    : QObject(parent) {
    drainTimer_.setInterval(20);
    drainTimer_.setTimerType(Qt::CoarseTimer);
    connect(&drainTimer_, &QTimer::timeout, this, &RemoteVfoService::drainFrames);
}

RemoteVfoService::~RemoteVfoService() {
    drainTimer_.stop();
    if (engine_) {
        DspGraphPlan empty;
        sourceSettings_.simplifiedAudioChannelizer = false;
        engine_->configure(empty, sourceSettings_, false);
        engine_->setAudioConsumer({});
    }
}

void RemoteVfoService::setSourceState(const RadioSettings &settings, bool receiverRunning) {
    const bool changed = receiverRunning_ != receiverRunning ||
        std::abs(sourceSettings_.centerFrequency - settings.centerFrequency) > 0.5 ||
        std::abs(sourceSettings_.sampleRate - settings.sampleRate) > 0.5 ||
        sourceSettings_.inputMode != settings.inputMode ||
        sourceSettings_.simplifiedAudioChannelizer != settings.simplifiedAudioChannelizer;
    sourceSettings_ = settings;
    receiverRunning_ = receiverRunning;
    if (changed) rebuildPlan();
}

void RemoteVfoService::setExternalEndpoint(const DspStreamEndpoint &endpoint) {
    if (externalEndpoint_ == endpoint) return;
    externalEndpoint_ = endpoint;
    if (engine_) engine_->setExternalEndpoint(externalEndpoint_);
    rebuildPlan();
}

void RemoteVfoService::openEndpoint(const QString &ownerId, const QString &label) {
    if (ownerId.trimmed().isEmpty()) return;
    Session &session = ensureSession(ownerId, label);
    sendSessionState(session);
}

void RemoteVfoService::closeEndpoint(const QString &ownerId) {
    if (!sessions_.remove(ownerId)) return;
    rebuildPlan();
    emit statusChanged();
}

RemoteVfoService::Session &RemoteVfoService::ensureSession(const QString &ownerId,
                                                           const QString &label) {
    auto it = sessions_.find(ownerId);
    if (it == sessions_.end()) {
        Session session;
        session.ownerId = ownerId;
        session.label = label.isEmpty() ? ownerId : label;
        session.sessionId = QStringLiteral("rvfo-%1-%2")
                                .arg(nextSessionNumber_++)
                                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
        it = sessions_.insert(ownerId, session);
        emit statusChanged();
    } else if (!label.isEmpty()) {
        it->label = label;
    }
    return it.value();
}

void RemoteVfoService::handleCommand(const QString &ownerId, const QJsonObject &command) {
    const QString action = command.value(QStringLiteral("action")).toString().trimmed();
    const QString requestId = command.value(QStringLiteral("requestId")).toString();
    Session &session = ensureSession(ownerId, command.value(QStringLiteral("clientName")).toString());

    if (action == QStringLiteral("open") || action == QStringLiteral("list") ||
        action == QStringLiteral("status")) {
        sendSessionState(session, action == QStringLiteral("status")
                                      ? QStringLiteral("status")
                                      : QStringLiteral("session"));
        return;
    }

    QString vfoId = command.value(QStringLiteral("vfoId")).toString(QStringLiteral("main")).trimmed();
    if (vfoId.isEmpty()) vfoId = QStringLiteral("main");
    vfoId = safeId(vfoId);

    if (action == QStringLiteral("delete")) {
        if (!session.vfos.remove(vfoId)) {
            sendError(ownerId, action, QStringLiteral("Unknown VFO"), requestId);
            return;
        }
        rebuildPlan();
        sendSessionState(session, QStringLiteral("updated"));
        return;
    }

    if (action == QStringLiteral("deactivate")) {
        auto it = session.vfos.find(vfoId);
        if (it == session.vfos.end()) {
            sendError(ownerId, action, QStringLiteral("Unknown VFO"), requestId);
            return;
        }
        it->active = false;
        rebuildPlan();
        sendSessionState(session, QStringLiteral("updated"));
        return;
    }

    if (action != QStringLiteral("create") && action != QStringLiteral("update") &&
        action != QStringLiteral("activate")) {
        sendError(ownerId, action, QStringLiteral("Unsupported remote VFO action"), requestId);
        return;
    }

    auto it = session.vfos.find(vfoId);
    bool inserted = false;
    if (it == session.vfos.end()) {
        if (session.vfos.size() >= MaxVfosPerSession) {
            sendError(ownerId, action, QStringLiteral("This session already uses its VFO quota"), requestId);
            return;
        }
        if (activeVfoCount() >= MaxVfos) {
            sendError(ownerId, action, QStringLiteral("The server has no free VFO slots"), requestId);
            return;
        }
        Vfo vfo;
        vfo.id = vfoId;
        vfo.source = DspSourceIdentity::normalized(QStringLiteral("main"), QStringLiteral("rx0"));
        vfo.frequencyHz = sourceSettings_.listeningFrequency > 0.0
                              ? sourceSettings_.listeningFrequency
                              : sourceSettings_.centerFrequency;
        const QString stem = safeId(session.sessionId + QLatin1Char('-') + vfoId);
        vfo.branchId = QStringLiteral("remote-ddc-") + stem;
        vfo.fftId = QStringLiteral("remote-fft-") + stem;
        vfo.audioId = QStringLiteral("remote-audio-") + stem;
        it = session.vfos.insert(vfoId, vfo);
        inserted = true;
    } else if (action == QStringLiteral("activate") && !it->active &&
               activeVfoCount() >= MaxVfos) {
        sendError(ownerId, action, QStringLiteral("The server has no free VFO slots"), requestId);
        return;
    }

    QJsonObject settings = command.value(QStringLiteral("settings")).toObject();
    for (auto value = command.constBegin(); value != command.constEnd(); ++value) {
        if (value.key() != QStringLiteral("type") && value.key() != QStringLiteral("action") &&
            value.key() != QStringLiteral("settings")) {
            settings.insert(value.key(), value.value());
        }
    }
    QString error;
    Vfo candidate = it.value();
    if (!applyVfoSettings(candidate, settings, &error)) {
        if (inserted) session.vfos.erase(it);
        sendError(ownerId, action, error, requestId);
        return;
    }
    it.value() = std::move(candidate);
    if (action == QStringLiteral("create") || action == QStringLiteral("activate")) it->active = true;
    rebuildPlan();
    sendSessionState(session, QStringLiteral("updated"));
}

bool RemoteVfoService::applyVfoSettings(Vfo &vfo,
                                        const QJsonObject &settings,
                                        QString *error) {
    vfo.frequencyHz = finiteNumber(settings, QStringLiteral("frequencyHz"),
        finiteNumber(settings, QStringLiteral("listeningFrequency"), vfo.frequencyHz, 0.0, 8.0e9),
        0.0, 8.0e9);
    vfo.bandwidthHz = finiteNumber(settings, QStringLiteral("bandwidthHz"),
        finiteNumber(settings, QStringLiteral("bandwidth"), vfo.bandwidthHz, 100.0, 250000.0),
        100.0, 250000.0);
    vfo.lowPassHz = finiteNumber(settings, QStringLiteral("audioLowPassHz"),
                                 vfo.lowPassHz, 100.0, 20000.0);
    vfo.squelchDb = finiteNumber(settings, QStringLiteral("squelchDb"),
                                 vfo.squelchDb, -200.0, 0.0);
    vfo.volume = finiteNumber(settings, QStringLiteral("volume"), vfo.volume, 0.0, 2.0);
    vfo.modulation = (std::clamp)(settings.value(QStringLiteral("modulationType"))
                                      .toInt(vfo.modulation), int(MOD_AM), int(MOD_WFM));
    vfo.audioEnabled = settings.value(QStringLiteral("audioEnabled")).toBool(vfo.audioEnabled);
    vfo.spectrumEnabled = settings.value(QStringLiteral("spectrumEnabled")).toBool(vfo.spectrumEnabled);
    vfo.refreshMs = (std::clamp)(settings.value(QStringLiteral("refreshMs")).toInt(vfo.refreshMs), 40, 2000);
    int fftLength = (std::clamp)(settings.value(QStringLiteral("fftLength")).toInt(vfo.fftLength), 512, 65536);
    int radix2 = 512;
    while (radix2 < fftLength) radix2 *= 2;
    vfo.fftLength = radix2;
    vfo.source = DspSourceIdentity::normalized(
        settings.value(QStringLiteral("receiverId")).toString(vfo.source.receiverId),
        settings.value(QStringLiteral("channelId")).toString(vfo.source.channelId));

    if (vfo.source.isPrimary()) {
        if (!std::isfinite(sourceSettings_.sampleRate) || sourceSettings_.sampleRate <= 0.0) {
            if (error) *error = QStringLiteral("The receiver sample rate is not available");
            return false;
        }
        const double halfBand = vfo.bandwidthHz * 0.5;
        if (std::abs(vfo.frequencyHz - sourceSettings_.centerFrequency) + halfBand >
            sourceSettings_.sampleRate * 0.5) {
            if (error) *error = QStringLiteral("The requested VFO is outside the captured receiver span");
            return false;
        }
    }
    return true;
}

void RemoteVfoService::rebuildPlan() {
    DspGraphPlan plan;
    branchOwners_.clear();
    audioOwners_.clear();
    fftOwners_.clear();
    frameErrors_.clear();
    QJsonArray keyBranches;

    for (auto sessionIt = sessions_.begin(); sessionIt != sessions_.end(); ++sessionIt) {
        for (auto vfoIt = sessionIt->vfos.begin(); vfoIt != sessionIt->vfos.end(); ++vfoIt) {
            Vfo &vfo = vfoIt.value();
            if (!vfo.active) continue;
            DspBranchConfig branch;
            branch.id = vfo.branchId;
            branch.source = vfo.source;
            branch.frequencyHz = vfo.frequencyHz;
            branch.bandwidthHz = (std::clamp)(vfo.bandwidthHz * 1.15, 1000.0, 300000.0);
            branch.outputRateHz = branchRateForBandwidth(branch.bandwidthHz);
            branch.firTaps = 32;
            branch.enabled = true;
            branch.external = externalEndpoint_.enabled;
            branch.audioEnabled = vfo.audioEnabled;
            branch.audioOutputId = vfo.audioId;
            branch.demodulatorId = QStringLiteral("remote-demod-") +
                                   safeId(sessionIt->sessionId + QLatin1Char('-') + vfo.id);
            branch.modulation = vfo.modulation;
            branch.audioBandwidthHz = vfo.bandwidthHz;
            branch.audioLowPassHz = vfo.lowPassHz;
            branch.squelchDb = vfo.squelchDb;
            branch.audioDevice = -1;
            branch.volume = vfo.volume;
            if (vfo.spectrumEnabled) {
                DspBranchFftConfig fft;
                fft.id = vfo.fftId;
                fft.length = vfo.fftLength;
                fft.refreshMs = vfo.refreshMs;
                branch.ffts.append(fft);
            }
            plan.branches.append(branch);
            branchOwners_.insert(branch.id, sessionIt.key());
            audioOwners_.insert(vfo.audioId, sessionIt.key());
            fftOwners_.insert(vfo.fftId, sessionIt.key());
            keyBranches.append(QJsonObject{
                {QStringLiteral("id"), branch.id},
                {QStringLiteral("source"), branch.source.toJson()},
                {QStringLiteral("frequencyHz"), branch.frequencyHz},
                {QStringLiteral("bandwidthHz"), branch.bandwidthHz},
                {QStringLiteral("outputRateHz"), branch.outputRateHz},
                {QStringLiteral("modulation"), branch.modulation},
                {QStringLiteral("audio"), branch.audioEnabled},
                {QStringLiteral("spectrum"), vfo.spectrumEnabled},
                {QStringLiteral("fftLength"), vfo.fftLength},
                {QStringLiteral("refreshMs"), vfo.refreshMs},
                {QStringLiteral("external"), branch.external}
            });
        }
    }
    plan.executionKey = QJsonDocument(QJsonObject{{QStringLiteral("remoteVfos"), keyBranches}})
                            .toJson(QJsonDocument::Compact);
    if (plan.branches.isEmpty()) {
        drainTimer_.stop();
        if (engine_) {
            DspGraphPlan empty;
            RadioSettings source = sourceSettings_;
            source.simplifiedAudioChannelizer = false;
            engine_->configure(empty, source, false);
            engine_->setAudioConsumer({});
            engine_.reset();
        }
        emit statusChanged();
        return;
    }
    if (!engine_) {
        engine_ = std::make_unique<DspBranchEngine>();
        engine_->setAudioConsumer([this](const DspBranchPcmFrame &frame) {
            QMetaObject::invokeMethod(this,
                                      [this, frame]() { deliverAudio(frame); },
                                      Qt::QueuedConnection);
        });
    }
    RadioSettings source = sourceSettings_;
    // An explicitly requested remote VFO is itself the opt-in to DDC work.
    source.simplifiedAudioChannelizer = false;
    engine_->setExternalEndpoint(externalEndpoint_);
    engine_->configure(plan, source, receiverRunning_ && !plan.branches.isEmpty());
    if (!drainTimer_.isActive()) drainTimer_.start();
    emit statusChanged();
}

void RemoteVfoService::drainFrames() {
    if (!engine_) return;
    const auto frames = engine_->takeFrames();
    for (const DspBranchFrame &frame : frames) {
        const QString ownerId = fftOwners_.value(frame.id, branchOwners_.value(frame.id));
        if (ownerId.isEmpty()) continue;
        if (!frame.error.isEmpty()) {
            if (frameErrors_.value(frame.id) == frame.error) continue;
            frameErrors_.insert(frame.id, frame.error);
            QJsonObject status{{QStringLiteral("type"), QStringLiteral("remoteVfo")},
                               {QStringLiteral("action"), QStringLiteral("branchStatus")},
                               {QStringLiteral("branchId"), frame.id},
                               {QStringLiteral("error"), frame.error}};
            emit jsonFrameReady(ownerId, status);
            continue;
        }
        if (frameErrors_.remove(frame.id)) {
            emit jsonFrameReady(ownerId,
                                QJsonObject{{QStringLiteral("type"), QStringLiteral("remoteVfo")},
                                            {QStringLiteral("action"), QStringLiteral("branchStatus")},
                                            {QStringLiteral("branchId"), frame.id},
                                            {QStringLiteral("error"), QString()}});
        }
        if (frame.levels.empty()) continue;
        const Vfo *vfo = vfoForFft(frame.id);
        QJsonArray frequencies;
        QJsonArray levels;
        for (float value : frame.frequencies) frequencies.append(value);
        // The established network display contract carries FFT-native level
        // order while its frequency array is ascending. Main spectrum and
        // waterfall widgets perform the half-frame shift themselves.
        const std::size_t levelCount = frame.levels.size();
        for (std::size_t i = 0; i < levelCount; ++i) {
            const float value = frame.levels[(i + levelCount / 2U) % levelCount];
            levels.append(std::isfinite(value) ? value : -160.0f);
        }
        QJsonObject spectrum;
        spectrum[QStringLiteral("type")] = QStringLiteral("spectrum");
        spectrum[QStringLiteral("remoteVfo")] = true;
        spectrum[QStringLiteral("sessionId")] = sessions_.value(ownerId).sessionId;
        spectrum[QStringLiteral("vfoId")] = vfo ? vfo->id : QString();
        spectrum[QStringLiteral("centerFrequency")] = frame.centerHz;
        spectrum[QStringLiteral("listeningFrequency")] = frame.centerHz;
        spectrum[QStringLiteral("sampleRate")] = frame.sampleRateHz;
        spectrum[QStringLiteral("bandwidth")] = vfo ? vfo->bandwidthHz : frame.sampleRateHz;
        spectrum[QStringLiteral("modulationType")] = vfo ? vfo->modulation : MOD_NFM;
        spectrum[QStringLiteral("fftLength")] = static_cast<int>(frame.levels.size());
        spectrum[QStringLiteral("sourceFftLength")] = static_cast<int>(frame.levels.size());
        spectrum[QStringLiteral("magnitudeOrder")] = QStringLiteral("fft-native");
        spectrum[QStringLiteral("fresh")] = true;
        spectrum[QStringLiteral("minFrequency")] = frame.frequencies.empty()
                                                         ? frame.centerHz - frame.sampleRateHz * 0.5
                                                         : frame.frequencies.front();
        spectrum[QStringLiteral("maxFrequency")] = frame.frequencies.empty()
                                                         ? frame.centerHz + frame.sampleRateHz * 0.5
                                                         : frame.frequencies.back();
        spectrum[QStringLiteral("frequencies")] = frequencies;
        spectrum[QStringLiteral("magnitudes")] = levels;
        ++spectrumFrames_;
        emit jsonFrameReady(ownerId, spectrum);
    }
}

void RemoteVfoService::deliverAudio(const DspBranchPcmFrame &frame) {
    const QString ownerId = ownerForAudioSink(frame.sinkId);
    if (ownerId.isEmpty()) return;
    if (frame.pcm.isEmpty()) {
        if (frame.reset) {
            emit jsonFrameReady(ownerId,
                                QJsonObject{{QStringLiteral("type"), QStringLiteral("remoteVfo")},
                                            {QStringLiteral("action"), QStringLiteral("audioReset")}});
        }
        return;
    }
    QJsonObject header{{QStringLiteral("type"), QStringLiteral("remoteVfoAudio")},
                       {QStringLiteral("sessionId"), sessions_.value(ownerId).sessionId},
                       {QStringLiteral("sampleRate"), 48000},
                       {QStringLiteral("channels"), 1},
                       {QStringLiteral("sampleFormat"), QStringLiteral("pcm_s16le")},
                       {QStringLiteral("reset"), frame.reset}};
    for (const Vfo &vfo : sessions_.value(ownerId).vfos) {
        if (vfo.audioId == frame.sinkId) {
            header[QStringLiteral("vfoId")] = vfo.id;
            break;
        }
    }
    ++audioFrames_;
    emit binaryFrameReady(ownerId, header, frame.pcm);
}

QJsonObject RemoteVfoService::vfoJson(const Vfo &vfo) const {
    return {{QStringLiteral("vfoId"), vfo.id},
            {QStringLiteral("receiverId"), vfo.source.receiverId},
            {QStringLiteral("channelId"), vfo.source.channelId},
            {QStringLiteral("frequencyHz"), vfo.frequencyHz},
            {QStringLiteral("bandwidthHz"), vfo.bandwidthHz},
            {QStringLiteral("audioLowPassHz"), vfo.lowPassHz},
            {QStringLiteral("squelchDb"), vfo.squelchDb},
            {QStringLiteral("volume"), vfo.volume},
            {QStringLiteral("modulationType"), vfo.modulation},
            {QStringLiteral("fftLength"), vfo.fftLength},
            {QStringLiteral("refreshMs"), vfo.refreshMs},
            {QStringLiteral("audioEnabled"), vfo.audioEnabled},
            {QStringLiteral("spectrumEnabled"), vfo.spectrumEnabled},
            {QStringLiteral("active"), vfo.active},
            {QStringLiteral("error"), vfo.lastError}};
}

QJsonObject RemoteVfoService::capabilitiesJson() const {
    QJsonArray modes;
    const char *names[] = {"AM", "NFM", "SAM", "USB", "LSB", "DSB", "CW", "WFM"};
    for (int i = MOD_AM; i <= MOD_WFM; ++i) {
        modes.append(QJsonObject{{QStringLiteral("id"), i},
                                 {QStringLiteral("name"), QString::fromLatin1(names[i])}});
    }
    const double half = sourceSettings_.sampleRate > 0.0 ? sourceSettings_.sampleRate * 0.5 : 0.0;
    return {{QStringLiteral("protocol"), 1},
            {QStringLiteral("maxVfos"), MaxVfos},
            {QStringLiteral("maxVfosPerSession"), MaxVfosPerSession},
            {QStringLiteral("receiverRunning"), receiverRunning_},
            {QStringLiteral("centerFrequency"), sourceSettings_.centerFrequency},
            {QStringLiteral("sampleRate"), sourceSettings_.sampleRate},
            {QStringLiteral("minFrequency"), sourceSettings_.centerFrequency - half},
            {QStringLiteral("maxFrequency"), sourceSettings_.centerFrequency + half},
            {QStringLiteral("modes"), modes},
            {QStringLiteral("audioFormat"), QStringLiteral("pcm_s16le/48000/mono")},
            {QStringLiteral("txAllowed"), false}};
}

void RemoteVfoService::sendSessionState(const Session &session, const QString &action) {
    QJsonArray vfos;
    for (const Vfo &vfo : session.vfos) vfos.append(vfoJson(vfo));
    QJsonObject state{{QStringLiteral("type"), QStringLiteral("remoteVfo")},
                      {QStringLiteral("action"), action},
                      {QStringLiteral("sessionId"), session.sessionId},
                      {QStringLiteral("role"), session.role},
                      {QStringLiteral("label"), session.label},
                      {QStringLiteral("capabilities"), capabilitiesJson()},
                      {QStringLiteral("vfos"), vfos},
                      {QStringLiteral("server"), statusJson()}};
    emit jsonFrameReady(session.ownerId, state);
}

void RemoteVfoService::sendError(const QString &ownerId,
                                 const QString &action,
                                 const QString &message,
                                 const QString &requestId) {
    QJsonObject error{{QStringLiteral("type"), QStringLiteral("remoteVfo")},
                      {QStringLiteral("action"), QStringLiteral("error")},
                      {QStringLiteral("requestAction"), action},
                      {QStringLiteral("message"), message}};
    if (!requestId.isEmpty()) error.insert(QStringLiteral("requestId"), requestId);
    emit jsonFrameReady(ownerId, error);
}

QString RemoteVfoService::ownerForBranch(const QString &branchId) const {
    return branchOwners_.value(branchId);
}

QString RemoteVfoService::ownerForAudioSink(const QString &sinkId) const {
    return audioOwners_.value(sinkId);
}

const RemoteVfoService::Vfo *RemoteVfoService::vfoForFft(const QString &fftId) const {
    const QString ownerId = fftOwners_.value(fftId);
    const auto session = sessions_.constFind(ownerId);
    if (session == sessions_.cend()) return nullptr;
    for (const Vfo &vfo : session->vfos) if (vfo.fftId == fftId) return &vfo;
    return nullptr;
}

int RemoteVfoService::sessionCount() const {
    return sessions_.size();
}

int RemoteVfoService::activeVfoCount() const {
    int count = 0;
    for (const Session &session : sessions_)
        for (const Vfo &vfo : session.vfos) if (vfo.active) ++count;
    return count;
}

QJsonObject RemoteVfoService::statusJson() const {
    return {{QStringLiteral("sessions"), sessionCount()},
            {QStringLiteral("activeVfos"), activeVfoCount()},
            {QStringLiteral("maxVfos"), MaxVfos},
            {QStringLiteral("receiverRunning"), receiverRunning_},
            {QStringLiteral("externalCompute"), externalEndpoint_.enabled},
            {QStringLiteral("spectrumFrames"), QString::number(spectrumFrames_)},
            {QStringLiteral("audioFrames"), QString::number(audioFrames_)}};
}
