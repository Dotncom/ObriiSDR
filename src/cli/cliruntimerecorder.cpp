#include "cliruntimerecorder.h"

#include "channelizerutils.h"
#include "iqchannelizer.h"
#include "recordingformatcore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

namespace {
constexpr int TypeCount = 5;
enum Type { Cf32, Ci8, Channel, Audio, Spectrum };
enum class State { Idle, Starting, Recording, Stopping, Completed, Failed };
const std::array<QString, TypeCount> names = {
    QStringLiteral("cf32"), QStringLiteral("ci8"), QStringLiteral("channel"),
    QStringLiteral("audio"), QStringLiteral("spectrum")};

QString stateName(State state) {
    switch (state) {
    case State::Starting: return QStringLiteral("starting");
    case State::Recording: return QStringLiteral("recording");
    case State::Stopping: return QStringLiteral("stopping");
    case State::Completed: return QStringLiteral("completed");
    case State::Failed: return QStringLiteral("failed");
    default: return QStringLiteral("idle");
    }
}
bool busy(State s) { return s == State::Starting || s == State::Recording || s == State::Stopping; }
bool accepting(State s) { return s == State::Starting || s == State::Recording; }
bool rawType(int t) { return t == Cf32 || t == Ci8 || t == Channel; }

QJsonObject settingsJson(const RadioSettings &s) {
    return {{"centerFrequency", s.centerFrequency}, {"actualFrequency", s.actualFrequency},
            {"listeningFrequency", s.listeningFrequency}, {"sourceSampleRate", s.sampleRate},
            {"bandwidth", s.bandwidth}, {"modulationType", s.modulationType},
            {"sstvDemodulationMode", s.sstvDemodulationMode}, {"dmrBasebandSampleRate", s.dmrBasebandSampleRate},
            {"inputMode", s.inputMode}, {"deviceIndex", s.deviceIndex},
            {"clockSource", s.clockSource}, {"fftLength", s.fftLength},
            {"fftWindowType", s.fftWindowType}, {"lnaGain", s.lnaGain}, {"vgaGain", s.vgaGain},
            {"hackRfLnaGainDb", s.hackRfLnaGainDb}, {"hackRfVgaGainDb", s.hackRfVgaGainDb},
            {"hackRfAmpEnabled", s.hackRfAmpEnabled}, {"rtlAgc", s.rtlAgc},
            {"rtlTunerGainTenthsDb", s.rtlTunerGainTenthsDb}, {"uhdRxGainDb", s.uhdRxGainDb},
            {"uhdRxChannel", s.uhdRxChannel}, {"uhdRxAntenna", s.uhdRxAntenna}, {"gpoValue", int(s.gpoValue)}};
}

QString validateSettings(const RadioSettings &s, int type) {
    if (!std::isfinite(s.sampleRate) || s.sampleRate <= 0 || s.sampleRate > 1e9 ||
        !std::isfinite(s.centerFrequency) || s.centerFrequency < 0 ||
        !std::isfinite(s.actualFrequency) || s.actualFrequency < 0 ||
        !std::isfinite(s.listeningFrequency) || !std::isfinite(s.bandwidth) || s.bandwidth <= 0 ||
        !std::isfinite(s.uhdRxGainDb) || s.inputMode < INPUT_RF || s.inputMode > INPUT_HF_NOISE_CANCEL)
        return QStringLiteral("invalid source settings");
    if ((type == Channel || type == Audio) &&
        (s.modulationType < MOD_AM || s.modulationType > MOD_DMR ||
         std::abs(s.listeningFrequency - s.centerFrequency) + s.bandwidth * 0.5 > s.sampleRate * 0.5))
        return QStringLiteral("channel is outside source passband or has invalid modulation");
    if (type == Channel) {
        const double rate = channelizerOutputRate(s);
        if (!std::isfinite(rate) || rate < 1 || rate > 1e9 || rate != std::round(rate))
            return QStringLiteral("channel WAV requires an integral output sample rate");
    }
    if (type == Spectrum && s.fftLength <= 0)
        return QStringLiteral("invalid spectrum FFT length");
    return {};
}

bool sameSource(const RadioSettings &a, const RadioSettings &b, int type) {
    if (a.sampleRate != b.sampleRate || a.centerFrequency != b.centerFrequency ||
        a.actualFrequency != b.actualFrequency || a.inputMode != b.inputMode ||
        a.deviceIndex != b.deviceIndex || a.clockSource != b.clockSource ||
        a.lnaGain != b.lnaGain || a.vgaGain != b.vgaGain || a.gpoValue != b.gpoValue ||
        a.hackRfLnaGainDb != b.hackRfLnaGainDb || a.hackRfVgaGainDb != b.hackRfVgaGainDb ||
        a.hackRfAmpEnabled != b.hackRfAmpEnabled || a.rtlAgc != b.rtlAgc ||
        a.rtlTunerGainTenthsDb != b.rtlTunerGainTenthsDb || a.uhdRxGainDb != b.uhdRxGainDb ||
        a.uhdRxChannel != b.uhdRxChannel || a.uhdRxAntenna != b.uhdRxAntenna) return false;
    if ((type == Channel || type == Audio) &&
        (a.listeningFrequency != b.listeningFrequency || a.bandwidth != b.bandwidth ||
         a.modulationType != b.modulationType || a.sstvDemodulationMode != b.sstvDemodulationMode ||
         a.dmrBasebandSampleRate != b.dmrBasebandSampleRate)) return false;
    return type != Spectrum || (a.fftLength == b.fftLength && a.fftWindowType == b.fftWindowType);
}

QString normalizedPath(const QString &path) {
    if (path.isEmpty() || path.size() > 4096 || path.endsWith('/') || path.endsWith('\\')) return {};
    for (QChar c : path) if (c.unicode() < 32) return {};
    const QString result = QDir::cleanPath(QDir::current().absoluteFilePath(path));
    const QString leaf = QFileInfo(result).fileName();
    if (leaf.isEmpty() || leaf == "." || leaf == "..") return {};
#ifdef Q_OS_WIN
    const QString base = leaf.section('.', 0, 0).toUpper();
    if (leaf.contains(':') || leaf.endsWith('.') || leaf.endsWith(' ') || base == "CON" ||
        base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" || base == "CONOUT$" ||
        (base.size() == 4 && (base.startsWith("COM") || base.startsWith("LPT")) &&
         base[3] >= '1' && base[3] <= '9')) return {};
#endif
    return result;
}

struct Counts {
    quint64 bytes = 0, samples = 0, inputSamples = 0, frames = 0;
    qint64 firstUtc = 0, lastUtc = 0;
};
QJsonObject countsJson(const Counts &c) {
    return {{"bytesWritten", double(c.bytes)}, {"sampleCount", double(c.samples)},
            {"inputSampleCount", double(c.inputSamples)}, {"framesWritten", double(c.frames)},
            {"bytesWrittenExact", QString::number(c.bytes)}, {"sampleCountExact", QString::number(c.samples)},
            {"firstFrameUtcMs", double(c.firstUtc)}, {"lastFrameUtcMs", double(c.lastUtc)}};
}

// File reservations and asynchronous lifecycle stay outside the borrowed-sink core.
class Writer {
public:
    Writer(int t, QString p, const RadioSettings &s, qint64 utc)
        : type(t), path(std::move(p)), settings(s), startedUtc(utc) {
        sampleRate = type == Audio ? 48000 : type == Channel ? channelizerOutputRate(s) : s.sampleRate;
        channelizer.reset();
    }
    bool open(QString &error) {
        const QStringList paths = type == Cf32 || type == Ci8
            ? QStringList{path, path + ".json", path + ".sigmf-meta"}
            : QStringList{path, path + ".json"};
        for (int i = 0; i < paths.size(); ++i) {
            files[std::size_t(i)].setFileName(paths[i]);
            if (!files[std::size_t(i)].open(QIODevice::ReadWrite | QIODevice::NewOnly)) {
                error = QStringLiteral("cannot create new file %1: %2").arg(paths[i], files[std::size_t(i)].errorString());
                for (int j = 0; j < i; ++j) files[std::size_t(j)].remove();
                return false;
            }
        }
        opened = true;
        if (type == Audio || type == Channel) {
            if (!RecordingFormatCore::beginWave(files[0], quint32(sampleRate), type == Audio ? 1 : 2,
                                                16, {}, waveLayout, &error)) return false;
        }
        if (type == Spectrum) {
            QJsonObject m = metadata(QStringLiteral("recording"), {});
            m["format"] = QStringLiteral("fobos_spectrum_frames"); m["version"] = 1;
            m["scalePercent"] = 100; m["targetBins"] = settings.fftLength;
            if (!RecordingFormatCore::beginSpectrum(files[0], m, &error)) return false;
        }
        headerBytes = quint64(files[0].pos());
        return writeMetadata(QStringLiteral("recording"), {}, error);
    }

