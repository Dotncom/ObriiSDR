#include "fftplanprofile.h"
#include "ffttuningguard.h"
#include "ffttransformcore.h"
#include "fftplannermutex.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <stdexcept>

static void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);QTemporaryDir temp;
    try{
        require(argc==2&&temp.isValid(),"CLI path and temporary cache required");
        const auto run=[&](const QStringList &args){QProcess process;process.start(QString::fromLocal8Bit(argv[1]),args);require(process.waitForStarted(3000)&&process.waitForFinished(30000),"Tuner process timeout");return qMakePair(process.exitCode(),process.readAllStandardOutput());};
        auto result=run({"fft-tune","--fft","1024","--max-threads","1","--planning-ms","100","--cache-dir",temp.path()});
        require(result.first==0&&result.second.contains("\"event\":\"complete\""),"Tuning failed");
        auto profile=loadFftPlanProfile(1024,16,temp.path());require(profile.threads==1&&!profile.wisdom.isEmpty()&&profile.medianMs>0,"Cross-process fingerprint/cache load failed");
        FftTransformCore core;require(core.allocateBuffers(1024,FftTransformCore::Storage::OutOfPlace),"Allocate cached plan fixture");
        {std::lock_guard<std::mutex> guard(fftwPlannerGlobalMutex());
            require(restoreFftPlanProfile(profile,core),"Saved plan could not be restored without benchmarking");}
        require(loadFftPlanProfile(2048,16,temp.path()).threads==0,"Different FFT reused profile");
        require(run({"fft-tune","--fft","777","--max-threads","1","--planning-ms","100","--cache-dir",temp.path()}).first==0&&
            loadFftPlanProfile(777,16,temp.path()).threads==1,"Non-power-of-two tuning failed");
        require(loadFftPlanProfile(1024,0,temp.path()).threads==0,"Thread budget ignored");
        QFile file(fftPlanProfilePath(1024,temp.path()));require(file.open(QIODevice::ReadOnly),"Read profile");auto object=QJsonDocument::fromJson(file.readAll()).object();file.close();
        const auto rewrite=[&](const QJsonObject &o){require(file.open(QIODevice::WriteOnly|QIODevice::Truncate),"Write fixture");file.write(QJsonDocument(o).toJson());file.close();};
        auto bad=object;bad["machine"]="other-host";rewrite(bad);require(!loadFftPlanProfile(1024,16,temp.path()).threads,"Foreign host accepted");
        bad=object;bad["wisdom"]="AAAA";rewrite(bad);require(!loadFftPlanProfile(1024,16,temp.path()).threads,"Corrupt wisdom accepted");
        rewrite(object);result=run({"fft-tune","--fft","511","--cache-dir",temp.path()});require(result.first!=0&&loadFftPlanProfile(1024,16,temp.path()).threads==1,"Invalid request destroyed valid profile");
        const auto pending=fftPlanProfilePath(1024,temp.path())+".test.pending";
        require(file.open(QIODevice::ReadOnly),"Read existing publication fixture");const auto previous=file.readAll();file.close();
        const QStringList deferred{"fft-tune","--fft","1024","--max-threads","1","--planning-ms","100","--cache-dir",temp.path(),"--pending-profile",pending};
        require(run(deferred).first==0&&QFile::exists(pending),"Pending publication failed");
        require(file.open(QIODevice::ReadOnly)&&file.readAll()==previous,"Pending result overwrote existing profile");file.close();
        QFile::remove(pending);require(loadFftPlanProfile(1024,16,temp.path()).threads==1,"Cancel at publication boundary removed old profile");
        require(run(deferred).first==0&&publishFftPlanProfile(pending,1024,temp.path())&&!QFile::exists(pending),"Explicit publication failed");
        require(run({"fft-tune","--fft","1048576","--max-threads","1","--planning-ms","100","--cache-dir",temp.path()}).first==0,"Same-size cancellation fixture failed");
        result=run({"fft-tune","--fft","1048576","--max-threads",QString::number(qMin(2,qMax(1,QThread::idealThreadCount()))),"--planning-ms","100","--cache-dir",temp.path()});
        require(result.first==0,"Time-limited threaded tuning failed");
        for(const auto &line:result.second.split('\n')) {
            const auto row=QJsonDocument::fromJson(line).object();
            if(row.value("event")=="candidate")require(row.value("restoreVerified").toBool()&&row.value("valid").toBool(),"Candidate was timed without a successful wisdom round trip");
        }
        core.releasePlan();require(core.allocateBuffers(1048576,FftTransformCore::Storage::OutOfPlace),"Allocate threaded cached fixture");
        {std::lock_guard<std::mutex> guard(fftwPlannerGlobalMutex());
            fftwf_forget_wisdom();require(restoreFftPlanProfile(loadFftPlanProfile(1048576,16,temp.path()),core),"Time-limited threaded cache did not restore across processes");}
        QFile large(fftPlanProfilePath(1048576,temp.path()));require(large.open(QIODevice::ReadOnly),"Read existing large profile");const auto largeBefore=large.readAll();large.close();
        QProcess cancel;cancel.start(QString::fromLocal8Bit(argv[1]),{"fft-tune","--fft","1048576","--max-threads","1","--planning-ms","1000","--cache-dir",temp.path()});
        require(cancel.waitForStarted(3000)&&cancel.waitForReadyRead(10000),"Cancelable tune did not start");cancel.kill();require(cancel.waitForFinished(3000),"Cancel did not terminate helper");
        require(large.open(QIODevice::ReadOnly)&&large.readAll()==largeBefore&&loadFftPlanProfile(1024,16,temp.path()).threads==1,"Cancel published partial plan or removed existing cache");large.close();
        require(removeFftPlanProfile(1024,temp.path())&&!loadFftPlanProfile(1024,16,temp.path()).threads,"Clear did not remove profile");
        core.releasePlan();require(core.allocateBuffers(1024,FftTransformCore::Storage::OutOfPlace),"Allocate cleared plan fixture");
        {std::lock_guard<std::mutex> guard(fftwPlannerGlobalMutex());require(!core.createPlanLocked(FFTW_WISDOM_ONLY|FFTW_ESTIMATE),"Cleared profile remained in planner wisdom");}
        bool expected=false;require(fftTuningGuard().compare_exchange_strong(expected,true)&&fftTuningActive(),"Guard acquire failed");expected=false;require(!fftTuningGuard().compare_exchange_strong(expected,true),"Overlapping tuning allowed");fftTuningGuard().store(false);
        std::puts("FFT tuning: native planner, threaded bounded-planning wisdom round trip, cross-process cache, identity/size/budget/integrity guards PASS");return 0;
    }catch(const std::exception &e){std::fprintf(stderr,"FFT tuning FAIL: %s\n",e.what());return 1;}
}
