#include "dspboardsignal.h"
#include "dspgraphmodel.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr int MaxSamples = 2000000;
constexpr double Tau = 6.2831853071795864769;
constexpr int PacketTagBytes = 8;
const QByteArray PacketMagic("OBQ1", 4);

QByteArray packetTag(const QByteArray &frame) {
    return QCryptographicHash::hash(frame, QCryptographicHash::Sha256).left(PacketTagBytes);
}

QByteArray encodePacket(DspBoardPayloadType type, const QByteArray &name, const QByteArray &payload) {
    QByteArray frame = PacketMagic;
    frame.append(char(type));
    frame.append(char(name.size()));
    char length[4];
    qToLittleEndian<quint32>(quint32(payload.size()), length);
    frame.append(length, sizeof(length));
    frame += name;
    frame += payload;
    frame += packetTag(frame);
    return QByteArray(1, '\0') + DspBoardQpsk::preamble() + frame;
}

QByteArray readQpskBytes(const SignalLabGenerationResult &signal, int startByte, int byteCount) {
    // The matched window starts four symbols into the burst, after the leading guard byte.
    const auto symbols = SignalLabGenerator::matchedSymbolBlock(
        signal, startByte * DspBoardQpsk::SymbolsPerByte, byteCount * DspBoardQpsk::SymbolsPerByte);
    if (symbols.size() != byteCount * DspBoardQpsk::SymbolsPerByte) return {};
    QByteArray bytes(byteCount, '\0');
    for (int i = 0; i < byteCount; ++i) {
        unsigned value = 0;
        for (int j = 0; j < DspBoardQpsk::SymbolsPerByte; ++j) {
            const auto sample = symbols[i * DspBoardQpsk::SymbolsPerByte + j];
            const float power = std::norm(sample);
            if (!(power > 1e-12f) || !std::isfinite(power)) return {};
            value = (value << 2) | DspBoardQpsk::symbolBits(sample);
        }
        bytes[i] = char(value);
    }
    return bytes;
}

SignalLabGenerationResult failure(const QString &message) {
    SignalLabGenerationResult result;
    result.error = message;
    return result;
}

QByteArray readBounded(const QString &path, int limit, QString &error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return {};
    }
    const QByteArray data = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError) error = file.errorString();
    else if (data.isEmpty() || data.size() > limit)
        error = QStringLiteral("The file is empty or exceeds the configured sample/payload limit");
    return data;
}

