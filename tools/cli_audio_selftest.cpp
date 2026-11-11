#include "cliaudiooutput.h"
#include "cliasyncwfmaudio.h"
#include "audioprocessor.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <vector>

struct AudioProcessorTestAccess {
    static void sequence(AudioProcessor &processor, std::uint64_t value) {
        processor.externalIqSequence.store(value);
    }
    static QByteArray demod(AudioProcessor &processor, const std::vector<float> &iq,
                            const RadioSettings &settings, bool reset) {
        if (reset) processor.resetDemodulatorState();
        std::vector<short> pcm, unused;
        int rate = 0;
        processor.processDemodulatorBlock(iq, pcm, unused, rate, settings);
        return QByteArray(reinterpret_cast<const char *>(pcm.data()), int(pcm.size() * sizeof(short)));
    }
};

static bool liveDiscontinuities() {
    for (bool simplified : {false, true}) {
        for (int mode = MOD_AM; mode <= MOD_WFM; ++mode) {
            RadioSettings settings;
            settings.inputMode = INPUT_RF;
            settings.sampleRate = 480000;
            settings.centerFrequency = settings.listeningFrequency = 100e6;
            settings.modulationType = mode;
            settings.bandwidth = mode == MOD_WFM ? 180000 : 12500;
            settings.simplifiedAudioChannelizer = simplified;
            std::vector<float> iq(48000);
            for (std::size_t n = 0; n < iq.size() / 2; ++n) {
                const double tone = std::sin(6.283185307179586 * 1000 * n / settings.sampleRate);
                const double phase = 6.283185307179586 * 1700 * n / settings.sampleRate + 0.4 * tone;
                iq[2*n] = float((0.3 + 0.1 * tone) * std::cos(phase));
                iq[2*n+1] = float((0.3 + 0.1 * tone) * std::sin(phase));
            }
            std::mutex mutex;
            std::condition_variable ready;
            std::vector<QByteArray> frames;
            AudioProcessor live, reference;
            live.setLocalPlaybackEnabled(false);
            live.setExternalIqSourceEnabled(true);
            live.configure(settings);
            QObject::connect(&live, &AudioProcessor::demodulatorFrameReady, &live,
                [&](const QByteArray &pcm) {
                    { std::lock_guard<std::mutex> lock(mutex); frames.push_back(pcm); }
                    ready.notify_one();
                }, Qt::DirectConnection);
            live.startDemodulation();
            for (std::size_t block = 0; block < 4; ++block) {
                // Forward loss, then a restarted source with a backwards sequence.
                if (block == 2) AudioProcessorTestAccess::sequence(live, 8);
                if (block == 3) AudioProcessorTestAccess::sequence(live, 0);
                const auto expected = AudioProcessorTestAccess::demod(reference, iq, settings, block != 1);
                live.submitExternalIq(iq.data(), iq.size(), settings.sampleRate, settings.centerFrequency);
                std::unique_lock<std::mutex> lock(mutex);
                if (!ready.wait_for(lock, std::chrono::seconds(3), [&] { return frames.size() > block; }) ||
                    expected.isEmpty() || frames[block] != expected) {
                    std::cerr << "Live gap reset differs from fresh demodulator: mode=" << mode
                              << " simplified=" << simplified << " block=" << block << '\n';
                    return false;
                }
            }
            live.stopDemodulation();
        }
    }
    std::cout << "Live analog IQ continuity/reset: 8 modes, full/simplified, no audio device PASS\n";
    return true;
}

static bool asyncWfmMatchesReference() {
    constexpr std::size_t blockSamples = 4096;
    CliAsyncWfmAudio::Format format{480000, 100e6, 100e6, 180000, QStringLiteral("[]"), 0};
    CliAsyncWfmAudio worker(blockSamples);
    std::vector<float> iq(blockSamples * 2);
    for (std::size_t n = 0; n < blockSamples; ++n) {
        const double phase = 0.04 * n + 0.8 * std::sin(0.017 * n);
        iq[2*n] = float(0.3 * std::cos(phase));
        iq[2*n + 1] = float(0.3 * std::sin(phase));
    }
    for (int pass = 0; pass < 3; ++pass) {
        if (pass == 1) worker.markGap();
        if (pass == 2) { format.listen += 20000; ++format.streamEpoch; }
        worker.setFormat(format);
        QByteArray expected, received;
        QString error;
        CliAudioOutput reference;
        if (!reference.open({}, false, INPUT_RF, MOD_WFM, format.bandwidth, format.rate,
                            format.center, format.listen, false, &error,
                            [&expected](const QByteArray &frame) { expected.append(frame); }))
            return false;
        reference.setFilterChain(format.filters);
        for (int i = 0; i < 4; ++i) {
            if (!reference.process(iq.data(), blockSamples, &error)) return false;
            worker.submit(iq.data(), blockSamples);
        }
        QElapsedTimer timeout;
        timeout.start();
        while (received.size() < expected.size() && timeout.elapsed() < 2000) {
            received.append(worker.takePcm(&error));
            if (!error.isEmpty()) return false;
            if (received.size() < expected.size()) QThread::msleep(1);
        }
        if (expected.isEmpty() || received != expected) return false;
    }
    return worker.suspend() && !worker.status().value("active").toBool() &&
           worker.status().value("droppedIqBlocks").toDouble() == 0;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (!liveDiscontinuities()) return 1;
    if (!asyncWfmMatchesReference()) {
        std::cerr << "Async WFM differs from synchronous PCM or did not reset on a gap/tune\n";
        return 1;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    const QString path = temporary.filePath(QStringLiteral("audio.wav"));
    CliAudioOutput output;
    QString error;
    constexpr double rate = 480000.0;
    if (!output.open(path, false, INPUT_RF, MOD_AM, 10000.0,
                     rate, 100e6, 100e6, true, &error)) {
        std::cerr << error.toStdString() << '\n'; return 1;
    }
    std::vector<float> iq(48000 * 2);
    for (std::size_t n = 0; n < iq.size() / 2; ++n) {
        iq[2 * n] = 0.3f * (1.0f + 0.4f * std::sin(6.283185307179586 * 1000.0 * n / rate));
        iq[2 * n + 1] = 0.0f;
    }
    for (std::size_t offset = 0; offset < iq.size() / 2; offset += 4096) {
        const std::size_t count = (std::min)(std::size_t(4096), iq.size() / 2 - offset);
        if (!output.process(iq.data() + offset * 2, count, &error)) {
            std::cerr << error.toStdString() << '\n'; return 1;
        }
    }
    if (!output.close(&error)) { std::cerr << error.toStdString() << '\n'; return 1; }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 1;
    const QByteArray wave = file.readAll();
    if (wave.size() < 9000 || wave.left(4) != "RIFF" || wave.mid(8, 4) != "WAVE" ||
        wave.mid(36, 4) != "data" || wave.size() % 2) return 1;
    const auto *bytes = reinterpret_cast<const unsigned char *>(wave.constData());
    const quint32 declared = quint32(bytes[40]) | (quint32(bytes[41]) << 8) |
                             (quint32(bytes[42]) << 16) | (quint32(bytes[43]) << 24);
    if (declared != static_cast<quint32>(wave.size() - 44)) return 1;
    double energy = 0.0;
    for (int i = 44; i + 1 < wave.size(); i += 2) {
        const qint16 sample = static_cast<qint16>(quint16(bytes[i]) | (quint16(bytes[i + 1]) << 8));
        energy += double(sample) * sample;
    }
    if (std::sqrt(energy / (declared / 2)) < 100.0) return 1;
    std::cout << "CLI audio self-test passed\n";
    return 0;
}
