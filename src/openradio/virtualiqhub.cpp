#include "virtualiqhub.h"

#include "virtualsignaltransport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <QtEndian>

namespace {
void setError(QString *error, const QString &message) {
    if (error) *error = message;
}

QByteArray encodeIq(const QVector<float> &iq) {
    QByteArray payload(iq.size() * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    for (int index = 0; index < iq.size(); ++index) {
        quint32 bits = 0;
        std::memcpy(&bits, &iq[index], sizeof(bits));
        qToLittleEndian(bits, reinterpret_cast<uchar *>(payload.data() + index * 4));
    }
    return payload;
}
}

VirtualIqHub::VirtualIqHub(const VirtualIqHubSettings &settings)
    : settings_(settings), random_(settings.randomSeed) {
    settings_.maximumSubscribers = std::clamp(settings_.maximumSubscribers, 1, 128);
    settings_.maximumQueuedFrames = std::clamp(settings_.maximumQueuedFrames, 1, 256);
    settings_.maximumQueuedBytes = std::clamp<qint64>(settings_.maximumQueuedBytes, 1024, 64 * 1024 * 1024);
    settings_.fixedDelayMs = std::clamp(settings_.fixedDelayMs, 0, 60000);
    settings_.collisionWindowMs = std::clamp(settings_.collisionWindowMs, 0, 1000);
    if (!std::isfinite(settings_.noiseSigma) || settings_.noiseSigma < 0.0f)
        settings_.noiseSigma = 0.0f;
    settings_.noiseSigma = std::min(settings_.noiseSigma, 10.0f);
}

bool VirtualIqHub::addSubscriber(const QString &id) {
    if (id.trimmed().isEmpty() || id.size() > 128 || subscribers_.contains(id) ||
        subscribers_.size() >= settings_.maximumSubscribers) return false;
    subscribers_.insert(id, Subscriber{});
    return true;
}

bool VirtualIqHub::setLink(const QString &sourceId, const QString &subscriberId,
                           const VirtualIqLinkSettings &settings) {
    if (!canSetLink(sourceId, subscriberId, settings)) return false;
    links_[subscriberId].insert(sourceId, settings);
    refreshGainSources();
    return true;
}

bool VirtualIqHub::canSetLink(const QString &sourceId, const QString &subscriberId,
                              const VirtualIqLinkSettings &settings) const {
    if (sourceId.trimmed().isEmpty() || sourceId.size() > 128 ||
        !subscribers_.contains(subscriberId) || settings.delayMs < 0 ||
        settings.delayMs > 60000 || !std::isfinite(settings.gainDb) ||
        settings.gainDb < -120.0f || settings.gainDb > 60.0f)
        return false;
    return true;
}

void VirtualIqHub::refreshGainSources() {
    gainSources_.clear();
    for (auto receiver = links_.cbegin(); receiver != links_.cend(); ++receiver) {
        const auto &routes = receiver.value();
        for (auto link = routes.cbegin(); link != routes.cend(); ++link) {
            if (link.value().enabled && link.value().gainDb != 0.0f)
                gainSources_.insert(link.key());
        }
    }
}

void VirtualIqHub::removeSubscriber(const QString &id) {
    total_.queuedBytes -= subscribers_.value(id).counters.queuedBytes;
    total_.queuedFrames -= subscribers_.value(id).counters.queuedFrames;
    subscribers_.remove(id);
    links_.remove(id);
    refreshGainSources();
}

QStringList VirtualIqHub::subscriberIds() const {
    return subscribers_.keys();
}

void VirtualIqHub::dropOldest(Subscriber *subscriber, bool collision) {
    if (!subscriber || subscriber->queue.isEmpty()) return;
    const QueuedFrame dropped = subscriber->queue.dequeue();
    subscriber->counters.queuedBytes -= dropped.bytes;
    total_.queuedBytes -= dropped.bytes;
    --subscriber->counters.queuedFrames;
    --total_.queuedFrames;
    ++subscriber->counters.droppedFrames;
    ++total_.droppedFrames;
    if (collision) {
        ++subscriber->counters.collisionFrames;
        ++total_.collisionFrames;
    }
    subscriber->discontinuity = true;
}

void VirtualIqHub::dropEvent(Subscriber *subscriber, quint64 eventId) {
    if (!subscriber) return;
    for (int index = 0; index < subscriber->queue.size(); ++index) {
        if (subscriber->queue.at(index).eventId != eventId) continue;
        subscriber->counters.queuedBytes -= subscriber->queue.at(index).bytes;
        total_.queuedBytes -= subscriber->queue.at(index).bytes;
        --subscriber->counters.queuedFrames;
        --total_.queuedFrames;
        subscriber->queue.removeAt(index);
        ++subscriber->counters.droppedFrames;
        ++subscriber->counters.collisionFrames;
        ++total_.droppedFrames;
        ++total_.collisionFrames;
        subscriber->discontinuity = true;
        break;
    }
}

bool VirtualIqHub::publish(const QString &sourceId, const VirtualSignalFrame &frame,
                           qint64 nowMs, QString *error) {
    if (sourceId.trimmed().isEmpty() || sourceId.size() > 128 || nowMs < 0 ||
        frame.payloadType != VirtualSignalPayloadType::ComplexFloat32 ||
        frame.payload.isEmpty() || frame.payload.size() % 8 != 0 ||
        frame.payload.size() > static_cast<int>(VirtualSignalStreamParser::MaximumWireBytes) - 8192 ||
        frame.sampleRate == 0) {
        setError(error, QStringLiteral("IQ hub requires a bounded CF32 frame, source ID and sample rate"));
        return false;
    }
    QVector<float> iq;
    if (settings_.noiseSigma > 0.0f || gainSources_.contains(sourceId)) {
        QString decodeError;
        if (!virtualSignalDecodeComplexFloat32(frame.payload, &iq, &decodeError)) {
            setError(error, decodeError);
            return false;
        }
        for (const float value : iq) {
            if (!std::isfinite(value)) {
                setError(error, QStringLiteral("IQ hub cannot transform non-finite samples"));
                return false;
            }
        }
    }
    if (error) error->clear();
    ++total_.receivedFrames;
    const quint64 eventId = ++nextEventId_;
    VirtualSignalFrame delivered = frame;
    delivered.metadata.insert(QStringLiteral("hubSource"), sourceId);
    delivered.metadata.insert(QStringLiteral("hubSourceSequence"), static_cast<qint64>(frame.sequence));
    if (settings_.noiseSigma > 0.0f) {
        // Fixed 24-bit uniforms make noise reproducible across standard libraries.
        for (float &value : iq) {
            double sum = 0.0;
            for (int draw = 0; draw < 12; ++draw)
                sum += static_cast<double>(random_() >> 8) / 16777216.0;
            value += static_cast<float>((sum - 6.0) * settings_.noiseSigma);
        }
        delivered.payload = encodeIq(iq);
    }
    for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
        Subscriber &subscriber = it.value();
        ++subscriber.counters.receivedFrames;
        const auto routes = links_.constFind(it.key());
        const bool explicitRoute = routes != links_.cend() && routes.value().contains(sourceId);
        const VirtualIqLinkSettings link = explicitRoute ? routes.value().value(sourceId)
                                                        : VirtualIqLinkSettings{};
        if ((explicitRoute && !link.enabled) || (!explicitRoute && !settings_.defaultRoute)) {
            ++subscriber.counters.filteredFrames;
            ++total_.filteredFrames;
            continue;
        }
        const bool sourceChanged = !subscriber.lastSource.isEmpty() &&
                                   sourceId != subscriber.lastSource;
        const bool collision = settings_.collisionWindowMs > 0 &&
            subscriber.lastArrivalMs >= 0 && nowMs >= subscriber.lastArrivalMs &&
            (nowMs <= subscriber.collisionUntilMs ||
             (nowMs - subscriber.lastArrivalMs <= settings_.collisionWindowMs && sourceChanged));
        if (collision) {
            if (!subscriber.lastEventCollided) dropEvent(&subscriber, subscriber.lastEventId);
            subscriber.collisionUntilMs = nowMs + settings_.collisionWindowMs;
            ++subscriber.counters.droppedFrames;
            ++subscriber.counters.collisionFrames;
            ++total_.droppedFrames;
            ++total_.collisionFrames;
            subscriber.discontinuity = true;
        }
        subscriber.lastEventCollided = collision;
        subscriber.lastEventId = eventId;
        subscriber.lastArrivalMs = nowMs;
        subscriber.lastSource = sourceId;
        if (collision) continue;

        VirtualSignalFrame outbound = delivered;
        if (link.gainDb != 0.0f) {
            const float gain = std::pow(10.0f, link.gainDb / 20.0f);
            QVector<float> attenuated = iq;
            for (float &value : attenuated) value *= gain;
            outbound.payload = encodeIq(attenuated);
        }
        const qint64 frameBytes = outbound.payload.size() + 512;
        const qint64 due = nowMs + settings_.fixedDelayMs + link.delayMs +
            settings_.collisionWindowMs + (settings_.collisionWindowMs > 0 ? 1 : 0);
        if (frameBytes > settings_.maximumQueuedBytes) {
            ++subscriber.counters.droppedFrames;
            ++total_.droppedFrames;
            subscriber.discontinuity = true;
            continue;
        }
        while (!subscriber.queue.isEmpty() &&
               (subscriber.queue.size() >= settings_.maximumQueuedFrames ||
                subscriber.counters.queuedBytes + frameBytes > settings_.maximumQueuedBytes))
            dropOldest(&subscriber, false);
        QueuedFrame queued{outbound, sourceId, nowMs, due, eventId, frameBytes};
        if (subscriber.queue.isEmpty() || subscriber.queue.last().dueMs <= due) {
            subscriber.queue.enqueue(queued);
        } else {
            int index = 0;
            while (index < subscriber.queue.size() && subscriber.queue.at(index).dueMs <= due)
                ++index;
            subscriber.queue.insert(index, queued);
        }
        subscriber.counters.queuedBytes += frameBytes;
        total_.queuedBytes += frameBytes;
        ++subscriber.counters.queuedFrames;
        ++total_.queuedFrames;
    }
    return true;
}