    bool appendIq(const std::vector<float> &iq, qint64 utc, QString &error) {
        // Preserve this wrapper's original fail-closed source policy. The CF32
        // format core itself borrows bits without validation or conversion.
        if (type == Cf32 || type == Channel) {
            for (float v : iq) if (!std::isfinite(v)) { error = QStringLiteral("non-finite IQ sample"); return false; }
        }
        const std::vector<float> *source = &iq;
        if (type == Channel) {
            const auto result = channelizer.processFloatIq(iq.data(), iq.size(), settings, channel, false);
            if (!result.valid || result.outputRate != sampleRate) {
                error = QStringLiteral("channelizer rejected input or changed rate"); return false;
            }
            source = &channel;
            channelResult = {{"frequencyShiftHz", result.frequencyShift}, {"cutoffHz", result.cutoff},
                             {"decimationFactor", result.decimationFactor}, {"firTapCount", result.firTapCount},
                             {"firDelayCoarseSamples", result.firDelayCoarseSamples}, {"agc", false}};
        }
        const auto encoding = type == Cf32 ? RecordingFormatCore::IqEncoding::Cf32Le :
            type == Ci8 ? RecordingFormatCore::IqEncoding::Ci8 : RecordingFormatCore::IqEncoding::Ci16Le;
        RecordingFormatCore::EncodedIq bytes;
        if (!RecordingFormatCore::encodeIq(source->data(), source->size(), encoding, iqScratch, bytes, &error) ||
            !appendBytes(bytes.data, bytes.size, error)) return false;
        counts.samples += source->size() / 2;
        counts.inputSamples += iq.size() / 2;
        stamp(utc);
        return true;
    }
    bool appendAudio(const QByteArray &pcm, qint64 utc, QString &error) {
        if (!appendBytes(pcm.constData(), pcm.size(), error)) return false;
        counts.samples += quint64(pcm.size() / 2); counts.inputSamples += quint64(pcm.size() / 2);
        stamp(utc); return true;
    }
    bool appendSpectrum(const std::vector<float> &levels, double first, double bin, qint64 utc, QString &error) {
        const RecordingFormatCore::SpectrumFrameView frame{utc - startedUtc, utc, settings.centerFrequency,
            first, first + bin * double(levels.size() - 1), settings.fftLength, levels.data(), levels.size()};
        if (!RecordingFormatCore::appendSpectrum(files[0], frame, &error)) return false;
        counts.bytes += RecordingFormatCore::SpectrumFrameHeaderBytes + levels.size() * sizeof(float);
        spectrumGeometry = {{"firstHz", first}, {"binHz", bin}, {"bins", int(levels.size())}};
        counts.samples += levels.size(); stamp(utc); return true;
    }

