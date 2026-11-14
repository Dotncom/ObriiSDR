#include "virtualiqmedium.h"

#include "virtualsignaltransport.h"

#include <QJsonArray>
#include <QSysInfo>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr double Tau = 6.2831853071795864769;
constexpr qint64 SampleUnits = 10000000000LL;
constexpr int FractionalPhases = 1024;
void setError(QString *error, const QString &message) {
    if (error) *error = message;
}

qint64 roundedSamples(qint64 nanoseconds, quint32 sampleRate) {
    return static_cast<qint64>(std::llround(static_cast<long double>(nanoseconds) *
                                           sampleRate / 1000000000.0L));
}
}

QVector<float> VirtualIqMedium::ClockResampler::process(const QVector<float> &iq, double ppm) {
    // Causal fractional-delay FIR: at each output position only the current
    // input and the 15 preceding samples are needed, even across a frame split.
    static const auto coefficients = [] {
        std::array<std::array<float, Taps>, FractionalPhases + 1> table{};
        constexpr double cutoff = 0.45;
        for (int phase = 0; phase <= FractionalPhases; ++phase) {
            const double fraction = double(phase) / FractionalPhases;
            double sum = 0;
            for (int tap = 0; tap < Taps; ++tap) {
                const double distance = tap - (Taps / 2 - 1) - fraction;
                const double angle = Tau * cutoff * distance;
                const double sinc = angle == 0 ? 1 : std::sin(angle) / angle;
                const double window = Tau * tap / (Taps - 1);
                const double coefficient = sinc * (0.42 - 0.5 * std::cos(window) + 0.08 * std::cos(2 * window));
                table[phase][tap] = float(coefficient);
                sum += coefficient;
            }
            for (float &coefficient : table[phase]) coefficient = float(coefficient / sum);
        }
        return table;
    }();

    const qint64 step = SampleUnits + std::llround(ppm * (SampleUnits / 1000000));
    const qint64 end = qint64(iq.size() / 2) * SampleUnits;
    const qint64 count = (end - position + step - 1) / step;
    QVector<float> output(int(count) * 2);
    const float *input = iq.constData();
    for (int dest = 0; dest < output.size(); dest += 2, position += step) {
        const int last = int(position / SampleUnits);
        const int first = last - (Taps - 1);
        const int phase = int(((position % SampleUnits) * FractionalPhases + SampleUnits / 2) / SampleUnits);
        const auto &weights = coefficients[phase];
        float re = 0, im = 0;
        if (first >= 0) {
            const auto *window = input + first * 2;
            for (int tap = 0; tap < Taps; ++tap) {
                re += window[tap * 2] * weights[tap];
                im += window[tap * 2 + 1] * weights[tap];
            }
        } else {
            for (int tap = 0; tap < Taps; ++tap) {
                const int index = first + tap;
                const float *sample = index < 0 ? history.data() + (Taps - 1 + index) * 2 : input + index * 2;
                re += sample[0] * weights[tap];
                im += sample[1] * weights[tap];
            }
        }
        output[dest] = re;
        output[dest + 1] = im;
    }
    position -= end;
    const int retained = (std::min)(int(history.size()), iq.size());
    std::copy(history.begin() + retained, history.end(), history.begin());
    std::copy_n(input + iq.size() - retained, retained, history.end() - retained);
    return output;
}

VirtualIqMedium::VirtualIqMedium(const VirtualIqMediumSettings &settings)
    : settings_(settings) {}

bool VirtualIqMedium::addSubscriber(const QString &id) {
    if (id.isEmpty() || subscribers_.contains(id) || subscribers_.size() >= 32) return false;
    Subscriber subscriber;
    subscriber.random.seed(settings_.randomSeed + static_cast<quint32>(subscribers_.size()));
    subscribers_.insert(id, subscriber);
    return true;
}

bool VirtualIqMedium::setLink(const QString &sourceId, const QString &subscriberId,
                              const VirtualIqLinkSettings &settings) {
    if (!canSetLink(sourceId, subscriberId, settings)) return false;
    auto &route = links_[sourceId][subscriberId];
    route.settings = settings;
    if (!settings.enabled) route.clock.reset();
    return true;
}

