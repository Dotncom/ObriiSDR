#include "meshwiremodem.h"
#include "meshiqmodem.h"
#include "../lora/loraphy.h"
#include "../openradio/ax25afsk1200.h"
#include "../openradio/ax25codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Mesh {
namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr double Deviation = 3000.0;
constexpr quint64 MaximumSamples = 2u * 1024 * 1024;
constexpr int MaximumRate = 2000000;
constexpr int AfskMaximumBytes = 768, LoRaMaximumBytes = 255;
constexpr int PreambleFlags = 24, ChunkSamples = 1024, MaximumReturnedFrames = 256;

bool fail(QString *error, const QString &message) {
    if (error) *error = message.left(256);
    return false;
}
void add(quint64 &counter, quint64 amount = 1) {
    counter += (std::min)(amount, (std::numeric_limits<quint64>::max)() - counter);
}
struct Configuration {
    QJsonObject canonical;
    obrii::lora::Config lora;
    bool isLoRa = false;
    double amplitude = 0.35;
    int rate = IqModem::Rate;
    quint64 receiverLimit = IqModem::Rate * 10;
    quint64 quietTail = IqModem::Rate / 3;
};

quint64 estimate(const Configuration &c, int bytes) {
    if (bytes < 0 || bytes > (c.isLoRa ? LoRaMaximumBytes : AfskMaximumBytes)) return 0;
    quint64 samples;
    if (c.isLoRa) {
        const quint64 n = c.lora.samplesPerSymbol();
        const quint64 symbols = obrii::lora::PacketCodec(c.lora).symbolCount(bytes);
        // Modem::modulateSymbols: preamble + two sync + 2.25 SFD + payload.
        samples = (c.lora.preambleSymbols + 4 + symbols) * n + n / 4 + c.quietTail;
    } else {
        // Two seven-byte addresses, control/PID, FCS, worst-case HDLC stuffing.
        const quint64 bits = quint64(bytes + 18) * 8;
        const quint64 stuffed = bits + bits / 5 + (PreambleFlags + 3) * 8;
        samples = stuffed * (IqModem::Rate / 1200) + c.quietTail;
        samples = ((samples + 1023) / 1024) * 1024;
        if (samples > quint64(IqModem::Rate) * 10) return 0;
    }
    if (samples > MaximumSamples || samples > quint64(c.rate) * 120) return 0;
    return samples;
}