    void finish(const QString &cause, QString &error) {
        if (!opened) return;
        if (!files[0].flush()) error = files[0].errorString();
        const qint64 expectedSize = qint64(headerBytes + counts.bytes);
        const qint64 actualSize = files[0].size();
        if (actualSize != expectedSize) {
            if (actualSize < expectedSize || !files[0].resize(expectedSize))
                error = QStringLiteral("cannot restore a complete data prefix");
        }
        if ((type == Audio || type == Channel) && headerBytes) {
            if (!RecordingFormatCore::finalizeWave(files[0], waveLayout, counts.bytes, &error) && error.isEmpty())
                error = QStringLiteral("cannot finalize WAV header");
        }
        if (!files[0].flush()) error = files[0].errorString();
        const QString failure = cause.isEmpty() ? error : cause;
        QString metaError;
        if (!writeMetadata(failure.isEmpty() ? QStringLiteral("completed") : QStringLiteral("failed"), failure, metaError))
            error = metaError;
        for (auto &file : files) if (file.isOpen()) file.close();
        opened = false;
    }
    Counts counts;
    bool broken = false;

private:
    bool appendBytes(const char *data, qint64 size, QString &error) {
        if (type == Audio || type == Channel) {
            return RecordingFormatCore::appendWave(files[0], waveLayout, counts.bytes, data, size, &error);
        }
        if (!RecordingFormatCore::checkedAppend(files[0], data, size, &error)) return false;
        counts.bytes += quint64(size); return true;
    }
    void stamp(qint64 utc) {
        if (!counts.frames) counts.firstUtc = utc;
        counts.lastUtc = utc; ++counts.frames;
    }
    QJsonObject metadata(const QString &state, const QString &error) const {
        QJsonObject m = settingsJson(settings);
        const auto c = countsJson(counts);
        for (auto it = c.begin(); it != c.end(); ++it) m[it.key()] = it.value();
        m["app"] = QStringLiteral("ObriiSDR"); m["type"] = names[std::size_t(type)];
        m["format"] = type == Cf32 ? "cf32_le" : type == Ci8 ? "full_iq_raw_s8_interleaved" :
            type == Channel ? "channel_iq_wav_s16le" : type == Audio ? "audio_wav_s16le" : "fobos_spectrum_frames";
        m["filePath"] = path; m["state"] = state; m["error"] = error; m["sampleRate"] = sampleRate;
        m["recordedAtUtc"] = QDateTime::fromMSecsSinceEpoch(startedUtc, Qt::UTC).toString(Qt::ISODateWithMs);
        m["durationSeconds"] = type == Spectrum ? 0.0 : double(counts.samples) / sampleRate;
        if (type == Channel) { m["channelizer"] = channelResult; m["channels"] = 2; }
        if (type == Audio) m["channels"] = 1;
        if (type == Spectrum) m["spectrum"] = spectrumGeometry;
        return m;
    }
    bool replaceJson(QFile &file, const QJsonObject &json, QString &error) {
        const QByteArray bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);
        if (!file.seek(0) || !file.resize(0) || file.write(bytes) != bytes.size() || !file.flush()) {
            error = QStringLiteral("metadata write failed for %1: %2").arg(file.fileName(), file.errorString());
            return false;
        }
        return true;
    }
    bool writeMetadata(const QString &state, const QString &error, QString &failure) {
        if (!replaceJson(files[1], metadata(state, error), failure)) return false;
        if (type != Cf32 && type != Ci8) return true;
        return replaceJson(files[2], RecordingFormatCore::sigMfMetadata(
            type == Cf32 ? QStringLiteral("cf32_le") : QStringLiteral("ci8"), sampleRate,
            QFileInfo(path).fileName(), settings.centerFrequency, counts.firstUtc,
            {{"obrii:sample_count", double(counts.samples)}, {"obrii:state", state}, {"obrii:error", error}}), failure);
    }
    int type;
    QString path;
    RadioSettings settings;
    qint64 startedUtc;
    double sampleRate = 0;
    quint64 headerBytes = 0;
    bool opened = false;
    std::array<QFile, 3> files;
    IqChannelizer channelizer;
    std::vector<float> channel;
    QByteArray iqScratch;
    RecordingFormatCore::WaveLayout waveLayout;
    QJsonObject channelResult, spectrumGeometry;
};
} // namespace

