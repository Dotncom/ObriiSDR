#pragma once

#include "dspbranchengine.h"
#include <QByteArray>
#include <QJsonObject>
#include <memory>

struct DspStreamBlock {
    QString stream;
    std::uint64_t generation = 0, epoch = 0, firstSample = 0;
    double sampleRate = 0.0, centerHz = 0.0;
    bool gap = false, returnIq = false;
    QVector<DspBranchFftConfig> ffts;
    std::vector<float> iq;
};

struct DspStreamResult {
    QString error;
    double processingMs = 0.0;
    std::shared_ptr<const std::vector<float>> iq;
    std::vector<DspBranchFrame> spectra;
};

// Stateful, bounded channel processor. Owned exclusively by its processing thread.
class DspStreamProcessor final {
public:
    DspStreamProcessor();
    ~DspStreamProcessor();
    void clear();
    DspStreamResult process(const DspStreamBlock &block);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

namespace DspStreamProtocol {
constexpr int MaxJsonBytes = 65536;
constexpr int MaxPayloadBytes = 2097152;
constexpr std::size_t MaxSamples = 65536;
QByteArray encode(const QJsonObject &json, const QByteArray &payload = {});
// 0: incomplete; 1: complete; -1: invalid (caller must close connection).
int extract(QByteArray &buffer, QJsonObject &json, QByteArray &payload, QString &error);
QString validate(const DspStreamBlock &block);
QJsonObject request(const DspStreamBlock &block);
QByteArray floats(const std::vector<float> &values);
bool decodeRequest(const QJsonObject &json, const QByteArray &payload, DspStreamBlock &block, QString &error);
QByteArray response(const DspStreamBlock &block, const DspStreamResult &result);
bool decodeResponse(const DspStreamBlock &block, const QJsonObject &json, const QByteArray &payload, DspStreamResult &result);
}
