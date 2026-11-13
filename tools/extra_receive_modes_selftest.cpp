#include "ft4audiodecoder.h"
#include "m17audiodecoder.h"
#include "m17adapter.h"
#include "audioprocessor.h"
#include "../src/cli/climoduleprotocol.h"
#include <QCoreApplication>
#include <QLibrary>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
extern "C" {
#include "ft8/encode.h"
#include "ft8/constants.h"
#include "ft8/message.h"
#include "m17.h"
}
#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>

namespace {
QString workerOutput(const QString &path, const QString &mode, const QByteArray &samples,
                     quint32 kind, quint32 rate) {
    QProcess worker;
    worker.setProgram(path);
    worker.setArguments({QStringLiteral("rx"), mode});
    worker.start();
    if (!worker.waitForStarted(3000)) return QStringLiteral("WORKER_START_FAILED");
    QByteArray input;
    const auto frame = [&](quint32 type, quint32 fs, const QByteArray &data) {
        char header[12];
        qToLittleEndian(type, reinterpret_cast<uchar *>(header));
        qToLittleEndian(fs, reinterpret_cast<uchar *>(header + 4));
        qToLittleEndian(quint32(data.size()), reinterpret_cast<uchar *>(header + 8));
        input.append(header, 12); input.append(data);
    };
    frame(CliModuleProtocol::Configure, 0, QJsonDocument(QJsonObject{
        {"listeningFrequencyHz", 100000000}, {"digitalProfile", QJsonObject{{"version", 1}, {"module", mode}}}})
        .toJson(QJsonDocument::Compact));
    for (int pass = 0; pass < 2; ++pass) {
        frame(CliModuleProtocol::Reset, 0, {});
        for (int offset = 0; offset < samples.size(); offset += 19200)
            frame(kind, rate, samples.mid(offset, 19200));
    }
    worker.write(input); worker.closeWriteChannel();
    if (!worker.waitForFinished(30000)) { worker.kill(); worker.waitForFinished(); return "WORKER_TIMEOUT"; }
    if (worker.exitStatus() != QProcess::NormalExit || worker.exitCode()) return "WORKER_FAILED: " + worker.readAllStandardError();
    return QString::fromUtf8(worker.readAllStandardOutput());
}
constexpr double Pi = 3.14159265358979323846;
QByteArray fmIq(const QByteArray &waveform) {
    QByteArray iq;
    double phase = 0;
    for (int n = 0; n < waveform.size() / 2; ++n) {
        const double sample = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(waveform.constData() + n * 2)) / 32768.0;
        phase += 2 * Pi * 10000 * sample / 48000;
        const float pair[] = {float(0.5 * std::cos(phase)), float(0.5 * std::sin(phase))};
        iq.append(reinterpret_cast<const char *>(pair), sizeof(pair));
    }
    return iq;
}
void appendPcm(QByteArray &pcm, double value) {
    char bytes[2]; qToLittleEndian<qint16>(qint16((std::clamp)(value, -0.99, 0.99) * 32767), reinterpret_cast<uchar *>(bytes));
    pcm.append(bytes, 2);
}
QByteArray modulate(const QVector<qint8> &symbols, bool inverted = false) {
    std::vector<float> shaped(symbols.size() * 10 + 300, 0.0f);
    for (int symbol = 0; symbol < symbols.size(); ++symbol)
        for (int tap = 0; tap < 81; ++tap) shaped[symbol * 10 + tap + 91] += symbols[symbol] * rrc_taps_10[tap] * (inverted ? -0.18f : 0.18f);
    std::mt19937 rng(17); std::normal_distribution<float> noise(0.0f, 0.0005f);
    QByteArray pcm;
    for (float sample : shaped) appendPcm(pcm, sample + 0.01f + noise(rng));
    return pcm;
}
M17AudioResult receive(M17AudioDecoder &decoder, const QByteArray &pcm, int rate = 48000) {
    M17AudioResult result;
    for (int pos = 0; pos < pcm.size(); pos += 246) {
        const auto part = decoder.process(pcm.mid(pos, 246), rate);
        result.text.append(part.text); result.voicePcm.append(part.voicePcm);
    }
    return result;
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    const int workerIndex = app.arguments().indexOf(QStringLiteral("--cli-worker"));
    const QString worker = workerIndex >= 0 ? app.arguments().value(workerIndex + 1) : QString();
    auto check = [&](bool okay, const char *what) { std::cout << (okay ? "PASS " : "FAIL ") << what << '\n'; if (!okay) ++failures; };
    ftx_message_t message{};
    check(ftx_message_encode(&message, nullptr, "CQ K1ABC FN42") == FTX_MESSAGE_RC_OK, "FT4 encode fixture");
    std::array<uint8_t, FT4_NN> tones{}; ft4_encode(message.payload, tones.data());
    QByteArray pcm;
    double phase = 0.0;
    for (int i = 0; i < 48000 * 9; ++i) {
        const int symbol = (i - 24000) / 2304;
        double value = 0;
        if (i >= 24000 && symbol < FT4_NN) {
            phase += 2 * Pi * (1000.0 + tones[symbol] / FT4_SYMBOL_PERIOD) / 48000.0;
            value = 0.4 * std::sin(phase);
        }
        appendPcm(pcm, value);
    }
    Ft4AudioDecoder ft4; QStringList ft4Messages;
    for (int pos = 0; pos < pcm.size(); pos += 9600) ft4Messages.append(ft4.process(pcm.mid(pos, 9600), 48000));
    std::cout << ft4Messages.join('\n').toStdString() << '\n';
    check(ft4Messages.join(' ').contains("CQ K1ABC FN42"), "FT4 live PCM to CRC-validated message");
    check(ft4Messages.size() == 1, "FT4 overlapping windows deduplicate message");
    if (!worker.isEmpty()) {
        const auto output = workerOutput(worker, "ft4", pcm, 1, 48000);
        check(output.count("CQ K1ABC FN42") >= 2, "FT4 real CLI worker PCM and reset/redecode");
        check(workerOutput(worker, "ft4", pcm.left(960), 1, 44100).startsWith("WORKER_FAILED"),
              "FT4 CLI rejects unsupported PCM rate");
    }
    {
        Ft4AudioDecoder repeated; QStringList messages;
        const QByteArray twoSlots = pcm.left(7500 * 96) + pcm;
        for (int pos = 0; pos < twoSlots.size(); pos += 9600) messages.append(repeated.process(twoSlots.mid(pos, 9600), 48000));
        check(messages.size() == 2, "FT4 repeated CQ remains visible in consecutive 7.5-second slots");
    }
    {
        AudioProcessor demod;
        RadioSettings settings; settings.modulationType = MOD_FT4; settings.inputMode = INPUT_RF;
        settings.sampleRate = 48000; settings.centerFrequency = settings.listeningFrequency = 14.074e6;
        settings.bandwidth = 3000;
        Ft4AudioDecoder receiver; QStringList messages;
        std::vector<float> iq; std::vector<short> output;
        double angle = 0; bool valid = true;
        for (int i = 0; i < 48000 * 9; ++i) {
            const int symbol = (i - 24000) / 2304;
            const bool active = i >= 24000 && symbol < FT4_NN;
            if (active) angle += 2 * Pi * (1000.0 + tones[symbol] / FT4_SYMBOL_PERIOD) / 48000;
            iq.push_back(active ? 0.4f * std::cos(angle) : 0.0f);
            iq.push_back(active ? 0.4f * std::sin(angle) : 0.0f);
            if (iq.size() >= 9600) {
                valid &= demod.processIndependentIq(iq, settings, i < 4800, output); iq.clear();
                messages.append(receiver.process(QByteArray(reinterpret_cast<const char *>(output.data()), int(output.size() * 2)), 48000));
            }
        }
        check(valid && messages.join(' ').contains("CQ K1ABC FN42"), "FT4 RF IQ through production USB demodulator to message");
    }
    ft4.reset();
    check(ft4.process(QByteArray(48000 * 18, 0), 48000).isEmpty(), "FT4 silence rejects false decodes");
    QString error;
    const auto packet = M17Adapter::buildPacket("N0CALL", "ALL", QByteArray::fromHex("05") + "M17 LIVE TEST", 3, &error);
    check(!packet.symbols.isEmpty(), "M17 packet fixture");
    M17AudioDecoder m17;
    const auto received = receive(m17, modulate(packet.symbols));
    std::cout << received.text.join('\n').toStdString() << '\n';
    check(received.text.join(' ').contains("M17 LIVE TEST"), "M17 noisy RRC waveform packet reception");
    if (!worker.isEmpty()) {
        const auto generated = M17Adapter::modulatePacket(packet, 0.5f);
        QByteArray generatedIq;
        for (const auto &sample : generated) {
            const float iq[] = {sample.real(), sample.imag()};
            generatedIq.append(reinterpret_cast<const char *>(iq), sizeof(iq));
        }
        check(!generated.isEmpty() && workerOutput(worker, "m17", generatedIq, 2, 48000).count("M17 LIVE TEST") >= 2,
              "M17 TX IQ through the production RX worker");
        const auto waveform = modulate(packet.symbols);
        QByteArray channelIq;
        double angle = 0;
        for (int n = 0; n < waveform.size() / 2; ++n) {
            const double sample = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(waveform.constData() + n * 2)) / 32768.0;
            angle += 2 * Pi * 10000 * sample / 48000;
            const float iq[] = {float(0.5 * std::cos(angle)), float(0.5 * std::sin(angle))};
            channelIq.append(reinterpret_cast<const char *>(iq), sizeof(iq));
        }
        const auto output = workerOutput(worker, "m17", channelIq, 2, 48000);
        check(output.count("M17 LIVE TEST") >= 2, "M17 real CLI worker channel IQ and reset/redecode");
        check(workerOutput(worker, "m17", QByteArray(7, 0), 2, 48000).startsWith("WORKER_FAILED"),
              "M17 CLI rejects incomplete complex sample");
    }
    {
        QByteArray faster;
        const auto original = modulate(packet.symbols);
        for (int i = 0; i < original.size(); i += 2) { faster.append(original.mid(i, 2)); faster.append(original.mid(i, 2)); }
        m17.reset();
        check(receive(m17, faster, 96000).text.join(' ').contains("M17 LIVE TEST"), "M17 96 kHz chunked PCM reception");
        auto damaged = packet.symbols;
        for (int i = 400; i < 555; ++i) damaged[i] = (i % 2) ? 3 : -3;
        m17.reset();
        check(!receive(m17, modulate(damaged)).text.join(' ').contains("M17 LIVE TEST"), "M17 corrupted packet fails CRC");
    }
    {
        const QByteArray baseband = modulate(packet.symbols);
        AudioProcessor demod;
        RadioSettings settings; settings.modulationType = MOD_M17;
        settings.inputMode = INPUT_RF; settings.centerFrequency = settings.listeningFrequency = 433.5e6;
        settings.sampleRate = 480000; settings.bandwidth = 12500;
        M17AudioDecoder receiver; QStringList decoded;
        double angle = 0; std::vector<float> iq; std::vector<short> output;
        bool first = true;
        for (int n = 0; n < baseband.size() / 2; ++n) {
            const float sample = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(baseband.constData() + 2 * n)) / 32768.0f;
            for (int sub = 0; sub < 10; ++sub) {
                angle += 2 * Pi * 10000 * sample / settings.sampleRate;
                iq.push_back(0.5f * std::cos(angle)); iq.push_back(0.5f * std::sin(angle));
            }
            if (iq.size() >= 48000 || n + 1 == baseband.size() / 2) {
                check(demod.processIndependentIq(iq, settings, first, output), "M17 independent IQ demodulation block");
                first = false; iq.clear();
                decoded.append(receiver.process(QByteArray(reinterpret_cast<const char *>(output.data()), int(output.size() * 2)), 48000).text);
            }
        }
        check(decoded.join(' ').contains("M17 LIVE TEST"), "M17 RF IQ through production demodulator to packet");
    }
    m17.reset();
    check(receive(m17, modulate(packet.symbols, true)).text.join(' ').contains("M17 LIVE TEST"), "M17 inverted discriminator polarity");
    m17.reset();
    check(receive(m17, QByteArray(48000, 0)).text.isEmpty(), "M17 silence rejects false packets");
    check(m17.voiceAvailable(), "Codec2 runtime loaded");
    {
        constexpr int frames = 8;
        QVector<float> voice((frames - 1) * 320 + 100);
        for (int i = 0; i < voice.size(); ++i)
            voice[i] = float(0.28 * std::sin(2 * Pi * 210 * i / 8000) +
                             0.12 * std::sin(2 * Pi * 390 * i / 8000));
        const auto iq = M17Adapter::modulateVoice("K1ABC", "@ALL", 7, voice, 0.5f, 48000, &error);
        check(error.isEmpty() && iq.size() == (frames + 3) * 192 * 10 + 80,
              "M17 voice TX includes setup, padded final frame and EOT");
        QByteArray discriminator;
        for (int i = 1; i < iq.size(); ++i)
            appendPcm(discriminator, std::arg(iq[i] * std::conj(iq[i - 1])));
        m17.reset();
        const auto voiceResult = receive(m17, discriminator);
        std::cout << "M17 voice round trip PCM bytes " << voiceResult.voicePcm.size() << ' '
                  << voiceResult.text.join(' ').toStdString() << '\n';
        check(voiceResult.text.join(' ').contains("K1ABC") &&
              voiceResult.text.join(' ').contains("CAN 7"), "M17 voice TX link setup decoded");
        check(voiceResult.voicePcm.size() == frames * 1920 * 2 &&
              voiceResult.voicePcm != QByteArray(frames * 1920 * 2, 0),
              "M17 voice TX and RX recover all Codec2 frames including the partial final frame");
        M17VoiceStream stream;
        auto liveIq = stream.begin("K1ABC", "@ALL", 7, 0.5f, &error);
        bool liveOk = error.isEmpty() && liveIq.size() == 3840 && stream.isActive();
        bool emittedWhileHeld = false;
        for (int offset = 0; offset < voice.size(); offset += 137) {
            const auto chunk = stream.pushAudio(voice.mid(offset, 137), &error);
            liveOk &= error.isEmpty();
            emittedWhileHeld |= !chunk.isEmpty();
            liveIq += chunk;
        }
        liveIq += stream.finish(&error);
        check(liveOk && emittedWhileHeld && error.isEmpty() && !stream.isActive() && liveIq == iq,
              "M17 microphone chunks reproduce the WAV waveform with continuous phase and a final frame");
        auto nextCall = stream.begin("K1ABC", "@ALL", 7, 0.5f, &error);
        nextCall += stream.finish(&error);
        check(error.isEmpty() && nextCall ==
              M17Adapter::modulateVoice("K1ABC", "@ALL", 7, QVector<float>(320, 0.0f), 0.5f, 48000),
              "M17 immediate PTT release ends cleanly and a new call has fresh encoder state");
        check(stream.finish(&error).isEmpty() && stream.pushAudio(voice, &error).isEmpty(),
              "M17 stream stays silent while PTT is released");
        if (!worker.isEmpty()) {
            QByteArray iqBytes;
            for (auto sample : iq) {
                const float pair[] = {sample.real(), sample.imag()};
                iqBytes.append(reinterpret_cast<const char *>(pair), sizeof(pair));
            }
            const auto output = workerOutput(worker, "m17", iqBytes, 2, 48000);
            check(output.count("K1ABC") >= 2 && output.contains("voicePcm") && output.contains("pcmBase64"),
                  "M17 voice TX IQ through the production RX worker");
        }
        check(M17Adapter::modulateVoice("K1ABC", "@ALL", 16, voice, 0.5f, 48000, &error).isEmpty() &&
              !error.isEmpty(), "M17 voice TX rejects an invalid CAN");
    }
    QLibrary codecLib(QCoreApplication::applicationDirPath() + "/libcodec2");
    if (!codecLib.load()) { codecLib.setFileName("libcodec2"); codecLib.load(); }
    auto create = reinterpret_cast<void *(*)(int)>(codecLib.resolve("codec2_create"));
    auto encode = reinterpret_cast<void (*)(void *, unsigned char *, short *)>(codecLib.resolve("codec2_encode"));
    auto destroy = reinterpret_cast<void (*)(void *)>(codecLib.resolve("codec2_destroy"));
    if (create && encode && destroy) {
        for (const int mode : {0, 2}) {
            void *encoder = create(mode);
            lsf_t lsf{}; set_LSF(&lsf, "N0CALL", "ALL", M17_TYPE_STREAM | ((mode == 0 ? 2 : 3) << 1), nullptr);
            QVector<qint8> symbols; std::array<int8_t, 192> frame{}; uint32_t count = 0;
            gen_preamble_i8(frame.data(), &count, PREAM_LSF);
            for (auto v : frame) symbols << v;
            gen_frame_i8(frame.data(), nullptr, FRAME_LSF, &lsf, 0, 0);
            for (auto v : frame) symbols << v;
            for (int seq = 0; seq < 8; ++seq) {
                std::array<unsigned char, 16> data{}; std::array<short, 320> voice{};
                for (int n = 0; n < (mode == 0 ? 160 : 320); ++n) voice[n] = short(9000 * std::sin(2 * Pi * 440 * n / 8000));
                encode(encoder, data.data(), voice.data());
                if (mode == 0) encode(encoder, data.data() + 8, voice.data());
                gen_frame_i8(frame.data(), data.data(), FRAME_STR, &lsf, seq % 6, seq);
                for (auto v : frame) symbols << v;
            }
            destroy(encoder);
            m17.reset(); const auto decoded = receive(m17, modulate(symbols));
            std::cout << "Voice mode " << mode << " bytes " << decoded.voicePcm.size() << " " << decoded.text.join(' ').toStdString() << '\n';
            check(decoded.voicePcm.size() >= 1920 * 2 * 6, mode == 0 ? "M17 Codec2 3200 live voice" : "M17 Codec2 1600 live voice+data");
            if (!worker.isEmpty()) {
                const auto output = workerOutput(worker, "m17", fmIq(modulate(symbols)), 2, 48000);
                check(output.contains("voicePcm") && output.contains("pcmBase64"), "M17 real CLI worker emits Codec2 PCM");
            }
            m17.reset();
            const auto late = receive(m17, modulate(symbols.mid(384)));
            check(!late.voicePcm.isEmpty(), "M17 late-entry LICH acquisition");
            lsf.type[1] |= 8; update_LSF_CRC(&lsf);
            gen_frame_i8(frame.data(), nullptr, FRAME_LSF, &lsf, 0, 0);
            for (int i = 0; i < 192; ++i) symbols[192 + i] = frame[i];
            std::array<unsigned char, 16> encryptedData{};
            encryptedData.fill(0x55);
            for (int seq = 0; seq < 8; ++seq) {
                gen_frame_i8(frame.data(), encryptedData.data(), FRAME_STR, &lsf, seq % 6, seq);
                for (int i = 0; i < 192; ++i) symbols[384 + seq * 192 + i] = frame[i];
            }
            m17.reset();
            check(receive(m17, modulate(symbols)).voicePcm.isEmpty(), "M17 encrypted stream never decoded as clear voice");
            if (!worker.isEmpty()) {
                const auto output = workerOutput(worker, "m17", fmIq(modulate(symbols)), 2, 48000);
                check(!output.contains("voicePcm") && !output.contains("WORKER_"), "M17 CLI worker mutes encrypted stream");
            }
        }
    } else check(false, "Codec2 encoder fixture API");
    return failures ? 1 : 0;
}