struct CliRuntimeRecorder::Impl {
    struct Slot {
        State state = State::Idle;
        QString path, error;
        RadioSettings settings;
        quint64 generation = 0;
        qint64 startedUtc = 0;
        std::size_t pending = 0;
        bool opened = false, hasGeometry = false;
        double firstHz = 0, binHz = 0;
        std::size_t bins = 0;
        Counts counts;
    };
    struct Block {
        enum Kind { Iq, Pcm, Fft } kind = Iq;
        std::array<quint64, TypeCount> targets{};
        std::vector<float> values;
        QByteArray pcm;
        double firstHz = 0, binHz = 0;
        qint64 utc = 0;
        std::size_t charge = 0;
    };
    explicit Impl(std::size_t bytes, std::size_t blocks, std::size_t types)
        : budget((std::min)(bytes, std::size_t(1024U * 1024U * 1024U))),
          blockLimit((std::min)(blocks, std::size_t(1024))),
          typeLimit((std::clamp)(types, std::size_t(1), std::size_t(TypeCount))) {}
    ~Impl() { shutdown(); }
    void shutdown() {
        std::lock_guard<std::mutex> control(commands);
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto &s : records) if (accepting(s.state)) s.state = State::Stopping;
        }
        wake.notify_one();
        // Regular-file OS writes are not cancellable. Destruction must be off RX.
        if (worker.joinable()) worker.join();
    }
    void fail(int t, const QString &error) {
        auto &s = records[std::size_t(t)];
        if (!busy(s.state)) return;
        if (s.error.isEmpty()) s.error = error;
        s.state = State::Stopping;
        wake.notify_one();
    }
    void validateSource(const RadioSettings &actual, bool discontinuity) {
        for (int i = 0; i < TypeCount; ++i) if (accepting(records[std::size_t(i)].state)) {
            if (discontinuity) fail(i, QStringLiteral("source discontinuity"));
            else if (!validateSettings(actual, i).isEmpty() || !sameSource(records[std::size_t(i)].settings, actual, i))
                fail(i, QStringLiteral("source settings changed (rate/center/gain/channel/FFT)"));
        }
    }
    std::array<quint64, TypeCount> targets(Block::Kind kind) const {
        std::array<quint64, TypeCount> result{};
        for (int i = 0; i < TypeCount; ++i)
            if (accepting(records[std::size_t(i)].state) &&
                ((kind == Block::Iq && rawType(i)) || (kind == Block::Pcm && i == Audio) || (kind == Block::Fft && i == Spectrum)))
                result[std::size_t(i)] = records[std::size_t(i)].generation;
        return result;
    }
    bool capacity(std::size_t payload, const std::array<quint64, TypeCount> &target) {
        if (std::none_of(target.begin(), target.end(), [](quint64 v) { return v != 0; })) return false;
        if (payload > 64U * 1024U * 1024U || payload > budget || sizeof(Block) > budget - payload ||
            chargedBytes > budget - payload - sizeof(Block) || chargedBlocks >= blockLimit) {
            for (int i = 0; i < TypeCount; ++i) if (target[std::size_t(i)]) fail(i, QStringLiteral("recording queue overflow"));
            return false;
        }
        return true;
    }
    void enqueue(Block &&block) {
        const auto target = block.targets;
        const auto charge = block.charge;
        queue.push_back(std::move(block));
        chargedBytes += charge; ++chargedBlocks;
        for (int i = 0; i < TypeCount; ++i) if (target[std::size_t(i)]) ++records[std::size_t(i)].pending;
        wake.notify_one();
    }
    void allocationFailed(const std::array<quint64, TypeCount> &target) {
        for (int i = 0; i < TypeCount; ++i) if (target[std::size_t(i)]) fail(i, QStringLiteral("recording allocation failed"));
    }
    QJsonObject slotJson(int i) const {
        const auto &s = records[std::size_t(i)];
        QJsonObject result = countsJson(s.counts);
        result["type"] = names[std::size_t(i)]; result["state"] = stateName(s.state);
        result["active"] = busy(s.state); result["path"] = s.path; result["error"] = s.error;
        result["generation"] = QString::number(s.generation); result["pendingBlocks"] = double(s.pending);
        result["metadataPath"] = s.path.isEmpty() ? QString() : s.path + ".json";
        if (i == Cf32 || i == Ci8) result["sigmfPath"] = s.path.isEmpty() ? QString() : s.path + ".sigmf-meta";
        result["settings"] = settingsJson(s.settings);
        return result;
    }
    QJsonObject statusLocked() const {
        QJsonArray recordings;
        int active = 0;
        for (int i = 0; i < TypeCount; ++i) { recordings.append(slotJson(i)); if (busy(records[std::size_t(i)].state)) ++active; }
        return {{"ok", true}, {"active", active != 0}, {"activeTypes", active}, {"workerRunning", running},
                {"queueBytes", double(chargedBytes)}, {"queueBlocks", double(chargedBlocks)},
                {"queueBudgetBytes", double(budget)}, {"maxQueueBlocks", double(blockLimit)},
                {"maxActiveTypes", double(typeLimit)}, {"recordings", recordings}};
    }
    QJsonObject rejected(const QString &error) const {
        auto result = statusLocked(); result["ok"] = false; result["error"] = error; return result;
    }
    void run() {
        std::array<std::unique_ptr<Writer>, TypeCount> writers;
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            int opening = -1, closing = -1;
            bool any = false;
            for (int i = 0; i < TypeCount; ++i) {
                const auto &s = records[std::size_t(i)];
                if (!busy(s.state)) continue;
                any = true;
                if (!s.opened && opening < 0) opening = i;
                else if (s.opened && s.state == State::Stopping && !s.pending && closing < 0) closing = i;
            }
            if (!any) { running = false; return; }
            if (opening >= 0) {
                const auto index = std::size_t(opening);
                const auto snapshot = records[index];
                records[index].opened = true;
                lock.unlock();
                QString error;
                std::unique_ptr<Writer> writer;
                try {
                    writer = std::make_unique<Writer>(opening, snapshot.path, snapshot.settings, snapshot.startedUtc);
                    if (!writer->open(error)) writer->broken = true;
                } catch (const std::exception &) {
                    error = QStringLiteral("recording worker failed while opening output");
                    if (writer) writer->broken = true;
                }
                lock.lock();
                writers[index] = std::move(writer);
                if (!error.isEmpty()) fail(opening, error);
                else if (records[index].state == State::Starting) records[index].state = State::Recording;
                continue;
            }
            if (closing >= 0) {
                const auto index = std::size_t(closing);
                const QString cause = records[index].error;
                lock.unlock();
                QString error;
                try { if (writers[index]) writers[index]->finish(cause, error); }
                catch (const std::exception &) { error = QStringLiteral("recording worker failed while finalizing output"); }
                const auto counts = writers[index] ? writers[index]->counts : Counts{};
                writers[index].reset();
                lock.lock();
                auto &s = records[index];
                s.counts = counts;
                if (!error.isEmpty()) s.error += (s.error.isEmpty() ? QString() : QStringLiteral("; ")) + error;
                s.state = s.error.isEmpty() ? State::Completed : State::Failed;
                notices[index] = slotJson(closing);
                notices[index]["event"] = s.state == State::Completed ? "recordingCompleted" : "recordingFailed";
                continue;
            }
            if (!queue.empty()) {
                Block block = std::move(queue.front()); queue.pop_front();
                lock.unlock();
                for (int i = 0; i < TypeCount; ++i) {
                    const auto index = std::size_t(i);
                    if (!block.targets[index]) continue;
                    QString error;
                    auto &writer = writers[index];
                    if (writer && !writer->broken) {
                        bool ok = false;
                        try {
                            ok = block.kind == Block::Iq ? writer->appendIq(block.values, block.utc, error) :
                                block.kind == Block::Pcm ? writer->appendAudio(block.pcm, block.utc, error) :
                                writer->appendSpectrum(block.values, block.firstHz, block.binHz, block.utc, error);
                        } catch (const std::exception &) { error = QStringLiteral("recording worker failed while encoding input"); }
                        if (!ok) { writer->broken = true; if (error.isEmpty()) error = QStringLiteral("recording write failed"); }
                    }
                    lock.lock();
                    if (writer) records[index].counts = writer->counts;
                    if (!error.isEmpty()) fail(i, error);
                    --records[index].pending;
                    lock.unlock();
                }
                const auto charge = block.charge;
                block = {}; // Release the in-flight payload before returning its budget.
                lock.lock();
                chargedBytes -= charge; --chargedBlocks;
                continue;
            }
            wake.wait(lock);
        }
    }

    mutable std::mutex mutex;
    std::mutex commands;
    std::condition_variable wake;
    std::thread worker;
    std::array<Slot, TypeCount> records;
    std::array<QJsonObject, TypeCount> notices;
    std::deque<Block> queue;
    const std::size_t budget, blockLimit, typeLimit;
    std::size_t chargedBytes = 0, chargedBlocks = 0;
    bool running = false;
};

