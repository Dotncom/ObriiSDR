#include "dspstreamprocessor.h"
#include "fftplannermutex.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <fftw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>

namespace {
using Clock = std::chrono::steady_clock;
double coefficient(int i, int n, int window) {
    const double p = 6.28318530717958647692 * i / (n - 1);
    switch (window) {
    case FFT_WINDOW_HANN: return 0.5 - 0.5 * std::cos(p);
    case FFT_WINDOW_HAMMING: return 0.54 - 0.46 * std::cos(p);
    case FFT_WINDOW_BLACKMAN_HARRIS: return 0.35875 - 0.48829*std::cos(p) + 0.14128*std::cos(2*p) - 0.01168*std::cos(3*p);
    case FFT_WINDOW_FLAT_TOP: return 0.21557895 - 0.41663158*std::cos(p) + 0.277263158*std::cos(2*p) - 0.083578947*std::cos(3*p) + 0.006947368*std::cos(4*p);
    default: return 1.0;
    }
}
struct Fft {
    DspBranchFftConfig config;
    fftwf_complex *input = nullptr, *output = nullptr;
    fftwf_plan plan = nullptr;
    std::vector<float> window;
    double sum = 0.0, sumSquares = 0.0;
    Clock::time_point last{};
    explicit Fft(const DspBranchFftConfig &f) : config(f) {
        window.reserve(f.length);
        for (int i = 0; i < f.length; ++i) {
            const float w = static_cast<float>(coefficient(i, f.length, f.window));
            window.push_back(w); sum += w; sumSquares += double(w)*w;
        }
        input = fftwf_alloc_complex(f.length); output = fftwf_alloc_complex(f.length);
        if (input && output) {
            std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
            plan = fftwf_plan_dft_1d(f.length, input, output, FFTW_FORWARD, FFTW_ESTIMATE);
        }
    }
    ~Fft() {
        if (plan) { std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex()); fftwf_destroy_plan(plan); }
        if (input) fftwf_free(input);
        if (output) fftwf_free(output);
    }
    bool compute(const DspStreamBlock &block, const std::vector<float> &history, std::size_t write,
                 std::size_t count, DspBranchFrame &frame) {
        if (!plan) throw std::runtime_error("Cannot allocate branch FFT");
        if (count < std::size_t(config.length)*2U) return false;
        const auto now = Clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now-last).count() < config.refreshMs) return false;
        last = now;
        const std::size_t start = (write+history.size()-std::size_t(config.length)*2U)%history.size();
        for (int i = 0; i < config.length; ++i) {
            const auto index = (start+2U*i)&(history.size()-1U);
            input[i][0] = history[index]*window[i]; input[i][1] = history[index+1U]*window[i];
        }
        fftwf_execute(plan);
        frame.id = config.id; frame.centerHz = block.centerHz; frame.sampleRateHz = block.sampleRate;
        frame.generation = block.generation; frame.sequence = block.firstSample+block.iq.size()/2U;
        frame.binWidthHz = block.sampleRate/config.length;
        frame.rbwHz = block.sampleRate*sumSquares/(sum*sum);
        frame.levels.resize(config.length); frame.frequencies.resize(config.length);
        for (int i = 0; i < config.length; ++i) {
            const int j = (i+config.length/2)%config.length;
            const double amplitude = std::hypot(double(output[j][0]), double(output[j][1]))/sum;
            if (!std::isfinite(amplitude)) throw std::runtime_error("FFT numeric range exceeded");
            frame.levels[i] = float(20.0*std::log10(std::max(amplitude, 1.0e-10)));
            frame.frequencies[i] = float(block.centerHz+(i-config.length/2)*frame.binWidthHz);
        }
        return true;
    }
};
struct State {
    std::uint64_t generation = 0, epoch = 0, next = 0;
    double rate = 0.0, center = 0.0;
    QVector<DspBranchFftConfig> configs;
    std::vector<float> history;
    std::size_t write = 0, count = 0;
    std::vector<std::unique_ptr<Fft>> ffts;
};
bool sameFfts(const QVector<DspBranchFftConfig> &a, const QVector<DspBranchFftConfig> &b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].length != b[i].length || a[i].window != b[i].window || a[i].refreshMs != b[i].refreshMs) return false;
    return true;
}
bool idValid(const QString &id) { return !id.isEmpty() && id.size() <= 128; }
bool u64(const QJsonValue &value, std::uint64_t &out) {
    if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 20) return false;
    for (QChar c : value.toString()) if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
    bool ok = false; out = value.toString().toULongLong(&ok); return ok;
}
bool readFloats(const QByteArray &data, int offset, int count, std::vector<float> &out) {
    if (offset < 0 || count < 0 || offset > data.size() || count > (data.size()-offset)/4) return false;
    out.resize(std::size_t(count));
    for (int i = 0; i < count; ++i) {
        const quint32 bits = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data.constData()+offset+4*i));
        std::memcpy(&out[std::size_t(i)], &bits, 4);
        if (!std::isfinite(out[std::size_t(i)])) return false;
    }
    return true;
}
}

