#include "videoprocessor.h"
#include "audioprocessor.h"
#include "transmitmediagenerator.h"
#include "transmitwaveformgenerator.h"

#include <QCoreApplication>
#include <QDebug>
#include <QImage>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <vector>

namespace {
constexpr int SampleRate = 48000;
constexpr double TwoPi = 6.28318530717958647692;
constexpr int HilbertTaps = 63;
constexpr int HilbertDelay = HilbertTaps / 2;

double hilbertCoefficient(int tap) {
    const int n = tap - HilbertDelay;
    if (n == 0 || (n & 1) == 0) return 0.0;
    const double ideal = 2.0 / (3.14159265358979323846 * n);
    const double window = 0.54 +
                          0.46 * std::cos(3.14159265358979323846 * n /
                                          static_cast<double>(HilbertDelay));
    return ideal * window;
}

std::vector<float> pcmSamples(const QByteArray &pcm) {
    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(pcm.size() / 2));
    const auto *raw = reinterpret_cast<const unsigned char *>(pcm.constData());
    for (int offset = 0; offset + 1 < pcm.size(); offset += 2) {
        const quint16 bits = static_cast<quint16>(raw[offset]) |
                             (static_cast<quint16>(raw[offset + 1]) << 8);
        samples.push_back(static_cast<qint16>(bits) / 32768.0f);
    }
    return samples;
}

std::vector<float> modulateSsb(const std::vector<float> &audio, bool lowerSideband) {
    std::vector<float> iq;
    iq.reserve(audio.size() * 2U);
    std::array<float, HilbertTaps> history{};
    int writeIndex = 0;
    for (float sample : audio) {
        history[static_cast<std::size_t>(writeIndex)] = sample;
        double quadrature = 0.0;
        for (int tap = 0; tap < HilbertTaps; ++tap) {
            int historyIndex = writeIndex - tap;
            if (historyIndex < 0) historyIndex += HilbertTaps;
            quadrature += hilbertCoefficient(tap) *
                          history[static_cast<std::size_t>(historyIndex)];
        }
        int delayedIndex = writeIndex - HilbertDelay;
        if (delayedIndex < 0) delayedIndex += HilbertTaps;
        const float delayed = history[static_cast<std::size_t>(delayedIndex)];
        writeIndex = (writeIndex + 1) % HilbertTaps;
        iq.push_back(delayed * 0.72f);
        iq.push_back(static_cast<float>(quadrature) *
                     (lowerSideband ? -0.72f : 0.72f));
    }
    return iq;
}

std::vector<float> modulateNfm(const std::vector<float> &audio,
                               int rfSampleRate = SampleRate,
                               double deviationHz = 2500.0,
                               double carrierOffsetHz = 0.0,
                               bool amplitudeFading = false) {
    const int oversample = (std::max)(1, rfSampleRate / SampleRate);
    std::vector<float> iq;
    iq.reserve(audio.size() * static_cast<std::size_t>(oversample) * 2U);
    double phase = 0.0;
    std::size_t rfIndex = 0;
    for (float sample : audio) {
        for (int subSample = 0; subSample < oversample; ++subSample, ++rfIndex) {
            phase = std::remainder(
                phase + TwoPi * (carrierOffsetHz + deviationHz * sample) / rfSampleRate,
                TwoPi);
            const double fading = amplitudeFading
                                      ? 0.58 + 0.34 * std::sin(
                                                     TwoPi * 7.3 * rfIndex / rfSampleRate)
                                      : 0.72;
            iq.push_back(static_cast<float>(fading * std::cos(phase)));
            iq.push_back(static_cast<float>(fading * std::sin(phase)));
        }
    }
    return iq;
}

