#pragma once
#include <QByteArray>
#include <QStringList>
#include <memory>
struct M17AudioResult {
    QStringList text;
    QByteArray voicePcm; // 48 kHz mono PCM16 LE
};
class M17AudioDecoder {
public:
    M17AudioDecoder();
    ~M17AudioDecoder();
    void reset();
    bool voiceAvailable() const;
    M17AudioResult process(const QByteArray &pcm, int sampleRate);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
