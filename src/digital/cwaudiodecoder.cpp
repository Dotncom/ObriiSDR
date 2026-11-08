#include "cwaudiodecoder.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr double TwoPi = 6.28318530717958647692;

const QHash<QString, QChar> &morseDecodeTable() {
    static const QHash<QString, QChar> table = {
        {".-", 'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'}, {".", 'E'},
        {"..-.", 'F'}, {"--.", 'G'}, {"....", 'H'}, {"..", 'I'}, {".---", 'J'},
        {"-.-", 'K'}, {".-..", 'L'}, {"--", 'M'}, {"-.", 'N'}, {"---", 'O'},
        {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'}, {"...", 'S'}, {"-", 'T'},
        {"..-", 'U'}, {"...-", 'V'}, {".--", 'W'}, {"-..-", 'X'}, {"-.--", 'Y'},
        {"--..", 'Z'}, {"-----", '0'}, {".----", '1'}, {"..---", '2'},
        {"...--", '3'}, {"....-", '4'}, {".....", '5'}, {"-....", '6'},
        {"--...", '7'}, {"---..", '8'}, {"----.", '9'}, {".-.-.-", '.'},
        {"--..--", ','}, {"..--..", '?'}, {"-..-.", '/'}, {"-...-", '='},
        {".-.-.", '+'}, {"-....-", '-'}, {"-.--.", '('}, {"-.--.-", ')'}
    };
    return table;
}

const QHash<QString, QChar> &ukrainianMorseDecodeTable() {
    static const QHash<QString, QChar> table = {
        {".-", QChar(0x0410)}, {"-...", QChar(0x0411)}, {".--", QChar(0x0412)},
        {"....", QChar(0x0413)}, {"--.", QChar(0x0490)}, {"-..", QChar(0x0414)},
        {".", QChar(0x0415)}, {"..-..", QChar(0x0404)}, {"...-", QChar(0x0416)},
        {"--..", QChar(0x0417)}, {"-.--", QChar(0x0418)}, {"..", QChar(0x0406)},
        {".---.", QChar(0x0407)}, {".---", QChar(0x0419)}, {"-.-", QChar(0x041A)},
        {".-..", QChar(0x041B)}, {"--", QChar(0x041C)}, {"-.", QChar(0x041D)},
        {"---", QChar(0x041E)}, {".--.", QChar(0x041F)}, {".-.", QChar(0x0420)},
        {"...", QChar(0x0421)}, {"-", QChar(0x0422)}, {"..-", QChar(0x0423)},
        {"..-.", QChar(0x0424)}, {"----", QChar(0x0425)}, {"-.-.", QChar(0x0426)},
        {"---.", QChar(0x0427)}, {"--.-", QChar(0x0428)}, {"--.--", QChar(0x0429)},
        {"-..-", QChar(0x042C)}, {"..--", QChar(0x042E)}, {".-.-", QChar(0x042F)},
        {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
        {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'},
        {"---..", '8'}, {"----.", '9'}, {".-.-.-", '.'}, {"--..--", ','},
        {"..--..", '?'}, {"-..-.", '/'}, {"-...-", '='}, {".-.-.", '+'},
        {"-....-", '-'}, {"-.--.", '('}, {"-.--.-", ')'}
    };
    return table;
}

qint16 readPcm16Le(const char *source) {
    qint16 value = 0;
    std::memcpy(&value, source, sizeof(value));
    return value;
}
}

void CwAudioDecoder::configure(const Settings &settings) {
    Settings normalized = settings;
    normalized.sampleRate = (std::clamp)(normalized.sampleRate, 8000, 384000);
    normalized.toneHz = (std::clamp)(normalized.toneHz, 100.0, normalized.sampleRate * 0.45);
    normalized.initialWpm = (std::clamp)(normalized.initialWpm, 5, 60);
    normalized.alphabet = (std::clamp)(normalized.alphabet, 0, 2);
    normalized.selectivity = (std::clamp)(normalized.selectivity, 1, 10);
    const bool timingChanged = normalized.sampleRate != currentSettings.sampleRate ||
                               std::abs(normalized.toneHz - currentSettings.toneHz) > 0.01 ||
                               normalized.alphabet != currentSettings.alphabet ||
                               normalized.selectivity != currentSettings.selectivity;
    currentSettings = normalized;
    if (timingChanged || !initialized) {
        reset();
    }
}

void CwAudioDecoder::reset() {
    oscillatorPhase = 0.0;
    mixedI = 0.0;
    mixedQ = 0.0;
    envelope = 0.0;
    noiseFloor = 1.0e-8;
    signalPeak = 1.0e-6;
    dotSamples = 1.2 * currentSettings.sampleRate /
                 static_cast<double>((std::max)(5, currentSettings.initialWpm));
    lastConfidence = 0.0;
    stateSamples = 0;
    keyDown = false;
    initialized = true;
    characterGapEmitted = false;
    wordGapEmitted = false;
    emittedText = false;
    currentPattern.clear();
    bilingualLatinWord.clear();
    bilingualUkrainianWord.clear();
    processedSamples = 0;
    lastStatusSample = 0;
}

CwAudioDecoder::Result CwAudioDecoder::processPcm16(const QByteArray &pcmData) {
    Result result;
    if (pcmData.size() < static_cast<int>(sizeof(qint16)) || currentSettings.sampleRate <= 0) {
        return result;
    }

    const double selectivity = (currentSettings.selectivity - 1) / 9.0;
    const double detectorBandwidthHz = 120.0 - 85.0 * selectivity;
    const double envelopeBandwidthHz = 24.0 - 6.0 * selectivity;
    const double phaseStep = TwoPi * currentSettings.toneHz / currentSettings.sampleRate;
    const double detectorAlpha =
        1.0 - std::exp(-TwoPi * detectorBandwidthHz / currentSettings.sampleRate);
    const double envelopeAlpha =
        1.0 - std::exp(-TwoPi * envelopeBandwidthHz / currentSettings.sampleRate);
    const int sampleCount = pcmData.size() / static_cast<int>(sizeof(qint16));
    const char *raw = pcmData.constData();

    for (int index = 0; index < sampleCount; ++index) {
        const double sample = readPcm16Le(raw + index * static_cast<int>(sizeof(qint16))) / 32768.0;
        const double cosine = std::cos(oscillatorPhase);
        const double sine = std::sin(oscillatorPhase);
        oscillatorPhase += phaseStep;
        if (oscillatorPhase >= TwoPi) oscillatorPhase -= TwoPi;

        mixedI += detectorAlpha * (sample * cosine - mixedI);
        mixedQ += detectorAlpha * (sample * sine - mixedQ);
        const double tonePower = mixedI * mixedI + mixedQ * mixedQ;
        envelope += envelopeAlpha * (tonePower - envelope);

        signalPeak = (std::max)(envelope, signalPeak * 0.999995);
        if (!keyDown || envelope < noiseFloor * 3.0) {
            const double noiseAlpha = envelope < noiseFloor ? 0.0025 : 0.00002;
            noiseFloor += noiseAlpha * (envelope - noiseFloor);
        }
        noiseFloor = (std::max)(noiseFloor, 1.0e-10);
        signalPeak = (std::max)(signalPeak, noiseFloor * 3.0);

        const double dynamicRange = signalPeak - noiseFloor;
        const double onThreshold = noiseFloor + dynamicRange * (0.34 + 0.16 * selectivity);
        const double offThreshold = noiseFloor + dynamicRange * (0.20 + 0.10 * selectivity);
        const bool nextKeyDown = keyDown ? envelope >= offThreshold : envelope >= onThreshold;
        lastConfidence = (std::clamp)((envelope - noiseFloor) /
                                          (dynamicRange + 1.0e-12),
                                      0.0,
                                      1.0);

        ++stateSamples;
        ++processedSamples;
        if (nextKeyDown != keyDown) {
            const qint64 finishedDuration = stateSamples;
            stateSamples = 0;
            if (keyDown) {
                finishKeyDown(finishedDuration, result.text);
                characterGapEmitted = false;
                wordGapEmitted = false;
            } else {
                updateGap(finishedDuration, result.text);
            }
            keyDown = nextKeyDown;
        } else if (!keyDown) {
            updateGap(stateSamples, result.text);
        }
    }

    result.estimatedWpm = 1.2 * currentSettings.sampleRate / (std::max)(1.0, dotSamples);
    result.confidence = lastConfidence;
    if (processedSamples - lastStatusSample >= currentSettings.sampleRate / 4) {
        lastStatusSample = processedSamples;
        result.status = QStringLiteral("CW decoder: %1 Hz, %2 WPM, selectivity %3, confidence %4, pattern %5")
                            .arg(currentSettings.toneHz, 0, 'f', 0)
                            .arg(result.estimatedWpm, 0, 'f', 1)
                            .arg(currentSettings.selectivity)
                            .arg(lastConfidence, 0, 'f', 2)
                            .arg(currentPattern.isEmpty() ? QStringLiteral("-") : currentPattern);
    }
    return result;
}

void CwAudioDecoder::finishKeyDown(qint64 durationSamples, QString &) {
    const double duration = static_cast<double>(durationSamples);
    const double selectivity = (currentSettings.selectivity - 1) / 9.0;
    const double minimumMarkSamples = dotSamples * (0.06 + 0.14 * selectivity);
    if (duration < minimumMarkSamples) {
        return;
    }
    const bool dash = duration >= dotSamples * 2.05;
    currentPattern.append(dash ? QLatin1Char('-') : QLatin1Char('.'));
    if (currentSettings.adaptiveSpeed) {
        const double estimate = dash ? duration / 3.0 : duration;
        const double lower = currentSettings.sampleRate * 1.2 / 60.0;
        const double upper = currentSettings.sampleRate * 1.2 / 5.0;
        if (estimate >= lower * 0.6 && estimate <= upper * 1.5) {
            dotSamples = 0.82 * dotSamples + 0.18 * (std::clamp)(estimate, lower, upper);
        }
    }
}

void CwAudioDecoder::finishCharacter(QString &output) {
    if (currentPattern.isEmpty()) return;
    if (currentSettings.alphabet == 2) {
        const QChar latin = morseDecodeTable().value(currentPattern);
        const QChar ukrainian = ukrainianMorseDecodeTable().value(currentPattern);
        bilingualLatinWord.append(latin.isNull() ? QLatin1Char('#') : latin);
        bilingualUkrainianWord.append(ukrainian.isNull() ? QLatin1Char('#') : ukrainian);
    } else {
        const QChar decoded = decodePattern(currentPattern);
        output.append(decoded.isNull() ? QLatin1Char('#') : decoded);
    }
    emittedText = true;
    currentPattern.clear();
}

void CwAudioDecoder::finishBilingualWord(QString &output) {
    if (bilingualLatinWord.isEmpty() && bilingualUkrainianWord.isEmpty()) return;
    output.append(QStringLiteral("EN: %1 | UK: %2\n")
                      .arg(bilingualLatinWord, bilingualUkrainianWord));
    bilingualLatinWord.clear();
    bilingualUkrainianWord.clear();
}

void CwAudioDecoder::updateGap(qint64 durationSamples, QString &output) {
    const double duration = static_cast<double>(durationSamples);
    if (!characterGapEmitted && duration >= dotSamples * 2.35) {
        finishCharacter(output);
        characterGapEmitted = true;
    }
    if (!wordGapEmitted && duration >= dotSamples * 6.2) {
        if (currentSettings.alphabet == 2) {
            finishBilingualWord(output);
        } else if (emittedText && !output.endsWith(QLatin1Char(' '))) {
            output.append(QLatin1Char(' '));
        }
        wordGapEmitted = true;
    }
}

QChar CwAudioDecoder::decodePattern(const QString &pattern) const {
    return currentSettings.alphabet == 1
               ? ukrainianMorseDecodeTable().value(pattern)
               : morseDecodeTable().value(pattern);
}