float readFloat(const char *bytes) {
    const quint32 raw = qFromLittleEndian<quint32>(bytes);
    float value;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

SignalLabGenerationResult loadIq(const QString &path, int rate) {
    QString error;
    const QByteArray data = readBounded(path, MaxSamples * 8, error);
    if (!error.isEmpty()) return failure(error);
    if (data.size() % 8 != 0) return failure(QStringLiteral("CF32LE must contain complete I/Q pairs"));
    SignalLabGenerationResult result;
    result.sampleRate = rate;
    result.iq.resize(data.size() / 8);
    for (int i = 0; i < result.iq.size(); ++i) {
        const float real = readFloat(data.constData() + 8 * i);
        const float imag = readFloat(data.constData() + 8 * i + 4);
        if (!std::isfinite(real) || !std::isfinite(imag) || std::abs(real) > 1.0f || std::abs(imag) > 1.0f)
            return failure(QStringLiteral("CF32LE samples must be finite and normalized to [-1, 1]"));
        result.iq[i] = {real, imag};
    }
    return result;
}

SignalLabGenerationResult modulateWav(const QString &path, const SignalLabConfiguration &c, bool fm) {
    QString error;
    const QByteArray data = readBounded(path, 16 * 1024 * 1024, error);
    if (!error.isEmpty()) return failure(error);
    if (data.size() < 12 || data.mid(0, 4) != "RIFF" || data.mid(8, 4) != "WAVE")
        return failure(QStringLiteral("Expected a PCM16 WAV file"));
    int sourceRate = 0, channels = 0, pcmSize = 0;
    const char *pcm = nullptr;
    for (int pos = 12; pos <= data.size() - 8;) {
        const char *chunk = data.constData() + pos;
        const quint32 length = qFromLittleEndian<quint32>(chunk + 4);
        pos += 8;
        if (length > quint32(data.size() - pos)) return failure(QStringLiteral("Truncated WAV chunk"));
        if (std::memcmp(chunk, "fmt ", 4) == 0 && length >= 16) {
            const char *format = data.constData() + pos;
            if (qFromLittleEndian<quint16>(format) != 1 || qFromLittleEndian<quint16>(format + 14) != 16)
                return failure(QStringLiteral("Expected a PCM16 WAV file"));
            channels = qFromLittleEndian<quint16>(format + 2);
            const quint32 wavRate = qFromLittleEndian<quint32>(format + 4);
            if (wavRate == 0 || wavRate > quint32(c.sampleRate))
                return failure(QStringLiteral("Set the I/Q rate at or above the WAV sample rate"));
            sourceRate = int(wavRate);
        } else if (std::memcmp(chunk, "data", 4) == 0 && !pcm) {
            pcm = data.constData() + pos;
            pcmSize = int(length);
        }
        pos += int(length) + int(length & 1);
    }
    if (!pcm || sourceRate == 0 || channels < 1 || channels > 2 || pcmSize % (channels * 2) != 0)
        return failure(QStringLiteral("Expected complete mono or stereo PCM16 WAV frames"));
    const int frames = pcmSize / (channels * 2);
    const int count = int(std::min<qint64>(std::llround(c.durationSeconds * c.sampleRate),
                                         qint64(frames) * c.sampleRate / sourceRate));
    const double halfBand = sourceRate * 0.5 + (fm ? c.fskDeviationHz : 0.0);
    if (std::abs(c.centerOffsetHz) + halfBand > c.sampleRate * 0.5)
        return failure(QStringLiteral("Increase the I/Q rate to fit the WAV bandwidth, deviation and carrier offset"));
    const auto mono = [pcm, channels](int frame) {
        double sum = 0.0;
        for (int channel = 0; channel < channels; ++channel)
            sum += qFromLittleEndian<qint16>(pcm + (frame * channels + channel) * 2);
        return sum / (channels * 32768.0);
    };
    SignalLabGenerationResult result;
    result.sampleRate = c.sampleRate;
    result.centerOffsetHz = c.centerOffsetHz;
    result.occupiedBandwidthHz = 2.0 * halfBand;
    result.iq.resize(count);
    double phase = 0.0;
    for (int i = 0; i < count; ++i) {
        const double position = double(i) * sourceRate / c.sampleRate;
        const int frame = int(position);
        const double first = mono(frame);
        const double value = first + (position - frame) * (mono(std::min(frame + 1, frames - 1)) - first);
        const double level = fm ? c.amplitude : c.amplitude * (1.0 + 0.8 * value) / 1.8;
        result.iq[i] = std::polar(float(level), float(phase));
        const double hz = c.centerOffsetHz + (fm ? value * c.fskDeviationHz : 0.0);
        phase = std::remainder(phase + Tau * hz / c.sampleRate, Tau);
    }
    return result;
}

QString typeOf(const QJsonObject &block) { return block.value("type").toString(); }
QJsonObject settingsOf(const QJsonObject &block) { return block.value("settings").toObject(); }
}

bool DspBoardSignalRequest::operator==(const DspBoardSignalRequest &other) const {
    return iq == other.iq && modulator == other.modulator && carrier == other.carrier &&
           payload == other.payload && error == other.error;
}

QString DspBoardSignalRequest::sourceId() const { return iq.value("id").toString(); }
double DspBoardSignalRequest::centerHz() const { return settingsOf(iq).value("centerHz").toDouble(100e6); }

DspBoardSignalRequest resolveDspBoardSignal(const QJsonObject &graph, const QString &sinkId) {
    DspBoardSignalRequest result;
    QHash<QString, QJsonObject> blocks;
    for (const auto &value : graph.value("blocks").toArray()) {
        const auto block = value.toObject();
        if (!dspIsSignalBlock(typeOf(block))) continue;
        // Geometry and display titles are irrelevant to execution.
        const QString id = block.value("id").toString();
        auto settings = settingsOf(block);
        settings.remove("workspaceCanvasScale");
        settings.remove("workspaceSector");
        blocks.insert(id, {{"id", id}, {"type", typeOf(block)}, {"settings", settings}});
    }
    const auto edges = graph.value("connections").toArray();
    const auto input = [&](const QString &id, DspLinkKind kind) {
        QJsonObject found;
        for (const auto &value : edges) {
            const auto edge = value.toObject();
            if (edge.value("target") != id) continue;
            const auto source = blocks.value(edge.value("source").toString());
            if (dspLinkKind(typeOf(source), typeOf(blocks.value(id))) != kind) continue;
            if (!found.isEmpty()) {
                result.error = QStringLiteral("Multiple connections to the same signal input");
                return QJsonObject{};
            }
            found = source;
        }
        return found;
    };
    result.iq = blocks.value(sinkId);
    if (typeOf(result.iq) == "lab_tx") result.iq = input(sinkId, DspLinkKind::LabIq);
    if (typeOf(result.iq) != "lab_iq" || result.sourceId().isEmpty()) {
        result.error = QStringLiteral("Connect an I/Q stream block");
        return result;
    }
    result.modulator = input(result.sourceId(), DspLinkKind::LabIq);
    if (result.modulator.isEmpty()) {
        result.error = QStringLiteral("Connect a modulator or CF32LE file to the I/Q stream block");
    } else if (typeOf(result.modulator) == "lab_modulator") {
        const QString id = result.modulator.value("id").toString();
        result.carrier = input(id, DspLinkKind::Carrier);
        result.payload = input(id, DspLinkKind::Payload);
        if (result.carrier.isEmpty()) result.error = QStringLiteral("Connect a carrier to the modulator");
    }
    return result;
}