CliRuntimeRecorder::CliRuntimeRecorder(std::size_t bytes, std::size_t blocks, std::size_t types)
    : d(std::make_unique<Impl>(bytes, blocks, types)) {}
CliRuntimeRecorder::~CliRuntimeRecorder() = default;

QJsonObject CliRuntimeRecorder::command(const QJsonObject &request, const RadioSettings &settings) {
    std::lock_guard<std::mutex> control(d->commands);
    std::unique_lock<std::mutex> lock(d->mutex);
    if (!d->running && d->worker.joinable()) {
        lock.unlock(); d->worker.join(); lock.lock();
    }
    const QString operation = request.value("operation").toString();
    if (operation == "status") return d->statusLocked();
    const QString type = request.value("type").toString();
    const auto found = std::find(names.begin(), names.end(), type);
    const int index = int(found - names.begin());
    if (operation == "stop") {
        if (!type.isEmpty() && index == TypeCount) return d->rejected(QStringLiteral("unknown recording type"));
        for (int i = 0; i < TypeCount; ++i)
            if ((type.isEmpty() || i == index) && accepting(d->records[std::size_t(i)].state))
                d->records[std::size_t(i)].state = State::Stopping;
        d->wake.notify_one(); return d->statusLocked();
    }
    if (operation != "start" || index == TypeCount) return d->rejected(QStringLiteral("expected start/stop/status and a valid recording type"));
    auto &slot = d->records[std::size_t(index)];
    if (busy(slot.state)) return d->rejected(QStringLiteral("recording type is already active or stopping"));
    std::size_t active = 0;
    for (const auto &s : d->records) if (busy(s.state)) ++active;
    if (active >= d->typeLimit) return d->rejected(QStringLiteral("active recording type limit reached"));
    const QString error = validateSettings(settings, index);
    if (!error.isEmpty()) return d->rejected(error);
    const QString path = normalizedPath(request.value("path").toString());
    if (path.isEmpty()) return d->rejected(QStringLiteral("an explicit regular-file path is required"));
    const quint64 generation = slot.generation + 1;
    slot = {}; slot.generation = generation; slot.path = path; slot.settings = settings;
    slot.startedUtc = QDateTime::currentMSecsSinceEpoch(); slot.state = State::Starting;
    if (!d->running) {
        try { d->running = true; d->worker = std::thread([impl = d.get()] { impl->run(); }); }
        catch (const std::exception &) {
            d->running = false; slot.state = State::Failed; slot.error = QStringLiteral("cannot create recording worker");
            return d->rejected(slot.error);
        }
    }
    d->wake.notify_one(); return d->statusLocked();
}

