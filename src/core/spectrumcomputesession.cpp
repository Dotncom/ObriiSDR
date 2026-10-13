#include "spectrumcomputesession.h"
#include "fft.h"
#include <QElapsedTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace SpectrumComputeProtocol {
int maxLengthForBudget(int memoryMiB) {
    return int(std::min<qint64>(MaxFftLength, qint64(std::clamp(memoryMiB, 64, 8192))*1024*1024/64));
}
bool readCounter(const QJsonValue &value, quint64 &out) {
    if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 20) return false;
    for (QChar c : value.toString()) if (c < '0' || c > '9') return false;
    bool ok = false; out = value.toString().toULongLong(&ok); return ok;
}
QByteArray floats(const float *values, int count) {
    if (count < 0 || count > ChunkSamples*2) return {};
    QByteArray bytes(count*4, Qt::Uninitialized);
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    if (count) std::memcpy(bytes.data(), values, std::size_t(count)*4);
#else
    for (int i = 0; i < count; ++i) {
        quint32 bits; std::memcpy(&bits, values+i, 4);
        qToLittleEndian(bits, reinterpret_cast<uchar *>(bytes.data()+4*i));
    }
#endif
    return bytes;
}
bool readFloats(const QByteArray &bytes, std::vector<float> &values) {
    if (bytes.isEmpty() || bytes.size()%4 || bytes.size() > ChunkSamples*8) return false;
    values.resize(std::size_t(bytes.size()/4));
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    std::memcpy(values.data(), bytes.constData(), std::size_t(bytes.size()));
#else
    for (std::size_t i = 0; i < values.size(); ++i) {
        quint32 bits = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData()+4*i));
        std::memcpy(values.data()+i, &bits, 4);
    }
#endif
    return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
}
}