SignalLabGenerationResult generateDspBoardSignal(const DspBoardSignalRequest &request) {
    if (!request.error.isEmpty()) return failure(request.error);
    const auto iq = settingsOf(request.iq);
    const int rate = iq.value("rate").toInt(48000);
    const double duration = iq.value("duration").toDouble(1.0);
    if (rate < 8000 || rate > 2000000 || !(duration > 0.0) || duration > 30.0 ||
        !(request.centerHz() >= 0.0 && request.centerHz() <= 8000e6))
        return failure(QStringLiteral("Invalid I/Q rate, duration or RF center"));
    if (typeOf(request.modulator) == "lab_cf32") {
        auto result = loadIq(settingsOf(request.modulator).value("path").toString(), rate);
        if (result.ok()) {
            result.durationSeconds = double(result.iq.size()) / rate;
            result.summary = QStringLiteral("CF32LE | %1 samples | %2 samples/s").arg(result.iq.size()).arg(rate);
        }
        return result;
    }
    const auto carrier = settingsOf(request.carrier);
    const auto mod = settingsOf(request.modulator);
    const auto payload = settingsOf(request.payload);
    const auto payloadType = typeOf(request.payload);
    const QString mode = mod.value("mode").toString("carrier");
    SignalLabConfiguration c;
    c.sampleRate = rate;
    c.durationSeconds = duration;
    c.centerOffsetHz = carrier.value("offset").toDouble();
    c.amplitude = carrier.value("amplitude").toDouble(0.7);
    c.toneFrequencyHz = mod.value("tone").toDouble(1000.0);
    c.symbolRate = mod.value("symbols").toDouble(1200.0);
    c.fskDeviationHz = mod.value("deviation").toDouble(2500.0);
    if (std::llround(rate * duration) < 1 || std::llround(rate * duration) > MaxSamples ||
        !(c.amplitude > 0.0 && c.amplitude <= 1.0) || std::abs(c.centerOffsetHz) >= rate * 0.5)
        return failure(QStringLiteral("I/Q generation requires up to 2 million samples, valid amplitude and a carrier inside Nyquist"));
    if (mode == "am" || mode == "fm") {
        if (payloadType != "lab_audio") return failure(QStringLiteral("AM and FM require a WAV audio source"));
        if (!(c.fskDeviationHz > 0.0 && c.fskDeviationHz < rate * 0.5))
            return failure(QStringLiteral("Invalid FM deviation"));
        auto result = modulateWav(payload.value("path").toString(), c, mode == "fm");
        if (result.error.isEmpty() && result.iq.isEmpty()) return failure(QStringLiteral("The WAV source has no usable audio"));
        if (result.ok()) {
            result.durationSeconds = double(result.iq.size()) / rate;
            result.summary = QStringLiteral("%1 WAV | %2 samples | %3 samples/s").arg(mode.toUpper()).arg(result.iq.size()).arg(rate);
        }
        return result;
    }
    if (mode == "carrier" || mode == "tone" || mode == "noise") {
        if (!payloadType.isEmpty()) return failure(QStringLiteral("Carrier, tone and noise use the carrier input only"));
        if (mode == "tone") {
            c.waveform = SignalLabWaveform::Sine;
            if (!(c.toneFrequencyHz > 0.0 && c.toneFrequencyHz + std::abs(c.centerOffsetHz) < rate * 0.5))
                return failure(QStringLiteral("Tone and carrier offset must fit inside Nyquist"));
        } else if (mode == "noise") {
            c.waveform = SignalLabWaveform::WhiteNoise;
        }
        return SignalLabGenerator::generate(c);
    }
    const QHash<QString, QPair<SignalLabWaveform, int>> digital = {
        {"bpsk", {SignalLabWaveform::Bpsk, 1}}, {"qpsk", {SignalLabWaveform::Qpsk, 2}},
        {"packet_qpsk", {SignalLabWaveform::Qpsk, 2}},
        {"qam16", {SignalLabWaveform::Qam16, 4}}, {"fsk", {SignalLabWaveform::Fsk2, 1}},
        {"gfsk", {SignalLabWaveform::Gfsk2, 1}}
    };
    if (!digital.contains(mode)) return failure(QStringLiteral("Unsupported modulation"));
    if (payloadType == "lab_audio") return failure(QStringLiteral("Connect text, image or file bytes for digital modulation"));
    c.waveform = digital.value(mode).first;
    const bool packet = mode == "packet_qpsk";
    if (packet && payloadType.isEmpty())
        return failure(QStringLiteral("Packet QPSK requires a text, image or file source"));
    if (!payloadType.isEmpty()) {
        if (!(c.symbolRate >= 1.0 && c.symbolRate <= rate * 0.45))
            return failure(QStringLiteral("Invalid symbol rate"));
        const int samplesPerSymbol = int(std::llround(rate / c.symbolRate));
        int capacity = std::max(0, (int(std::llround(rate * duration)) / samplesPerSymbol - 8) *
                                digital.value(mode).second / 8);
        QByteArray name;
        DspBoardPayloadType kind = DspBoardPayloadType::Text;
        if (packet) {
            if (payloadType != "lab_text") {
                kind = payloadType == "lab_image" ? DspBoardPayloadType::Image : DspBoardPayloadType::File;
                name = QFileInfo(payload.value("path").toString()).fileName().toUtf8();
                if (name.size() > 255) name.clear();
            }
            capacity -= 1 + DspBoardQpsk::preamble().size() + DspBoardQpsk::HeaderBytes + PacketTagBytes + name.size();
            if (capacity < 1) return failure(QStringLiteral("No room for packet data. Increase duration or symbol rate"));
        }
        if (payloadType == "lab_text") c.payload = payload.value("text").toString().toUtf8();
        else {
            QString error;
            c.payload = readBounded(payload.value("path").toString(), capacity, error);
            if (!error.isEmpty()) return failure(error);
        }
        if (c.payload.isEmpty() || c.payload.size() > capacity)
            return failure(QStringLiteral("Payload is empty or does not fit. Increase duration or symbol rate"));
        if (packet) c.payload = encodePacket(kind, name, c.payload);
    }
    return SignalLabGenerator::generate(c);
}