bool VirtualIqMedium::canSetLink(const QString &sourceId, const QString &subscriberId,
                                 const VirtualIqLinkSettings &settings) const {
    const qint64 bufferedSamples = static_cast<qint64>(settings_.maximumBufferedBlocks) *
        settings_.blockSamples;
    const qint64 waitMs = static_cast<qint64>(settings_.playoutDelayMs) +
        settings_.fixedDelayMs + settings.delayMs;
    const qint64 requiredSamples = static_cast<qint64>(std::ceil(
        static_cast<long double>(waitMs) * settings_.sampleRate / 1000.0L)) +
        settings_.blockSamples;
    if (sourceId.isEmpty() || !subscribers_.contains(subscriberId) ||
        settings.delayMs < 0 || settings.delayMs > 60000 ||
        !std::isfinite(settings.gainDb) || settings.gainDb < -120.0f ||
        settings.gainDb > 60.0f ||
        !std::isfinite(settings.frequencyOffsetHz) || std::abs(settings.frequencyOffsetHz) >= 0.5 * settings_.sampleRate ||
        !std::isfinite(settings.clockPpm) || std::abs(settings.clockPpm) > VirtualIqLinkSettings::MaximumClockPpm ||
        (settings.enabled && requiredSamples > bufferedSamples)) return false;
    return true;
}

qint64 VirtualIqMedium::sampleIndex(qint64 timestampNs) const {
    return roundedSamples(timestampNs - epochNs_, settings_.sampleRate);
}

qint64 VirtualIqMedium::floorBlock(qint64 index, int blockSamples) {
    const qint64 quotient = index / blockSamples;
    return index < 0 && index % blockSamples ? quotient - 1 : quotient;
}

bool VirtualIqMedium::publish(const QString &sourceId, const VirtualSignalFrame &frame,
                              qint64 nowNs, QString *error) {
    if (frame.payloadType != VirtualSignalPayloadType::ComplexFloat32 ||
        frame.sampleRate != settings_.sampleRate ||
        frame.centerFrequencyHz != settings_.centerFrequencyHz ||
        frame.timestampNs <= 0 || frame.payload.isEmpty()) {
        setError(error, QStringLiteral("Mix mode requires timestamped CF32 IQ at configured sample rate and center frequency"));
        return false;
    }
    QVector<float> iq;
    if (!virtualSignalDecodeComplexFloat32(frame.payload, &iq, error)) return false;
    for (float value : iq) {
        if (!std::isfinite(value)) {
            setError(error, QStringLiteral("Mix mode rejects non-finite IQ"));
            return false;
        }
    }
    if (!epochNs_) {
        epochNs_ = frame.timestampNs;
        wallEpochNs_ = nowNs;
    }
    const auto elapsed = static_cast<long double>(nowNs) - wallEpochNs_;
    if (std::fabs(static_cast<long double>(frame.timestampNs - epochNs_) - elapsed) > 60000000000.0L) {
        setError(error, QStringLiteral("IQ timestamp differs from the running medium clock by over 60 seconds; synchronize TX clocks"));
        return false;
    }
    const QString sessionId = frame.metadata.value(QStringLiteral("hubSessionId")).toString();
    const qint64 inputIndex = sampleIndex(frame.timestampNs);
    auto routes = links_.find(sourceId);
    for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
        Subscriber &subscriber = it.value();
        ++subscriber.counters.receivedFrames;
        Route *route = nullptr;
        if (routes != links_.end()) {
            auto found = routes->find(it.key());
            if (found != routes->end()) route = &found.value();
        }
        if (route ? !route->settings.enabled : !settings_.defaultRoute) {
            ++subscriber.counters.filteredFrames;
            continue;
        }
        const VirtualIqLinkSettings link = route ? route->settings : VirtualIqLinkSettings{};
        const float gain = route ? std::pow(10.0f, link.gainDb / 20.0f) : 1.0f;
        const qint64 delay = roundedSamples(qint64(settings_.fixedDelayMs) * 1000000LL, settings_.sampleRate) +
            roundedSamples(qint64(link.delayMs) * 1000000LL, settings_.sampleRate);
        const qint64 start = inputIndex + delay;
        bool discontinuity = frame.discontinuity;
        if (route && route->clock && (frame.discontinuity || route->clock->session != sessionId ||
                                      route->clock->nextInput != inputIndex)) {
            route->clock.reset();
            discontinuity = true;
        }
        if (route && (route->clock || link.clockPpm != 0.0)) {
            if (!route->clock) {
                auto &clock = route->clock.emplace();
                clock.nextOutput = start;
                clock.delay = delay;
                clock.session = sessionId;
                discontinuity = true;
            }
            auto &clock = *route->clock;
            const qint64 outputStart = clock.nextOutput + delay - clock.delay;
            const auto samples = clock.process(iq, link.clockPpm);
            clock.nextInput = inputIndex + iq.size() / 2;
            clock.nextOutput = outputStart + samples.size() / 2;
            clock.delay = delay;
            if (!samples.isEmpty())
                addToSubscriber(&subscriber, sourceId, sessionId, discontinuity, samples, outputStart, gain, link.frequencyOffsetHz);
        } else {
            addToSubscriber(&subscriber, sourceId, sessionId, discontinuity, iq, start, gain, link.frequencyOffsetHz);
        }
    }
    if (error) error->clear();
    return true;
}

