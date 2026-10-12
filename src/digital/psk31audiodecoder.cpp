/*
 * BPSK31 receive adaptation of gMFSK psk31rx.c, coeff.c and varicode.c.
 * Copyright (C) 2001-2003 Tomi Manninen (OH2BNS).
 * Receive filter designed by Peter Martinez (G3PLX).
 * Obrii adaptation: arbitrary PCM rates, bounded state, NCO recurrence and Qt output.
 * Distributed under GNU GPL version 2 or (at your option) any later version.
 * No warranty. See LICENSE and docs/psk31_decoder.md for provenance.
 */
#include "psk31audiodecoder.h"
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr unsigned int Codes[256] = {
    0x2AB,0x2DB,0x2ED,0x377,0x2EB,0x35F,0x2EF,0x2FD,0x2FF,0x0EF,0x01D,0x36F,0x2DD,0x01F,0x375,0x3AB,
    0x2F7,0x2F5,0x3AD,0x3AF,0x35B,0x36B,0x36D,0x357,0x37B,0x37D,0x3B7,0x355,0x35D,0x3BB,0x2FB,0x37F,
    0x001,0x1FF,0x15F,0x1F5,0x1DB,0x2D5,0x2BB,0x17F,0x0FB,0x0F7,0x16F,0x1DF,0x075,0x035,0x057,0x1AF,
    0x0B7,0x0BD,0x0ED,0x0FF,0x177,0x15B,0x16B,0x1AD,0x1AB,0x1B7,0x0F5,0x1BD,0x1ED,0x055,0x1D7,0x2AF,
    0x2BD,0x07D,0x0EB,0x0AD,0x0B5,0x077,0x0DB,0x0FD,0x155,0x07F,0x1FD,0x17D,0x0D7,0x0BB,0x0DD,0x0AB,
    0x0D5,0x1DD,0x0AF,0x06F,0x06D,0x157,0x1B5,0x15D,0x175,0x17B,0x2AD,0x1F7,0x1EF,0x1FB,0x2BF,0x16D,
    0x2DF,0x00B,0x05F,0x02F,0x02D,0x003,0x03D,0x05B,0x02B,0x00D,0x1EB,0x0BF,0x01B,0x03B,0x00F,0x007,
    0x03F,0x1BF,0x015,0x017,0x005,0x037,0x07B,0x06B,0x0DF,0x05D,0x1D5,0x2B7,0x1BB,0x2B5,0x2D7,0x3B5,
    0x3BD,0x3BF,0x3D5,0x3D7,0x3DB,0x3DD,0x3DF,0x3EB,0x3ED,0x3EF,0x3F5,0x3F7,0x3FB,0x3FD,0x3FF,0x555,
    0x557,0x55B,0x55D,0x55F,0x56B,0x56D,0x56F,0x575,0x577,0x57B,0x57D,0x57F,0x5AB,0x5AD,0x5AF,0x5B5,
    0x5B7,0x5BB,0x5BD,0x5BF,0x5D5,0x5D7,0x5DB,0x5DD,0x5DF,0x5EB,0x5ED,0x5EF,0x5F5,0x5F7,0x5FB,0x5FD,
    0x5FF,0x6AB,0x6AD,0x6AF,0x6B5,0x6B7,0x6BB,0x6BD,0x6BF,0x6D5,0x6D7,0x6DB,0x6DD,0x6DF,0x6EB,0x6ED,
    0x6EF,0x6F5,0x6F7,0x6FB,0x6FD,0x6FF,0x755,0x757,0x75B,0x75D,0x75F,0x76B,0x76D,0x76F,0x775,0x777,
    0x77B,0x77D,0x77F,0x7AB,0x7AD,0x7AF,0x7B5,0x7B7,0x7BB,0x7BD,0x7BF,0x7D5,0x7D7,0x7DB,0x7DD,0x7DF,
    0x7EB,0x7ED,0x7EF,0x7F5,0x7F7,0x7FB,0x7FD,0x7FF,0xAAB,0xAAD,0xAAF,0xAB5,0xAB7,0xABB,0xABD,0xABF,
    0xAD5,0xAD7,0xADB,0xADD,0xADF,0xAEB,0xAED,0xAEF,0xAF5,0xAF7,0xAFB,0xAFD,0xAFF,0xB55,0xB57,0xB5B
};
constexpr float Matched[64] = {
    .000625000f,.000820912f,.001374651f,.002188141f,.003110600f,.003956273f,.004526787f,.004635947f,
    .004134515f,.002932456f,.001016352f,-.001539947f,-.004572751f,-.007834665f,-.011009254f,-.013733305f,
    -.015625000f,-.016315775f,-.015483216f,-.012882186f,-.008371423f,-.001933193f,.006315933f,.016124399f,
    .027115485f,.038807198f,.050640928f,.062016866f,.072333574f,.081028710f,.087617820f,.091728168f,
    .093125000f,.091728168f,.087617820f,.081028710f,.072333574f,.062016866f,.050640928f,.038807198f,
    .027115485f,.016124399f,.006315933f,-.001933193f,-.008371423f,-.012882186f,-.015483216f,-.016315775f,
    -.015625000f,-.013733305f,-.011009254f,-.007834665f,-.004572751f,-.001539947f,.001016352f,.002932456f,
    .004134515f,.004635947f,.004526787f,.003956273f,.003110600f,.002188141f,.001374651f,.000820912f
};
}

