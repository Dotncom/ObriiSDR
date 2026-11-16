#pragma once

#include "dspboardsignal.h"

#include <QHash>
#include <memory>

// Consumes contiguous interleaved complex samples at a channelized rate.
class DspBoardQpskReceiver {
public:
    DspBoardQpskReceiver();
    ~DspBoardQpskReceiver();
    void reset();
    static bool supports(double sampleRate, double symbolRate);
    QVector<DspBoardDecodedPayload> feed(const float *iq, std::size_t complexCount,
                                        double sampleRate, double symbolRate);

private:
    struct State;
    std::unique_ptr<State> state_;
};

struct DspBoardPacketRoute {
    QString decoderId;
    QString branchId;
    double symbolRate = 1200;
    bool operator==(const DspBoardPacketRoute &other) const {
        return decoderId == other.decoderId && branchId == other.branchId && symbolRate == other.symbolRate;
    }
};

class DspBoardPacketWorker {
public:
    DspBoardPacketWorker();
    ~DspBoardPacketWorker();
    void configure(const QVector<DspBoardPacketRoute> &routes);
    QHash<QString, DspBoardDecodedPayload> takePackets();

private:
    struct State;
    std::unique_ptr<State> state_;
};