bool validate(const QJsonObject &input, Configuration *result, QString *error) {
    Configuration c;
    const auto type = input.value(QStringLiteral("type"));
    if (!type.isUndefined() && (!type.isString() ||
        (type.toString() != QLatin1String("lora") && type.toString() != QLatin1String("afsk1200"))))
        return fail(error, QStringLiteral("Modem type must be 'afsk1200' or 'lora'"));
    c.isLoRa = type.toString() == QLatin1String("lora");
    for (auto it = input.constBegin(); it != input.constEnd(); ++it) {
        const auto &key = it.key();
        const bool common = key == QLatin1String("type") || key == QLatin1String("amplitude");
        const bool lora = key == QLatin1String("sf") || key == QLatin1String("bw") ||
            key == QLatin1String("cr") || key == QLatin1String("os") ||
            key == QLatin1String("preamble") || key == QLatin1String("syncWord") ||
            key == QLatin1String("invertIq") || key == QLatin1String("payloadCrc") || key == QLatin1String("ldro");
        if (!common && !(c.isLoRa && lora))
            return fail(error, QStringLiteral("Unknown %1 modem field '%2'")
                .arg(c.isLoRa ? QStringLiteral("LoRa") : QStringLiteral("AFSK1200"), key.left(48)));
    }
    c.amplitude = c.isLoRa ? 0.7 : 0.35;
    if (input.contains(QStringLiteral("amplitude"))) {
        const auto value = input.value(QStringLiteral("amplitude"));
        if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
            value.toDouble() <= 0 || value.toDouble() > 1)
            return fail(error, QStringLiteral("Modem amplitude must be a finite number in (0,1]"));
        c.amplitude = value.toDouble();
        if (float(c.amplitude) == 0)
            return fail(error, QStringLiteral("Modem amplitude is too small for CF32"));
    }
    c.canonical = {{"type", c.isLoRa ? "lora" : "afsk1200"}, {"amplitude", c.amplitude}};
    if (c.isLoRa) {
        auto integer = [&](const char *key, int &target, int low, int high) {
            const auto value = input.value(QLatin1String(key));
            if (value.isUndefined()) return true;
            const double n = value.toDouble();
            if (!value.isDouble() || !std::isfinite(n) || n < low || n > high || std::floor(n) != n)
                return fail(error, QStringLiteral("LoRa %1 must be an integer in %2..%3")
                    .arg(QLatin1String(key)).arg(low).arg(high));
            target = int(n);
            return true;
        };
        int sync = c.lora.syncWord;
        if (!integer("sf", c.lora.spreadingFactor, 7, 12) ||
            !integer("bw", c.lora.bandwidthHz, 31250, 500000) ||
            !integer("cr", c.lora.codingRate, 1, 4) ||
            !integer("os", c.lora.oversampling, 1, 8) ||
            !integer("preamble", c.lora.preambleSymbols, 6, 1024) ||
            !integer("syncWord", sync, 0, 255)) return false;
        if (c.lora.bandwidthHz != 31250 && c.lora.bandwidthHz != 62500 &&
            c.lora.bandwidthHz != 125000 && c.lora.bandwidthHz != 250000 && c.lora.bandwidthHz != 500000)
            return fail(error, QStringLiteral("LoRa bw must be 31250, 62500, 125000, 250000 or 500000 Hz"));
        c.lora.syncWord = std::uint8_t(sync);
        if (input.contains(QStringLiteral("invertIq"))) {
            if (!input.value(QStringLiteral("invertIq")).isBool())
                return fail(error, QStringLiteral("LoRa invertIq must be a boolean"));
            c.lora.invertIq = input.value(QStringLiteral("invertIq")).toBool();
        }
        if (input.contains(QStringLiteral("payloadCrc"))) {
            if (!input.value(QStringLiteral("payloadCrc")).isBool())
                return fail(error, QStringLiteral("LoRa payloadCrc must be a boolean"));
            c.lora.payloadCrc = input.value(QStringLiteral("payloadCrc")).toBool();
        }
        QString ldro = QStringLiteral("auto");
        if (input.contains(QStringLiteral("ldro"))) {
            const auto value = input.value(QStringLiteral("ldro"));
            if (!value.isString() || (value.toString() != QLatin1String("auto") &&
                value.toString() != QLatin1String("on") && value.toString() != QLatin1String("off")))
                return fail(error, QStringLiteral("LoRa ldro must be 'auto', 'on' or 'off'"));
            ldro = value.toString();
        }
        c.lora.ldro = ldro == QLatin1String("on") ? obrii::lora::Ldro::On :
            ldro == QLatin1String("off") ? obrii::lora::Ldro::Off : obrii::lora::Ldro::Auto;
        try {
            // Config and PacketCodec validate/count only; neither allocates waveform/FFT state.
            c.lora.validate();
            c.rate = c.lora.sampleRate();
            if (c.rate > MaximumRate)
                return fail(error, QStringLiteral("LoRa sample rate BW * OS exceeds 2000000 samples/s"));
            const quint64 n = c.lora.samplesPerSymbol();
            c.quietTail = 2 * n;
            auto maximum = c.lora;
            maximum.codingRate = 4;
            maximum.payloadCrc = true;
            // Keep this in step with Receiver's 255-byte, CR4 acquisition/buffer limit.
            c.receiverLimit = (c.lora.preambleSymbols + 17 +
                quint64(obrii::lora::PacketCodec(maximum).symbolCount(255))) * n;
            if (c.receiverLimit > MaximumSamples || c.receiverLimit > quint64(c.rate) * 120)
                return fail(error, QStringLiteral("LoRa maximum-packet RX buffer exceeds 2097152 CF32 samples or 120 seconds; reduce SF, OS or preamble"));
            if (!estimate(c, LoRaMaximumBytes))
                return fail(error, QStringLiteral("LoRa maximum TX burst exceeds 2097152 CF32 samples or 120 seconds"));
        } catch (const std::invalid_argument &e) {
            return fail(error, QString::fromLatin1(e.what()));
        }
        c.canonical.insert(QStringLiteral("sf"), c.lora.spreadingFactor);
        c.canonical.insert(QStringLiteral("bw"), c.lora.bandwidthHz);
        c.canonical.insert(QStringLiteral("cr"), c.lora.codingRate);
        c.canonical.insert(QStringLiteral("os"), c.lora.oversampling);
        c.canonical.insert(QStringLiteral("preamble"), c.lora.preambleSymbols);
        c.canonical.insert(QStringLiteral("syncWord"), sync);
        c.canonical.insert(QStringLiteral("invertIq"), c.lora.invertIq);
        c.canonical.insert(QStringLiteral("payloadCrc"), c.lora.payloadCrc);
        c.canonical.insert(QStringLiteral("ldro"), ldro);
    }
    if (result) *result = std::move(c);
    if (error) error->clear();
    return true;
}
}