struct DspStreamProcessor::Impl { std::map<QString, std::unique_ptr<State>> states; };
DspStreamProcessor::DspStreamProcessor() : impl(std::make_unique<Impl>()) {}
DspStreamProcessor::~DspStreamProcessor() = default;
void DspStreamProcessor::clear() { impl->states.clear(); }
DspStreamResult DspStreamProcessor::process(const DspStreamBlock &block) {
    DspStreamResult result;
    result.error = DspStreamProtocol::validate(block);
    if (!result.error.isEmpty()) return result;
    const auto started = Clock::now();
    try {
        if (!impl->states.count(block.stream) && impl->states.size() >= 2) {
            result.error = "DSP stream count exceeds limit";
            return result;
        }
        auto &state = impl->states[block.stream];
        if (!state || block.gap || state->generation != block.generation || state->epoch != block.epoch ||
            state->next != block.firstSample || state->rate != block.sampleRate || state->center != block.centerHz || !sameFfts(state->configs, block.ffts)) {
            state = std::make_unique<State>();
            state->generation = block.generation; state->epoch = block.epoch;
            state->rate = block.sampleRate; state->center = block.centerHz; state->configs = block.ffts;
            if (!block.ffts.isEmpty()) state->history.resize(DspStreamProtocol::MaxSamples*2U);
            for (const auto &f : block.ffts) state->ffts.push_back(std::make_unique<Fft>(f));
        }
        state->next = block.firstSample+block.iq.size()/2U;
        if (!state->history.empty()) {
            const auto first = std::min(block.iq.size(), state->history.size()-state->write);
            std::copy_n(block.iq.begin(), first, state->history.begin()+std::ptrdiff_t(state->write));
            std::copy(block.iq.begin()+std::ptrdiff_t(first), block.iq.end(), state->history.begin());
            state->write = (state->write+block.iq.size())%state->history.size();
            state->count = std::min(state->history.size(), state->count+block.iq.size());
        }
        if (block.returnIq) result.iq = std::make_shared<const std::vector<float>>(block.iq);
        for (auto &fft : state->ffts) {
            DspBranchFrame frame;
            if (fft->compute(block, state->history, state->write, state->count, frame)) result.spectra.push_back(std::move(frame));
        }
    } catch (const std::exception &) { impl->states.erase(block.stream); result.error = "DSP stream resource limit"; }
    result.processingMs = std::chrono::duration<double, std::milli>(Clock::now()-started).count();
    return result;
}

