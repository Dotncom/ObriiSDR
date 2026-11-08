#include "meshwiremodem.h"
#include "meshiqmodem.h"
#include "../lora/loraphy.h"
#include "../openradio/ax25afsk1200.h"
#include "../openradio/ax25codec.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using Mesh::WireModem;
using Iq = QVector<std::complex<float>>;
constexpr double Pi = 3.14159265358979323846;
constexpr quint64 SampleBudget = 2u * 1024 * 1024;

double cpuMilliseconds() {
#ifdef Q_OS_WIN
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw std::runtime_error("Cannot read process CPU time");
    const quint64 kernelTicks = (quint64(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
    const quint64 userTicks = (quint64(user.dwHighDateTime) << 32) | user.dwLowDateTime;
    return double(kernelTicks + userTicks) / 10000;
#else
    const auto ticks = std::clock();
    if (ticks == std::clock_t(-1)) throw std::runtime_error("Cannot read process CPU time");
    return 1000.0 * ticks / CLOCKS_PER_SEC;
#endif
}

void check(bool ok, const char *name) {
    if (!ok) throw std::runtime_error(name);
    std::printf("PASS %s\n", name);
    std::fflush(stdout);
}
quint64 counter(const WireModem &modem, const char *key) {
    return modem.status().value(QLatin1String(key)).toString().toULongLong();
}
QJsonObject fastLoRa() {
    return {{"type", "lora"}, {"sf", 7}, {"bw", 125000}, {"cr", 1}, {"os", 1},
        {"preamble", 16}, {"syncWord", 43}, {"invertIq", false}};
}
QByteArray nativeBytes(int count) {
    QByteArray wire(count, '\0');
    for (int i = 0; i < count; ++i) wire[i] = char((i * 79 + 137) & 255);
    if (count) wire[0] = '\0';
    if (count > 1) wire[1] = char(0xff);
    return wire;
}
QList<QByteArray> feed(WireModem &modem, const Iq &iq, int chunkSize,
                      float scale = 1, bool discontinuity = false) {
    QList<QByteArray> received;
    QVector<float> chunk(2 * chunkSize);
    for (int at = 0; at < iq.size();) {
        const int count = (std::min)(chunkSize, int(iq.size()) - at);
        for (int i = 0; i < count; ++i) {
            chunk[2 * i] = iq[at + i].real() * scale;
            chunk[2 * i + 1] = iq[at + i].imag() * scale;
        }
        received.append(modem.receive(chunk.constData(), count, discontinuity));
        discontinuity = false;
        at += count;
    }
    return received;
}
Iq fm(const QVector<float> &audio) {
    Iq iq;
    iq.reserve(int(audio.size()) + Mesh::IqModem::Rate / 3 + 1024);
    double phase = 0;
    for (const auto sample : audio) {
        phase = std::remainder(phase + 2 * Pi * 3000 * sample / Mesh::IqModem::Rate, 2 * Pi);
        iq.append(std::complex<float>(float(0.35 * std::cos(phase)), float(0.35 * std::sin(phase))));
    }
    iq.resize(((int(iq.size()) + Mesh::IqModem::Rate / 3 + 1023) / 1024) * 1024);
    return iq;
}
QVector<float> afskAudio(const QByteArray &wire) {
    Ax25UiFrame frame;
    frame.source.callsign = frame.destination.callsign = QStringLiteral("OBRII");
    frame.information = wire;
    return Ax25Afsk1200::modulate(Ax25Codec::encodeUiFrame(frame), Mesh::IqModem::Rate, 24);
}

void configurations() {
    QString error = QStringLiteral("old error");
    QJsonObject canonical, again;
    check(WireModem::normalize({}, &canonical, &error) && error.isEmpty() &&
        canonical.value("type") == "afsk1200" && canonical.value("amplitude").toDouble() == 0.35,
        "empty configuration canonically selects legacy AFSK1200");
    check(WireModem::normalize(canonical, &again, &error) && canonical == again,
        "AFSK normalization is idempotent");
    check(WireModem::normalize({{"type", "lora"}}, &canonical, &error) &&
        canonical.value("sf").toInt() == 11 && canonical.value("bw").toInt() == 250000 &&
        canonical.value("os").toInt() == 2 && canonical.value("ldro") == "auto" &&
        canonical.value("amplitude").toDouble() == 0.7,
        "LoRa defaults match the independent module's SF11 configuration");
    check(WireModem::normalize(canonical, &again, &error) && canonical == again,
        "LoRa normalization is idempotent");
    check(WireModem::normalize(fastLoRa(), nullptr, &error), "validation can omit canonical output");

    QList<QJsonObject> invalid{
        {{"type", "unknown"}}, {{"type", true}}, {{"type", QJsonValue::Null}},
        {{"sf", 7}}, {{"sampleRate", 48000}}, {{"amplitude", 0}}, {{"amplitude", 1.01}},
        {{"amplitude", "0.35"}}, {{"amplitude", true}}, {{"amplitude", 1e-100}},
        {{"amplitude", (std::numeric_limits<double>::infinity)()}},
        {{"amplitude", (std::numeric_limits<double>::quiet_NaN)()}}
    };
    const QList<QPair<QString, QJsonValue>> badLoRa{
        {"sf", 6}, {"sf", 13}, {"sf", 7.5}, {"sf", "7"}, {"sf", true}, {"sf", 1e300},
        {"bw", 100000}, {"cr", 0}, {"cr", 5}, {"os", 3}, {"os", 16},
        {"preamble", 5}, {"preamble", 1025}, {"syncWord", -1}, {"syncWord", 256},
        {"invertIq", 1}, {"invertIq", "false"}, {"ldro", "yes"}, {"ldro", true},
        {"explicitHeader", false}, {"payloadCrc", 1}, {QString(1000, 'x'), 1},
        {"sf", QJsonArray{7}}
    };
    for (const auto &field : badLoRa) {
        auto c = fastLoRa();
        c.insert(field.first, field.second);
        invalid.append(c);
    }
    invalid.append({{"type", "lora"}, {"sf", 12}, {"bw", 31250}, {"os", 8}});
    invalid.append({{"type", "lora"}, {"sf", 7}, {"bw", 500000}, {"os", 8}});
    invalid.append({{"type", "lora"}, {"sf", 12}, {"os", 1}, {"preamble", 1024}});
    for (const auto &c : invalid) {
        QJsonObject untouched{{"sentinel", true}};
        if (WireModem::normalize(c, &untouched, &error) || error.isEmpty() || error.size() > 256 ||
            untouched != QJsonObject{{"sentinel", true}})
            throw std::runtime_error("Invalid configuration must fail with a bounded error and no partial output");
        bool rejected = false;
        try { WireModem modem(c); } catch (const std::invalid_argument &) { rejected = true; }
        if (!rejected) throw std::runtime_error("Constructor must reject invalid configuration before creating DSP state");
    }
    check(true, "invalid configurations fail canonically in normalize and constructor, with bounded errors");
    auto capped = fastLoRa();
    capped.insert("bw", 500000);
    capped.insert("os", 4);
    WireModem atCap(capped);
    check(atCap.sampleRate() == 2000000, "sample rate at exactly 2 MS/s is allowed");
    for (const char *ldro : {"auto", "on", "off"}) {
        auto c = fastLoRa();
        c.insert("ldro", ldro);
        check(WireModem::normalize(c, &canonical, &error) && canonical.value("ldro") == QLatin1String(ldro),
            "LDRO selection is explicit and canonical");
    }
    // Exercise the cheap preflight across PHY extremes, without instantiating Modem/Receiver.
    int accepted = 0, rejected = 0;
    for (int sf = 7; sf <= 12; ++sf) for (int bw : {31250, 62500, 125000, 250000, 500000})
        for (int os : {1, 2, 4, 8}) for (const char *ldro : {"auto", "on", "off"}) {
            QJsonObject c{{"type", "lora"}, {"sf", sf}, {"bw", bw}, {"os", os}, {"ldro", ldro}};
            if (!WireModem::normalize(c, &canonical, &error)) { ++rejected; continue; }
            ++accepted;
            obrii::lora::Config phy;
            phy.spreadingFactor = sf; phy.bandwidthHz = bw; phy.oversampling = os;
            phy.ldro = QString::fromLatin1(ldro) == QLatin1String("on") ? obrii::lora::Ldro::On :
                QString::fromLatin1(ldro) == QLatin1String("off") ? obrii::lora::Ldro::Off : obrii::lora::Ldro::Auto;
            phy.codingRate = 4;
            const quint64 limit = (phy.preambleSymbols + 17 +
                quint64(obrii::lora::PacketCodec(phy).symbolCount(255))) * phy.samplesPerSymbol();
            if (phy.sampleRate() > 2000000 || limit > SampleBudget || limit > quint64(phy.sampleRate()) * 120)
                throw std::runtime_error("Every accepted configuration must budget a 255-byte CR4 receiver");
        }
    check(accepted > 0 && rejected > 0, "preflight matrix budgets 255-byte CR4 receive buffers and rejects PHY extremes");
}

void roundtrip(const QJsonObject &config, int chunkSize, int bytes, float scale) {
    WireModem tx(config), rx(config);
    const auto wire = config.isEmpty() && bytes == 768 ? QByteArray(bytes, char(0xff)) : nativeBytes(bytes);
    QString error = QStringLiteral("stale");
    const auto iq = tx.modulate(wire, &error);
    check(!iq.isEmpty() && error.isEmpty() && quint64(iq.size()) <= tx.estimatedSamples(bytes) &&
        quint64(iq.size()) <= SampleBudget && iq.size() <= tx.sampleRate() * 120,
        "TX allocation and duration remain within preflight estimates");
    check(feed(rx, iq, chunkSize, scale) == QList<QByteArray>{wire},
        "segmented scaled CF32 preserves exact native bytes without an Obrii envelope");
    const quint64 expectedFcs = config.contains("payloadCrc") && !config.value("payloadCrc").toBool() ? 0 : 1;
    check(counter(rx, "decoded") == 1 && counter(rx, "fcs") == expectedFcs && counter(rx, "bad") == 0 &&
        counter(tx, "txBursts") == 1, "TX, decoded and successful FCS/CRC counters agree");
    check(tx.estimatedSamples(-1) == 0 && tx.estimatedSamples(tx.maximumWireBytes() + 1) == 0 &&
        tx.estimatedSamples((std::numeric_limits<int>::max)()) == 0,
        "invalid payload sizes have no sample estimate");
    check(tx.modulate(QByteArray(tx.maximumWireBytes() + 1, 'x'), &error).isEmpty() && !error.isEmpty() &&
        counter(tx, "txBursts") == 1, "oversized TX payload is rejected before waveform allocation");
}

void discontinuityAndFinite(const QJsonObject &config) {
    WireModem tx(config), rx(config);
    QString error;
    const QByteArray wire = nativeBytes(48);
    const auto iq = tx.modulate(wire, &error);
    const int split = int(iq.size()) / 2;
    check(feed(rx, iq.mid(0, split), 137).isEmpty() &&
        feed(rx, iq.mid(split), 1024, 1, true).isEmpty(), "a discontinuity cannot join two partial packets");
    check(counter(rx, "resets") >= 1 && feed(rx, iq, 137, 1, true) == QList<QByteArray>{wire},
        "receiver reacquires an intact packet after discontinuity reset");
    rx.reset();
    auto damaged = iq;
    damaged[split] = {(std::numeric_limits<float>::quiet_NaN)(), 0};
    check(feed(rx, damaged, 1024).isEmpty() && counter(rx, "invalidSamples") == 1,
        "non-finite IQ discards partial decoder state across arbitrary chunk boundaries");
    const float invalid[]{0, (std::numeric_limits<float>::infinity)(),
                          (std::numeric_limits<float>::infinity)(), 0};
    check(rx.receive(invalid, 2).isEmpty() && counter(rx, "invalidSamples") == 3,
        "both I and Q are checked for NaN and infinity");
    const auto resets = counter(rx, "resets");
    check(rx.receive(nullptr, 0).isEmpty() && counter(rx, "resets") == resets,
        "empty receives are inert");
    check(rx.receive(nullptr, 1).isEmpty() && rx.receive(nullptr, -1).isEmpty() &&
        counter(rx, "resets") == resets + 2, "invalid input pointers and negative counts fail safely");
    check(feed(rx, iq, 1024, 1, true) == QList<QByteArray>{wire},
        "finite packet reception recovers after invalid input");
}

void rejectionAndBounds() {
    WireModem afsk;
    auto audio = afskAudio(nativeBytes(96));
    // Invert three body-bit tones, leaving the original flags and FCS on air.
    // A multi-bit interval cannot be hidden by the demodulator's phase-offset search.
    const int length = Mesh::IqModem::Rate / 1200;
    for (int bit = 400; bit < 403; ++bit) {
        const int start = bit * length;
        auto energy = [&](double hz) {
            std::complex<double> sum = 0;
            for (int i = 0; i < length; ++i)
                sum += double(audio[start + i]) * std::polar(1.0, -2 * Pi * hz * i / Mesh::IqModem::Rate);
            return std::norm(sum);
        };
        const double otherTone = energy(1200) > energy(2200) ? 2200 : 1200;
        for (int i = 0; i < length; ++i)
            audio[start + i] = float(0.8 * std::sin(2 * Pi * otherTone * i / Mesh::IqModem::Rate));
    }
    check(Ax25Afsk1200::demodulate(audio, Mesh::IqModem::Rate).isEmpty(), "damaged AX.25 body fails the original FCS");
    check(feed(afsk, fm(audio), 137).isEmpty() && counter(afsk, "bad") > 0 && counter(afsk, "fcs") == 0,
        "AFSK bad-FCS waveform cannot deliver native bytes");
    const auto oversized = fm(afskAudio(nativeBytes(769)));
    check(feed(afsk, oversized, 1024).isEmpty() && counter(afsk, "dropped") > 0,
        "valid AX.25 FCS does not bypass the 768-byte receive limit");

    obrii::lora::Config phy;
    phy.spreadingFactor = 7; phy.bandwidthHz = 125000; phy.oversampling = 1;
    obrii::lora::PacketCodec codec(phy);
    const auto wire = nativeBytes(16);
    obrii::lora::Bytes bytes(reinterpret_cast<const std::uint8_t *>(wire.constData()),
        reinterpret_cast<const std::uint8_t *>(wire.constData()) + wire.size());
    auto symbols = codec.encode(bytes);
    bytes[0] ^= 1;
    const auto changed = codec.encode(bytes);
    // A whole, valid FEC block changes the payload while preserving the old CRC block.
    std::copy(changed.begin() + 8, changed.begin() + 13, symbols.begin() + 8);
    bool crcFailure = false;
    try { codec.decode(symbols); }
    catch (const std::runtime_error &e) { crcFailure = QString::fromLatin1(e.what()).contains(QLatin1String("CRC")); }
    check(crcFailure, "negative LoRa fixture has valid FEC but a mismatched payload CRC");
    const auto burst = obrii::lora::Modem(phy).modulateSymbols(symbols);
    Iq bad;
    bad.reserve(int(burst.size()) + 2 * int(phy.samplesPerSymbol()));
    for (const auto &sample : burst) bad.append(sample);
    bad.resize(int(burst.size()) + 2 * int(phy.samplesPerSymbol()));
    WireModem lora(fastLoRa());
    check(feed(lora, bad, 137).isEmpty() && counter(lora, "bad") > 0 && counter(lora, "decoded") == 0,
        "LoRa CRC failure is rejected by the streaming wire receiver");
    phy.payloadCrc = false;
    const auto unchecked = obrii::lora::Modem(phy).modulate(bytes);
    Iq noCrc;
    for (const auto &sample : unchecked) noCrc.append(sample);
    noCrc.resize(int(unchecked.size()) + 2 * int(phy.samplesPerSymbol()));
    check(feed(lora, noCrc, 1024).isEmpty() && counter(lora, "fcs") == 0,
        "an explicit header cannot disable integrity checking for the wire transport");

    for (const QJsonObject &config : {QJsonObject{}, fastLoRa()}) {
        WireModem rx(config);
        std::mt19937 random(501);
        std::normal_distribution<float> normal(0, 0.1f);
        Iq noise(rx.sampleRate() / 2);
        for (auto &sample : noise) sample = {normal(random), normal(random)};
        noise.resize(int(noise.size()) + rx.sampleRate() / 2);
        check(feed(rx, noise, 1024).isEmpty() && counter(rx, "decoded") == 0 && counter(rx, "bad") > 0,
            "deterministic noise never bypasses FCS/CRC validation");
        const auto limit = rx.status().value("receiverLimitSamples").toInt();
        Iq carrier(1024, {0.3f, 0});
        for (int samples = 0; samples <= limit + 2048; samples += int(carrier.size()))
            if (!feed(rx, carrier, 1024).isEmpty()) throw std::runtime_error("Continuous carrier decoded a packet");
        check(rx.status().value("bufferedSamples").toInt() <= limit,
            "streaming history cannot exceed the preflight receiver limit");
        Iq quiet(1024);
        for (int i = 0; i < 32; ++i)
            if (!feed(rx, quiet, 1024).isEmpty()) throw std::runtime_error("Silence decoded a packet");
        check(true, "continuous carrier and silence remain bounded and undecoded");
    }
}

void benchmarks() {
    for (const auto &config : {QJsonObject{}, fastLoRa()}) {
        WireModem tx(config), rx(config);
        QElapsedTimer wall;
        wall.start();
        const auto cpu = cpuMilliseconds();
        quint64 samples = 0;
        for (int i = 0; i < 10; ++i) {
            auto wire = nativeBytes(16);
            wire[0] = char(i);
            QString error;
            const auto iq = tx.modulate(wire, &error);
            check(!iq.isEmpty() && feed(rx, iq, 1024, 0.25f) == QList<QByteArray>{wire},
                "CPU benchmark packet roundtrip is exact");
            samples += quint64(iq.size());
        }
        const double cpuMs = cpuMilliseconds() - cpu;
        std::printf("BENCH %s packets=10 cpu_ms=%.3f wall_ms=%lld samples=%llu\n",
            config.isEmpty() ? "afsk1200" : "lora-sf7-bw125k-os1", cpuMs,
            static_cast<long long>(wall.elapsed()), static_cast<unsigned long long>(samples));
    }
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        configurations();
        roundtrip({}, 137, 48, 0.5f);
        roundtrip({}, 1024, 768, 1e-12f);
        roundtrip({}, 137, 0, 1000);
        roundtrip(fastLoRa(), 137, 48, 0.03f);
        roundtrip(fastLoRa(), 1024, 255, 2.0f);
        roundtrip(fastLoRa(), 137, 0, 1);
        roundtrip({{"type", "lora"}}, 1024, 24, 0.25f);
        auto inverted = fastLoRa();
        inverted.insert("invertIq", true);
        inverted.insert("ldro", "on");
        inverted.insert("amplitude", 0.1);
        roundtrip(inverted, 137, 32, 0.5f);
        inverted.insert("payloadCrc", false);
        roundtrip(inverted, 137, 48, 0.25f);
        roundtrip(inverted, 1024, 255, 1);
        auto beacon=fastLoRa();beacon.insert("sf",9);beacon.insert("preamble",10);beacon.insert("syncWord",52);
        beacon.insert("payloadCrc",false);beacon.insert("explicitHeader",false);beacon.insert("implicitPayloadBytes",17);
        roundtrip(beacon,137,17,1);
        QJsonObject ignored;QString beaconError;auto invalidBeacon=beacon;invalidBeacon.insert("explicitHeader",true);
        check(!WireModem::normalize(invalidBeacon,&ignored,&beaconError),"explicit LoRa rejects an implicit payload length");
        std::printf("CRC-less inverted PHY preserves native bytes but provides no authentication; upper-layer MIC is required\n");
        discontinuityAndFinite({});
        discontinuityAndFinite(fastLoRa());
        rejectionAndBounds();
        WireModem source(fastLoRa());
        WireModem moved(std::move(source));
        QString error;
        check(source.sampleRate() == 0 && source.estimatedSamples(1) == 0 &&
            source.modulate("x", &error).isEmpty() && source.receive(nullptr, 0).isEmpty() &&
            !source.status().value("active").toBool() && moved.sampleRate() == 125000,
            "move ownership leaves an inert source instance");
        benchmarks();
        std::printf("Wire modem headless self-test passed; no RF or network used\n");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