struct WireModem::Private {
    explicit Private(Configuration c) : config(std::move(c)) {
        if (config.isLoRa) {
            modem = std::make_unique<obrii::lora::Modem>(config.lora);
            receiver = std::make_unique<obrii::lora::Receiver>(config.lora);
        }
    }
    Configuration config;
    IqReceiver afsk;
    std::unique_ptr<obrii::lora::Modem> modem;
    std::unique_ptr<obrii::lora::Receiver> receiver;
    quint64 decoded = 0, fcs = 0, bad = 0, resets = 0, invalidSamples = 0, dropped = 0;
    quint64 txBursts = 0, txSamples = 0, rxSamples = 0;
    quint64 quiet = 0, withoutPacket = 0;
    bool burst = false, deliveredInBurst = false;

    void reset(bool counted = true) {
        if (receiver) receiver->reset();
        else afsk.reset();
        quiet = withoutPacket = 0;
        burst = deliveredInBurst = false;
        if (counted) add(resets);
    }
    void accept(const QByteArray &wire, QList<QByteArray> &out) {
        if (wire.size() > (config.isLoRa ? LoRaMaximumBytes : AfskMaximumBytes)) {
            add(bad);
            add(dropped);
            return;
        }
        add(decoded);
        deliveredInBurst = true;
        withoutPacket = 0;
        // Arbitrarily large input chunks must not create unbounded returned lists.
        if (out.size() < MaximumReturnedFrames) out.append(wire);
        else add(dropped);
    }
    void pushAfsk(const float *iq, int count, QList<QByteArray> &out) {
        const auto oldFcs = afsk.validFrames, oldBad = afsk.badBursts;
        const auto frames = afsk.append(iq, count);
        add(fcs, afsk.validFrames - oldFcs);
        add(bad, afsk.badBursts - oldBad);
        for (const auto &wire : frames) accept(wire, out);
    }
    void pushLoRa(const std::complex<float> *iq, int count, QList<QByteArray> &out) {
        if (!count) return;
        try {
            for (const auto &r : receiver->push(iq, std::size_t(count))) {
                if (config.lora.payloadCrc && !r.packet.crcChecked) { add(bad); continue; }
                if (r.packet.crcChecked) add(fcs);
                const auto &bytes = r.packet.payload;
                accept(QByteArray(reinterpret_cast<const char *>(bytes.data()), int(bytes.size())), out);
            }
        } catch (const std::exception &) {
            add(bad);
            reset();
        }
    }
};

WireModem::WireModem(const QJsonObject &configuration) {
    Configuration config;
    QString error;
    if (!validate(configuration, &config, &error)) throw std::invalid_argument(error.toStdString());
    d = std::make_unique<Private>(std::move(config));
}
WireModem::~WireModem() = default;
WireModem::WireModem(WireModem &&) noexcept = default;
WireModem &WireModem::operator=(WireModem &&) noexcept = default;

bool WireModem::normalize(const QJsonObject &input, QJsonObject *output, QString *error) {
    Configuration config;
    if (!validate(input, &config, error)) return false;
    if (output) *output = std::move(config.canonical);
    return true;
}
int WireModem::sampleRate() const { return d ? d->config.rate : 0; }
int WireModem::maximumWireBytes() const { return d ? (d->config.isLoRa ? LoRaMaximumBytes : AfskMaximumBytes) : 0; }
quint64 WireModem::estimatedSamples(int bytes) const { return d ? estimate(d->config, bytes) : 0; }

QVector<std::complex<float>> WireModem::modulate(const QByteArray &wire, QString *error) {
    if (!d) { fail(error, QStringLiteral("Modem has been moved from")); return {}; }
    if (wire.size() > maximumWireBytes()) {
        fail(error, QStringLiteral("Native wire payload exceeds %1 bytes").arg(maximumWireBytes()));
        return {};
    }
    const auto bound = estimatedSamples(int(wire.size()));
    if (!bound) { fail(error, QStringLiteral("Modem burst exceeds sample or duration budget")); return {}; }
    QVector<std::complex<float>> output;
    try {
        if (d->config.isLoRa) {
            const auto *begin = reinterpret_cast<const std::uint8_t *>(wire.constData());
            const auto iq = d->modem->modulate(obrii::lora::Bytes(begin, begin + wire.size()), float(d->config.amplitude));
            if (iq.size() + d->config.quietTail > bound)
                throw std::length_error("LoRa waveform exceeded its preflight estimate");
            output.reserve(int(bound));
            for (const auto &sample : iq) output.append(sample);
            output.resize(int(bound));
        } else {
            Ax25UiFrame frame;
            frame.source.callsign = frame.destination.callsign = QStringLiteral("OBRII");
            frame.information = wire;
            const auto bytes = Ax25Codec::encodeUiFrame(frame, false, error);
            if (bytes.isEmpty()) return {};
            const auto audio = Ax25Afsk1200::modulate(bytes, IqModem::Rate, PreambleFlags, error);
            if (audio.isEmpty()) return {};
            const quint64 length = ((quint64(audio.size()) + d->config.quietTail + 1023) / 1024) * 1024;
            if (length > bound) throw std::length_error("AFSK waveform exceeded its preflight estimate");
            output.reserve(int(length));
            double phase = 0;
            for (const auto sample : audio) {
                phase = std::remainder(phase + 2 * Pi * Deviation * sample / IqModem::Rate, 2 * Pi);
                output.append({float(d->config.amplitude * std::cos(phase)), float(d->config.amplitude * std::sin(phase))});
            }
            output.resize(int(length));
        }
    } catch (const std::exception &e) {
        fail(error, QString::fromLatin1(e.what()));
        return {};
    }
    add(d->txBursts);
    add(d->txSamples, quint64(output.size()));
    if (error) error->clear();
    return output;
}

