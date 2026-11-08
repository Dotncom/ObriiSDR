#include "meshradioegress.h"
#include "meshwiremodem.h"
#include "../backends/uhdbackend.h"
#include "../transmit/hackrftransmitterbackend.h"
#include <QElapsedTimer>
#include <QThread>

namespace Mesh {
namespace {
class NativeBurstSink final : public BurstSink {
public:
    bool send(const PortBinding &port,const QJsonObject &profile,const QVector<std::complex<float>> &wave,const std::atomic<bool> &cancel,QString *error) override {
        if(!RadioEgress::validate(port,profile,error)||cancel.load())return false;
        const int rate=WireModem(port.modem).sampleRate();
        const int device=profile.value("deviceIndex").toInt(),channel=profile.value("channel").toInt();
        if(profile.value("backend")=="uhd"){
            UhdTxSession tx;
            if(!tx.open(device,channel,rate,port.centerHz,profile.value("gainDb").toDouble(),profile.value("bandwidthHz").toDouble(),profile.value("antenna").toString(),{},error))return false;
            for(int at=0;at<wave.size();at+=8192){if(cancel.load())return false;const int count=qMin(8192,wave.size()-at);if(!tx.send(wave.constData()+at,count,at==0,error))return false;}
            return !cancel.load()&&tx.finish(error);
        }
        HackRfTransmitterBackend tx;TxConfiguration config;
        config.sampleRate=rate;config.frequencyHz=port.centerHz;config.deviceIndex=device;config.txVgaGainDb=int(profile.value("gainDb").toDouble());config.deviceSampleRate=profile.value("deviceRate").toInt(2000000);config.bandwidthHz=quint32(profile.value("bandwidthHz").toDouble());
        config.prebufferMilliseconds=20;config.liveSource=false;
        if(!tx.start(config,error))return false;
        // The modem has already bounded the burst; prebuffer it before USB starts.
        if(cancel.load()||!tx.writeFiniteBurst(wave.constData(),wave.size(),error))return false;
        QElapsedTimer wait;wait.start();const qint64 limit=qint64(wave.size())*1000/rate+2000;
        while(!tx.finiteBurstComplete()&&!cancel.load()&&wait.elapsed()<limit)QThread::msleep(5);
        if(cancel.load())return false;
        if(!tx.finiteBurstSucceeded()){if(error)*error="HackRF burst USB flush failed or timed out";return false;}
        return true;
    }
};
}
std::unique_ptr<BurstSink> makeRadioBurstSink(){return std::make_unique<NativeBurstSink>();}
}