DspBoardDecodedPayload decodeDspBoardPacket(const SignalLabGenerationResult &signal) {
    using namespace DspBoardQpsk;
    int offset = 0;
    auto header = readQpskBytes(signal, offset, HeaderBytes);
    if (!header.startsWith(PacketMagic)) {
        // Legacy local bursts begin with the header. Live bursts prepend training.
        offset = preamble().size();
        header = readQpskBytes(signal, offset, HeaderBytes);
    }
    const int length = bodyBytes(header);
    return decodeFrame(header, length > 0 ? readQpskBytes(signal, offset + HeaderBytes, length) : QByteArray{});
}

namespace DspBoardQpsk {
const QByteArray &preamble() {
    static const QByteArray bytes = QByteArray::fromHex("9d06f3a8c157e42bc6b9d28f03ac517e");
    return bytes;
}

unsigned symbolBits(std::complex<float> sample) {
    const bool lower = sample.imag() < 0;
    const bool left = sample.real() < 0;
    return (lower ? 2U : 0U) | unsigned(lower != left);
}

int bodyBytes(const QByteArray &header) {
    if (header.size() != HeaderBytes || !header.startsWith(PacketMagic)) return 0;
    const int kind = quint8(header.at(4));
    const int nameSize = quint8(header.at(5));
    const quint32 size = qFromLittleEndian<quint32>(header.constData() + 6);
    if (kind < int(DspBoardPayloadType::Text) || kind > int(DspBoardPayloadType::File) ||
        size == 0 || size > quint32(MaxSamples / SymbolsPerByte) ||
        (kind == int(DspBoardPayloadType::Text) && nameSize != 0)) return 0;
    return nameSize + int(size) + PacketTagBytes;
}

DspBoardDecodedPayload decodeFrame(const QByteArray &header, const QByteArray &body) {
    const auto fail = [](const QString &error) {
        DspBoardDecodedPayload result;
        result.error = error;
        return result;
    };
    if (!header.startsWith(PacketMagic))
        return fail(QStringLiteral("No packet found. Select Packet QPSK on the modulator"));
    const int expected = bodyBytes(header);
    if (expected == 0) return fail(QStringLiteral("Invalid packet header"));
    if (body.size() != expected) return fail(QStringLiteral("Incomplete packet"));
    const auto data = body.left(expected - PacketTagBytes);
    if (packetTag(header + data) != body.right(PacketTagBytes))
        return fail(QStringLiteral("Packet integrity check failed"));
    const int nameSize = quint8(header.at(5));
    DspBoardDecodedPayload result;
    result.type = DspBoardPayloadType(quint8(header.at(4)));
    result.fileName = QFileInfo(QString::fromUtf8(body.constData(), nameSize)).fileName();
    result.bytes = data.mid(nameSize);
    return result;
}
}