void VirtualIqMedium::addToSubscriber(Subscriber *subscriber, const QString &sourceId,
                                      const QString &sessionId, bool discontinuity,
                                      const QVector<float> &iq, qint64 startIndex, float gain,
                                      double frequencyOffsetHz) {
    const qint64 count = iq.size() / 2;
    const qint64 endIndex = startIndex + count;
    if (!subscriber->initialized) {
        subscriber->nextBlock = floorBlock(startIndex, settings_.blockSamples);
        subscriber->initialized = true;
    }
    const qint64 spanLimit = static_cast<qint64>(settings_.maximumBufferedBlocks) *
        settings_.blockSamples;
    if (startIndex - subscriber->nextBlock * settings_.blockSamples > spanLimit) {
        const qint64 newBlock = floorBlock(startIndex, settings_.blockSamples);
        subscriber->counters.bufferDrops += static_cast<quint64>(
            (newBlock - subscriber->nextBlock) * settings_.blockSamples);
        subscriber->blocks.clear();
        subscriber->nextBlock = newBlock;
        subscriber->discontinuity = true;
    }
    subscriber->highestEnd = (std::max)(subscriber->highestEnd, endIndex);
    const qint64 firstAllowed = subscriber->nextBlock * settings_.blockSamples;
    if (startIndex < firstAllowed) {
        subscriber->counters.lateSamples += static_cast<quint64>((std::min)(endIndex, firstAllowed) - startIndex);
        subscriber->discontinuity = true;
    }
    const bool shift = frequencyOffsetHz != 0.0;
    const double anglePerSample = Tau * frequencyOffsetHz / settings_.sampleRate;
    const double stepRe = shift ? std::cos(anglePerSample) : 1.0;
    const double stepIm = shift ? std::sin(anglePerSample) : 0.0;
    for (qint64 index = (std::max)(startIndex, firstAllowed); index < endIndex;) {
        const qint64 blockIndex = floorBlock(index, settings_.blockSamples);
        if (!subscriber->blocks.contains(blockIndex) &&
            subscriber->blocks.size() >= settings_.maximumBufferedBlocks) {
            auto oldest = subscriber->blocks.begin();
            subscriber->counters.bufferDrops += settings_.blockSamples;
            if (oldest.key() >= subscriber->nextBlock)
                subscriber->nextBlock = oldest.key() + 1;
            subscriber->blocks.erase(oldest);
            subscriber->discontinuity = true;
        }
        Block &block = subscriber->blocks[blockIndex];
        if (block.iq.isEmpty()) {
            block.iq.fill(0.0f, settings_.blockSamples * 2);
            block.occupied.fill(0, settings_.blockSamples);
        }
        block.sources.insert(sourceId);
        if (!sessionId.isEmpty()) block.sessions.insert(sourceId, sessionId);
        block.discontinuity |= discontinuity;
        discontinuity = false;
        const qint64 blockStart = blockIndex * settings_.blockSamples;
        const qint64 until = (std::min)(endIndex, blockStart + settings_.blockSamples);
        // Derive phase from the shared sample timeline so frame boundaries do not reset it.
        const double phase = shift ? std::remainder(index * anglePerSample, Tau) : 0.0;
        double oscillatorRe = shift ? std::cos(phase) * gain : gain;
        double oscillatorIm = shift ? std::sin(phase) * gain : 0.0;
        for (; index < until; ++index) {
            const int dest = static_cast<int>(index - blockStart);
            const int src = static_cast<int>(index - startIndex);
            if (block.occupied[dest]) {
                ++block.overlapSamples;
                ++subscriber->counters.overlapSamples;
            }
            block.occupied[dest] = 1;
            const float re = iq[src * 2];
            const float im = iq[src * 2 + 1];
            if (shift) {
                block.iq[dest * 2] += static_cast<float>(re * oscillatorRe - im * oscillatorIm);
                block.iq[dest * 2 + 1] += static_cast<float>(re * oscillatorIm + im * oscillatorRe);
                const double nextRe = oscillatorRe * stepRe - oscillatorIm * stepIm;
                oscillatorIm = oscillatorRe * stepIm + oscillatorIm * stepRe;
                oscillatorRe = nextRe;
            } else {
                block.iq[dest * 2] += re * gain;
                block.iq[dest * 2 + 1] += im * gain;
            }
        }
    }
    subscriber->counters.bufferedBlocks = subscriber->blocks.size();
}