QJsonObject CliRuntimeRecorder::status() const {
    std::lock_guard<std::mutex> lock(d->mutex); return d->statusLocked();
}
bool CliRuntimeRecorder::active() const {
    std::lock_guard<std::mutex> lock(d->mutex);
    return std::any_of(d->records.begin(), d->records.end(), [](const Impl::Slot &s) { return busy(s.state); });
}
bool CliRuntimeRecorder::needsIq() const {
    std::lock_guard<std::mutex> lock(d->mutex);
    return std::any_of(d->records.begin(), d->records.end(), [](const Impl::Slot &s) { return accepting(s.state); });
}
bool CliRuntimeRecorder::needsAudio() const {
    std::lock_guard<std::mutex> lock(d->mutex); return accepting(d->records[Audio].state);
}
bool CliRuntimeRecorder::needsSpectrum() const {
    std::lock_guard<std::mutex> lock(d->mutex); return accepting(d->records[Spectrum].state);
}
QJsonArray CliRuntimeRecorder::takeNotifications() {
    std::lock_guard<std::mutex> lock(d->mutex);
    QJsonArray result;
    for (auto &n : d->notices) if (!n.isEmpty()) { result.append(n); n = {}; }
    return result;
}
void CliRuntimeRecorder::shutdown() { d->shutdown(); }

