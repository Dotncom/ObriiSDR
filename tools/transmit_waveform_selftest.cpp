#include "transmitwaveformgenerator.h"
#include "signallabgenerator.h"
#include "transmitterbackend.h"
#include "transmitmediagenerator.h"
#include "dmrtransmitencoder.h"
#include "ft8stationutils.h"
#include "videoprocessor.h"

#include <QCoreApplication>
#include <QColor>
#include <QDebug>
#include <QImage>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

// The standalone test does not link the live receiver FFT processor.
std::mutex &fftwPlannerGlobalMutex() {
    static std::mutex mutex;
    return mutex;
}

namespace {

bool testBit(const QByteArray &bytes, int bit) {
    return bit >= 0 && bit < bytes.size() * 8 &&
           (static_cast<quint8>(bytes.at(bit / 8)) & (0x80U >> (bit % 8))) != 0;
}

QByteArray dmrSyncField(const QByteArray &burst) {
    QByteArray sync(6, '\0');
    for (int bit = 0; bit < 48; ++bit) {
        if (!testBit(burst, 108 + bit)) continue;
        sync[bit / 8] = static_cast<char>(static_cast<quint8>(sync.at(bit / 8)) |
                                          (0x80U >> (bit % 8)));
    }
    return sync;
}

int oneBits(const QByteArray &bytes) {
    int count = 0;
    for (char raw : bytes) {
        quint8 value = static_cast<quint8>(raw);
        while (value != 0U) {
            count += value & 1U;
            value >>= 1U;
        }
    }
    return count;
}

} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    TxConfiguration configuration;
    configuration.modulation = TxModulation::Cw;
    configuration.sampleRate = 8000;
    configuration.toneHz = 700.0;
    configuration.cwWpm = 18;
    const TxGenerationResult cw =
        TransmitWaveformGenerator::generateText(configuration,
                                                QString::fromUtf8(u8"SOS УКРАЇНА"));
    if (!cw.ok() || cw.durationSeconds < 1.0) {
        qCritical().noquote() << "CW generation failed:" << cw.error;
        return 1;
    }

    configuration.modulation = TxModulation::Ft8;
    configuration.sampleRate = 48000;
    const TxGenerationResult ft8 =
        TransmitWaveformGenerator::generateText(configuration, QStringLiteral("CQ TEST KN34"));
    const int expectedSamples = qRound(79.0 * 0.160 * configuration.sampleRate);
    if (!ft8.ok() || ft8.iq.size() != expectedSamples) {
        qCritical().noquote() << "FT8 generation failed:" << ft8.error
                              << "samples" << ft8.iq.size()
                              << "expected" << expectedSamples;
        return 2;
    }

    const Ft8MessageInfo cqInfo = parseFt8Message(QStringLiteral("CQ UR5ABC KN34"));
    const Ft8MessageInfo reportInfo = parseFt8Message(QStringLiteral("UR5ABC K1ABC -12"));
    if (!cqInfo.valid || !cqInfo.cq || cqInfo.senderCall != QStringLiteral("UR5ABC") ||
        cqInfo.grid != QStringLiteral("KN34") || !reportInfo.valid ||
        reportInfo.senderCall != QStringLiteral("K1ABC") ||
        reportInfo.recipientCall != QStringLiteral("UR5ABC")) return 26;    const QVector<Ft8CallsignRule> filterRules = {
        {QStringLiteral("Russia"),
         {QStringLiteral("R"), QStringLiteral("UA"), QStringLiteral("UB"),
          QStringLiteral("UC"), QStringLiteral("UD"), QStringLiteral("UE"),
          QStringLiteral("UF"), QStringLiteral("UG"), QStringLiteral("UH"),
          QStringLiteral("UI")}, true},
        {QStringLiteral("Ukraine"),
         {QStringLiteral("EM"), QStringLiteral("EN"), QStringLiteral("EO"),
          QStringLiteral("UR"), QStringLiteral("US"), QStringLiteral("UT"),
          QStringLiteral("UU"), QStringLiteral("UV"), QStringLiteral("UW"),
          QStringLiteral("UX"), QStringLiteral("UY"), QStringLiteral("UZ")}, false}
    };
    if (!ft8CallsignPatternMatches(QStringLiteral("RA3ABC"), QStringLiteral("RA*")) ||
        ft8CallsignPatternMatches(QStringLiteral("UR5ABC"), QStringLiteral("RA*"))) return 38;
    const Ft8CallsignFilterResult exactBlocked = evaluateFt8Callsign(
        QStringLiteral("K1ABC"), {QStringLiteral("K1ABC")}, filterRules);
    if (!exactBlocked.blocked || !exactBlocked.callsignBlocked) return 39;
    const Ft8CallsignFilterResult russian = evaluateFt8Callsign(
        QStringLiteral("R3ABC"), {}, filterRules);
    if (!russian.blocked || !russian.countryBlocked ||
        russian.country != QStringLiteral("Russia")) return 40;
    const Ft8CallsignFilterResult ukrainian = evaluateFt8Callsign(
        QStringLiteral("DL/UR5ABC/P"), {}, filterRules);
    if (ukrainian.blocked || ukrainian.country != QStringLiteral("Ukraine") ||
        normalizeFt8Callsign(QStringLiteral("<ur5abc>")) != QStringLiteral("UR5ABC")) return 41;
    Ft8QsoRecord qso;
    qso.timeOnUtc = QDateTime::fromString(QStringLiteral("2026-10-11T12:00:00Z"), Qt::ISODate);
    qso.timeOffUtc = qso.timeOnUtc.addSecs(90);
    qso.stationCall = QStringLiteral("UR5ABC");
    qso.stationGrid = QStringLiteral("KN34");
    qso.call = QStringLiteral("K1ABC");
    qso.grid = QStringLiteral("FN20");
    qso.reportSent = QStringLiteral("-10");
    qso.reportReceived = QStringLiteral("-12");
    qso.frequencyHz = 14074000.0;
    const QString adif = ft8AdifRecord(qso);
    const QVector<Ft8QsoRecord> parsed = parseFt8Adif(ft8AdifDocument({qso}).toUtf8());
    if (!adif.contains(QStringLiteral("<MODE:3>FT8")) || parsed.size() != 1 ||
        parsed.front().call != qso.call || std::abs(parsed.front().frequencyHz - qso.frequencyHz) > 0.5) return 27;
    Ft8ReceptionReport reception;
    reception.senderCall = QStringLiteral("K1ABC");
    reception.senderGrid = QStringLiteral("FN20");
    reception.frequencyHz = 14075500U;
    reception.epochSeconds = 1791720000U;
    const QByteArray psk = buildPskReporterPacket(QStringLiteral("UR5ABC"), QStringLiteral("KN34"),
        QStringLiteral("ObriiSDR test"), QStringLiteral("dipole"), {reception}, 1, 2, true);
    if (psk.size() < 64 || qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(psk.constData() + 2)) != psk.size()) return 28;
    const QByteArray wsjt = buildWsjtDecode(QStringLiteral("ObriiSDR"), qso.timeOnUtc,
                                            -12, 0.1, 1500, QStringLiteral("UR5ABC K1ABC -12"));
    if (wsjt.size() < 24 || qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(wsjt.constData())) != 0xadbccbdaU) return 29;
    configuration.sampleRate = 48000;
    configuration.signalBandwidthHz = 0.0;
    configuration.toneHz = 0.0;
    configuration.symbolRate = 1200.0;
    configuration.deviationHz = 600.0;
    configuration.gaussianBt = 0.5;
    configuration.askDepth = 0.70f;
    configuration.level = 0.60f;
    configuration.invertData = false;
    for (TxModulation mode : {TxModulation::Ook,
                              TxModulation::Ask,
                              TxModulation::Fsk,
                              TxModulation::Gfsk,
                              TxModulation::Gmsk,
                              TxModulation::Bpsk,
                              TxModulation::Qpsk}) {
        configuration.modulation = mode;
        const TxGenerationResult digital =
            TransmitWaveformGenerator::generateText(configuration,
                                                    QStringLiteral("HELLO 123"));
        if (!digital.ok() || digital.iq.isEmpty() || digital.durationSeconds <= 0.0) {
            qCritical().noquote() << "Digital text generation failed:"
                                  << TransmitWaveformGenerator::modulationName(mode)
                                  << digital.error;
            return 3;
        }
        float minimumMagnitude = std::numeric_limits<float>::max();
        float maximumMagnitude = 0.0f;
        for (const std::complex<float> &sample : digital.iq) {
            if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) return 10;
            const float magnitude = std::abs(sample);
            minimumMagnitude = (std::min)(minimumMagnitude, magnitude);
            maximumMagnitude = (std::max)(maximumMagnitude, magnitude);
        }
        if (maximumMagnitude < 0.5f) return 5;
        const bool amplitudeKeying = mode == TxModulation::Ook || mode == TxModulation::Ask;
        if (amplitudeKeying && minimumMagnitude > maximumMagnitude * 0.90f) return 6;
        if (!amplitudeKeying && maximumMagnitude - minimumMagnitude > 0.001f) return 7;
    }

    SimulatorTransmitterBackend highRateSimulator;
    TxConfiguration highRateConfiguration;
    highRateConfiguration.sampleRate = 100000000;
    QString highRateError;
    const std::complex<float> highRateSamples[] = {
        {0.25f, -0.25f}, {-0.5f, 0.5f}};
    if (!highRateSimulator.start(highRateConfiguration, &highRateError) ||
        !highRateSimulator.writeIq(highRateSamples, 2, &highRateError) ||
        highRateSimulator.capturedIq().size() != 2) {
        qCritical().noquote() << "TX simulator high-rate capacity failed:" << highRateError;
        return 36;
    }

    SignalLabConfiguration highRateLab;
    highRateLab.sampleRate = 50000000;
    highRateLab.durationSeconds = 0.0001;
    highRateLab.amplitude = 0.5;
    highRateLab.waveform = SignalLabWaveform::MultiTone;
    highRateLab.multiToneFrequencies = QStringLiteral("-1000000, 1000000");
    const SignalLabGenerationResult highRateSignal =
        SignalLabGenerator::generate(highRateLab);
    if (!highRateSignal.ok() || highRateSignal.iq.size() != 5000 ||
        highRateSignal.sampleRate != 50000000) {
        qCritical().noquote() << "Signal Lab 50 MS/s generation failed:"
                              << highRateSignal.error;
        return 37;
    }

    SignalLabConfiguration lab;
    highRateLab.sampleRate = 1000000000;
    highRateLab.durationSeconds = 0.00001;
    const auto gigaSignal = SignalLabGenerator::generate(highRateLab);
    highRateConfiguration.sampleRate = 1000000000;
    if (!gigaSignal.ok() || gigaSignal.iq.size() != 10000 ||
        !highRateSimulator.start(highRateConfiguration, &highRateError) ||
        !highRateSimulator.writeIq(gigaSignal.iq.constData(), gigaSignal.iq.size(), &highRateError)) {
        qCritical() << "1 Gsps short-buffer generation/simulation failed" << gigaSignal.error;
        return 38;
    }
    highRateLab.durationSeconds = 1e30;
    if (SignalLabGenerator::generate(highRateLab).ok()) {
        qCritical() << "High-rate buffer budget was not enforced";
        return 39;
    }
    lab.sampleRate = 48000;
    lab.durationSeconds = 0.05;
    lab.amplitude = 0.60;
    lab.symbolRate = 2400.0;
    lab.rolloff = 0.35;
    lab.randomSeed = 1234;
    lab.waveform = SignalLabWaveform::Qam64;
    const SignalLabGenerationResult qam = SignalLabGenerator::generate(lab);
    if (!qam.ok() || qam.iq.size() != 2400 || qam.peak > 0.601 ||
        qam.rms <= 0.05 || qam.occupiedBandwidthHz <= 2400.0) {
        qCritical().noquote() << "Signal Lab 64-QAM generation failed:" << qam.error
                              << qam.summary;
        return 30;
    }
    for (const std::complex<float> &sample : qam.iq) {
        if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) return 31;
    }

    lab.waveform = SignalLabWaveform::Ofdm;
    lab.ofdmFftSize = 128;
    lab.ofdmActiveCarriers = 72;
    lab.ofdmCyclicPrefix = 16;
    lab.ofdmSymbolCount = 10;
    lab.ofdmPilotSpacing = 12;
    lab.ofdmConstellation = SignalLabConstellation::Qam16;
    lab.ofdmNullDc = true;
    const SignalLabGenerationResult ofdm = SignalLabGenerator::generate(lab);
    if (!ofdm.ok() || ofdm.iq.size() != 1440 ||
        std::abs(ofdm.occupiedBandwidthHz - 27000.0) > 1.0 ||
        ofdm.peak > 0.601 || ofdm.paprDb < 3.0) {
        qCritical().noquote() << "Signal Lab OFDM generation failed:" << ofdm.error
                              << ofdm.summary;
        return 32;
    }
    for (const std::complex<float> &sample : ofdm.iq) {
        if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) return 33;
    }
    lab.ofdmActiveCarriers = 73;
    lab.ofdmCyclicPrefix = 0;
    lab.ofdmSymbolCount = 1;
    lab.ofdmPilotSpacing = 1;
    lab.ofdmNullDc = false;
    lab.ofdmEdgeWindowPercent = 0.0;
    const SignalLabGenerationResult ofdmWithDc = SignalLabGenerator::generate(lab);
    std::complex<double> dcMean(0.0, 0.0);
    for (const std::complex<float> &sample : ofdmWithDc.iq) dcMean += sample;
    if (!ofdmWithDc.ok() || ofdmWithDc.iq.size() != 128 ||
        std::abs(dcMean / static_cast<double>(ofdmWithDc.iq.size())) < 0.001) {
        qCritical().noquote() << "Signal Lab OFDM DC carrier toggle failed:"
                              << ofdmWithDc.error;
        return 34;
    }

    lab.waveform = SignalLabWaveform::Qam64;
    lab.centerOffsetHz = 23000.0;
    const SignalLabGenerationResult aliased = SignalLabGenerator::generate(lab);
    if (aliased.ok() || !aliased.iq.isEmpty()) {
        qCritical() << "Signal Lab accepted an out-of-band center offset";
        return 35;
    }
    configuration.modulation = TxModulation::Rtty;
    configuration.symbolRate = 45.45;
    configuration.toneHz = 2210.0;
    configuration.deviationHz = 85.0;
    const TxGenerationResult rtty =
        TransmitWaveformGenerator::generateText(configuration,
                                                QStringLiteral("CQ TEST 123"));
    if (!rtty.ok() || rtty.durationSeconds < 1.0 ||
        std::abs(TransmitWaveformGenerator::effectiveFskDeviationHz(configuration) - 85.0) >
            0.01) {
        qCritical().noquote() << "RTTY generation failed:" << rtty.error;
        return 8;
    }
    QVector<float> audio(4096);
    for (int i = 0; i < audio.size(); ++i) {
        audio[i] = static_cast<float>(0.5 * std::sin(6.283185307179586 * 1000.0 * i /
                                                   configuration.sampleRate));
    }
    for (TxModulation mode : {TxModulation::Am,
                              TxModulation::Nfm,
                              TxModulation::Wfm,
                              TxModulation::Dsb,
                              TxModulation::Usb,
                              TxModulation::Lsb}) {
        configuration.modulation = mode;
        TxModulatorState state;
        const QVector<std::complex<float>> iq =
            TransmitWaveformGenerator::modulateAudio(configuration, audio, &state);
        if (iq.size() != audio.size()) return 9;
        for (const std::complex<float> &sample : iq) {
            if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) return 10;
        }
    }

    QImage image(32, 24, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor::fromRgb((x * 255) / image.width(),
                                                      (y * 255) / image.height(),
                                                      ((x + y) * 255) /
                                                          (image.width() + image.height())));
        }
    }
    const TxAudioMedia sstv = TransmitMediaGenerator::generateSstv(
        image, QStringLiteral("robot36"), 12000);
    if (!sstv.ok() || sstv.samples.size() < 12000) return 11;

    configuration.sampleRate = 2400000;
    configuration.modulation = TxModulation::Am;
    const QVector<std::complex<float>> atv =
        TransmitMediaGenerator::generateAtvFrame(image, configuration, false, 384, 288);
    if (atv.isEmpty()) return 12;
    for (const std::complex<float> &sample : atv) {
        if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) return 13;
    }

    QByteArray atvIq;
    atvIq.resize(atv.size() * 4);
    char *atvBytes = atvIq.data();
    for (int i = 0; i < atv.size(); ++i) {
        const qint16 iSample = static_cast<qint16>(std::lround(
            (std::clamp)(atv[i].real(), -1.0f, 1.0f) * 32767.0f));
        const qint16 qSample = static_cast<qint16>(std::lround(
            (std::clamp)(atv[i].imag(), -1.0f, 1.0f) * 32767.0f));
        std::memcpy(atvBytes + i * 4, &iSample, sizeof(iSample));
        std::memcpy(atvBytes + i * 4 + 2, &qSample, sizeof(qSample));
    }
    VideoProcessor decoder;
    QImage decodedAtv;
    QObject::connect(&decoder, &VideoProcessor::frameReady,
                     [&decodedAtv](const QImage &frame) { decodedAtv = frame; });
    decoder.configure(true, VideoProcessor::AmVideo, 15625.0, 384, 288,
                      false, true, true);
    decoder.processIqFrame(atvIq, configuration.sampleRate, atv.size());
    if (decodedAtv.isNull()) return 14;
    int darkest = 255;
    int brightest = 0;
    for (int y = 0; y < decodedAtv.height(); y += 8) {
        const uchar *line = decodedAtv.constScanLine(y);
        for (int x = 0; x < decodedAtv.width(); x += 8) {
            darkest = (std::min)(darkest, static_cast<int>(line[x]));
            brightest = (std::max)(brightest, static_cast<int>(line[x]));
        }
    }
    if (brightest - darkest < 40) return 15;
    DmrTxParameters dmr;
    dmr.outputSampleRate = 48000;
    dmr.level = 0.5f;
    dmr.colorCode = 1;
    dmr.timeslot = 1;
    dmr.sourceId = 123456;
    dmr.destinationId = 9;
    const QByteArray dmrHeader = DmrTransmitEncoder::buildLinkControlBurst(dmr, false);
    const QByteArray dmrTerminator = DmrTransmitEncoder::buildLinkControlBurst(dmr, true);
    if (dmrHeader.size() != 33 || dmrTerminator.size() != 33 ||
        dmrSyncField(dmrHeader).toHex() != QByteArray("f7fdd5ddfd55")) {
        qCritical() << "DMR TS1 LC burst structure failed";
        return 16;
    }
    dmr.timeslot = 2;
    if (dmrSyncField(DmrTransmitEncoder::buildLinkControlBurst(dmr, false)).toHex() !=
        QByteArray("d7557f5ff7f5")) {
        qCritical() << "DMR TS2 LC sync failed";
        return 17;
    }
    dmr.timeslot = 1;
    std::array<QByteArray, 3> canonical = {
        QByteArray::fromHex("aa55aa55aa55aa55aa"),
        QByteArray::fromHex("0123456789abcdef01"),
        QByteArray::fromHex("fedcba9876543210fe")
    };
    const QByteArray onAir = DmrTransmitEncoder::canonicalToOnAir(canonical[0]);
    if (onAir.size() != 9 || oneBits(onAir) != oneBits(canonical[0])) return 18;
    for (int burstIndex = 0; burstIndex < 6; ++burstIndex) {
        const QByteArray voice = DmrTransmitEncoder::buildVoiceBurst(dmr, burstIndex, canonical);
        if (voice.size() != 33) return 19;
        if (burstIndex == 0 && dmrSyncField(voice).toHex() != QByteArray("5d577f7757ff")) {
            qCritical() << "DMR TS1 voice sync failed";
            return 20;
        }
    }
    QByteArray oneDmoFrame = dmrHeader;
    oneDmoFrame.append(QByteArray(39, '\0'));
    if (DmrTransmitEncoder::burstDibits(oneDmoFrame).size() != 288) return 21;

    DmrTransmitEncoder dmrEncoder;
    if (dmrEncoder.canEncodeVoice()) {
        QString dmrError;
        if (!dmrEncoder.configure(dmr, &dmrError)) {
            qCritical().noquote() << "DMR encoder configuration failed:" << dmrError;
            return 22;
        }
        const QVector<std::complex<float>> beginIq = dmrEncoder.begin(&dmrError);
        if (!dmrError.isEmpty() || beginIq.size() != 18240) return 23;
        QVector<float> voiceAudio(2880, 0.0f);
        const QVector<std::complex<float>> voiceIq = dmrEncoder.pushAudio(voiceAudio, &dmrError);
        if (!dmrError.isEmpty() || voiceIq.size() != 2880) return 24;
        const QVector<std::complex<float>> endIq = dmrEncoder.finish(&dmrError);
        if (!dmrError.isEmpty() || endIq.size() != 5760) return 25;
    } else {
        qInfo() << "DMR AMBE+2 encode backend unavailable; RF framing tests passed";
    }
    return 0;
}