bool decodeIqThroughReceiver(const std::vector<float> &iq,
                             const RadioSettings &settings,
                             const char *label,
                             int expectedVis,
                             const QString &expectedMode,
                             const QSize &expectedSize) {
    AudioProcessor receiver;
    receiver.setLocalPlaybackEnabled(false);
    std::vector<short> demodulated;
    std::vector<short> blockPcm;
    constexpr std::size_t IqBlockSamples = 32768;
    bool firstBlock = true;
    const std::size_t iqSampleCount = iq.size() / 2U;
    for (std::size_t first = 0; first < iqSampleCount; first += IqBlockSamples) {
        const std::size_t count = (std::min)(IqBlockSamples, iqSampleCount - first);
        const std::vector<float> block(iq.begin() + static_cast<std::ptrdiff_t>(first * 2U),
                                       iq.begin() + static_cast<std::ptrdiff_t>((first + count) * 2U));
        if (!receiver.processIndependentIq(block, settings, firstBlock, blockPcm)) {
            std::fprintf(stderr, "%s SSTV receiver rejected IQ\n", label);
            return false;
        }
        firstBlock = false;
        demodulated.insert(demodulated.end(), blockPcm.begin(), blockPcm.end());
    }

    VideoProcessor decoder;
    QStringList statuses;
    QImage frame;
    QObject::connect(&decoder, &VideoProcessor::statusChanged,
                     [&statuses](const QString &status) { statuses.append(status); });
    QObject::connect(&decoder, &VideoProcessor::frameReady,
                     [&frame](const QImage &next) { frame = next; });
    decoder.configureSstv(true);
    constexpr int PcmChunkSamples = 960;
    for (std::size_t first = 0; first < demodulated.size(); first += PcmChunkSamples) {
        const std::size_t count = (std::min)(static_cast<std::size_t>(PcmChunkSamples),
                                             demodulated.size() - first);
        const QByteArray bytes(
            reinterpret_cast<const char *>(demodulated.data() + first),
            static_cast<int>(count * sizeof(short)));
        decoder.processSstvPcmFrame(bytes, SampleRate);
    }

    const QString visNeedle = QStringLiteral("VIS %1").arg(expectedVis);
    const bool visDetected = std::any_of(statuses.cbegin(), statuses.cend(),
                                         [&visNeedle, &expectedMode](const QString &status) {
                                             return status.contains(visNeedle) &&
                                                    status.contains(expectedMode);
                                         });
    const bool pixelsDecoded = frame.size() == expectedSize &&
                               frame.pixel(0, 0) != qRgb(8, 10, 12);
    if (!visDetected || !pixelsDecoded) {
        qWarning().noquote() << label << "SSTV pipeline failed:"
                             << statuses.join(QStringLiteral(" | "));
        return false;
    }
    qInfo() << label << "SSTV IQ pipeline PASS" << demodulated.size() << "PCM samples";
    return true;
}

bool decodeThroughReceiver(const QByteArray &sourcePcm,
                           int sstvDemodulationMode,
                           const char *label,
                           bool simplifiedChannelizer = false,
                           int rfSampleRate = SampleRate,
                           double deviationHz = 2500.0,
                           double carrierOffsetHz = 0.0) {
    const std::vector<float> audio = pcmSamples(sourcePcm);
    const std::vector<float> iq = sstvDemodulationMode == SSTV_DEMOD_NFM
                                      ? modulateNfm(audio,
                                                    rfSampleRate,
                                                    deviationHz,
                                                    carrierOffsetHz,
                                                    simplifiedChannelizer)
                                      : modulateSsb(audio,
                                                    sstvDemodulationMode == SSTV_DEMOD_LSB);

    RadioSettings settings;
    settings.inputMode = INPUT_RF;
    settings.sampleRate = rfSampleRate;
    settings.centerFrequency = 145500000.0;
    settings.listeningFrequency = settings.centerFrequency + carrierOffsetHz;
    settings.bandwidth = 3000.0;
    settings.modulationType = MOD_SSTV;
    settings.sstvDemodulationMode = sstvDemodulationMode;
    settings.simplifiedAudioChannelizer = simplifiedChannelizer;
    settings.audioFilterChainJson = QStringLiteral("[]");
    return decodeIqThroughReceiver(iq,
                                   settings,
                                   label,
                                   44,
                                   QStringLiteral("Martin M1"),
                                   QSize(320, 256));
}