QList<VirtualSignalFrame> VirtualIqHub::takeDue(const QString &subscriberId,
                                                qint64 nowMs, int maximumFrames) {
    QList<VirtualSignalFrame> result;
    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end() || maximumFrames <= 0) return result;
    Subscriber &subscriber = it.value();
    while (!subscriber.queue.isEmpty() && result.size() < maximumFrames &&
           subscriber.queue.head().dueMs <= nowMs) {
        QueuedFrame queued = subscriber.queue.dequeue();
        subscriber.counters.queuedBytes -= queued.bytes;
        total_.queuedBytes -= queued.bytes;
        --subscriber.counters.queuedFrames;
        --total_.queuedFrames;
        queued.frame.sequence = subscriber.nextSequence++;
        queued.frame.discontinuity |= subscriber.discontinuity ||
            (!subscriber.deliveredSource.isEmpty() &&
             queued.sourceId != subscriber.deliveredSource);
        subscriber.discontinuity = false;
        subscriber.deliveredSource = queued.sourceId;
        const qint64 delay = std::max<qint64>(0, nowMs - queued.arrivedMs);
        subscriber.counters.maximumDelayMs = std::max(subscriber.counters.maximumDelayMs, delay);
        total_.maximumDelayMs = std::max(total_.maximumDelayMs, delay);
        subscriber.counters.accumulatedDelayMs += delay;
        total_.accumulatedDelayMs += delay;
        ++subscriber.counters.deliveredFrames;
        ++total_.deliveredFrames;
        subscriber.counters.deliveredBytes += static_cast<quint64>(queued.frame.payload.size());
        total_.deliveredBytes += static_cast<quint64>(queued.frame.payload.size());
        result << queued.frame;
    }
    return result;
}

VirtualIqHubCounters VirtualIqHub::counters(const QString &subscriberId) const {
    return subscribers_.value(subscriberId).counters;
}

void VirtualIqHub::reset() {
    subscribers_.clear();
    links_.clear();
    total_ = {};
    random_.seed(settings_.randomSeed);
    nextEventId_ = 0;
    gainSources_.clear();
}
