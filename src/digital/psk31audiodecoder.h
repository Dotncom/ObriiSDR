#ifndef PSK31AUDIODECODER_H
#define PSK31AUDIODECODER_H
#include <QByteArray>
#include <memory>

class Psk31AudioDecoder {
public:
    Psk31AudioDecoder();
    ~Psk31AudioDecoder();
    void configure(int sampleRate, double toneHz, int squelchPercent, bool afc);
    void reset();
    QByteArray process(const QByteArray &pcm);
    double qualityPercent() const;
    double carrierOffsetHz() const;
    bool isLocked() const;
    static unsigned int varicode(unsigned char c);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif
