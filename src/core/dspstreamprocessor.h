#pragma once

#include "dspbranchengine.h"
#include "dspiqcodec.h"
#include <QByteArray>
#include <QJsonObject>
#include <memory>

struct DspStreamBlock {
    QString stream;
    DspSourceIdentity source;
    std::uint64_t generation = 0, epoch = 0, firstSample = 0;
    double sampleRate = 0.0, centerHz = 0.0;
    bool gap = false, returnIq = false;
    DspIqFormat wireFormat = DspIqFormat::Cf32Le;
    QVector<DspBranchFftConfig> ffts;
    std::vector<float> iq;
};

struct DspStreamResult {
    QString error;
    double processingMs = 0.0;
    // Processed IQ domain. Native/legacy replies retain the input clock.
    double sampleRate = 0.0;
    std::uint64_t firstSample = 0, originFirstSample = 0;
    std::size_t sampleCount = 0;
    int rateNumerator = 1, rateDenominator = 1;
    int inputReserve = 0;
    bool gap = false;
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
constexpr std::size_t MaxOutputSamples = 131072;
constexpr std::size_t MaxStreams = 8;
constexpr int SelectedBandVersion = 1;
QByteArray encode(const QJsonObject &json, const QByteArray &payload = {});
// 0: incomplete; 1: complete; -1: invalid (caller must close connection).
int extract(QByteArray &buffer, QJsonObject &json, QByteArray &payload, QString &error);
QString validate(const DspStreamBlock &block);
QJsonObject request(const DspStreamBlock &block);
QByteArray floats(const std::vector<float> &values);
bool decodeRequest(const QJsonObject &json, const QByteArray &payload, DspStreamBlock &block, QString &error);
QString selectedBandKey(const DspStreamBlock &block);
QJsonObject selectedBandOpen(const DspStreamBlock &block);
bool decodeSelectedBandOpen(const QJsonObject &json, DspStreamBlock &block, QString &error);
bool selectedBandMatches(const DspStreamBlock &configured, const DspStreamBlock &block);
QByteArray response(const DspStreamBlock &block, const DspStreamResult &result);
bool decodeResponse(const DspStreamBlock &block, const QJsonObject &json, const QByteArray &payload, DspStreamResult &result);
}
