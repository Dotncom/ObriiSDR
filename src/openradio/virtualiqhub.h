#ifndef VIRTUALIQHUB_H
#define VIRTUALIQHUB_H

#include "virtualsignalframe.h"

#include <QHash>
#include <QList>
#include <QQueue>
#include <QSet>
#include <QString>

#include <random>

struct VirtualIqHubSettings {
    int maximumSubscribers = 32;
    int maximumQueuedFrames = 32;
    qint64 maximumQueuedBytes = 8 * 1024 * 1024;
    int fixedDelayMs = 0;
    int collisionWindowMs = 0;
    float noiseSigma = 0.0f;
    quint32 randomSeed = 1;
    bool defaultRoute = true;
};

struct VirtualIqLinkSettings {
    bool enabled = true;
    int delayMs = 0;
    float gainDb = 0.0f;
};

struct VirtualIqHubCounters {
    quint64 receivedFrames = 0;
    quint64 deliveredFrames = 0;
    quint64 droppedFrames = 0;
    quint64 collisionFrames = 0;
    quint64 filteredFrames = 0;
    quint64 deliveredBytes = 0;
    qint64 queuedBytes = 0;
    qint64 queuedFrames = 0;
    qint64 maximumDelayMs = 0;
    qint64 accumulatedDelayMs = 0;
};

class VirtualIqHub {
public:
    explicit VirtualIqHub(const VirtualIqHubSettings &settings = {});

    bool addSubscriber(const QString &id);
    bool setLink(const QString &sourceId, const QString &subscriberId,
                 const VirtualIqLinkSettings &settings);
    void removeSubscriber(const QString &id);
    QStringList subscriberIds() const;
    bool publish(const QString &sourceId, const VirtualSignalFrame &frame,
                 qint64 nowMs, QString *error = nullptr);
    QList<VirtualSignalFrame> takeDue(const QString &subscriberId, qint64 nowMs,
                                     int maximumFrames = 8);
    VirtualIqHubCounters counters(const QString &subscriberId) const;
    VirtualIqHubCounters totalCounters() const { return total_; }
    void reset();

private:
    struct QueuedFrame {
        VirtualSignalFrame frame;
        QString sourceId;
        qint64 arrivedMs = 0;
        qint64 dueMs = 0;
        quint64 eventId = 0;
        qint64 bytes = 0;
    };
    struct Subscriber {
        QQueue<QueuedFrame> queue;
        VirtualIqHubCounters counters;
        quint32 nextSequence = 0;
        bool discontinuity = true;
        QString lastSource;
        QString deliveredSource;
        qint64 lastArrivalMs = -1;
        qint64 collisionUntilMs = -1;
        quint64 lastEventId = 0;
        bool lastEventCollided = false;
    };

    void dropEvent(Subscriber *subscriber, quint64 eventId);
    void dropOldest(Subscriber *subscriber, bool collision);
    void refreshGainSources();

    VirtualIqHubSettings settings_;
    QHash<QString, Subscriber> subscribers_;
    QHash<QString, QHash<QString, VirtualIqLinkSettings>> links_;
    VirtualIqHubCounters total_;
    std::mt19937 random_;
    QSet<QString> gainSources_;
    quint64 nextEventId_ = 0;
};

#endif // VIRTUALIQHUB_H