bool CliRuntimeRecorder::submitIq(const float *samples, std::size_t count,
                                  const RadioSettings &actual, bool discontinuity) {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->validateSource(actual, discontinuity);
    const auto target = d->targets(Impl::Block::Iq);
    const bool observed = std::any_of(d->records.begin(), d->records.end(),
        [](const Impl::Slot &s) { return accepting(s.state); });
    if (!count || std::none_of(target.begin(), target.end(), [](quint64 v) { return v != 0; }))
        return observed; // Metadata-only observation; no payload is needed or copied.
    if (!samples || count > std::size_t((std::numeric_limits<int>::max)()) / 8) {
        for (int i = 0; i < TypeCount; ++i) if (target[std::size_t(i)]) d->fail(i, QStringLiteral("invalid IQ block"));
        return false;
    }
    const auto bytes = count * 2 * sizeof(float);
    if (!d->capacity(bytes, target)) return false;
    try {
        Impl::Block block; block.targets = target; block.charge = bytes + sizeof(block);
        block.utc = QDateTime::currentMSecsSinceEpoch(); block.values.assign(samples, samples + count * 2);
        d->enqueue(std::move(block)); return true;
    } catch (const std::bad_alloc &) { d->allocationFailed(target); return false; }
}

bool CliRuntimeRecorder::submitAudio(const QByteArray &pcm) {
    std::lock_guard<std::mutex> lock(d->mutex);
    const auto target = d->targets(Impl::Block::Pcm);
    if (pcm.isEmpty()) return false;
    if (pcm.size() % 2) { if (target[Audio]) d->fail(Audio, QStringLiteral("unaligned PCM16 audio block")); return false; }
    if (!d->capacity(std::size_t(pcm.size()), target)) return false;
    try {
        Impl::Block block; block.kind = Impl::Block::Pcm; block.targets = target;
        block.charge = std::size_t(pcm.size()) + sizeof(block); block.utc = QDateTime::currentMSecsSinceEpoch();
        block.pcm = QByteArray(pcm.constData(), pcm.size());
        d->enqueue(std::move(block)); return true;
    } catch (const std::bad_alloc &) { d->allocationFailed(target); return false; }
}