bool decodeGeneratedTxNfm() {
    QImage image(320, 240, QImage::Format_RGB32);
    image.fill(QColor(190, 70, 220));
    TxAudioMedia media = TransmitMediaGenerator::generateSstv(
        image, QStringLiteral("robot36"), SampleRate);
    if (!media.ok()) {
        qWarning().noquote() << "Generated NFM SSTV media failed:" << media.error;
        return false;
    }

    const int usefulAudioSamples = (std::min)(media.samples.size(), SampleRate * 3);
    media.samples.resize(usefulAudioSamples);
    TxConfiguration configuration;
    configuration.modulation = TxModulation::Nfm;
    configuration.sampleRate = SampleRate;
    configuration.level = 0.72f;
    configuration.deviationHz = 2500.0;
    configuration.signalBandwidthHz = 12500.0;
    const QVector<std::complex<float>> baseband =
        TransmitWaveformGenerator::modulateAudio(configuration, media.samples);
    if (baseband.isEmpty()) return false;

    constexpr int RfSampleRate = 384000;
    constexpr double CarrierOffsetHz = 40000.0;
    constexpr double ResidualTuneErrorHz = 275.0;
    constexpr int Interpolation = RfSampleRate / SampleRate;
    std::vector<float> iq;
    iq.reserve(static_cast<std::size_t>(baseband.size()) * Interpolation * 2U);
    double carrierPhase = 0.0;
    std::size_t rfIndex = 0;
    for (int source = 0; source < baseband.size(); ++source) {
        const std::complex<float> first = baseband[source];
        const std::complex<float> second = baseband[(std::min)(source + 1, baseband.size() - 1)];
        for (int subSample = 0; subSample < Interpolation; ++subSample, ++rfIndex) {
            const float fraction = static_cast<float>(subSample) / Interpolation;
            const std::complex<float> interpolated = first + (second - first) * fraction;
            const float fading = static_cast<float>(
                0.70 + 0.20 * std::sin(TwoPi * 5.7 * rfIndex / RfSampleRate));
            const std::complex<float> shifted = interpolated * fading *
                std::polar(1.0f, static_cast<float>(carrierPhase));
            iq.push_back(std::real(shifted));
            iq.push_back(std::imag(shifted));
            carrierPhase = std::remainder(
                carrierPhase + TwoPi * CarrierOffsetHz / RfSampleRate,
                TwoPi);
        }
    }

    RadioSettings settings;
    settings.inputMode = INPUT_RF;
    settings.sampleRate = RfSampleRate;
    settings.centerFrequency = 145500000.0;
    settings.listeningFrequency = settings.centerFrequency +
                                  CarrierOffsetHz - ResidualTuneErrorHz;
    settings.bandwidth = 12500.0;
    settings.modulationType = MOD_SSTV;
    settings.sstvDemodulationMode = SSTV_DEMOD_NFM;
    settings.simplifiedAudioChannelizer = false;
    settings.audioFilterChainJson = QStringLiteral("[]");
    return decodeIqThroughReceiver(iq,
                                   settings,
                                   "Generated TX NFM",
                                   8,
                                   QStringLiteral("Robot 36"),
                                   QSize(320, 240));
}

void appendTone(QByteArray &pcm,
                double frequencyHz,
                double seconds,
                double &phase,
                double toneOffsetHz = 0.0) {
    const int sampleCount = qRound(seconds * SampleRate);
    const double phaseStep = TwoPi * (frequencyHz + toneOffsetHz) / SampleRate;
    pcm.reserve(pcm.size() + sampleCount * static_cast<int>(sizeof(qint16)));
    for (int sample = 0; sample < sampleCount; ++sample) {
        const qint16 value = static_cast<qint16>(std::lround(std::sin(phase) * 22000.0));
        pcm.append(static_cast<char>(value & 0xff));
        pcm.append(static_cast<char>((value >> 8) & 0xff));
        phase = std::remainder(phase + phaseStep, TwoPi);
    }
}

void appendSilence(QByteArray &pcm, double seconds) {
    pcm.append(QByteArray(qRound(seconds * SampleRate) * static_cast<int>(sizeof(qint16)), '\0'));
}

