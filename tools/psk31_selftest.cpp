#include "psk31audiodecoder.h"
#include <QCoreApplication>
#include <QtEndian>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

namespace {
constexpr double Pi = 3.14159265358979323846;
QByteArray signal(const QByteArray &message, int rate, double tone, double offset, double amplitude, double noise) {
    std::vector<int> bits(64,0);
    for (unsigned char c : message) {
        const auto code = Psk31AudioDecoder::varicode(c);
        int high=31; while (high>0 && !(code&(1u<<high))) --high;
        for (int i=high; i>=0; --i) bits.push_back((code>>i)&1u);
        bits.push_back(0); bits.push_back(0);
    }
    bits.insert(bits.end(),32,1);
    QByteArray pcm;
    std::mt19937 random(177); std::normal_distribution<double> gaussian(0,noise);
    double sign=1;
    for (size_t b=0; b<bits.size(); ++b) {
        const int start=int(std::llround(double(b)*rate/31.25)), end=int(std::llround(double(b+1)*rate/31.25));
        for (int n=start; n<end; ++n) {
            const double position=double(n-start)/double(end-start);
            const double envelope=sign*(bits[b] ? 1 : std::cos(Pi*position));
            const double sample=amplitude*envelope*std::cos(2*Pi*(tone+offset)*n/rate+.73)+gaussian(random);
            const qint16 value=qint16(std::clamp(std::llround(sample*32767),-32768LL,32767LL));
            char bytes[2]; qToLittleEndian<qint16>(value,reinterpret_cast<uchar *>(bytes)); pcm.append(bytes,2);
        }
        if (!bits[b]) sign=-sign;
    }
    return pcm;
}
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    int failures=0,checks=0;
    const auto check=[&](bool ok,const char *message) { ++checks; if(!ok){++failures;std::cerr<<message<<'\n';} };
    QSet<unsigned int> codes;
    for(int c=0;c<256;++c) codes.insert(Psk31AudioDecoder::varicode(static_cast<unsigned char>(c)));
    check(codes.size()==256,"unique varicode mapping");
    check(Psk31AudioDecoder::varicode('e')==3 && Psk31AudioDecoder::varicode('t')==5 && Psk31AudioDecoder::varicode(' ')==1,"standard golden varicodes");
    const QByteArray message="CQ CQ DE TEST 123 hello\n";
    for(const int rate:{8000,44100,48000}) for(const double offset:{-3.0,0.0,3.0}) {
        const auto pcm=signal(message,rate,1000,offset,.3,.005);
        Psk31AudioDecoder whole,chunked;
        whole.configure(rate,1000,50,true); chunked.configure(rate,1000,50,true);
        const auto text=whole.process(pcm);
        if(!text.contains(message)) std::cerr<<"rate="<<rate<<" offset="<<offset<<" text="<<text.constData()<<'\n';
        check(text.contains(message),"shaped BPSK31 noisy carrier decoding");
        QByteArray chunks;
        for(int position=0;position<pcm.size();) {
            const int length=std::min(701+(position%217),pcm.size()-position);
            chunks+=chunked.process(pcm.mid(position,length)); position+=length;
        }
        check(chunks==text,"PCM byte chunk boundaries invariant");
        check(std::abs(whole.carrierOffsetHz()-offset)<1,"AFC residual");
    }
    Psk31AudioDecoder weak;
    weak.configure(48000,1800,50,true);
    check(weak.process(signal(message,48000,1800,1,.004,.00015)).contains(message),"weak tone / timing normalization");
    weak.reset(); check(!weak.isLocked() && weak.qualityPercent()==0,"reset clears lock and history");
    QByteArray silence(192000,'\0'); check(weak.process(silence).isEmpty() && !weak.isLocked(),"silence emits no text");
    std::cout<<"PSK31: "<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
