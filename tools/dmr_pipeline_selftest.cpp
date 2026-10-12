#include "audioprocessor.h"
#include "recordingmanager.h"
#include "playbackmanager.h"
#include "iqbuffer.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QtEndian>
#include <iostream>
#include <cstring>

struct AudioProcessorTestAccess {
    static void enqueueLivePcm(AudioProcessor &processor, short sample, std::size_t count) {
        {
            std::lock_guard<std::mutex> lock(processor.audioMutex);
            processor.audioBuffer.insert(processor.audioBuffer.end(), count, sample);
        }
        processor.cv.notify_one();
    }
    static bool stopped(const AudioProcessor &processor) {
        return !processor.running.load() && !processor.sdrWorker.joinable() && !processor.audioWorker.joinable();
    }
    static bool bufferEmpty(const AudioProcessor &processor) { return processor.audioBuffer.empty(); }
    static bool outputIdle(const AudioProcessor &processor) {
        for (int index = 0; index < NUM_BUFFERS; ++index)
            if (!processor.bufferReady[index].load()) return false;
        return true;
    }
    static std::uintptr_t outputHandle(const AudioProcessor &processor) {
#ifdef _WIN32
        return reinterpret_cast<std::uintptr_t>(processor.hWaveOut);
#else
        return 0;
#endif
    }
    static void demod(AudioProcessor &processor, const std::vector<float> &iq,
                      const RadioSettings &settings, std::vector<short> &pcm, int &rate) {
        processor.processDmrIqDemodulatorBlock(iq, pcm, rate, settings);
    }
};
namespace {
int failures = 0;
void check(bool ok, const char *name) {
    if (!ok) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
}
void testRecording() {
    RecordingManager recorder;
    RadioSettings settings;
    settings.modulationType = MOD_DMR;
    settings.sampleRate = 50e6;
    QString error;
    check(recorder.start(RecordingManager::Mode::ChannelIqWav, settings, &error), "record start");
    const QString path = recorder.currentFilePath();
    recorder.appendIqFrame(QByteArray(32, '\0'), 50e6, 16);
    check(!QFileInfo::exists(path), "full int8 frame cannot become channel WAV");
    recorder.appendIqFrame(QByteArray(64, '\0'), 192308, 16);
    recorder.appendIqFrame(QByteArray(64, '\0'), 192308, 16);
    recorder.appendIqFrame(QByteArray(64, '\0'), 384000, 16);
    check(!recorder.isRecording(), "sample rate change stops rather than corrupts WAV");
    PlaybackManager::WavInfo info;
    check(PlaybackManager::readWavInfo(path, info, &error), "recorded WAV readable");
    check(info.sampleRate == 192308 && info.channels == 2 && info.bitsPerSample == 16 &&
          info.dataSize == 128, "recorded channel format and size");
    QFile::remove(path);
    QFile::remove(QFileInfo(path).absoluteDir().filePath(QFileInfo(path).completeBaseName() + ".json"));
}
void testStreamRestart(bool localAudio) {
    AudioProcessor processor;
    processor.setLocalPlaybackEnabled(localAudio);
    RadioSettings settings;
    settings.sampleRate = 50e6;
    processor.configure(settings);
    processor.startDemodulation();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto handle = AudioProcessorTestAccess::outputHandle(processor);
#ifdef _WIN32
    if (localAudio) check(handle != 0, "waveOut test device opened");
#endif
    processor.stopDemodulationForStreamRestart();
    check(AudioProcessorTestAccess::stopped(processor) && AudioProcessorTestAccess::bufferEmpty(processor),
          "stream restart joins workers and flushes PCM");
    check(AudioProcessorTestAccess::outputHandle(processor) == handle, "stream restart retains audio device");
    settings.sampleRate = 25e6;
    processor.configure(settings);
    processor.startDemodulation();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(AudioProcessorTestAccess::outputHandle(processor) == handle, "restarted audio reuses device");
    processor.stopDemodulationForStreamRestart();
    processor.stopDemodulation();
    processor.stopDemodulation();
    check(AudioProcessorTestAccess::stopped(processor) && AudioProcessorTestAccess::outputHandle(processor) == 0,
          "ordinary Stop closes retained output and is idempotent");
}
void testLivePcmBuffering() {
    IqBuffer::clear();
    AudioProcessor processor;
    processor.setLocalPlaybackEnabled(false);
    RadioSettings settings;
    settings.sampleRate = 2.4e6;
    settings.modulationType = MOD_NFM;
    processor.configure(settings);
    std::atomic<int> frames{0};
    std::atomic<bool> contentValid{true};
    QObject::connect(&processor, &AudioProcessor::audioFrameReady, &processor,
                     [&](const QByteArray &pcm) {
        const int frameIndex = frames.load();
        const short expected = frameIndex == 0 ? 101 : frameIndex == 1 ? 202 : 303;
        if (pcm.size() != BUFFER_SIZE * int(sizeof(short))) contentValid.store(false);
        for (int n = 0; n < pcm.size() / int(sizeof(short)); ++n) {
            short value;
            std::memcpy(&value, pcm.constData() + n * sizeof(short), sizeof(short));
            if (value != expected) contentValid.store(false);
        }
        frames.fetch_add(1);
    }, Qt::DirectConnection);
    processor.startDemodulation();
    AudioProcessorTestAccess::enqueueLivePcm(processor, 101, BUFFER_SIZE);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(frames.load() == 0, "live PCM waits for a jitter reserve before starting");
    AudioProcessorTestAccess::enqueueLivePcm(processor, 202, BUFFER_SIZE);
    const auto waitForFrames = [&](int target) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (frames.load() < target && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return frames.load() >= target;
    };
    check(waitForFrames(2), "primed PCM output drains without changing frame cadence");
    AudioProcessorTestAccess::enqueueLivePcm(processor, 303, BUFFER_SIZE);
    check(waitForFrames(3), "already-primed network PCM does not wait for a second chunk");
    processor.stopDemodulation();
    check(contentValid.load() && frames.load() == 3, "PCM priming preserves every sample exactly once");
    check(AudioProcessorTestAccess::stopped(processor), "Stop wakes and joins an idle PCM output worker");
    IqBuffer::clear();
}
void testWaveOutRebuffering() {
#ifdef _WIN32
    IqBuffer::clear();
    AudioProcessor processor;
    RadioSettings settings;
    settings.sampleRate = 2.4e6;
    settings.modulationType = MOD_NFM;
    processor.configure(settings);
    std::atomic<int> frames{0};
    QObject::connect(&processor, &AudioProcessor::audioFrameReady, &processor,
                     [&](const QByteArray &) { frames.fetch_add(1); }, Qt::DirectConnection);
    const auto waitUntil = [](const auto &predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return predicate();
    };
    processor.startDemodulation();
    check(AudioProcessorTestAccess::outputHandle(processor) != 0, "rebuffer test opens waveOut");
    AudioProcessorTestAccess::enqueueLivePcm(processor, 0, BUFFER_SIZE*2);
    check(waitUntil([&] { return frames.load() >= 2; }), "initial waveOut reserve submitted");
    check(waitUntil([&] { return AudioProcessorTestAccess::outputIdle(processor); }),
          "waveOut completion callbacks release the submitted buffers");
    AudioProcessorTestAccess::enqueueLivePcm(processor, 0, BUFFER_SIZE);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(frames.load() == 2, "waveOut starvation rebuilds the reserve instead of stuttering one chunk at a time");
    AudioProcessorTestAccess::enqueueLivePcm(processor, 0, BUFFER_SIZE);
    check(waitUntil([&] { return frames.load() >= 4; }), "waveOut resumes when its reserve is ready");
    processor.stopDemodulation();
    check(frames.load() == 4 && AudioProcessorTestAccess::stopped(processor),
          "waveOut rebuffering preserves frame count and Stop joins the output worker");
    IqBuffer::clear();
#endif
}
void testDemodulator() {
    RadioSettings settings;
    settings.inputMode = INPUT_RF;
    settings.sampleRate = 2.4e6;
    settings.modulationType = MOD_DMR;
    settings.centerFrequency = 145e6;
    settings.listeningFrequency = settings.centerFrequency + 17000;
    settings.dmrBasebandSampleRate = 24000;
    std::vector<float> input(480000);
    double phase = 0.0;
    for (std::size_t n = 0; n < input.size() / 2; ++n) {
        const double deviation = 1944.0 * std::sin(6.283185307179586 * 700.0 * n / settings.sampleRate);
        phase += 6.283185307179586 * (17000.0 + deviation) / settings.sampleRate;
        input[2*n] = float(std::cos(phase) * 0.2);
        input[2*n+1] = float(std::sin(phase) * 0.2);
    }
    AudioProcessor full, chunks;
    chunks.setDmrChannelIqEnabled(true);
    std::size_t capturedPairs = 0;
    QObject::connect(&chunks, &AudioProcessor::dmrChannelIqFrameReady, &chunks,
        [&](const QByteArray &bytes, double rate, int pairs) {
            check(rate == 200000 && bytes.size() == pairs * 4, "shared DMR channel IQ format");
            capturedPairs += std::size_t(pairs);
        });
    std::vector<short> expected, output, joined;
    int rate = 0;
    AudioProcessorTestAccess::demod(full, input, settings, expected, rate);
    check(rate == 24000 && expected.size() >= 2390 && expected.size() <= 2400, "DMR output duration");
    for (std::size_t offset = 0; offset < input.size();) {
        const std::size_t count = (std::min)(std::size_t(18422), input.size() - offset);
        std::vector<float> part(input.begin() + offset, input.begin() + offset + count);
        AudioProcessorTestAccess::demod(chunks, part, settings, output, rate);
        joined.insert(joined.end(), output.begin(), output.end()); offset += count;
    }
    check(joined.size() == expected.size(), "DMR arbitrary block boundaries preserve sample count");
    int worst = 0;
    for (std::size_t n = 0; n < (std::min)(joined.size(), expected.size()); ++n)
        worst = (std::max)(worst, std::abs(int(joined[n]) - int(expected[n])));
    check(worst <= 3, "DMR arbitrary block boundaries preserve discriminator output");
    check(capturedPairs == 20000, "shared DMR channel IQ contains every decimated sample");
    chunks.setDmrChannelIqEnabled(false);
    AudioProcessorTestAccess::demod(chunks, input, settings, output, rate);
    check(capturedPairs == 20000, "disabled DMR capture does not emit IQ");
    std::cout << "DMR chunk maxPcmError=" << worst << '\n';
}
void testDiscriminatorAmplitude() {
    RadioSettings settings;
    settings.inputMode = INPUT_RF;
    settings.sampleRate = 192000;
    settings.modulationType = MOD_DMR;
    settings.centerFrequency = settings.listeningFrequency = 145e6;
    settings.dmrBasebandSampleRate = 24000;
    std::vector<float> referenceIq(38400);
    double phase = 0.0;
    for (std::size_t n = 0; n < referenceIq.size() / 2; ++n) {
        phase += 6.283185307179586 * 1944.0 *
                 std::sin(6.283185307179586 * 700.0 * n / settings.sampleRate) /
                 settings.sampleRate;
        referenceIq[2*n] = float(std::cos(phase) * 0.2);
        referenceIq[2*n+1] = float(std::sin(phase) * 0.2);
    }
    AudioProcessor reference;
    std::vector<short> expected;
    int rate = 0;
    AudioProcessorTestAccess::demod(reference, referenceIq, settings, expected, rate);
    for (float scale : {0.0001f, 0.000001f, 0.00000001f}) {
        auto iq = referenceIq;
        for (float &value : iq) value *= scale;
        AudioProcessor processor;
        std::vector<short> actual;
        AudioProcessorTestAccess::demod(processor, iq, settings, actual, rate);
        check(actual.size() == expected.size(), "DMR amplitude preserves sample count");
        int worst = 0;
        for (std::size_t n = 0; n < (std::min)(actual.size(), expected.size()); ++n)
            worst = (std::max)(worst, std::abs(int(actual[n]) - int(expected[n])));
        std::cout << "DMR amplitude scale=" << scale << " maxPcmError=" << worst << '\n';
        check(worst <= 3, "DMR discriminator preserves weak IQ phase");
    }
    AudioProcessor silence;
    std::vector<short> pcm;
    AudioProcessorTestAccess::demod(silence, std::vector<float>(38400, 0.0f), settings, pcm, rate);
    check(pcm.size() >= 2390 && pcm.size() <= 2400 &&
          std::all_of(pcm.begin(), pcm.end(), [](short value) { return value == 0; }),
          "DMR zero IQ preserves silence and duration");
}
int inspectRecording(const QString &path, const QString &outputPath, int outputRate) {
    PlaybackManager::WavInfo info;
    QString error;
    if (!PlaybackManager::readWavInfo(path, info, &error) || info.channels != 2) {
        std::cerr << "Not a readable IQ WAV\n"; return 1;
    }
    QFile file(path), pcmFile(outputPath);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(qint64(info.dataOffset)) ||
        !pcmFile.open(QIODevice::WriteOnly)) return 1;
    RadioSettings settings = info.radioSettings;
    settings.sampleRate = info.sampleRate;
    settings.centerFrequency = settings.listeningFrequency;
    settings.inputMode = INPUT_RF;
    settings.modulationType = MOD_DMR;
    settings.dmrBasebandSampleRate = outputRate;
    AudioProcessor processor;
    quint64 remaining = info.dataSize, inputPairs = 0, pcmSamples = 0, clipped = 0;
    double power = 0.0;
    QElapsedTimer timer; timer.start();
    while (remaining > 0) {
        const QByteArray bytes = file.read(qint64((std::min)(remaining, quint64(32768))));
        if (bytes.isEmpty() || bytes.size() % 4 != 0) return 1;
        remaining -= quint64(bytes.size());
        std::vector<float> iq(std::size_t(bytes.size() / 2));
        const auto *raw = reinterpret_cast<const uchar *>(bytes.constData());
        for (int n = 0; n < bytes.size() / 2; ++n) {
            const qint16 sample = qFromLittleEndian<qint16>(raw + 2*n);
            clipped += std::abs(int(sample)) >= 32766;
            iq[std::size_t(n)] = float(sample) / 32767.0f;
            power += double(iq[std::size_t(n)]) * iq[std::size_t(n)];
        }
        inputPairs += iq.size() / 2;
        std::vector<short> pcm;
        int rate = 0;
        AudioProcessorTestAccess::demod(processor, iq, settings, pcm, rate);
        QByteArray output(int(pcm.size() * 2), '\0');
        for (std::size_t n = 0; n < pcm.size(); ++n)
            qToLittleEndian<qint16>(pcm[n], reinterpret_cast<uchar *>(output.data()) + 2*n);
        if (pcmFile.write(output) != output.size()) return 1;
        pcmSamples += pcm.size();
    }
    std::cout << "IQ rate=" << info.sampleRate << " pairs=" << inputPairs
              << " duration=" << double(inputPairs) / info.sampleRate
              << " rms=" << std::sqrt(power / (2 * inputPairs))
              << " clippingPercent=" << 100.0 * clipped / (2 * inputPairs)
              << " PCM rate=" << outputRate << " samples=" << pcmSamples
              << " demodMs=" << timer.elapsed() << '\n';
    return 0;
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if ((argc == 4 || argc == 5) && app.arguments()[1] == "--inspect-iq") {
        const int rate = argc == 5 ? app.arguments()[4].toInt() : 24000;
        if (rate != 24000 && rate != 48000) return 1;
        return inspectRecording(app.arguments()[2], app.arguments()[3], rate);
    }
    testRecording(); testDemodulator(); testDiscriminatorAmplitude();
    testStreamRestart(app.arguments().contains(QStringLiteral("--waveout-restart")));
    testLivePcmBuffering();
    if (app.arguments().contains(QStringLiteral("--waveout-restart"))) testWaveOutRebuffering();
    std::cout << "DMR pipeline failures=" << failures << '\n';
    return failures ? 1 : 0;
}
