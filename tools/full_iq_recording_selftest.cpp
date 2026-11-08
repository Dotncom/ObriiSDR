#include "fulliqrecordingencoder.h"
#include "atviqworker.h"
#include "spectrumiqeventrecorder.h"
#include "iqframequeuebudget.h"
#include "videoprocessor.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QThread>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QFile>
#include <QtEndian>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace {
struct VideoDecodeThread {
    QThread thread;
    VideoProcessor *processor = new VideoProcessor;
    VideoDecodeThread() {
        processor->moveToThread(&thread);
        QObject::connect(&thread,&QThread::finished,processor,&QObject::deleteLater);
        thread.start();
        QMetaObject::invokeMethod(processor,[this] {
            processor->configure(true,VideoProcessor::FmVideo,15625,640,480,false,false,false);
        },Qt::QueuedConnection);
    }
    ~VideoDecodeThread() {thread.quit();thread.wait();}
};
double threadCpuMs() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    const auto ticks=[](FILETIME v){return (std::uint64_t(v.dwHighDateTime)<<32)|v.dwLowDateTime;};
    return double(ticks(kernel)+ticks(user))/10000.;
#else
    return 0;
#endif
}
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
QByteArray read(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "read capture"); return file.readAll();
}
void encoding() {
    std::vector<float> input(100003);
    std::mt19937 random(7741);
    std::uniform_real_distribution<float> distribution(-1.2f, 1.2f);
    for (auto &value : input) value = distribution(random);
    input[0] = 0.1f; input[1] = -0.1f;
    input[2] = 0.0007f; input[3] = -0.0007f;
    input[4] = std::numeric_limits<float>::quiet_NaN();
    input[5] = std::numeric_limits<float>::infinity();
    input[6] = -std::numeric_limits<float>::infinity();
    input[7] = std::nextafter(0.5f/32768,0.0f);input[8] = -input[7];
    input[9] = 0.5f/32768;input[10] = -input[9];
    input[input.size()-1] = input[7]; // Exercise the scalar tail at the same boundary.
    QByteArray encoded(int(input.size()*2), '\0');
    encodeFullIq16(input.data(), encoded.data(), input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto actual = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(encoded.constData()+i*2));
        const float value = std::isfinite(input[i]) ? input[i] : 0;
        const int expected = (std::clamp)(int(std::lround((std::clamp)(value,-1.0f,1.0f)*32768)), -32768,32767);
        require(actual == expected, "archival PCM16 encoding changed amplitude/finite policy");
        if (std::abs(value) < .99f) require(std::abs(actual/32768.0f-value) <= 1.0f/65536,
                                          "archival quantization error exceeds half a LSB");
    }
    // Model the source amplitudes that formerly saturated after implicit x16.
    require(qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(encoded.constData())) == 3277,
            "hidden gain or clipping reintroduced");
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) encodeFullIq16(input.data(), encoded.data(), input.size());
    const auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    std::cout << "PCM16: " << ms/100 * (524288.0/input.size()) << " ms per 262144-IQ block\n";
}
void record(const QTemporaryDir &directory) {
    RadioSettings settings; settings.sampleRate=50000000; settings.centerFrequency=100e6;
    SpectrumIqEventRecorder recorder;
    const QString base=directory.filePath("quality");
    QString error;
    require(recorder.start(base,SpectrumIqEventRecorder::Mode::FullIqS16,settings,
                           settings.sampleRate,1000,1000,&error),"CS16 recorder start");
    const float input[]={.1f,-.1f,.25f,-.25f,.0007f,-.0007f,1.,-1.};
    QByteArray encoded(16,Qt::Uninitialized);
    encodeFullIq16(input,encoded.data(),8);
    require(recorder.appendFrame(encoded,settings.sampleRate,4,1001),"CS16 append");
    require(!recorder.appendFrame(QByteArray("x"),settings.sampleRate,4,1002),"truncated IQ accepted");
    recorder.stop();
    require(read(base+"_full_iq_s16.cs16")==encoded,"CS16 file payload changed");
    const auto metadata=QJsonDocument::fromJson(read(base+"_full_iq_s16.json")).object();
    require(metadata["sampleCount"].toInt()==4 && metadata["bytesWritten"].toInt()==16,
            "CS16 sample counters incorrect");
    require(metadata["inputGain"].toDouble()==1. && metadata["sampleScale"].toDouble()==1./32768,
            "missing physical sample scale");
    const auto sigmf=QJsonDocument::fromJson(read(base+"_full_iq_s16.sigmf-meta")).object();
    require(sigmf["global"].toObject()["core:datatype"].toString()=="ci16_le","wrong SigMF datatype");
    SpectrumIqEventRecorder legacy;
    require(legacy.start(directory.filePath("legacy"),SpectrumIqEventRecorder::Mode::FullIqS8,
                          settings,settings.sampleRate),"legacy IQ8 mode broken");
    require(legacy.appendFrame(QByteArray(8,'\1'),settings.sampleRate,4),"legacy IQ8 layout broken");
    legacy.stop();
}
void concurrentVideo(const QTemporaryDir &directory) {
    RadioSettings settings;
    settings.sampleRate=50000000; settings.centerFrequency=100e6;
    settings.listeningFrequency=115e6; settings.modulationType=MOD_ATV; settings.bandwidth=5e6;
    std::vector<float> input(524288);
    for (int n=0;n<262144;++n) {
        const double phase=6.283185307179586*.3*n;
        input[n*2]=float(.1*std::cos(phase)); input[n*2+1]=float(.1*std::sin(phase));
    }
    std::mutex mutex;
    std::condition_variable wake;
    std::uint64_t videoSamples=0, gaps=0;
    AtvIqWorker worker([&](const QByteArray &data,double rate,int count) {
        require(rate==7e6 && data.size()==count*4,"ATV layout/rate changed");
        std::lock_guard<std::mutex> lock(mutex); videoSamples+=count; wake.notify_one();
    },[&](std::uint64_t lost){gaps+=lost;});
    worker.configure(settings,true,true);
    SpectrumIqEventRecorder recorder;
    require(recorder.start(directory.filePath("simultaneous"),SpectrumIqEventRecorder::Mode::FullIqS16,
                           settings,settings.sampleRate),"concurrent recorder start");
    QByteArray encoded(int(input.size()*2),Qt::Uninitialized);
    // Wait each block out to verify sample continuity, rather than relying on
    // a machine-speed-specific producer pacing in this functional test.
    for (int i=0;i<4;++i) {
        encodeFullIq16(input.data(),encoded.data(),input.size());
        require(recorder.appendFrame(encoded,settings.sampleRate,262144),"record while ATV active");
        std::unique_lock<std::mutex> lock(mutex);
        const auto previous=videoSamples;
        worker.push(input.data(),input.size());
        require(wake.wait_for(lock,std::chrono::seconds(3),[&]{return videoSamples>previous;}),
                "ATV stopped when full IQ recording enabled");
    }
    recorder.stop();
    require(gaps==0,"unexpected video input gap");
    require(recorder.bytesWritten()==quint64(input.size()*2*4),"full capture lost samples while video active");
    require(videoSamples>146000 && videoSamples<147000,"ATV channel lost/resampled discontinuously");
    std::cout << "Concurrent CS16 + ATV: " << recorder.bytesWritten() << " raw bytes, " << videoSamples << " channel samples\n";
    // An oversized input is rejected before dereferencing its payload; the
    // next accepted block must explicitly report the gap and recover.
    {
        std::unique_lock<std::mutex> lock(mutex);
        const auto previous=videoSamples;
        const auto oversized=AtvIqWorker::MaxInputBytes/sizeof(float)+2;
        worker.push(input.data(),oversized);
        worker.push(input.data(),input.size());
        require(wake.wait_for(lock,std::chrono::seconds(3),[&]{return videoSamples>previous;}),
                "ATV did not recover after bounded input overflow");
        require(gaps==oversized/2,"ATV input overflow was not reported exactly");
    }
    worker.configure(settings,false,true);
    IqFrameQueueBudget videoBudget, recordingBudget;
    require(videoBudget.reserve(IqFrameQueueBudget::MaxPendingBytes,1),"fill video budget");
    require(!videoBudget.reserve(1,1) && recordingBudget.reserve(1024,256),"video pressure leaked into recording budget");
    videoBudget.release(IqFrameQueueBudget::MaxPendingBytes);recordingBudget.release(1024);
    IqFrameQueueBudget boundedVideo(8*1024*1024);
    require(boundedVideo.reserve(8*1024*1024, 2000000) && !boundedVideo.reserve(4, 1),
            "custom video queue limit ignored");
    boundedVideo.release(8*1024*1024);

    IqChannelizer channel;
    std::vector<float> output;
    const auto begin=std::chrono::steady_clock::now();
    const double cpuBegin=threadCpuMs();
    for(int i=0;i<100;++i)channel.processFloatIq(input.data(),input.size(),settings,output,false);
    const double cpuMs=(threadCpuMs()-cpuBegin)/100;
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/100;
    std::cout << "ATV DDC: " << ms << " ms wall / " << cpuMs << " ms thread CPU per 5.243 ms of 50-MS/s input\n";
}

