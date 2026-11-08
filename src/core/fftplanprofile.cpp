#include "fftplanprofile.h"
#include "ffttransformcore.h"
#include "fftplannermutex.h"
#include <fftw3.h>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#endif

namespace {
constexpr int MaxCacheBytes = 2 * 1024 * 1024;
void emitJson(const QJsonObject &o) {
    const auto bytes = QJsonDocument(o).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, std::size_t(bytes.size()), stdout); std::fputc('\n', stdout); std::fflush(stdout);
}
QByteArray digest(const QByteArray &bytes) { return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex(); }
QByteArray exportWisdom() {
    QByteArray wisdom;
    // Export by callback because the DLL can use a different CRT allocator.
    fftwf_export_wisdom([](char c,void *data){auto &bytes=*static_cast<QByteArray *>(data);if(bytes.size()<MaxCacheBytes)bytes.append(c);},&wisdom);
    if(wisdom.isEmpty()||wisdom.size()>=MaxCacheBytes)throw std::runtime_error("Cannot export bounded FFTW plan");
    return wisdom;
}
QByteArray fftwIdentity() {
#ifdef _WIN32
    HMODULE module=nullptr;wchar_t path[32768]{};
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&fftwf_execute),&module)&&GetModuleFileNameW(module,path,32768)) {
        QFile file(QString::fromWCharArray(path));
        if(file.open(QIODevice::ReadOnly))return digest(file.readAll());
    }
    return "unavailable-fftw-binary";
#else
    return QByteArray(fftwf_version);
#endif
}
void fill(fftwf_complex *input, int length) {
    quint32 rng = 1729;
    for(int i=0;i<length;++i) for(int c=0;c<2;++c) {
        rng = rng * 1664525u + 1013904223u;
        input[i][c] = float(double(rng)/4294967296.0 - 0.5);
    }
}
quint64 availableMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX memory{}; memory.dwLength=sizeof(memory);
    if(GlobalMemoryStatusEx(&memory)) return memory.ullAvailPhys;
#else
    QFile file(QStringLiteral("/proc/meminfo"));
    if(file.open(QIODevice::ReadOnly)) for(const auto &line:file.readAll().split('\n')) {
        const auto fields=line.simplified().split(' ');
        if(fields.value(0)=="MemAvailable:") return fields.value(1).toULongLong()*1024;
    }
#endif
    return 0;
}
}

