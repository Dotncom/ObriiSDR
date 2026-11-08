#include "clitextdecoder.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>

namespace {
double tonePowerAt(const QByteArray &pcm, double frequencyHz) {
    const int count = pcm.size() / 2;
    if (count < 8) return 0.0;
    const double coefficient = 2.0 * std::cos(2.0 * 3.14159265358979323846 * frequencyHz / 48000.0);
    double previous = 0.0, beforePrevious = 0.0;
    const auto *bytes = reinterpret_cast<const uchar *>(pcm.constData());
    for (int i = 0; i < count; ++i) {
        const double sample = qFromLittleEndian<qint16>(bytes + i * 2) / 32768.0;
        const double current = sample + coefficient * previous - beforePrevious;
        beforePrevious = previous;
        previous = current;
    }
    const double power = previous * previous + beforePrevious * beforePrevious -
                         coefficient * previous * beforePrevious;
    return std::max(0.0, power / (static_cast<double>(count) * count));
}
}

QString CliTextDecoder::modeName(Mode mode) {
    if (mode == Mode::Cw) return QStringLiteral("cw");
    if (mode == Mode::Psk31) return QStringLiteral("psk31");
    return QStringLiteral("off");
}

void CliTextDecoder::configure(const Settings &settings) {
    const bool changedMode = current.mode != settings.mode;
    current = settings;
    CwAudioDecoder::Settings cwSettings;
    cwSettings.sampleRate = 48000;
    cwSettings.toneHz = current.toneHz;
    cwSettings.initialWpm = current.cwWpm;
    cwSettings.selectivity = current.cwSelectivity;
    cwSettings.alphabet = current.cwAlphabet;
    cw.configure(cwSettings);
    psk.configure(48000, current.toneHz, current.pskSquelch, current.pskAfc);
    if (changedMode) reset();
}

void CliTextDecoder::reset() {
    cw.reset();
    psk.reset();
    decodedCharacters = 0;
    confidence = 0.0;
    estimatedWpm = 0.0;
    lastText.clear();
    tonePower = sidePower = toneMarginDb = 0.0;
    cwHoldSamples = 0;
    cwGateOpen = false;
}

void CliTextDecoder::process(const QByteArray &pcm,
                             const std::function<void(const QJsonObject &)> &onText) {
    if (pcm.isEmpty() || !enabled()) return;
    QString decoded;
    if (current.mode == Mode::Cw) {
        if (current.cwSquelchDb > 0.0) {
            const double center = tonePowerAt(pcm, current.toneHz);
            const double sides = (tonePowerAt(pcm, current.toneHz - 150.0) +
                                  tonePowerAt(pcm, current.toneHz + 150.0)) * 0.5;
            tonePower = tonePower == 0.0 ? center : tonePower * 0.75 + center * 0.25;
            sidePower = sidePower == 0.0 ? sides : sidePower * 0.75 + sides * 0.25;
            toneMarginDb = 10.0 * std::log10((tonePower + 1e-12) / (sidePower + 1e-12));
            const qint64 sampleCount = pcm.size() / 2;
            if (toneMarginDb >= current.cwSquelchDb && tonePower > 1e-9)
                cwHoldSamples = static_cast<qint64>(48000.0 *
                    (7.0 * 1.2 / current.cwWpm + 0.25));
            else cwHoldSamples = std::max<qint64>(0, cwHoldSamples - sampleCount);
            const bool open = cwHoldSamples > 0;
            if (open && !cwGateOpen) cw.reset();
            cwGateOpen = open;
            if (!open) return;
        } else cwGateOpen = true;
        const auto result = cw.processPcm16(pcm);
        decoded = result.text;
        confidence = result.confidence;
        estimatedWpm = result.estimatedWpm;
    } else if (current.mode == Mode::Psk31) {
        decoded = QString::fromLatin1(psk.process(pcm));
    }
    if (decoded.isEmpty()) return;
    decodedCharacters += static_cast<quint64>(decoded.size());
    lastText = decoded.right(200);
    onText({{QStringLiteral("event"), QStringLiteral("digitalText")},
            {QStringLiteral("mode"), modeName(current.mode)},
            {QStringLiteral("text"), decoded}});
}

QJsonObject CliTextDecoder::status() const {
    QJsonObject result{{QStringLiteral("mode"), modeName(current.mode)},
                       {QStringLiteral("toneHz"), current.toneHz},
                       {QStringLiteral("decodedCharacters"), static_cast<double>(decodedCharacters)},
                       {QStringLiteral("lastText"), lastText}};
    if (current.mode == Mode::Cw) {
        result.insert(QStringLiteral("wpm"), estimatedWpm);
        result.insert(QStringLiteral("confidence"), confidence);
        result.insert(QStringLiteral("selectivity"), current.cwSelectivity);
        result.insert(QStringLiteral("alphabet"), current.cwAlphabet);
        result.insert(QStringLiteral("squelchDb"), current.cwSquelchDb);
        result.insert(QStringLiteral("toneMarginDb"), toneMarginDb);
        result.insert(QStringLiteral("gateOpen"), cwGateOpen);
    } else if (current.mode == Mode::Psk31) {
        result.insert(QStringLiteral("locked"), psk.isLocked());
        result.insert(QStringLiteral("qualityPercent"), psk.qualityPercent());
        result.insert(QStringLiteral("afcOffsetHz"), psk.carrierOffsetHz());
        result.insert(QStringLiteral("squelch"), current.pskSquelch);
        result.insert(QStringLiteral("afcEnabled"), current.pskAfc);
    }
    return result;
}
