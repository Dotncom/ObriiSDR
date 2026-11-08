#pragma once

#include "signallabgenerator.h"

#include <QJsonObject>

struct DspBoardSignalRequest {
    QJsonObject iq;
    QJsonObject modulator;
    QJsonObject carrier;
    QJsonObject payload;
    QString error;

    bool operator==(const DspBoardSignalRequest &other) const;
    QString sourceId() const;
    double centerHz() const;
};

DspBoardSignalRequest resolveDspBoardSignal(const QJsonObject &graph, const QString &sinkId);
SignalLabGenerationResult generateDspBoardSignal(const DspBoardSignalRequest &request);

enum class DspBoardPayloadType { Text = 1, Image, File };

struct DspBoardDecodedPayload {
    DspBoardPayloadType type = DspBoardPayloadType::File;
    QString fileName;
    QByteArray bytes;
    QString error;
    bool ok() const { return error.isEmpty() && !bytes.isEmpty(); }
};

DspBoardDecodedPayload decodeDspBoardPacket(const SignalLabGenerationResult &signal);

namespace DspBoardQpsk {
constexpr int HeaderBytes = 10;
constexpr int SymbolsPerByte = 4;
const QByteArray &preamble();
int bodyBytes(const QByteArray &header);
DspBoardDecodedPayload decodeFrame(const QByteArray &header, const QByteArray &body);
unsigned symbolBits(std::complex<float> sample);
}
