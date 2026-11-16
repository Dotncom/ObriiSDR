#ifndef VIRTUALIQMEDIUM_H
#define VIRTUALIQMEDIUM_H

#include "virtualiqhub.h"

#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QVector>

#include <array>
#include <optional>
#include <random>

struct VirtualIqMediumSettings {
    quint32 sampleRate = 48000;
    qint64 centerFrequencyHz = 1000000;
    int blockSamples = 1024;
    int playoutDelayMs = 100;
    int maximumBufferedBlocks = 256;
    int fixedDelayMs = 0;
    float noiseSigma = 0.0f;
    quint32 randomSeed = 1;
    bool defaultRoute = true;
};

struct VirtualIqMediumCounters {
    quint64 receivedFrames = 0;
    quint64 emittedFrames = 0;
    quint64 filteredFrames = 0;
    quint64 overlapSamples = 0;
    quint64 gapSamples = 0;
    quint64 lateSamples = 0;
    quint64 bufferDrops = 0;
    quint64 clippedComponents = 0;
    int bufferedBlocks = 0;
};

class VirtualIqMedium {
public:
    explicit VirtualIqMedium(const VirtualIqMediumSettings &settings);

    bool addSubscriber(const QString &id);
    bool canSetLink(const QString &sourceId, const QString &subscriberId,
                    const VirtualIqLinkSettings &settings) const;
    bool setLink(const QString &sourceId, const QString &subscriberId,
                 const VirtualIqLinkSettings &settings);
    bool publish(const QString &sourceId, const VirtualSignalFrame &frame,
                 qint64 nowNs, QString *error = nullptr);
    QList<VirtualSignalFrame> takeReady(const QString &subscriberId, qint64 nowNs,
                                       int maximumFrames = 8);
    VirtualIqMediumCounters counters(const QString &subscriberId) const;
    VirtualIqMediumCounters totalCounters() const;

private:
    struct ClockResampler {
        static constexpr int Taps = 16;
        std::array<float, (Taps - 1) * 2> history{};
        qint64 position = 0;
        qint64 nextInput = 0;
        qint64 nextOutput = 0;
        qint64 delay = 0;
        QString session;

        QVector<float> process(const QVector<float> &iq, double ppm);
    };
    struct Route {
        VirtualIqLinkSettings settings;
        std::optional<ClockResampler> clock;
    };
    struct Block {
        QVector<float> iq;
        QVector<quint8> occupied;
        QSet<QString> sources;
        QJsonObject sessions;
        bool discontinuity = false;
        quint64 overlapSamples = 0;
    };
    struct Subscriber {
        QMap<qint64, Block> blocks;
        qint64 nextBlock = 0;
        qint64 highestEnd = 0;
        quint32 sequence = 0;
        bool initialized = false;
        bool discontinuity = true;
        VirtualIqMediumCounters counters;
        std::mt19937 random;
    };

    qint64 sampleIndex(qint64 timestampNs) const;
    static qint64 floorBlock(qint64 sampleIndex, int blockSamples);
    void addToSubscriber(Subscriber *subscriber, const QString &sourceId,
                         const QString &sessionId, bool discontinuity,
                         const QVector<float> &iq, qint64 startIndex, float gain,
                         double frequencyOffsetHz);

    VirtualIqMediumSettings settings_;
    QHash<QString, Subscriber> subscribers_;
    QHash<QString, QHash<QString, Route>> links_;
    qint64 epochNs_ = 0;
    qint64 wallEpochNs_ = 0;
};

#endif // VIRTUALIQMEDIUM_H