QList<QByteArray> WireModem::receive(const float *iq, int count, bool discontinuity) {
    QList<QByteArray> output;
    if (!d) return output;
    if (discontinuity) d->reset();
    if (count < 0 || (!iq && count)) { add(d->bad); d->reset(); return output; }
    if (!count) return output;
    add(d->rxSamples, quint64(count));
    if (!d->config.isLoRa) {
        std::array<float, 2 * ChunkSamples> chunk;
        int used = 0;
        auto flush = [&] { if (used) d->pushAfsk(chunk.data(), used, output); used = 0; };
        for (int i = 0; i < count; ++i) {
            const auto at = std::size_t(i) * 2;
            const float real = iq[at], imag = iq[at + 1];
            if (!std::isfinite(real) || !std::isfinite(imag)) {
                flush(); add(d->invalidSamples); add(d->bad); d->reset(); continue;
            }
            // Preserve phase and exact zero-IQ tails, without a fixed receive-gain threshold.
            const double amplitude = std::hypot(double(real), double(imag));
            chunk[2 * used] = amplitude ? float(real / amplitude) : 0;
            chunk[2 * used + 1] = amplitude ? float(imag / amplitude) : 0;
            if (++used == ChunkSamples) flush();
        }
        flush();
    } else {
        std::array<std::complex<float>, ChunkSamples> chunk;
        int used = 0;
        auto flush = [&] { d->pushLoRa(chunk.data(), used, output); used = 0; };
        for (int i = 0; i < count; ++i) {
            const auto at = std::size_t(i) * 2;
            const float real = iq[at], imag = iq[at + 1];
            if (!std::isfinite(real) || !std::isfinite(imag)) {
                flush(); add(d->invalidSamples); add(d->bad); d->reset(); continue;
            }
            const bool signal = real != 0 || imag != 0;
            if (!d->burst && !signal) continue;
            d->burst = true;
            d->quiet = signal ? 0 : d->quiet + 1;
            ++d->withoutPacket;
            chunk[used++] = {real, imag};
            if (used == ChunkSamples) flush();
            if (d->quiet >= d->config.quietTail) {
                flush();
                // The PHY silently rejects bad FEC/CRC; count undecoded, quiet-ended bursts.
                if (!d->deliveredInBurst) add(d->bad);
                d->reset(false);
            } else if (d->withoutPacket >= d->config.receiverLimit) {
                flush();
                if (d->withoutPacket >= d->config.receiverLimit) { add(d->bad); d->reset(); }
            }
        }
        flush();
    }
    return output;
}
void WireModem::reset() { if (d) d->reset(); }

QJsonObject WireModem::status() const {
    if (!d) return {{"active", false}};
    // FCS counts successful integrity checks; bad counts failed/over-budget bursts,
    // not exact PHY CRC failures (the primitive does not expose those).
    // Counters are strings to preserve all 64 bits through JSON consumers.
    return {{"active", true}, {"configuration", d->config.canonical},
        {"sampleRate", sampleRate()}, {"maximumWireBytes", maximumWireBytes()},
        {"maximumSamples", int(MaximumSamples)}, {"receiverLimitSamples", int(d->config.receiverLimit)},
        {"bufferedSamples", d->receiver ? QJsonValue(int(d->receiver->bufferedSamples())) : QJsonValue()},
        {"decoded", QString::number(d->decoded)}, {"fcs", QString::number(d->fcs)},
        {"bad", QString::number(d->bad)}, {"resets", QString::number(d->resets)},
        {"invalidSamples", QString::number(d->invalidSamples)}, {"dropped", QString::number(d->dropped)},
        {"txBursts", QString::number(d->txBursts)}, {"txSamples", QString::number(d->txSamples)},
        {"rxSamples", QString::number(d->rxSamples)}};
}
}
