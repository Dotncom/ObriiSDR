#include "kissvirtualtx.h"

#include "ax25afsk1200.h"
#include "ax25codec.h"
#include "transmit/transmitwaveformgenerator.h"

namespace {
constexpr int MaximumPendingFrames = 16;
constexpr int MaximumInfoBytes = 256;
constexpr int MaximumAx25Bytes = 7 * (2 + 8) + 2 + MaximumInfoBytes;
constexpr int ChunkMs = 20;
}

KissVirtualTx::KissVirtualTx(QObject *parent) : QObject(parent) {
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(ChunkMs);
    connect(&timer, &QTimer::timeout, this, &KissVirtualTx::pump);
}

KissVirtualTx::~KissVirtualTx() { disable(); }

void KissVirtualTx::enable(const QString &channel, double centerHz) {
    disable();
    VirtualSignalEndpoint endpoint;
    endpoint.localName = channel;
    transmitter.setEndpoint(endpoint);
    configuration.frequencyHz = centerHz;
    configuration.deviationHz = 3000;
    enabled = true;
}

void KissVirtualTx::disable() {
    enabled = false;
    timer.stop();
    pending.clear();
    burst.clear();
    transmitter.stop();
}

bool KissVirtualTx::enqueue(const KissFrame &frame, QString *error) {
    QString reason;
    Ax25UiFrame decoded;
    if (!enabled) {
        reason = QStringLiteral("Virtual KISS TX is disabled");
    } else if (frame.port != 0 || frame.command != 0) {
        reason = QStringLiteral("Virtual KISS TX accepts data frames on port 0");
    } else if (frame.payload.size() > MaximumAx25Bytes ||
               !Ax25Codec::decodeUiFrame(frame.payload, &decoded, false) ||
               (decoded.control & ~0x10) != 0x03 || decoded.information.size() > MaximumInfoBytes) {
        reason = QStringLiteral("Expected an AX.25 UI frame without FCS, up to 256 information bytes");
    } else if (pending.size() >= MaximumPendingFrames) {
        reason = QStringLiteral("Virtual KISS TX queue is full");
    } else {
        pending.enqueue(frame.payload);
        if (!timer.isActive()) timer.start();
    }
    if (error) *error = reason;
    return reason.isEmpty();
}

void KissVirtualTx::abortBurst(const QString &error) {
    disable();
    emit failed(error);
}

void KissVirtualTx::pump() {
    if (!burst.isEmpty() && offset == burst.size()) {
        burst.clear();
        emit sent(frameBytes);
    }
    if (burst.isEmpty()) {
        if (pending.isEmpty()) {
            timer.stop();
            return;
        }
        QString error;
        if ((!transmitter.isRunning() && !transmitter.start(configuration, &error)) ||
            !transmitter.pollConnection()) {
            abortBurst(error.isEmpty() ? QStringLiteral("Virtual IQ receiver is unavailable") : error);
            return;
        }
        const QByteArray frame = pending.dequeue();
        frameBytes = frame.size();
        const auto audio = Ax25Afsk1200::modulate(frame, configuration.sampleRate, 32, &error);
        burst = TransmitWaveformGenerator::modulateAudio(configuration, audio);
        if (burst.isEmpty()) {
            abortBurst(error);
            return;
        }
        transmitter.setOutputEnabled(false, nullptr);
        transmitter.setOutputEnabled(true, nullptr);
        offset = 0;
        burstTime.restart();
    }
    const int chunkSamples = configuration.sampleRate * ChunkMs / 1000;
    if (burstTime.elapsed() > 1000LL * offset / configuration.sampleRate + 250) {
        abortBurst(QStringLiteral("Virtual KISS TX could not keep up with the audio clock"));
        return;
    }
    QString error;
    const int count = qMin(chunkSamples, burst.size() - offset);
    if (!transmitter.writeIq(burst.constData() + offset, count, &error)) {
        abortBurst(error);
        return;
    }
    offset += count;
}
