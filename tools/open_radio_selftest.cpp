#include "ax25codec.h"
#include "ax25afsk1200.h"
#include "ax25livedecoder.h"
#include "kisscodec.h"
#include "m17adapter.h"
#include "openradioprotocol.h"
#include "virtualsignalframe.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QJsonObject>
#include <QTimer>
#include <QtEndian>

#include <cmath>

namespace {
bool check(bool condition, const char *message) {
    if (!condition) qCritical().noquote() << message;
    return condition;
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QString error;

    Ax25UiFrame input;
    if (!Ax25Codec::parseAddress(QStringLiteral("APRS"), &input.destination, &error) ||
        !Ax25Codec::parseAddress(QStringLiteral("N0CALL-7"), &input.source, &error)) {
        qCritical().noquote() << error;
        return 1;
    }
    input.information = QByteArray("hello\xC0kiss\xDB", 12);
    const QByteArray ax25 = Ax25Codec::encodeUiFrame(input, false, &error);
    Ax25UiFrame decoded;
    if (!check(!ax25.isEmpty(), "AX.25 encode failed") ||
        !check(Ax25Codec::decodeUiFrame(ax25, &decoded, false, &error), "AX.25 decode failed") ||
        !check(decoded.information == input.information, "AX.25 information mismatch") ||
        !check(Ax25Codec::addressText(decoded.source) == QStringLiteral("N0CALL-7"), "AX.25 source mismatch"))
        return 2;
    if (!check(Ax25Codec::fcs(QByteArray("123456789")) == 0x906EU,
               "AX.25 CRC-16/X.25 fixture mismatch"))
        return 2;

    const QVector<float> afsk = Ax25Afsk1200::modulate(ax25, 48000, 32, &error);
    const QByteArray wav = Ax25Afsk1200::encodePcm16Wav(afsk, 48000, &error);
    QVector<float> wavAudio;
    int wavRate = 0;
    if (!check(!afsk.isEmpty() && !wav.isEmpty(), "AX.25 AFSK1200 generation failed") ||
        !check(Ax25Afsk1200::decodePcm16Wav(wav, &wavAudio, &wavRate, &error),
               "AX.25 AFSK1200 WAV decode failed") ||
        !check(wavRate == 48000 && wavAudio.size() == afsk.size(),
               "AX.25 AFSK1200 WAV properties mismatch")) return 8;
    const QList<QByteArray> recovered = Ax25Afsk1200::demodulate(wavAudio, wavRate, &error);
    if (!check(!recovered.isEmpty(), "AX.25 AFSK1200 demodulation failed") ||
        !check(recovered.first().left(ax25.size()) == ax25,
               "AX.25 AFSK1200 payload mismatch")) return 8;
    QVector<float> delayed(13, 0.0f);
    delayed += wavAudio;
    const QList<QByteArray> delayedFrames = Ax25Afsk1200::demodulate(delayed, wavRate, &error);
    if (!check(!delayedFrames.isEmpty() && delayedFrames.first() == recovered.first(),
               "AX.25 AFSK1200 sample-offset recovery failed")) return 8;
    wavAudio.fill(0.0f);
    if (!check(Ax25Afsk1200::demodulate(wavAudio, wavRate).isEmpty(),
               "AX.25 AFSK1200 accepted silence as frame")) return 8;

    QByteArray livePcm(48000 * 2 * 2, '\0');
    const int signalOffset = 4800;
    for (int i = 0; i < afsk.size() && signalOffset + i < 48000 * 2; ++i) {
        const qint16 value = static_cast<qint16>(std::lround(afsk[i] * 32767.0f));
        qToLittleEndian<qint16>(value,
            reinterpret_cast<uchar *>(livePcm.data() + (signalOffset + i) * 2));
    }
    Ax25LiveDecoder live;
    QEventLoop liveWait;
    QByteArray liveRecovered;
    QObject::connect(&live, &Ax25LiveDecoder::frameDecoded, &application,
                     [&](const QByteArray &data) { liveRecovered = data; liveWait.quit(); });
    QTimer::singleShot(5000, &liveWait, &QEventLoop::quit);
    live.start();
    live.submitPcm16(livePcm, 48000);
    liveWait.exec();
    live.stop();
    if (!check(liveRecovered == ax25, "Live AX.25 decoder did not recover the AFSK frame")) return 8;

    QByteArray shortPcm((4800 + afsk.size()) * 2, '\0');
    for (int i = 0; i < afsk.size(); ++i) {
        const qint16 value = static_cast<qint16>(std::lround(afsk[i] * 32767.0f));
        qToLittleEndian<qint16>(value,
            reinterpret_cast<uchar *>(shortPcm.data() + (4800 + i) * 2));
    }
    Ax25LiveDecoder shortLive;
    QByteArray shortRecovered;
    QEventLoop shortWait;
    QObject::connect(&shortLive, &Ax25LiveDecoder::frameDecoded, &application,
                     [&](const QByteArray &data) { shortRecovered = data; shortWait.quit(); });
    QTimer::singleShot(5000, &shortWait, &QEventLoop::quit);
    shortLive.start();
    shortLive.submitPcm16(shortPcm, 48000);
    shortWait.exec();
    shortLive.stop();
    if (!check(shortRecovered == ax25, "Short live AX.25 frame was not recovered")) return 8;

    const QByteArray kiss = KissCodec::encode({3, 0, ax25}, &error);
    KissStreamDecoder stream;
    const QList<KissFrame> first = stream.push(kiss.left(kiss.size() / 2));
    const QList<KissFrame> second = stream.push(kiss.mid(kiss.size() / 2));
    if (!check(first.isEmpty(), "KISS stream emitted a partial frame") ||
        !check(second.size() == 1, "KISS stream did not emit one frame") ||
        !check(second.front().port == 3 && second.front().payload == ax25, "KISS round trip mismatch"))
        return 3;

    QByteArray callsign;
    if (!check(M17Adapter::encodeCallsign(QStringLiteral("N0CALL"), &callsign, &error), "M17 callsign encode failed") ||
        !check(callsign.toHex().toUpper() == QByteArray("00004B13D106"), "M17 callsign reference fixture mismatch") ||
        !check(M17Adapter::decodeCallsign(callsign, &error) == QStringLiteral("N0CALL"), "M17 callsign round trip failed"))
        return 4;
    const M17PacketSymbols m17 = M17Adapter::buildPacket(
        QStringLiteral("N0CALL"), QStringLiteral("@ALL"), QByteArray("open-radio"), 3, &error);
    if (!check(m17.linkSetupFrame.size() == 30, "M17 LSF size mismatch") ||
        !check(m17.packetFrameCount == 1, "M17 packet frame count mismatch") ||
        !check(m17.symbols.size() == 192 * 4, "M17 preamble/LSF/packet/EOT symbol count mismatch"))
        return 5;
    const M17PacketSymbols maximumM17 = M17Adapter::buildPacket(
        QStringLiteral("N0CALL"), QStringLiteral("@ALL"), QByteArray(823, 'A'), 15, &error);
    if (!check(maximumM17.packetFrameCount == 33, "M17 maximum packet segmentation mismatch"))
        return 5;

    VirtualSignalFrame transport;
    transport.sequence = 42;
    transport.timestampNs = 123456789;
    transport.streamId = QStringLiteral("selftest/m17/1");
    transport.protocolId = QStringLiteral("m17");
    transport.payloadType = VirtualSignalPayloadType::SymbolsInt8;
    transport.symbolRate = 4800;
    transport.payload.resize(m17.symbols.size());
    for (int i = 0; i < m17.symbols.size(); ++i) transport.payload[i] = static_cast<char>(m17.symbols[i]);
    M17DecodedPacket decodedM17;
    if (!check(M17Adapter::decodePacketSymbols(transport.payload, &decodedM17, &error),
               "M17 symbol packet decode failed") ||
        !check(decodedM17.source == QStringLiteral("N0CALL") &&
               decodedM17.destination == QStringLiteral("@ALL") &&
               decodedM17.channelAccessNumber == 3 &&
               decodedM17.payload == QByteArray("open-radio") &&
               decodedM17.packetFrameCount == 1,
               "M17 symbol packet content mismatch")) return 1;
    QByteArray invalidSymbols = transport.payload;
    invalidSymbols[192] = 0;
    if (!check(!M17Adapter::decodePacketSymbols(invalidSymbols, &decodedM17, &error),
               "M17 accepted a corrupt LSF sync word")) return 1;
    invalidSymbols = transport.payload;
    invalidSymbols[invalidSymbols.size() - 1] = 0;
    if (!check(!M17Adapter::decodePacketSymbols(invalidSymbols, &decodedM17, &error),
               "M17 accepted a corrupt EOT")) return 1;
    QByteArray maximumSymbols(maximumM17.symbols.size(), '\0');
    for (int i = 0; i < maximumM17.symbols.size(); ++i)
        maximumSymbols[i] = static_cast<char>(maximumM17.symbols[i]);
    if (!check(M17Adapter::decodePacketSymbols(maximumSymbols, &decodedM17, &error) &&
               decodedM17.payload == QByteArray(823, 'A') && decodedM17.packetFrameCount == 33,
               "M17 multi-frame packet decode failed")) return 1;
    transport.metadata.insert(QStringLiteral("fixture"), true);
    const QByteArray wire = VirtualSignalFrameCodec::encode(transport, &error);
    VirtualSignalFrame restored;
    if (!check(VirtualSignalFrameCodec::decode(wire, &restored, &error), "Virtual signal decode failed") ||
        !check(restored.sequence == transport.sequence && restored.payload == transport.payload,
               "Virtual signal round trip mismatch"))
        return 6;

    if (!check(OpenRadioProtocolCatalog::protocols().size() == 7,
               "Open radio protocol catalog size mismatch"))
        return 7;

    qInfo().noquote() << "Open radio self-test PASS; libm17" << M17Adapter::libraryVersion();
    return 0;
}