void atvWaveform() {
    RadioSettings atv;
    atv.sampleRate=50e6; atv.centerFrequency=100e6; atv.listeningFrequency=114.8e6;
    atv.modulationType=MOD_ATV; atv.bandwidth=5e6;
    std::vector<float> input(524288);
    for (std::size_t n=0;n<input.size()/2;++n) {
        const double phase=6.283185307179586*14.9e6/50e6*n;
        const double edge=6.283185307179586*17.6e6/50e6*n;
        const double stop=6.283185307179586*21.8e6/50e6*n;
        input[n*2]=float(.1*std::cos(phase)+.03*std::cos(edge)+.02*std::cos(stop));
        input[n*2+1]=float(.1*std::sin(phase)+.03*std::sin(edge)+.02*std::sin(stop));
    }
    input[6]+=.2f;input[7]-=.1f; // Include the filter's startup/impulse response.
    for(int taps:{18,32,64}) {
        IqChannelizer::Options options;
        options.outputRateHz=7e6;options.cutoffHz=3e6;options.firTapCount=taps;
        IqChannelizer whole,chunked,reference;
        std::vector<float> expected,actual,part,scalar;
        require(whole.processFloatIq(input.data(),input.size(),atv,expected,false,&options).valid,
                "ATV waveform configuration");
        for(std::size_t offset=0;offset<input.size();) {
            const auto count=(std::min)(std::size_t(7142),input.size()-offset);
            require(chunked.processFloatIq(input.data()+offset,count,atv,part,false,&options).valid,
                    "ATV split waveform configuration");
            actual.insert(actual.end(),part.begin(),part.end());offset+=count;
        }
        RadioSettings scalarSettings=atv; scalarSettings.modulationType=MOD_AM;
        // Short scalar blocks re-anchor the legacy float oscillator often
        // enough to serve as a phase reference for the vector oscillator.
        for(std::size_t offset=0;offset<input.size();) {
            const auto count=(std::min)(std::size_t(2048),input.size()-offset);
            reference.processFloatIq(input.data()+offset,count,scalarSettings,part,false,&options);
            scalar.insert(scalar.end(),part.begin(),part.end());offset+=count;
        }
        require(actual.size()==expected.size() && scalar.size()==expected.size(),"ATV sample cadence changed");
        double error=0,referenceError=0;
        for(std::size_t i=0;i<actual.size();++i) {
            error=(std::max)(error,double(std::abs(actual[i]-expected[i])));
            referenceError=(std::max)(referenceError,double(std::abs(scalar[i]-expected[i])));
        }
        std::cout<<"ATV taps="<<taps<<" chunk error="<<error<<" scalar-NCO error="<<referenceError<<'\n';
        require(error<1e-4 && referenceError<1e-4,"ATV phase/waveform discontinuity");
    }
}
void pacedVideo(const QTemporaryDir &directory) {
    RadioSettings settings;
    settings.sampleRate=50e6;settings.centerFrequency=100e6;
    settings.listeningFrequency=115e6;settings.modulationType=MOD_ATV;settings.bandwidth=5e6;
    constexpr int iqPerBlock=262144, blocks=384;
    std::vector<float> input(iqPerBlock*2);
    for(int n=0;n<iqPerBlock;++n) {
        const double phase=6.283185307179586*.3*n;
        input[n*2]=float(.1*std::cos(phase));input[n*2+1]=float(.1*std::sin(phase));
    }
    std::mutex mutex;std::condition_variable wake;
    int frames=0,decodedFrames=0;std::uint64_t dropped=0;
    std::atomic<int> rendered{0},decoderDrops{0};
    double workerFirstCpu=-1,workerLastCpu=0;
    IqFrameQueueBudget videoBudget(8*1024*1024);
    VideoDecodeThread decoder;
    QObject::connect(decoder.processor,&VideoProcessor::frameReady,[&](const QImage &frame) {
        if(!frame.isNull())++rendered;
    });
    AtvIqWorker worker([&](const QByteArray &data,double rate,int count) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            const double now=threadCpuMs();
            if(workerFirstCpu<0)workerFirstCpu=now;
            workerLastCpu=now;++frames;wake.notify_one();
        }
        if(!videoBudget.reserve(data.size(),count)){++decoderDrops;wake.notify_one();return;}
        QMetaObject::invokeMethod(decoder.processor,[&,data,rate,count] {
            decoder.processor->processIqFrame(data,rate,count);
            videoBudget.release(data.size());
            std::lock_guard<std::mutex> lock(mutex);++decodedFrames;wake.notify_one();
        },Qt::QueuedConnection);
    },[&](std::uint64_t count) {
        std::lock_guard<std::mutex> lock(mutex);dropped+=count;wake.notify_one();
    });
    worker.configure(settings,true,true);
    SpectrumIqEventRecorder recorder;
    require(recorder.start(directory.filePath("paced"),SpectrumIqEventRecorder::Mode::FullIqS16,
        settings,settings.sampleRate),"paced recording start");
    QByteArray encoded(iqPerBlock*4,Qt::Uninitialized);
    const auto started=std::chrono::steady_clock::now();
    const double cpuStarted=threadCpuMs();
    for(int block=0;block<blocks;++block) {
        std::this_thread::sleep_until(started+std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(block*double(iqPerBlock)/settings.sampleRate)));
        encodeFullIq16(input.data(),encoded.data(),input.size());
        require(recorder.appendFrame(encoded,settings.sampleRate,iqPerBlock),"paced disk write");
        worker.push(input.data(),input.size());
    }
    const double producerCpu=threadCpuMs()-cpuStarted;
    const double producerWall=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock,std::chrono::seconds(5),[&]{return decodedFrames==blocks || dropped>0 || decoderDrops>0;});
        std::cout<<"Paced 50 MS/s + disk: "<<recorder.bytesWritten()<<" bytes in "<<producerWall
                 <<" s; producer CPU="<<producerCpu<<" ms; video="<<frames<<'/'<<blocks
                 <<" frames, lost="<<dropped<<" IQ samples, worker CPU/block="
                 <<(frames>1?(workerLastCpu-workerFirstCpu)/(frames-1):0)<<" ms; decoder="
                 <<decodedFrames<<" blocks, "<<rendered<<" raster frames, "<<decoderDrops<<" queue drops\n";
        require(frames==blocks && dropped==0 && decodedFrames==blocks && decoderDrops==0 && rendered>0,
                "paced 50-MS/s video exceeded processing capacity");
    }
    recorder.stop();
}
}
int main(int argc,char **argv) {
    qputenv("QT_QPA_PLATFORM","offscreen");
    QGuiApplication app(argc,argv);
    try { QTemporaryDir dir;require(dir.isValid(),"temp directory");encoding();record(dir);concurrentVideo(dir);atvWaveform();
        if(app.arguments().contains("--paced"))pacedVideo(dir); }
    catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"Full IQ recording and concurrent ATV regression PASS\n";
}