struct Psk31AudioDecoder::Impl {
    int rate = 48000, squelch = 50;
    double tone = 1000, afcHz = 0, outputClock = 0, symbolClock = 0;
    bool afc = true, locked = false;
    std::complex<float> oscillator{1,0}, step{1,0}, previous{0,0};
    std::complex<double> quality{0,0};
    std::vector<float> firstCoefficients;
    std::vector<std::complex<float>> firstHistory;
    std::array<std::complex<float>,64> secondHistory{};
    std::array<float,16> sync{};
    size_t firstPosition = 0, secondPosition = 0;
    std::uint32_t shift = 0, carrierShift = 0;
    std::uint64_t samples = 0;
    int pendingByte = -1;
    void updateStep() { step = {float(std::cos(-2*Pi*(tone+afcHz)/rate)), float(std::sin(-2*Pi*(tone+afcHz)/rate))}; }
    void reset() {
        afcHz = outputClock = symbolClock = 0; locked = false;
        oscillator = {1,0}; previous = {0,0}; quality = {0,0};
        firstPosition = secondPosition = 0; shift = carrierShift = 0; samples = 0;
        pendingByte = -1;
        std::fill(firstHistory.begin(), firstHistory.end(), std::complex<float>{0,0});
        secondHistory.fill({0,0}); sync.fill(0); updateStep();
    }
    void symbol(std::complex<float> z, QByteArray &text) {
        if (std::norm(z) < 1e-12f || std::norm(previous) < 1e-12f) {
            previous = z; quality *= .98; locked = false; shift = carrierShift = 0; return;
        }
        const double phase = std::arg(z*std::conj(previous));
        const bool reversal = std::abs(phase) > Pi/2;
        quality = .02*std::complex<double>(std::cos(2*phase), std::sin(2*phase))+.98*quality;
        carrierShift = (carrierShift<<2) | (reversal ? 2u : 0u);
        if (carrierShift == 0xAAAAAAAAu) { quality = {1,0}; locked = true; }
        else if (carrierShift == 0) { quality = {0,0}; locked = false; }
        else locked = 100*std::norm(quality) >= squelch;
        if (locked) {
            shift = (shift<<1) | (reversal ? 0u : 1u);
            if ((shift & 3u) == 0) {
                const unsigned int code = shift>>2;
                for (int c=0; c<256; ++c) if (code == Codes[c]) {
                    if (c >= 32 && c != 127) text.append(char(c));
                    else if (c == 9 || c == 10 || c == 13) text.append(char(c));
                    break;
                }
                shift = 0;
            }
            if (afc) {
                const double error = std::remainder(phase-(reversal ? std::copysign(Pi, phase) : 0), 2*Pi);
                afcHz = std::clamp(afcHz+error*31.25/(2*Pi*16), -7.5, 7.5);
                updateStep();
            }
        } else shift = 0;
        previous = z;
    }
    void downsampled(std::complex<float> z, QByteArray &text) {
        secondHistory[secondPosition] = z;
        std::complex<float> filtered{0,0};
        for (size_t k=0; k<64; ++k) filtered += Matched[k]*secondHistory[(secondPosition+64-k)%64];
        secondPosition = (secondPosition+1)%64;
        sync[std::clamp(int(symbolClock),0,15)] = std::abs(filtered);
        double first = 0, last = 0;
        for (int i=0; i<8; ++i) { first += sync[i]; last += sync[i+8]; }
        const double error = (first-last)/std::max(1e-12, first+last);
        symbolClock += 1-.2*error;
        if (symbolClock >= 16) { symbolClock -= 16; symbol(filtered, text); }
    }
};

Psk31AudioDecoder::Psk31AudioDecoder() : impl(std::make_unique<Impl>()) { configure(48000,1000,50,true); }
Psk31AudioDecoder::~Psk31AudioDecoder() = default;
void Psk31AudioDecoder::configure(int rate, double tone, int squelch, bool afc) {
    rate = std::clamp(rate, 8000, 192000);
    tone = std::clamp(std::isfinite(tone) ? tone : 1000.0, 100.0, rate*.45);
    impl->squelch = std::clamp(squelch, 1, 100);
    if (!afc && impl->afc) { impl->afcHz = 0; impl->updateStep(); }
    impl->afc = afc;
    const int length = std::max(64, int(std::lround(rate/125.0)));
    if (impl->rate == rate && impl->tone == tone && int(impl->firstHistory.size()) == length) return;
    impl->rate = rate; impl->tone = tone;
    impl->firstHistory.resize(size_t(length)); impl->firstCoefficients.resize(size_t(length));
    for (int i=0; i<length; ++i) impl->firstCoefficients[i] = float((1-std::cos(2*Pi*i/length))/length);
    impl->reset();
}
void Psk31AudioDecoder::reset() { impl->reset(); }
QByteArray Psk31AudioDecoder::process(const QByteArray &pcm) {
    QByteArray text;
    QByteArray joined;
    const QByteArray *input = &pcm;
    if (impl->pendingByte >= 0) { joined.append(char(impl->pendingByte)); joined.append(pcm); input = &joined; }
    impl->pendingByte = input->size()%2 ? int(static_cast<unsigned char>(input->back())) : -1;
    const size_t length = impl->firstHistory.size();
    for (int i=0; i+1<input->size(); i+=2) {
        const float sample = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(input->constData()+i))/32768.f;
        impl->firstHistory[impl->firstPosition] = sample*impl->oscillator;
        impl->oscillator *= impl->step;
        if ((++impl->samples & 4095u) == 0) impl->oscillator /= std::max(1e-12f, std::abs(impl->oscillator));
        impl->outputClock += 500.0/impl->rate;
        if (impl->outputClock >= 1) {
            impl->outputClock -= 1;
            std::complex<float> value{0,0};
            for (size_t k=0; k<length; ++k)
                value += impl->firstCoefficients[k]*impl->firstHistory[(impl->firstPosition+length-k)%length];
            impl->downsampled(value, text);
        }
        impl->firstPosition = (impl->firstPosition+1)%length;
    }
    return text;
}
double Psk31AudioDecoder::qualityPercent() const { return std::clamp(100*std::norm(impl->quality), 0.0, 100.0); }
double Psk31AudioDecoder::carrierOffsetHz() const { return impl->afcHz; }
bool Psk31AudioDecoder::isLocked() const { return impl->locked; }
unsigned int Psk31AudioDecoder::varicode(unsigned char c) { return Codes[c]; }