QList<VirtualSignalFrame> VirtualIqMedium::takeReady(const QString &subscriberId,
                                                     qint64 nowNs, int maximumFrames) {
    QList<VirtualSignalFrame> ready;
    auto found = subscribers_.find(subscriberId);
    if (found == subscribers_.end() || maximumFrames <= 0) return ready;
    Subscriber &subscriber = found.value();
    if (!subscriber.initialized) return ready;
    std::normal_distribution<float> noise(0.0f, settings_.noiseSigma);
    while (ready.size() < maximumFrames &&
           subscriber.nextBlock * settings_.blockSamples < subscriber.highestEnd) {
        const qint64 blockEnd = (subscriber.nextBlock + 1) * settings_.blockSamples;
        const qint64 dueNs = wallEpochNs_ + static_cast<qint64>(
            std::llround(static_cast<long double>(blockEnd) * 1000000000.0L / settings_.sampleRate)) +
            static_cast<qint64>(settings_.playoutDelayMs) * 1000000LL;
        if (nowNs < dueNs) break;
        Block block = subscriber.blocks.take(subscriber.nextBlock);
        if (block.iq.isEmpty()) {
            block.iq.fill(0.0f, settings_.blockSamples * 2);
            block.occupied.fill(0, settings_.blockSamples);
        }
        int gaps = 0;
        for (quint8 occupied : block.occupied) if (!occupied) ++gaps;
        subscriber.counters.gapSamples += gaps;
        VirtualSignalFrame frame;
        frame.sequence = subscriber.sequence++;
        frame.timestampNs = epochNs_ + static_cast<qint64>(
            std::llround(static_cast<long double>(subscriber.nextBlock) *
                         settings_.blockSamples * 1000000000.0L / settings_.sampleRate));
        frame.streamId = QStringLiteral("obrii-iq-medium-%1").arg(subscriberId);
        frame.protocolId = QStringLiteral("raw-iq");
        frame.payloadType = VirtualSignalPayloadType::ComplexFloat32;
        frame.sampleRate = settings_.sampleRate;
        frame.centerFrequencyHz = settings_.centerFrequencyHz;
        frame.discontinuity = subscriber.discontinuity || block.discontinuity || gaps > 0;
        frame.metadata.insert(QStringLiteral("hubMedium"), QStringLiteral("sample-mix-v1"));
        frame.metadata.insert(QStringLiteral("hubGapSamples"), gaps);
        frame.metadata.insert(QStringLiteral("hubOverlapSamples"), static_cast<double>(block.overlapSamples));
        frame.metadata.insert(QStringLiteral("hubSampleIndex"), static_cast<double>(subscriber.nextBlock * settings_.blockSamples));
        QJsonArray sources;
        QStringList sortedSources = block.sources.values();
        sortedSources.sort();
        for (const QString &source : sortedSources) sources.append(source);
        frame.metadata.insert(QStringLiteral("hubSources"), sources);
        frame.metadata.insert(QStringLiteral("hubSessions"), block.sessions);
        for (float &component : block.iq) {
            if (settings_.noiseSigma > 0.0f) component += noise(subscriber.random);
            if (component > 1.0f || component < -1.0f) {
                component = (std::max)(-1.0f, (std::min)(1.0f, component));
                ++subscriber.counters.clippedComponents;
            }
        }
        frame.payload.resize(block.iq.size() * static_cast<int>(sizeof(float)));
        if (QSysInfo::ByteOrder == QSysInfo::LittleEndian) {
            std::memcpy(frame.payload.data(), block.iq.constData(),
                        static_cast<size_t>(frame.payload.size()));
        } else {
            auto *bytes = reinterpret_cast<uchar *>(frame.payload.data());
            for (int index = 0; index < block.iq.size(); ++index) {
                quint32 bits;
                std::memcpy(&bits, block.iq.constData() + index, sizeof(bits));
                qToLittleEndian<quint32>(bits, bytes + index * sizeof(bits));
            }
        }
        ready.append(frame);
        ++subscriber.counters.emittedFrames;
        ++subscriber.nextBlock;
        subscriber.discontinuity = false;
    }
    subscriber.counters.bufferedBlocks = subscriber.blocks.size();
    return ready;
}

VirtualIqMediumCounters VirtualIqMedium::counters(const QString &subscriberId) const {
    auto found = subscribers_.constFind(subscriberId);
    return found == subscribers_.cend() ? VirtualIqMediumCounters{} : found->counters;
}

VirtualIqMediumCounters VirtualIqMedium::totalCounters() const {
    VirtualIqMediumCounters total;
    for (const Subscriber &subscriber : subscribers_) {
        const auto &c = subscriber.counters;
        total.receivedFrames += c.receivedFrames;
        total.emittedFrames += c.emittedFrames;
        total.filteredFrames += c.filteredFrames;
        total.overlapSamples += c.overlapSamples;
        total.gapSamples += c.gapSamples;
        total.lateSamples += c.lateSamples;
        total.bufferDrops += c.bufferDrops;
        total.clippedComponents += c.clippedComponents;
        total.bufferedBlocks += c.bufferedBlocks;
    }
    return total;
}