bool CliRuntimeRecorder::submitSpectrum(const std::vector<float> &levels, double first, double bin,
                                       const RadioSettings &actual, bool discontinuity, qint64 utc) {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->validateSource(actual, discontinuity);
    const auto target = d->targets(Impl::Block::Fft);
    if (!target[Spectrum]) return false;
    auto &slot = d->records[Spectrum];
    if (levels.empty() || levels.size() > 2097152 || !std::isfinite(first) || !std::isfinite(bin) || bin <= 0 ||
        !std::isfinite(first + bin * double(levels.size() - 1)) || utc < 0 ||
        (slot.hasGeometry && (slot.firstHz != first || slot.binHz != bin || slot.bins != levels.size()))) {
        d->fail(Spectrum, QStringLiteral("invalid or changed spectrum geometry")); return false;
    }
    if (!d->capacity(levels.size() * sizeof(float), target)) return false;
    try {
        Impl::Block block; block.kind = Impl::Block::Fft; block.targets = target;
        block.charge = levels.size() * sizeof(float) + sizeof(block);
        block.utc = utc ? utc : QDateTime::currentMSecsSinceEpoch();
        block.firstHz = first; block.binHz = bin; block.values = levels;
        d->enqueue(std::move(block));
        slot.hasGeometry = true; slot.firstHz = first; slot.binHz = bin; slot.bins = levels.size();
        return true;
    } catch (const std::bad_alloc &) { d->allocationFailed(target); return false; }
}