QString fftPlanMachineKey() {
    static const QString key=[] {
        QByteArray identity = QSysInfo::machineUniqueId()+QSysInfo::machineHostName().toUtf8()+
            QSysInfo::currentCpuArchitecture().toUtf8()+QSysInfo::kernelType().toUtf8()+
            fftwIdentity()+QByteArray::number(QThread::idealThreadCount());
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        int cpu[4]{}; __cpuid(cpu,1); cpu[1]&=0x00ffffff; identity.append(reinterpret_cast<const char *>(cpu),sizeof(cpu));
        for(unsigned leaf=0x80000002;leaf<=0x80000004;++leaf) {
            __cpuid(cpu,int(leaf));identity.append(reinterpret_cast<const char *>(cpu),sizeof(cpu));
        }
#else
        QFile cpu(QStringLiteral("/proc/cpuinfo"));
        if(cpu.open(QIODevice::ReadOnly)) {
            const auto lines=cpu.readAll().split('\n');
            for(const auto &line:lines) if(line.startsWith("model name") || line.startsWith("CPU part") ||
                line.startsWith("Hardware") || line.startsWith("Features")) identity+=line;
        }
#endif
        return QString::fromLatin1(digest(identity));
    }();
    return key;
}
QString fftPlanCacheDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("ObriiSDR/fft-tuning"));
}
QString fftPlanProfilePath(int length,const QString &directory) {
    return QDir(directory.isEmpty()?fftPlanCacheDirectory():directory).filePath(
        fftPlanMachineKey()+QStringLiteral("-%1.json").arg(length));
}
static FftPlanProfile readProfile(const QString &path,int length,int maxThreads) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)||file.size()>MaxCacheBytes) return {};
    const auto doc=QJsonDocument::fromJson(file.readAll()); if(!doc.isObject())return {};
    const auto o=doc.object();const int threads=o.value("threads").toInt();
    const auto encoded=o.value("wisdom").toString().toLatin1();const auto wisdom=QByteArray::fromBase64(encoded);
    const double ms=o.value("medianMs").toDouble();
    if(o.value("schema")!=1 || o.value("machine")!=fftPlanMachineKey() || o.value("length")!=length ||
        o.value("layout")!="cf32-forward-outofplace" || threads<1 || threads>maxThreads || threads>16 ||
        !std::isfinite(ms)||ms<=0 ||wisdom.isEmpty()||wisdom.toBase64()!=encoded ||wisdom.contains('\0')||
        o.value("wisdomSha256").toString().toLatin1()!=digest(wisdom))return {};
    return {threads,wisdom,ms};
}
FftPlanProfile loadFftPlanProfile(int length,int maxThreads,const QString &directory) {
    return readProfile(fftPlanProfilePath(length,directory),length,maxThreads);
}
bool publishFftPlanProfile(const QString &pendingPath,int length,const QString &directory) {
    const auto destination=fftPlanProfilePath(length,directory);
    if(!pendingPath.startsWith(destination+".")||!pendingPath.endsWith(".pending")||QFileInfo(pendingPath).isSymLink()||QFileInfo(destination).isSymLink()||
        QFileInfo(pendingPath).absolutePath()!=QFileInfo(destination).absolutePath())return false;
    QLockFile lock(QDir(QFileInfo(destination).absolutePath()).filePath("tuning.lock"));if(!lock.tryLock(0))return false;
    if(!readProfile(pendingPath,length,16).threads)return false;
    QFile source(pendingPath);if(!source.open(QIODevice::ReadOnly)||source.size()>MaxCacheBytes)return false;
    const auto bytes=source.readAll();QSaveFile target(destination);
    if(!target.open(QIODevice::WriteOnly)||target.write(bytes)!=bytes.size()||!target.commit())return false;
    QFile::setPermissions(destination,QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    source.close();QFile::remove(pendingPath);++fftPlanProfileGeneration();return true;
}
bool removeFftPlanProfile(int length,const QString &directory) {
    const auto path=fftPlanProfilePath(length,directory);
    const bool okay=!QFile::exists(path)||QFile::remove(path);
    if(okay){std::lock_guard<std::mutex> guard(fftwPlannerGlobalMutex());fftwf_forget_wisdom();++fftPlanProfileGeneration();}return okay;
}
bool restoreFftPlanProfile(const FftPlanProfile &profile,FftTransformCore &transform) {
    if(profile.threads<1 || profile.wisdom.isEmpty())return false;
#ifdef FOBOSAPP_HAS_FFTW_THREADS
    static const bool threadsAvailable=fftwf_init_threads()!=0;
    if(!threadsAvailable&&profile.threads>1)return false;
    if(threadsAvailable)fftwf_plan_with_nthreads(profile.threads);
#else
    if(profile.threads>1)return false;
#endif
    const bool ready=fftwf_import_wisdom_from_string(profile.wisdom.constData())&&
        transform.createPlanLocked(FFTW_WISDOM_ONLY|FFTW_ESTIMATE);
#ifdef FOBOSAPP_HAS_FFTW_THREADS
    if(threadsAvailable)fftwf_plan_with_nthreads(1);
#endif
    return ready;
}

int runFftTuningCommand(const QStringList &arguments) {
    QCommandLineParser parser;parser.setApplicationDescription(QStringLiteral("Offline FFT tuner; stop all receivers before running."));parser.addHelpOption();
    parser.addOption({"fft","FFT size (512..134217728)","length"});
    parser.addOption({"max-threads","Maximum CPU threads (1..16)","count","4"});
    parser.addOption({"planning-ms","MEASURE planning budget per candidate (100..5000)","ms","500"});
    parser.addOption({"cache-dir","Profile directory (optional)","directory"});
    parser.addOption({"pending-profile","Stage profile for GUI acceptance without replacing the cache","path"});
    if(!parser.parse(arguments)){emitJson({{"ok",false},{"error",parser.errorText()}});return 2;}
    if(parser.isSet("help")){std::fputs(parser.helpText().toUtf8().constData(),stdout);return 0;}
    bool nOkay=false,tOkay=false,pOkay=false;
    const int length=parser.value("fft").toInt(&nOkay),maximum=parser.value("max-threads").toInt(&tOkay),planning=parser.value("planning-ms").toInt(&pOkay);
    if(!nOkay||length<512||length>134217728||!tOkay||maximum<1||maximum>16||maximum>qMax(1,QThread::idealThreadCount())||!pOkay||planning<100||planning>5000){
        emitJson({{"ok",false},{"error","Invalid FFT size, thread count or planning budget"}});return 2;
    }
    const auto directory=parser.value("cache-dir").isEmpty()?fftPlanCacheDirectory():QFileInfo(parser.value("cache-dir")).absoluteFilePath();
    if(!QDir().mkpath(directory)){emitJson({{"ok",false},{"error","Cannot create FFT profile directory"}});return 1;}
    const auto finalPath=fftPlanProfilePath(length,directory);
    const auto path=parser.isSet("pending-profile")?QFileInfo(parser.value("pending-profile")).absoluteFilePath():finalPath;
    if(path!=finalPath&&(!path.startsWith(finalPath+".")||!path.endsWith(".pending")||QFileInfo(path).absolutePath()!=QFileInfo(finalPath).absolutePath()||QFileInfo(path).isSymLink())){
        emitJson({{"ok",false},{"error","Invalid pending profile path"}});return 2;
    }
    QLockFile lock(QDir(directory).filePath("tuning.lock"));
    if(!lock.tryLock(0)){emitJson({{"ok",false},{"error","Another FFT tune is already active in this cache"}});return 1;}
    const quint64 required=quint64(length)*sizeof(fftwf_complex)*2;
    const auto memory=availableMemory();
    if(!memory&&required>128*1024*1024){emitJson({{"ok",false},{"error","Cannot verify available RAM for this FFT size"}});return 1;}
    if(memory && required+required/4+64*1024*1024>memory){emitJson({{"ok",false},{"error","Insufficient available RAM for safe FFT tuning"}});return 1;}
    try {
        using Buffer=std::unique_ptr<fftwf_complex,decltype(&fftwf_free)>;
        Buffer input(static_cast<fftwf_complex *>(fftwf_malloc(std::size_t(length)*sizeof(fftwf_complex))),fftwf_free);
        Buffer output(static_cast<fftwf_complex *>(fftwf_malloc(std::size_t(length)*sizeof(fftwf_complex))),fftwf_free);
        if(!input||!output)throw std::runtime_error("FFT buffer allocation failed");
        int limit=1;
#ifdef FOBOSAPP_HAS_FFTW_THREADS
        if(!fftwf_init_threads())throw std::runtime_error("FFTW thread initialization failed");limit=maximum;
#endif
        std::array<std::complex<float>,257> reference{};bool referenceReady=false;
        QJsonArray trials;double best=1e300;int bestThreads=0;QByteArray bestWisdom;QString bestStrategy;
        for(unsigned flags:{unsigned(FFTW_ESTIMATE),unsigned(FFTW_MEASURE)})for(int threads=1;threads<=limit;++threads){
            fftwf_forget_wisdom();
#ifdef FOBOSAPP_HAS_FFTW_THREADS
            fftwf_plan_with_nthreads(threads);
#endif
            fftwf_set_timelimit(planning/1000.0);
            using Plan=std::unique_ptr<std::remove_pointer_t<fftwf_plan>,decltype(&fftwf_destroy_plan)>;
            Plan plan(fftwf_plan_dft_1d(length,input.get(),output.get(),FFTW_FORWARD,flags),fftwf_destroy_plan);
            if(!plan)throw std::runtime_error("FFTW planning failed");
            // A timed-out MEASURE can leave an unserializable root in FFTW 3.3.5.
            // Materialize its partial wisdom cheaply, then test a real cache round trip.
            fftwf_set_timelimit(FFTW_NO_TIMELIMIT);
            plan.reset(fftwf_plan_dft_1d(length,input.get(),output.get(),FFTW_FORWARD,FFTW_ESTIMATE));
            if(!plan)throw std::runtime_error("Cannot materialize reusable FFTW plan");
            const auto wisdom=exportWisdom();
            plan.reset();fftwf_forget_wisdom();
            if(fftwf_import_wisdom_from_string(wisdom.constData()))
                plan.reset(fftwf_plan_dft_1d(length,input.get(),output.get(),FFTW_FORWARD,FFTW_WISDOM_ONLY|FFTW_ESTIMATE));
            const QString strategy=flags==FFTW_ESTIMATE?"estimate":"measure";
            if(!plan){
                QJsonObject row{{"event","candidate"},{"threads",threads},{"strategy",strategy},{"medianMs",0.0},{"valid",false},{"restoreVerified",false}};
                trials.append(row);emitJson(row);continue;
            }
            fill(input.get(),length);fftwf_execute(plan.get());bool valid=true;
            for(int i=0;i<int(reference.size());++i){const int bin=int(quint64(i)*(length-1)/(reference.size()-1));const std::complex<float> value(output.get()[bin][0],output.get()[bin][1]);
                if(!std::isfinite(value.real())||!std::isfinite(value.imag()))valid=false;
                if(!referenceReady)reference[i]=value;
                else if(std::abs(value-reference[i])>0.0001f*(1.0f+std::abs(reference[i])))valid=false;
            }
            if(!referenceReady&&!valid)throw std::runtime_error("Non-finite baseline FFT result");
            referenceReady=true;std::array<double,3> times{};
            for(double &ms:times){const auto start=std::chrono::steady_clock::now();fftwf_execute(plan.get());ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}
            std::sort(times.begin(),times.end());
            QJsonObject row{{"event","candidate"},{"threads",threads},{"strategy",strategy},{"medianMs",times[1]},{"valid",valid},{"restoreVerified",true}};trials.append(row);emitJson(row);
            if(valid&&times[1]<best){
                bestWisdom=wisdom;best=times[1];bestThreads=threads;bestStrategy=strategy;
            }
        }
        if(!bestThreads)throw std::runtime_error("No numerically valid FFT candidate");
        QJsonObject profile{{"schema",1},{"machine",fftPlanMachineKey()},{"length",length},{"layout","cf32-forward-outofplace"},
            {"threads",bestThreads},{"strategy",bestStrategy},{"medianMs",best},{"wisdom",QString::fromLatin1(bestWisdom.toBase64())},
            {"wisdomSha256",QString::fromLatin1(digest(bestWisdom))},{"createdUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},{"trials",trials}};
        const auto bytes=QJsonDocument(profile).toJson(QJsonDocument::Compact);if(bytes.size()>MaxCacheBytes)throw std::runtime_error("FFT profile exceeds cache budget");
        QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())throw std::runtime_error("Cannot atomically save FFT profile");
        QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner);
        emitJson({{"ok",true},{"event","complete"},{"length",length},{"threads",bestThreads},{"strategy",bestStrategy},{"medianMs",best},{"profile",path}});return 0;
    }catch(const std::exception &e){emitJson({{"ok",false},{"error",QString::fromUtf8(e.what())}});return 1;}
}
