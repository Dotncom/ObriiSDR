#ifndef OBRII_CLI_TEXT_DECODER_H
#define OBRII_CLI_TEXT_DECODER_H

#include "cwaudiodecoder.h"
#include "psk31audiodecoder.h"

#include <QJsonObject>
#include <QString>

#include <functional>

class CliTextDecoder {
public:
    enum class Mode { Off, Cw, Psk31 };
    struct Settings {
        Mode mode = Mode::Off;
        double toneHz = 700.0;
        int cwWpm = 18;
        int cwSelectivity = 5;
        int cwAlphabet = 0;
        double cwSquelchDb = 8.0;
        int pskSquelch = 50;
        bool pskAfc = true;
    };

    void configure(const Settings &settings);
    void reset();
    void process(const QByteArray &pcm, const std::function<void(const QJsonObject &)> &onText);
    QJsonObject status() const;
    bool enabled() const { return current.mode != Mode::Off; }
    Settings settings() const { return current; }
    static QString modeName(Mode mode);

private:
    Settings current;
    CwAudioDecoder cw;
    Psk31AudioDecoder psk;
    quint64 decodedCharacters = 0;
    double confidence = 0.0;
    double estimatedWpm = 0.0;
    QString lastText;
    double tonePower = 0.0;
    double sidePower = 0.0;
    double toneMarginDb = 0.0;
    qint64 cwHoldSamples = 0;
    bool cwGateOpen = false;
};

#endif