void addPcmInterference(QByteArray &pcm,
                        int firstSample,
                        int sampleCount,
                        double frequencyHz,
                        double amplitude) {
    const int availableSamples = pcm.size() / static_cast<int>(sizeof(qint16));
    const int endSample = (std::min)(availableSamples, firstSample + sampleCount);
    auto *raw = reinterpret_cast<unsigned char *>(pcm.data());
    for (int sample = (std::max)(0, firstSample); sample < endSample; ++sample) {
        const int byteOffset = sample * static_cast<int>(sizeof(qint16));
        const quint16 bits = static_cast<quint16>(raw[byteOffset]) |
                             (static_cast<quint16>(raw[byteOffset + 1]) << 8);
        const int original = static_cast<qint16>(bits);
        const int interference = qRound(
            amplitude * std::sin(TwoPi * frequencyHz * sample / SampleRate));
        const qint16 mixed = static_cast<qint16>((std::clamp)(original + interference,
                                                              -32768,
                                                              32767));
        raw[byteOffset] = static_cast<unsigned char>(mixed & 0xff);
        raw[byteOffset + 1] = static_cast<unsigned char>((mixed >> 8) & 0xff);
    }
}

void appendVis(QByteArray &pcm, int visCode, double &phase, double toneOffsetHz) {
    appendTone(pcm, 1900.0, 0.300, phase, toneOffsetHz);
    appendTone(pcm, 1200.0, 0.010, phase, toneOffsetHz);
    appendTone(pcm, 1900.0, 0.300, phase, toneOffsetHz);
    appendTone(pcm, 1200.0, 0.030, phase, toneOffsetHz);

    int oneCount = 0;
    for (int bit = 0; bit < 7; ++bit) {
        const bool one = (visCode & (1 << bit)) != 0;
        oneCount += one ? 1 : 0;
        appendTone(pcm, one ? 1100.0 : 1300.0, 0.030, phase, toneOffsetHz);
    }
    appendTone(pcm, (oneCount & 1) != 0 ? 1100.0 : 1300.0, 0.030, phase, toneOffsetHz);
    appendTone(pcm, 1200.0, 0.030, phase, toneOffsetHz);
}

void appendMartinM1Line(QByteArray &pcm,
                        double &phase,
                        double toneOffsetHz,
                        double timingScale) {
    const int initialBytes = pcm.size();
    appendTone(pcm, 1200.0, 0.004862 * timingScale, phase, toneOffsetHz);
    appendTone(pcm, 1500.0, 0.000572 * timingScale, phase, toneOffsetHz);
    appendTone(pcm, 1700.0, 0.146432 * timingScale, phase, toneOffsetHz);
    appendTone(pcm, 1500.0, 0.000572 * timingScale, phase, toneOffsetHz);
    appendTone(pcm, 1900.0, 0.146432 * timingScale, phase, toneOffsetHz);
    appendTone(pcm, 1500.0, 0.000572 * timingScale, phase, toneOffsetHz);
    appendTone(pcm, 2100.0, 0.146432 * timingScale, phase, toneOffsetHz);
    appendTone(pcm, 1500.0, 0.000572 * timingScale, phase, toneOffsetHz);

    const int writtenSamples = (pcm.size() - initialBytes) /
                               static_cast<int>(sizeof(qint16));
    const int targetSamples = qRound(0.446446 * timingScale * SampleRate);
    if (writtenSamples < targetSamples) {
        appendTone(pcm,
                   1500.0,
                   static_cast<double>(targetSamples - writtenSamples) / SampleRate,
                   phase,
                   toneOffsetHz);
    }
}

void appendScottieStart(QByteArray &pcm,
                        double &phase,
                        double toneOffsetHz) {
    appendTone(pcm, 1200.0, 0.009, phase, toneOffsetHz);
    appendTone(pcm, 1500.0, 0.0015, phase, toneOffsetHz);
    appendTone(pcm, 1700.0, 0.138240, phase, toneOffsetHz); // G0
    appendTone(pcm, 1500.0, 0.0015, phase, toneOffsetHz);
    appendTone(pcm, 1900.0, 0.138240, phase, toneOffsetHz); // B0
}