namespace DspStreamProtocol {
QString validate(const DspStreamBlock &b) {
    if (!idValid(b.stream) || b.iq.empty() || b.iq.size()%2 || b.iq.size()/2U > MaxSamples ||
        !std::isfinite(b.sampleRate) || b.sampleRate < 1000 || b.sampleRate > 2000000 ||
        !std::isfinite(b.centerHz) || b.centerHz < 0 || b.centerHz > 8e9 || b.ffts.size() > 2 ||
        b.firstSample > UINT64_MAX-b.iq.size()/2U) return "Invalid stream metadata or block size";
    QStringList ids;
    for (const auto &f : b.ffts) {
        if (!idValid(f.id) || ids.contains(f.id) || f.length < 512 || f.length > 65536 || (f.length & (f.length-1)) ||
            f.window < 0 || f.window > 4 || f.refreshMs < 20 || f.refreshMs > 2000) return "Invalid stream FFT configuration";
        ids.append(f.id);
    }
    for (float x : b.iq) if (!std::isfinite(x)) return "Non-finite IQ samples";
    return {};
}
QByteArray encode(const QJsonObject &json, const QByteArray &payload) {
    const QByteArray header = QJsonDocument(json).toJson(QJsonDocument::Compact);
    if (header.isEmpty() || header.size() > MaxJsonBytes || payload.size() > MaxPayloadBytes) return {};
    QByteArray out("OBDSP001", 8); out.resize(16);
    qToLittleEndian<quint32>(quint32(header.size()), reinterpret_cast<uchar *>(out.data()+8));
    qToLittleEndian<quint32>(quint32(payload.size()), reinterpret_cast<uchar *>(out.data()+12));
    out.append(header); out.append(payload); return out;
}
int extract(QByteArray &buffer, QJsonObject &json, QByteArray &payload, QString &error) {
    if (buffer.size() < 16) return 0;
    if (!buffer.startsWith("OBDSP001")) { error = "Invalid DSP stream signature"; return -1; }
    const auto *bytes = reinterpret_cast<const uchar *>(buffer.constData());
    const quint32 j = qFromLittleEndian<quint32>(bytes+8), p = qFromLittleEndian<quint32>(bytes+12);
    if (!j || j > MaxJsonBytes || p > MaxPayloadBytes) { error = "DSP stream packet exceeds limit"; return -1; }
    const int size = 16+int(j)+int(p);
    if (buffer.size() < size) return 0;
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(buffer.mid(16, int(j)), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) { error = "Invalid DSP stream JSON"; return -1; }
    json = document.object(); payload = buffer.mid(16+int(j), int(p)); buffer.remove(0, size); return 1;
}
QByteArray floats(const std::vector<float> &values) {
    if (values.size() > std::size_t(MaxPayloadBytes/4)) return {};
    QByteArray out(int(values.size()*4U), Qt::Uninitialized);
    for (std::size_t i = 0; i < values.size(); ++i) {
        quint32 bits; std::memcpy(&bits, &values[i], 4);
        qToLittleEndian<quint32>(bits, reinterpret_cast<uchar *>(out.data()+i*4U));
    }
    return out;
}
QJsonObject request(const DspStreamBlock &b) {
    QJsonArray ffts;
    for (const auto &f : b.ffts) ffts.append(QJsonObject{{"id", f.id}, {"length", f.length}, {"window", f.window}, {"refreshMs", f.refreshMs}});
    return {{"type", "work"}, {"stream", b.stream}, {"generation", QString::number(qulonglong(b.generation))},
        {"epoch", QString::number(qulonglong(b.epoch))}, {"firstSample", QString::number(qulonglong(b.firstSample))},
        {"sampleCount", int(b.iq.size()/2U)}, {"sampleRate", b.sampleRate}, {"centerHz", b.centerHz},
        {"gap", b.gap}, {"returnIq", b.returnIq}, {"ffts", ffts}};
}
bool decodeRequest(const QJsonObject &j, const QByteArray &payload, DspStreamBlock &b, QString &error) {
    const int count = j.value("sampleCount").toInt(-1);
    if (j.value("type") != "work" || count < 1 || count > int(MaxSamples) || payload.size() != count*8 ||
        !u64(j.value("generation"), b.generation) || !u64(j.value("epoch"), b.epoch) || !u64(j.value("firstSample"), b.firstSample) ||
        !j.value("ffts").isArray() || j.value("ffts").toArray().size() > 2 ||
        !j.value("gap").isBool() || !j.value("returnIq").isBool() || !j.value("centerHz").isDouble()) { error = "Invalid DSP work request"; return false; }
    b.stream = j.value("stream").toString(); b.sampleRate = j.value("sampleRate").toDouble(); b.centerHz = j.value("centerHz").toDouble();
    b.gap = j.value("gap").toBool(); b.returnIq = j.value("returnIq").toBool(); b.ffts.clear();
    for (const auto &v : j.value("ffts").toArray()) {
        const auto f = v.toObject();
        b.ffts.append({f.value("id").toString(), f.value("length").toInt(-1), f.value("window").toInt(-1), f.value("refreshMs").toInt(-1)});
    }
    if (!readFloats(payload, 0, count*2, b.iq)) { error = "Invalid DSP IQ payload"; return false; }
    error = validate(b); return error.isEmpty();
}
QByteArray response(const DspStreamBlock &b, const DspStreamResult &r) {
    if (!r.error.isEmpty()) return encode({{"type", "error"}, {"error", r.error.left(256)}});
    QJsonObject json = request(b); json["type"] = "result"; json.remove("ffts");
    QByteArray payload = r.iq ? floats(*r.iq) : QByteArray(); json["iqBytes"] = payload.size();
    QJsonArray spectra;
    for (const auto &f : r.spectra) {
        spectra.append(QJsonObject{{"id", f.id}, {"length", int(f.levels.size())}, {"binWidthHz", f.binWidthHz}, {"rbwHz", f.rbwHz}, {"offsetBytes", payload.size()}});
        payload.append(floats(f.levels));
    }
    json["spectra"] = spectra; json["processingMs"] = r.processingMs;
    return encode(json, payload);
}
bool decodeResponse(const DspStreamBlock &b, const QJsonObject &j, const QByteArray &payload, DspStreamResult &r) {
    r = {};
    if (j.value("type") == "error") { r.error = "External DSP worker rejected the stream"; return true; }
    const auto invalid = [&r]() { r.error = "Invalid or stale DSP worker result"; return false; };
    std::uint64_t generation, epoch, first;
    if (j.value("type") != "result" || j.value("stream").toString() != b.stream ||
        !u64(j.value("generation"), generation) || generation != b.generation ||
        !u64(j.value("epoch"), epoch) || epoch != b.epoch || !u64(j.value("firstSample"), first) || first != b.firstSample ||
        j.value("sampleRate").toDouble() != b.sampleRate || j.value("centerHz").toDouble(-1) != b.centerHz ||
        j.value("sampleCount").toInt(-1) != int(b.iq.size()/2U) || !j.value("spectra").isArray()) return invalid();
    int offset = j.value("iqBytes").toInt(-1);
    if (offset != (b.returnIq ? int(b.iq.size()*4U) : 0)) return invalid();
    if (b.returnIq) {
        std::vector<float> iq;
        if (!readFloats(payload, 0, int(b.iq.size()), iq)) return invalid();
        r.iq = std::make_shared<const std::vector<float>>(std::move(iq));
    }
    const auto spectra = j.value("spectra").toArray();
    if (spectra.size() > b.ffts.size()) return invalid();
    QStringList ids;
    for (const auto &value : spectra) {
        const auto s = value.toObject(); const QString id = s.value("id").toString();
        auto config = std::find_if(b.ffts.begin(), b.ffts.end(), [&id](const auto &f) { return f.id == id; });
        if (config == b.ffts.end() || ids.contains(id) || s.value("length").toInt(-1) != config->length || s.value("offsetBytes").toInt(-1) != offset) return invalid();
        ids.append(id); DspBranchFrame frame; frame.id = id; frame.centerHz = b.centerHz;
        frame.sampleRateHz = b.sampleRate; frame.binWidthHz = s.value("binWidthHz").toDouble(); frame.rbwHz = s.value("rbwHz").toDouble();
        if (!std::isfinite(frame.binWidthHz) || std::abs(frame.binWidthHz-b.sampleRate/config->length) > 1e-6 ||
            !std::isfinite(frame.rbwHz) || frame.rbwHz < frame.binWidthHz*0.99 || frame.rbwHz > frame.binWidthHz*5 ||
            !readFloats(payload, offset, config->length, frame.levels)) return invalid();
        frame.generation = b.generation; frame.sequence = b.firstSample+b.iq.size()/2U;
        for (int i = 0; i < config->length; ++i) frame.frequencies.push_back(float(b.centerHz+(i-config->length/2)*frame.binWidthHz));
        offset += config->length*4; r.spectra.push_back(std::move(frame));
    }
    if (offset != payload.size()) return invalid();
    r.processingMs = j.value("processingMs").toDouble(-1);
    if (!std::isfinite(r.processingMs) || r.processingMs < 0 || r.processingMs > 60000) return invalid();
    return true;
}
}
