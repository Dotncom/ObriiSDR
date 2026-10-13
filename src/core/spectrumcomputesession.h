#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <functional>
#include <memory>
#include <vector>

namespace SpectrumComputeProtocol {
constexpr int MaxFftLength = 67108864;
constexpr int ChunkSamples = 131072;
int maxLengthForBudget(int memoryMiB);
bool readCounter(const QJsonValue &value, quint64 &out);
QByteArray floats(const float *values, int count);
bool readFloats(const QByteArray &bytes, std::vector<float> &values);
}

// One authenticated main-spectrum session, independent of any receiver device.
class SpectrumComputeSession final {
public:
    using Reply = std::function<bool(const QJsonObject &, const QByteArray &)>;
    explicit SpectrumComputeSession(int memoryMiB);
    ~SpectrumComputeSession();
    bool handle(const QJsonObject &request, const QByteArray &payload, const Reply &reply);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