void appendScottieRegularSegment(QByteArray &pcm,
                                 double &phase,
                                 double toneOffsetHz) {
    appendTone(pcm, 1200.0, 0.009, phase, toneOffsetHz);
    appendTone(pcm, 1500.0, 0.0015, phase, toneOffsetHz);
    appendTone(pcm, 2100.0, 0.138240, phase, toneOffsetHz); // current R
    appendTone(pcm, 1500.0, 0.0015, phase, toneOffsetHz);
    appendTone(pcm, 1700.0, 0.138240, phase, toneOffsetHz); // next G
    appendTone(pcm, 1500.0, 0.0015, phase, toneOffsetHz);
    appendTone(pcm, 1900.0, 0.138240, phase, toneOffsetHz); // next B
}

void appendRobot36Line(QByteArray &pcm,
                       double &phase,
                       bool redDifferenceLine) {
    appendTone(pcm, 1200.0, 0.009, phase);
    appendTone(pcm, 1500.0, 0.003, phase);
    appendTone(pcm, 1500.0, 0.044, phase);
    appendTone(pcm, 2300.0, 0.044, phase);
    appendTone(pcm, redDifferenceLine ? 1500.0 : 2300.0, 0.0045, phase);
    appendTone(pcm, 1900.0, 0.0015, phase);
    appendTone(pcm, redDifferenceLine ? 2100.0 : 1700.0, 0.044, phase);
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    VideoProcessor decoder;
    QStringList statuses;
    QImage latestFrame;
    int frameCount = 0;
    QObject::connect(&decoder, &VideoProcessor::statusChanged,
                     [&statuses](const QString &status) { statuses.append(status); });
    QObject::connect(&decoder, &VideoProcessor::frameReady,
                     [&latestFrame, &frameCount](const QImage &frame) {
                         latestFrame = frame;
                         ++frameCount;
                     });

    QByteArray pcm;
    double phase = 0.0;
    constexpr double ToneOffsetHz = 75.0;
    constexpr double TimingScale = 0.997;
    appendSilence(pcm, 1.2037);
    appendVis(pcm, 44, phase, ToneOffsetHz);
    addPcmInterference(pcm,
                       qRound(1.2037 * SampleRate),
                       qRound(0.910 * SampleRate),
                       7400.0,
                       6500.0);
    for (int line = 0; line < 6; ++line) {
        appendMartinM1Line(pcm, phase, ToneOffsetHz, TimingScale);
        if (line == 1) {
            appendSilence(pcm, 0.180);
        }
    }
    appendSilence(pcm, 3.0);

    decoder.configureSstv(true);
    const int chunkBytes = SampleRate / 50 * static_cast<int>(sizeof(qint16));
    for (int offset = 0; offset < pcm.size(); offset += chunkBytes) {
        decoder.processSstvPcmFrame(pcm.mid(offset, (std::min)(chunkBytes, pcm.size() - offset)),
                                    SampleRate);
    }

    bool martinDetected = false;
    for (const QString &status : statuses) {
        if (status.contains(QStringLiteral("VIS 44")) &&
            status.contains(QStringLiteral("Martin M1"))) {
            martinDetected = true;
            break;
        }
    }
    qInfo().noquote() << statuses.join(QStringLiteral("\n"));
    qInfo() << "Frame" << latestFrame.size();
    if (!martinDetected) return 1;
    if (latestFrame.size() != QSize(320, 256)) return 2;
    if (frameCount < 2) return 3;
    if (latestFrame.pixel(0, 0) == qRgb(8, 10, 12)) return 4;
    bool timedOutAfterTransmission = false;
    for (const QString &status : statuses) {
        timedOutAfterTransmission = timedOutAfterTransmission ||
                                    status.contains(QStringLiteral("sync timeout"));
    }
    if (!timedOutAfterTransmission) return 5;

    VideoProcessor scottieDecoder;
    QImage scottieFrame;
    QObject::connect(&scottieDecoder, &VideoProcessor::frameReady,
                     [&scottieFrame](const QImage &frame) { scottieFrame = frame; });
    QByteArray scottiePcm;
    phase = 0.0;
    appendVis(scottiePcm, 60, phase, 0.0);
    appendScottieStart(scottiePcm, phase, 0.0);
    for (int line = 0; line < 4; ++line) {
        appendScottieRegularSegment(scottiePcm, phase, 0.0);
    }
    appendSilence(scottiePcm, 0.5);
    scottieDecoder.configureSstv(true);
    for (int offset = 0; offset < scottiePcm.size(); offset += chunkBytes) {
        scottieDecoder.processSstvPcmFrame(
            scottiePcm.mid(offset, (std::min)(chunkBytes, scottiePcm.size() - offset)),
            SampleRate);
    }
    if (scottieFrame.size() != QSize(320, 256)) return 6;
    const QRgb scottiePixel = scottieFrame.pixel(160, 0);
    if (!(qRed(scottiePixel) > qBlue(scottiePixel) + 30 &&
          qBlue(scottiePixel) > qGreen(scottiePixel) + 30)) {
        qWarning() << "Unexpected Scottie channel order" << QColor(scottiePixel);
        return 7;
    }


    VideoProcessor robotDecoder;
    QImage robotFrame;
    QObject::connect(&robotDecoder, &VideoProcessor::frameReady,
                     [&robotFrame](const QImage &frame) { robotFrame = frame; });
    QByteArray robotPcm;
    phase = 0.0;
    appendVis(robotPcm, 8, phase, 0.0);
    for (int line = 0; line < 6; ++line) {
        appendRobot36Line(robotPcm, phase, (line % 2) == 0);
    }
    appendSilence(robotPcm, 0.5);
    robotDecoder.configureSstv(true);
    for (int offset = 0; offset < robotPcm.size(); offset += chunkBytes) {
        robotDecoder.processSstvPcmFrame(
            robotPcm.mid(offset, (std::min)(chunkBytes, robotPcm.size() - offset)),
            SampleRate);
    }
    if (robotFrame.size() != QSize(320, 240)) return 8;
    const QRgb robotPixel = robotFrame.pixel(160, 0);
    if (qGray(robotFrame.pixel(40, 0)) >= qGray(robotFrame.pixel(280, 0)) - 80) {
        qWarning() << "Unexpected Robot36 luma decode" << QColor(robotPixel);
        return 9;
    }
    int firstTransition = -1;
    int lastTransition = -1;
    for (int y = 0; y < 4; ++y) {
        int transition = -1;
        for (int x = 1; x < 320; ++x) {
            if (qGray(robotFrame.pixel(x, y)) > 150) {
                transition = x;
                break;
            }
        }
        if (transition < 0) return 10;
        firstTransition = firstTransition < 0 ? transition : (std::min)(firstTransition, transition);
        lastTransition = (std::max)(lastTransition, transition);
    }
    if (lastTransition - firstTransition > 3) {
        std::fprintf(stderr, "Robot36 line jitter %d..%d\n", firstTransition, lastTransition);
        qWarning() << "Robot36 line jitter" << firstTransition << lastTransition;
        return 11;
    }

    VideoProcessor noiseDecoder;
    int falseFrames = 0;
    QObject::connect(&noiseDecoder, &VideoProcessor::frameReady,
                     [&falseFrames](const QImage &) { ++falseFrames; });
    QByteArray noisePcm;
    quint32 randomState = 0x12345678u;
    for (int sample = 0; sample < SampleRate * 3; ++sample) {
        randomState = randomState * 1664525u + 1013904223u;
        const qint16 value = static_cast<qint16>((randomState >> 16) - 32768);
        noisePcm.append(static_cast<char>(value & 0xff));
        noisePcm.append(static_cast<char>((value >> 8) & 0xff));
    }
    noiseDecoder.configureSstv(true);
    for (int offset = 0; offset < noisePcm.size(); offset += chunkBytes) {
        noiseDecoder.processSstvPcmFrame(
            noisePcm.mid(offset, (std::min)(chunkBytes, noisePcm.size() - offset)),
            SampleRate);
    }
    if (falseFrames != 0) return 12;
    if (!decodeThroughReceiver(pcm, SSTV_DEMOD_LSB, "LSB")) return 13;
    if (!decodeThroughReceiver(pcm, SSTV_DEMOD_NFM, "NFM")) return 14;
    if (!decodeThroughReceiver(pcm,
                               SSTV_DEMOD_NFM,
                               "NFM high-rate streaming",
                               true,
                               384000,
                               5000.0,
                               40000.0)) return 15;
    if (!decodeGeneratedTxNfm()) return 16;
    return 0;
}