struct SpectrumComputeSession::Impl {
    explicit Impl(int memoryMiB) : maxLength(SpectrumComputeProtocol::maxLengthForBudget(memoryMiB)) {}
    int maxLength;
    FFTResult fft{true};
    RadioSettings settings;
    QString job;
    bool continuous = false, configured = false;
    quint64 nextSample = 0;
    bool haveNext = false;
    std::size_t write = 0, count = 0;
    std::vector<float> history, linear, chunk, frequencies, levels;
};
SpectrumComputeSession::SpectrumComputeSession(int memoryMiB) : impl(std::make_unique<Impl>(memoryMiB)) {}
SpectrumComputeSession::~SpectrumComputeSession() = default;
bool SpectrumComputeSession::handle(const QJsonObject &q, const QByteArray &payload, const Reply &reply) {
    const auto fail = [&reply](const QString &message) {
        reply({{"type", "error"}, {"error", message}}, {}); return false;
    };
    try {
        const QString type = q.value("type").toString();
        if (type == "main_fft_begin") {
            const int length = q.value("length").toInt();
            const double rate = q.value("rate").toDouble(), center = q.value("center").toDouble(-1);
            const int window = q.value("window").toInt(-1), backend = q.value("backend").toInt(-1);
            const QString job = q.value("job").toString(), mode = q.value("mode").toString();
            const auto pipeline = q.value("pipelineChunks");
            if (!pipeline.isUndefined() && (!pipeline.isDouble() || pipeline.toDouble() < 1 ||
                pipeline.toDouble() > SpectrumComputeProtocol::MaxPipelineChunks ||
                pipeline.toDouble() != pipeline.toInt())) return fail("Invalid main FFT pipeline bound");
            if (!payload.isEmpty() || length < 512 || length > impl->maxLength ||
                !std::isfinite(rate) || rate < 1000 || rate > 1e9 || !std::isfinite(center) || center < 0 || center > 8e9 ||
                window < 0 || window > 4 || backend < 0 || backend > 2 || job.isEmpty() || job.size() > 80 ||
                (mode != "windows" && mode != "continuous")) return fail("Invalid main FFT parameters or worker memory budget exceeded");
            impl->configured = false;
            impl->settings.inputMode = INPUT_RF; impl->settings.sampleRate = rate;
            impl->settings.centerFrequency = center; impl->settings.fftLength = length;
            impl->settings.fftWindowType = window;
            impl->fft.setBackendPreference(backend);
            impl->history.resize(std::size_t(length)*2);
            impl->linear.clear(); impl->count = impl->write = 0; impl->haveNext = false;
            impl->job = job; impl->continuous = mode == "continuous"; impl->configured = true;
            QJsonObject ready{{"type", "main_fft_ready"}, {"job", job}};
            if (!pipeline.isUndefined()) ready["pipelineChunks"] = pipeline.toInt();
            return reply(ready, {});
        }
        if (!impl->configured || q.value("job") != impl->job) return fail("Stale or missing main FFT job");
        if (type == "main_fft_iq") {
            quint64 first = 0;
            if (!SpectrumComputeProtocol::readCounter(q.value("first"), first) || payload.size()%8 ||
                !SpectrumComputeProtocol::readFloats(payload, impl->chunk) ||
                first > std::numeric_limits<quint64>::max()-impl->chunk.size()/2) return fail("Invalid main FFT IQ chunk");
            const bool discontinuity = q.value("gap").toBool() || (impl->haveNext && first != impl->nextSample);
            if (discontinuity) {
                if (!impl->continuous && impl->count) return fail("IQ gap inside FFT window");
                impl->write = impl->count = 0;
            }
            if (!impl->continuous && impl->count+impl->chunk.size() > impl->history.size()) return fail("FFT window exceeded");
            if (impl->chunk.size() > impl->history.size()) {
                std::copy(impl->chunk.end()-std::ptrdiff_t(impl->history.size()), impl->chunk.end(), impl->history.begin());
                impl->write = 0; impl->count = impl->history.size();
            } else {
                const auto firstCount = std::min(impl->chunk.size(), impl->history.size()-impl->write);
                std::copy_n(impl->chunk.begin(), firstCount, impl->history.begin()+std::ptrdiff_t(impl->write));
                std::copy(impl->chunk.begin()+std::ptrdiff_t(firstCount), impl->chunk.end(), impl->history.begin());
                impl->write = (impl->write+impl->chunk.size())%impl->history.size();
                impl->count = std::min(impl->history.size(), impl->count+impl->chunk.size());
            }
            impl->nextSample = first+impl->chunk.size()/2; impl->haveNext = true;
            return reply({{"type", "main_fft_ack"}, {"job", impl->job},
                {"next", QString::number(impl->nextSample)}, {"filled", double(impl->count/2)}}, {});
        }
        if (type != "main_fft_run" || !payload.isEmpty()) return fail("Unknown main FFT command");
        if (impl->count != impl->history.size()) return fail("Main FFT needs a complete IQ window");
        const std::vector<float> *samples = &impl->history;
        if (impl->write) {
            impl->linear.resize(impl->history.size());
            const auto tail = impl->history.size()-impl->write;
            std::copy(impl->history.begin()+std::ptrdiff_t(impl->write), impl->history.end(), impl->linear.begin());
            std::copy_n(impl->history.begin(), impl->write, impl->linear.begin()+std::ptrdiff_t(tail));
            samples = &impl->linear;
        }
        QElapsedTimer timer; timer.start();
        auto &levels = impl->levels;
        if (!impl->fft.computeSnapshot(impl->settings, *samples, impl->frequencies, levels)) return fail("Main FFT computation failed");
        const double elapsedMs = timer.nsecsElapsed()/1e6;
        if (!reply({{"type", "main_fft_result"}, {"job", impl->job}, {"length", int(levels.size())},
            {"next", QString::number(impl->nextSample)}, {"dspMs", elapsedMs}}, {})) return false;
        for (int offset = 0; offset < int(levels.size());) {
            const int n = std::min(SpectrumComputeProtocol::ChunkSamples*2, int(levels.size())-offset);
            if (!reply({{"type", "main_fft_bins"}, {"job", impl->job}, {"offset", offset}},
                SpectrumComputeProtocol::floats(levels.data()+offset, n))) return false;
            offset += n;
        }
        return reply({{"type", "main_fft_done"}, {"job", impl->job}}, {});
    } catch (const std::exception &) { return fail("Main FFT resource limit; reduce FFT length or worker memory budget"); }
}
