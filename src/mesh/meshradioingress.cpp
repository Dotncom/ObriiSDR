#include "meshradioingress.h"
#include "meshwiremodem.h"
#include "../core/dspiqsourceregistry.h"
#include "../core/iqchannelizer.h"
#include <QElapsedTimer>
#include <atomic>
#include <cmath>
#include <vector>

namespace Mesh {
namespace {std::atomic<int> activeReaders{0};}
struct RadioIngress::Private {
    struct Port {PortBinding binding;DspSourceIdentity source;IqChannelizer channelizer;quint64 epoch=0,end=0,samples=0,gaps=0;int rate=0;QString state="waiting";qint64 lastMs=-1;std::vector<float> output;};
    std::vector<Port> ports;
    QElapsedTimer clock;
    bool subscribed=false;
    Private(){clock.start();}
};
RadioIngress::RadioIngress():d(new Private){}
RadioIngress::~RadioIngress(){clear();}
bool RadioIngress::configure(const QVector<PortBinding> &ports,QString *error){
    clear();QSet<QString> sources;
    for(const auto &binding:ports)if(binding.backend=="radio"&&binding.enabled&&binding.direction=="rx"){
        Private::Port p;p.binding=binding;p.source=DspSourceIdentity::normalized(binding.receiverId,binding.channelId);
        if(!p.source.isValid()){if(error)*error="Invalid registered RX source identity";return false;}
        try{WireModem modem(binding.modem);p.rate=modem.sampleRate();}catch(const std::exception &e){if(error)*error=QString::fromUtf8(e.what());return false;}
        if(binding.modem.value("type")=="lora"&&binding.modem.value("os").toInt(1)<2){if(error)*error="Wideband radio LoRa RX requires oversampling >=2 for the DDC guard band";return false;}
        sources.insert(p.source.key());d->ports.push_back(std::move(p));
    }
    if(!d->ports.empty()){DspIqSourceRegistry::instance().setSubscriptions(d.get(),sources);d->subscribed=true;activeReaders.fetch_add(1,std::memory_order_release);}
    return true;
}
void RadioIngress::clear(){if(d->subscribed){DspIqSourceRegistry::instance().removeSubscriptions(d.get());d->subscribed=false;activeReaders.fetch_sub(1,std::memory_order_release);}d->ports.clear();}
bool RadioIngress::active() const{return d->subscribed;}
bool RadioIngress::anyActive(){return activeReaders.load(std::memory_order_acquire)>0;}
void RadioIngress::poll(const Consumer &consume){
    for(auto &p:d->ports){
        DspIqSourceSnapshot input;
        if(!DspIqSourceRegistry::instance().snapshotSince(p.source,p.epoch,p.end,524288,input))continue;
        const bool gap=input.gap||input.epoch!=p.epoch||(p.lastMs>=0&&d->clock.elapsed()-p.lastMs>1000);p.lastMs=d->clock.elapsed();
        p.epoch=input.epoch;p.end=input.endFloatCount;
        if(gap){++p.gaps;p.channelizer.reset();}
        const double width=p.binding.modem.value("type")=="lora"?p.binding.modem.value("bw").toDouble(125000):12500;
        if(input.sampleRateHz<=0||std::abs(p.binding.centerHz-input.centerHz)+width/2>=input.sampleRateHz/2){p.state="outside_source_band";p.channelizer.reset();continue;}
        RadioSettings settings;settings.sampleRate=input.sampleRateHz;settings.centerFrequency=input.centerHz;settings.listeningFrequency=p.binding.centerHz;settings.bandwidth=width;settings.inputMode=INPUT_RF;
        IqChannelizer::Options options;options.outputRateHz=p.rate;options.cutoffHz=width*0.6;options.firQuality=IqChannelizer::FirQuality::Balanced;
        const auto result=p.channelizer.processFloatIq(input.iq.data(),input.iq.size(),settings,p.output,false,&options);
        if(!result.valid||std::abs(result.outputRate-p.rate)>0.1){p.state="ddc_rate_mismatch";continue;}
        p.state="receiving";p.samples+=p.output.size()/2;
        if(!p.output.empty())consume(p.binding.id,p.output.data(),int(p.output.size()/2),gap);
    }
}
QJsonArray RadioIngress::status() const {QJsonArray result;for(const auto &p:d->ports)result.append(QJsonObject{{"port",p.binding.id},{"receiverId",p.source.receiverId},{"channelId",p.source.channelId},{"sampleRate",p.rate},{"centerHz",p.binding.centerHz},{"state",p.state},{"samples",QString::number(p.samples)},{"gaps",QString::number(p.gaps)}});return result;}
}
