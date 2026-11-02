#include "cliaudiooutput.h"
#include "clivfomanager.h"
#include "clifrequencyscanner.h"
#include "cliiqtrigger.h"
#include "cliiqquality.h"
#include "clizerospan.h"
#include "clidspgraph.h"
#include "climodulemanager.h"
#include "clinetworkbridge.h"
#include "clispectrummeter.h"
#include "cliasyncspectrum.h"
#include "clispectrumaccumulator.h"
#include "climultisdr.h"
#include "clivirtualiqsource.h"
#include "clicapabilities.h"
#include "clicalibration.h"
#include "cliwebservice.h"
#include "clignsssession.h"
#include "cliresearchtools.h"
#include "clidoctor.h"
#include "cliruntimerecorder.h"
#include "iqbuffer.h"
#include "clitextdecoder.h"
#include "iqchannelizer.h"
#include "spectrumframerecorder.h"
#include "spectrumiqeventrecorder.h"
#include "spectrumscienceanalyzer.h"
#include "tonalspectrum.h"
#include "fobos.h"
#include "fobos_sdr.h"
#include "fobosreceivercore.h"
#include "recordingformatcore.h"
#include "commanddispatcher.h"
#include "commandcontrolserver.h"
#include "clicommandadapter.h"
#include "hackrfbackend.h"
#include "remoteaudioplayer.h"
#include "rtlsdrbackend.h"
#include "soapysdrbackend.h"
#include "uhdbackend.h"
#include "virtualsignalframe.h"
#include "virtualsignaltransport.h"
#include "audiofilterchain.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
bool binaryStdout = false;
void interruptHandler(int) { interrupted = 1; }

void printJson(const QJsonObject &object) {
    QTextStream out(binaryStdout ? stderr : stdout);
    out << QJsonDocument(object).toJson(QJsonDocument::Compact) << Qt::endl;
}

int fail(const QString &message) {
    QTextStream err(stderr);
    err << message << Qt::endl;
    return 1;
}

QJsonObject optionalToolRequest(const QJsonObject &input) {
    if (!input.contains(QStringLiteral("path"))) return input;
    QFile file(input.value(QStringLiteral("path")).toString());
    QJsonParseError error;
    const auto document = file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024
        ? QJsonDocument::fromJson(file.readAll(), &error) : QJsonDocument{};
    return document.isObject() && error.error == QJsonParseError::NoError ? document.object() : QJsonObject{};
}

constexpr qint64 ProfileLimit = 16 * 1024 * 1024;
const QString ProfileFormat = QStringLiteral("obrii-cli-experiment");

bool validProfile(const QJsonObject &profile, QString *error) {
    const auto reject = [error](const QString &message) { if (error) *error = message; return false; };
    if (profile.value(QStringLiteral("format")).toString() != ProfileFormat ||
        profile.value(QStringLiteral("version")).toInt() != 1 ||
        !QStringList{QStringLiteral("preset"), QStringLiteral("experiment")}
            .contains(profile.value(QStringLiteral("kind")).toString()) ||
        !profile.value(QStringLiteral("launch")).isArray() ||
        !profile.value(QStringLiteral("state")).isObject())
        return reject(QStringLiteral("Unsupported or incomplete CLI experiment profile"));
    const QSet<QString> valued{QStringLiteral("--backend"), QStringLiteral("--api"),
        QStringLiteral("--device"), QStringLiteral("--input"), QStringLiteral("--freq"),
        QStringLiteral("--rate"), QStringLiteral("--listen"), QStringLiteral("--mode"),
        QStringLiteral("--bandwidth"), QStringLiteral("--gain-db"), QStringLiteral("--lna"),
        QStringLiteral("--vga"), QStringLiteral("--rx-channel"), QStringLiteral("--antenna"),
        QStringLiteral("--fpga-image"), QStringLiteral("--iq-file"), QStringLiteral("--rtl-host"),
        QStringLiteral("--rtl-port"), QStringLiteral("--block-samples"), QStringLiteral("--fft"),
        QStringLiteral("--spectrum-ms"), QStringLiteral("--decode"), QStringLiteral("--decode-tone"),
        QStringLiteral("--cw-wpm"), QStringLiteral("--cw-selectivity"),
        QStringLiteral("--cw-squelch-db"), QStringLiteral("--cw-alphabet"),
        QStringLiteral("--psk-squelch"), QStringLiteral("--psk-afc"),
        QStringLiteral("--control-port"), QStringLiteral("--bind"),
        QStringLiteral("--network-port"), QStringLiteral("--virtual-host"),
        QStringLiteral("--virtual-port"), QStringLiteral("--virtual-node-id"),
        QStringLiteral("--virtual-name"), QStringLiteral("--virtual-rx-transport"),
        QStringLiteral("--virtual-rx-bind"), QStringLiteral("--virtual-rx-port"),
        QStringLiteral("--virtual-rx-queue-mib"), QStringLiteral("--hz-per-bin"),
        QStringLiteral("--web-port"), QStringLiteral("--web-bind")};
    const QSet<QString> flags{QStringLiteral("--serve"), QStringLiteral("--no-audio"),
        QStringLiteral("--spectrum"), QStringLiteral("--progress"),
        QStringLiteral("--audio-play"), QStringLiteral("--audio-stdout"),
        QStringLiteral("--module-voice-stdout")};
    const QJsonArray launch = profile.value(QStringLiteral("launch")).toArray();
    if (launch.isEmpty() || launch.size() > 128)
        return reject(QStringLiteral("CLI launch option count is invalid"));
    QSet<QString> seen;
    for (int i = 0; i < launch.size(); ++i) {
        if (!launch[i].isString()) return reject(QStringLiteral("CLI launch options must be strings"));
        const QString key = launch[i].toString();
        if (seen.contains(key) || (!valued.contains(key) && !flags.contains(key)))
            return reject(QStringLiteral("Unsafe or duplicate CLI launch option: %1").arg(key));
        seen.insert(key);
        if (valued.contains(key)) {
            if (++i >= launch.size() || !launch[i].isString() || launch[i].toString().isEmpty() ||
                launch[i].toString().size() > 4096)
                return reject(QStringLiteral("Missing or invalid value for %1").arg(key));
        }
    }
    if (!seen.contains(QStringLiteral("--backend")) || !seen.contains(QStringLiteral("--freq")) ||
        !seen.contains(QStringLiteral("--rate")))
        return reject(QStringLiteral("CLI experiment profile lacks receiver identity"));
    const QJsonObject state = profile.value(QStringLiteral("state")).toObject();
    if (!state.value(QStringLiteral("science")).isObject() ||
        !state.value(QStringLiteral("filters")).isArray() ||
        !state.value(QStringLiteral("vfos")).isArray() ||
        !state.value(QStringLiteral("multisdr")).isArray())
        return reject(QStringLiteral("CLI experiment state is incomplete"));
    if (state.value(QStringLiteral("filters")).toArray().size() > 32 ||
        state.value(QStringLiteral("vfos")).toArray().size() > 4 ||
        state.value(QStringLiteral("multisdr")).toArray().size() > 7)
        return reject(QStringLiteral("CLI experiment state exceeds supported limits"));
    return true;
}

bool readProfile(const QString &path, QJsonObject *profile, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > ProfileLimit) {
        if (error) *error = QStringLiteral("Cannot read CLI profile or it exceeds 16 MiB: %1").arg(file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Invalid CLI profile JSON: %1").arg(parseError.errorString());
        return false;
    }
    if (!validProfile(document.object(), error)) return false;
    *profile = document.object();
    return true;
}

bool writeProfile(const QString &path, const QJsonObject &profile, QString *error) {
    const QByteArray bytes = QJsonDocument(profile).toJson(QJsonDocument::Indented);
    if (bytes.size() > ProfileLimit) {
        if (error) *error = QStringLiteral("CLI experiment profile exceeds 16 MiB");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        if (error) *error = file.errorString();
        file.close();
        file.remove();
        return false;
    }
    return true;
}

QJsonObject measureStoredTrace(const QJsonObject &trace, const QString &source,
                               double selectedStart, double selectedEnd, bool selected) {
    const QJsonArray bins = trace.value(QStringLiteral("levelsDbfs")).toArray();
    const double start = trace.value(QStringLiteral("startHz")).toDouble();
    const double binHz = trace.value(QStringLiteral("binHz")).toDouble();
    if (bins.size() < 2 || bins.size() > 65536 || !std::isfinite(start) ||
        !std::isfinite(binHz) || binHz <= 0.0)
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Invalid stored trace")}};
    const double end = start + bins.size() * binHz;
    if (selected && (!std::isfinite(selectedStart) || !std::isfinite(selectedEnd) ||
        selectedStart < start || selectedEnd > end || selectedEnd <= selectedStart))
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Measurement range is outside stored trace")}};
    std::vector<float> frequencies, levels;
    frequencies.reserve(bins.size()); levels.reserve(bins.size());
    for (int i = 0; i < bins.size(); ++i) {
        const double level = bins[i].toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(level))
            return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Stored trace contains invalid levels")}};
        frequencies.push_back(static_cast<float>(i));
        levels.push_back(static_cast<float>(level));
    }
    SpectrumScienceAnalyzer analyzer;
    analyzer.update(frequencies, levels);
    if (selected) {
        analyzer.setMarker(0, (selectedStart - start) / binHz);
        analyzer.setMarker(1, (selectedEnd - start) / binHz);
    }
    const SpectrumScienceMetrics &m = analyzer.metrics();
    if (!m.valid)
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Stored trace could not be measured")}};
    QJsonObject result{{QStringLiteral("ok"), true}, {QStringLiteral("source"), source},
        {QStringLiteral("rangeStartHz"), start + m.rangeStartHz * binHz},
        {QStringLiteral("rangeEndHz"), start + m.rangeEndHz * binHz},
        {QStringLiteral("binHz"), binHz}, {QStringLiteral("binCount"), m.binCount},
        {QStringLiteral("peakFrequencyHz"), start + m.peakFrequencyHz * binHz},
        {QStringLiteral("peakDbfs"), m.peakDb},
        {QStringLiteral("noiseFloorDbfs"), m.noiseFloorDb},
        {QStringLiteral("snrEstimateDb"), m.snrDb},
        {QStringLiteral("centroidFrequencyHz"), start + m.centroidFrequencyHz * binHz},
        {QStringLiteral("channelPowerDbfsRelative"), m.channelPowerDb},
        {QStringLiteral("obw90Hz"), m.occupiedBandwidth90Hz * binHz},
        {QStringLiteral("obw95Hz"), m.occupiedBandwidth95Hz * binHz},
        {QStringLiteral("obw99Hz"), m.occupiedBandwidthHz * binHz},
        {QStringLiteral("width3DbHz"), m.width3DbHz * binHz},
        {QStringLiteral("width6DbHz"), m.width6DbHz * binHz},
        {QStringLiteral("width20DbHz"), m.width20DbHz * binHz}};
    if (selected) {
        result.insert(QStringLiteral("acprLowerDb"), m.acprLowerDb);
        result.insert(QStringLiteral("acprUpperDb"), m.acprUpperDb);
    }
    return result;
}

void writeModuleVoice(const QByteArray &pcm) {
    if (pcm.isEmpty()) return;
    const auto size = static_cast<std::size_t>(pcm.size());
    if (std::fwrite(pcm.constData(), 1, size, stdout) != size || std::fflush(stdout) != 0) {
        fail(QStringLiteral("Cannot write module voice PCM to stdout"));
        interrupted = 1;
    }
}

class CliPcmRecorder {
public:
    bool open(const QString &path, QString *error) {
        if (path.isEmpty()) return true;
        file.setFileName(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            *error = file.errorString();
            return false;
        }
        return RecordingFormatCore::beginWave(file, 48000, 1, 16, {}, layout, error);
    }
    bool append(const QByteArray &pcm, QString *error) {
        if (!file.isOpen()) return true;
        return RecordingFormatCore::appendWave(file, layout, bytes, pcm.constData(), pcm.size(), error);
    }
    bool close(QString *error) {
        if (!file.isOpen()) return true;
        const bool okay = RecordingFormatCore::finalizeWave(file, layout, bytes, error);
        file.close();
        return okay;
    }
private:
    QFile file;
    quint64 bytes = 0;
    RecordingFormatCore::WaveLayout layout;
};

double frequency(const QString &value, bool *okay) {
    QString text = value.trimmed().toLower();
    double multiplier = 1.0;
    if (text.endsWith(QStringLiteral("ghz"))) { text.chop(3); multiplier = 1e9; }
    else if (text.endsWith(QStringLiteral("mhz"))) { text.chop(3); multiplier = 1e6; }
    else if (text.endsWith(QStringLiteral("khz"))) { text.chop(3); multiplier = 1e3; }
    else if (text.endsWith('g')) { text.chop(1); multiplier = 1e9; }
    else if (text.endsWith('m')) { text.chop(1); multiplier = 1e6; }
    else if (text.endsWith('k')) { text.chop(1); multiplier = 1e3; }
    bool parsed = false;
    const double result = text.toDouble(&parsed) * multiplier;
    *okay = parsed && std::isfinite(result) && result > 0.0;
    return result;
}

QJsonObject fobosError(const char *operation, int code) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("operation"), QString::fromLatin1(operation)},
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), QString::fromLatin1(fobos_rx_error_name(code))}};
}

using ApiKind = FobosApiKind;
enum class BackendKind { Fobos, HackRf, Soapy, Uhd, RtlNative, RtlTcp, File, Virtual };

QString apiName(ApiKind kind) {
    return kind == ApiKind::Agile ? QStringLiteral("agile") : QStringLiteral("standard");
}

double spectrumFirstHz(double center, double rate, int bins) {
    return center - (bins / 2) * (rate / bins);
}

QString backendName(BackendKind kind) {
    if (kind == BackendKind::HackRf) return QStringLiteral("hackrf");
    if (kind == BackendKind::Soapy) return QStringLiteral("soapy");
    if (kind == BackendKind::Uhd) return QStringLiteral("uhd");
    if (kind == BackendKind::RtlNative) return QStringLiteral("rtl");
    if (kind == BackendKind::RtlTcp) return QStringLiteral("rtl-tcp");
    if (kind == BackendKind::File) return QStringLiteral("file");
    if (kind == BackendKind::Virtual) return QStringLiteral("virtual");
    return QStringLiteral("fobos");
}

QString inputName(int mode) {
    if (mode == INPUT_HF1) return QStringLiteral("hf1");
    if (mode == INPUT_HF2) return QStringLiteral("hf2");
    if (mode == INPUT_HF_COMBINED) return QStringLiteral("hf-combined");
    return QStringLiteral("rf");
}

int audioModeFromName(const QString &name) {
    if (name == QStringLiteral("am")) return MOD_AM;
    if (name == QStringLiteral("nfm")) return MOD_NFM;
    if (name == QStringLiteral("wfm")) return MOD_WFM;
    if (name == QStringLiteral("usb")) return MOD_USB;
    if (name == QStringLiteral("lsb")) return MOD_LSB;
    if (name == QStringLiteral("cw")) return MOD_CW;
    return -1;
}

double defaultAudioBandwidth(int mode) {
    if (mode == MOD_WFM) return 200000.0;
    if (mode == MOD_NFM) return 12500.0;
    if (mode == MOD_AM) return 10000.0;
    if (mode == MOD_CW) return 1200.0;
    return 3000.0;
}

ApiKind chooseApi(const QString &requested) {
    if (requested == QStringLiteral("agile") ||
        (requested == QStringLiteral("auto") && FobosReceiverCore::deviceCount(ApiKind::Agile) > 0))
        return ApiKind::Agile;
    return ApiKind::Standard;
}

struct Device {
    explicit Device(BackendKind selectedBackend = BackendKind::Fobos)
        : backend(selectedBackend) {}
    BackendKind backend = BackendKind::Fobos;
    FobosReceiverCore fobos;
    void *soapy = nullptr;
    void *soapyStream = nullptr;
    void *rtlNative = nullptr;
    void *hackrf = nullptr;
    std::vector<std::int8_t> hackrfRing;
    std::mutex hackrfMutex;
    std::condition_variable hackrfReady;
    std::size_t hackrfRead = 0, hackrfWrite = 0, hackrfAvailable = 0;
    bool hackrfRunning = false;
    std::atomic<quint64> hackrfDropped{0};
    std::vector<unsigned char> rtlNativeBuffer;
    UhdRxSession uhd;
    QTcpSocket rtlSocket;
    QByteArray rtlBuffer;
    QFile replayFile;
    QElapsedTimer replayClock;
    quint64 replaySamples = 0;
    bool replayRunning = false;
    bool replayFinished = false;
    bool replayCenterSet = false;
    QString replayPath;
    QString rtlHost = QStringLiteral("127.0.0.1");
    quint16 rtlPort = 1234;
    unsigned int index = 0;
    int channel = 0;
    double requestedRate = 0.0;
    double requestedCenter = 0.0;
    double gainDb = 0.0;
    QString antenna;
    QString fpgaImage;
    QString lastError;
    CliVirtualIqSource virtualSource;
    VirtualSignalEndpoint virtualEndpoint;
    qint64 virtualQueueBytes = 16 * 1024 * 1024;
    ~Device() {
        stop();
        fobos.close();
        if (soapy) closeSoapySdrDeviceSafely(soapy);
        if (rtlNative) closeRtlSdrDeviceSafely(rtlNative);
        if (hackrf) closeHackRfDeviceSafely(hackrf);
    }
    static int hackrfCallback(HackRfTransfer *transfer) {
        auto *self = transfer ? static_cast<Device *>(transfer->rxContext) : nullptr;
        if (!self || !transfer->buffer || transfer->validLength < 2 || transfer->bufferLength < 2) return 0;
        std::lock_guard<std::mutex> lock(self->hackrfMutex);
        if (!self->hackrfRunning || self->hackrfRing.empty()) return -1;
        const std::size_t length = static_cast<std::size_t>(
            (std::min)(transfer->validLength, transfer->bufferLength)) & ~std::size_t{1};
        const std::size_t capacity = self->hackrfRing.size();
        const std::size_t retained = (std::min)(length, capacity);
        const auto *bytes = reinterpret_cast<const std::int8_t *>(transfer->buffer) + length - retained;
        const std::size_t evicted = self->hackrfAvailable > capacity - retained
            ? self->hackrfAvailable - (capacity - retained) : 0;
        self->hackrfDropped.fetch_add((length - retained + evicted) / 2, std::memory_order_relaxed);
        self->hackrfRead = (self->hackrfRead + evicted) % capacity;
        self->hackrfAvailable -= evicted;
        const std::size_t first = (std::min)(retained, capacity - self->hackrfWrite);
        std::memcpy(self->hackrfRing.data() + self->hackrfWrite, bytes, first);
        if (retained > first)
            std::memcpy(self->hackrfRing.data(), bytes + first, retained - first);
        self->hackrfWrite = (self->hackrfWrite + retained) % capacity;
        self->hackrfAvailable += retained;
        self->hackrfReady.notify_one();
        return 0;
    }
    int open(ApiKind api, unsigned int index) {
        this->index = index;
        if (backend == BackendKind::Virtual) {
            if (index == 0) return 0;
            lastError = QStringLiteral("Virtual receiver exposes index 0");
            return -1;
        }
        if (backend == BackendKind::File) {
            replayFile.setFileName(replayPath);
            if (index != 0 || !replayFile.open(QIODevice::ReadOnly)) {
                lastError = index != 0 ? QStringLiteral("File source exposes index 0")
                                       : replayFile.errorString();
                return -1;
            }
            if (replayFile.size() == 0 || replayFile.size() % 8 != 0) {
                lastError = QStringLiteral("CF32LE file must contain whole complex samples");
                replayFile.close();
                return -1;
            }
            return 0;
        }
        if (backend == BackendKind::Soapy) {
            if (openSoapySdrDeviceSafely(&soapy, static_cast<int>(index)) == 0) return 0;
            lastError = soapySdrLastErrorMessage();
            return -1;
        }
        if (backend == BackendKind::Uhd) {
            const QStringList devices = enumerateUhdDeviceArgs(&lastError);
            if (index < static_cast<unsigned int>(devices.size())) return 0;
            if (lastError.isEmpty()) lastError = QStringLiteral("No UHD device at the selected index");
            return -1;
        }
        if (backend == BackendKind::RtlNative) {
            const int result = openRtlSdrDeviceSafely(&rtlNative, index);
            if (result != 0) lastError = QStringLiteral("RTL-SDR device %1 open failed: %2").arg(index).arg(result);
            return result;
        }
        if (backend == BackendKind::HackRf) {
            const int result = openHackRfDeviceSafely(&hackrf, static_cast<int>(index));
            if (result != 0) lastError = result == -16003
                ? QStringLiteral("No HackRF device at index %1").arg(index)
                : hackRfLastErrorMessage();
            return result;
        }
        if (backend == BackendKind::RtlTcp) {
            if (index == 0) return 0;
            lastError = QStringLiteral("rtl_tcp exposes one endpoint at index 0");
            return -1;
        }
        return fobos.open(api, index);
    }
    int boardInfo(char *hw, char *fw, char *maker, char *product, char *serial) {
        return fobos.boardInfo(hw, fw, maker, product, serial);
    }
    int sampleRates(double *values, unsigned int *count) {
        if (backend != BackendKind::Fobos) return -1;
        return fobos.sampleRates(values, count);
    }
    int directSampling(unsigned int enabled) {
        if (backend != BackendKind::Fobos) return enabled == 0 ? 0 : -1;
        return fobos.directSampling(enabled);
    }
    int sampleRate(double requested, double *actual) {
        if (backend != BackendKind::Fobos) {
            requestedRate = requested;
            *actual = requested;
            if (backend == BackendKind::File) return 0;
            if (backend == BackendKind::Virtual) return 0;
            if (backend == BackendKind::Uhd) return 0;
            if (backend == BackendKind::HackRf) {
                const int result = setHackRfSampleRateSafely(hackrf, requested);
                if (result != 0) lastError = hackRfLastErrorMessage();
                return result;
            }
            if (backend == BackendKind::RtlNative) {
                const int result = setRtlSdrSampleRateSafely(rtlNative, static_cast<quint32>(requested));
                if (result != 0) lastError = QStringLiteral("RTL-SDR sample-rate error %1").arg(result);
                return result;
            }
            if (backend == BackendKind::RtlTcp) return rtlSocket.state() == QAbstractSocket::ConnectedState
                ? sendRtlCommand(0x02, static_cast<quint32>(requested)) : 0;
            const int result = setSoapySdrSampleRateSafely(soapy, requested, channel);
            if (result != 0) lastError = soapySdrLastErrorMessage();
            return result;
        }
        return fobos.sampleRate(requested, actual);
    }
    int frequency(double requested, double *actual) {
        if (backend != BackendKind::Fobos) {
            if (backend == BackendKind::Virtual && requestedCenter > 0.0 &&
                std::abs(requested - requestedCenter) > 0.5) {
                lastError = QStringLiteral("Virtual IQ center is fixed by the publisher; change listening frequency within its band");
                return -1;
            }
            if (backend == BackendKind::File && replayCenterSet &&
                std::abs(requested - requestedCenter) > 0.5) {
                lastError = QStringLiteral("Recorded IQ center is fixed; change listening frequency instead");
                return -1;
            }
            requestedCenter = requested;
            *actual = requested;
            if (backend == BackendKind::File) { replayCenterSet = true; return 0; }
            if (backend == BackendKind::Virtual) return 0;
            if (backend == BackendKind::Uhd) return uhdRunning
                ? (uhd.retune(requested, &lastError) ? 0 : -1) : 0;
            if (backend == BackendKind::HackRf) {
                const int result = setHackRfCenterFrequencySafely(hackrf, static_cast<std::uint64_t>(requested));
                if (result != 0) lastError = hackRfLastErrorMessage();
                return result;
            }
            if (backend == BackendKind::RtlNative) {
                const int result = setRtlSdrCenterFrequencySafely(rtlNative, static_cast<quint32>(requested));
                if (result != 0) lastError = QStringLiteral("RTL-SDR tuning error %1").arg(result);
                return result;
            }
            if (backend == BackendKind::RtlTcp) return rtlSocket.state() == QAbstractSocket::ConnectedState
                ? sendRtlCommand(0x01, static_cast<quint32>(requested)) : 0;
            const int result = setSoapySdrCenterFrequencySafely(soapy, requested, channel);
            if (result != 0) lastError = soapySdrLastErrorMessage();
            return result;
        }
        return fobos.frequency(requested, actual);
    }
    int lna(unsigned int gain) {
        if (backend != BackendKind::Fobos) return gain == 0 ? 0 : -1;
        return fobos.lna(gain);
    }
    int vga(unsigned int gain) {
        if (backend != BackendKind::Fobos) return gain == 0 ? 0 : -1;
        return fobos.vga(gain);
    }
    int start(unsigned int count) {
        readCapacity = count;
        if (backend == BackendKind::Virtual)
            return virtualSource.start(virtualEndpoint, static_cast<quint32>(std::llround(requestedRate)),
                static_cast<qint64>(std::llround(requestedCenter)), virtualQueueBytes, &lastError) ? 0 : -1;
        if (backend == BackendKind::File) {
            if (!replayFile.seek(0)) { lastError = replayFile.errorString(); return -1; }
            replaySamples = 0;
            replayFinished = false;
            replayRunning = true;
            replayClock.start();
            return 0;
        }
        if (backend == BackendKind::RtlNative) {
            if (rtlNativeRunning) return 0;
            int result = setRtlSdrTunerGainModeSafely(rtlNative, gainDb > 0.0 ? 1 : 0);
            if (result == 0 && gainDb > 0.0)
                result = setRtlSdrTunerGainSafely(rtlNative, static_cast<int>(std::lround(gainDb * 10.0)));
            if (result == 0) result = resetRtlSdrBufferSafely(rtlNative);
            if (result != 0) {
                lastError = QStringLiteral("RTL-SDR start error %1").arg(result);
                return result;
            }
            rtlNativeBuffer.resize(static_cast<std::size_t>(count) * 2);
            rtlNativeRunning = true;
            return 0;
        }
        if (backend == BackendKind::HackRf) {
            if (hackrfRunning) return 0;
            const int lna = (std::min)(40, static_cast<int>(gainDb / 8.0) * 8);
            const int vga = (std::min)(62, static_cast<int>((gainDb - lna) / 2.0) * 2);
            int result = setHackRfBandwidthSafely(hackrf,
                recommendedHackRfBandwidth(static_cast<std::uint32_t>(requestedRate)));
            if (result == 0) result = setHackRfLnaGainSafely(hackrf, static_cast<std::uint32_t>(lna));
            if (result == 0) result = setHackRfVgaGainSafely(hackrf, static_cast<std::uint32_t>(vga));
            if (result != 0) { lastError = hackRfLastErrorMessage(); return result; }
            {
                std::lock_guard<std::mutex> lock(hackrfMutex);
                hackrfRing.assign(static_cast<std::size_t>(count) * 64, 0);
                hackrfRead = hackrfWrite = hackrfAvailable = 0;
                hackrfDropped.store(0, std::memory_order_relaxed);
                hackrfRunning = true;
            }
            result = startHackRfRxSafely(hackrf, &Device::hackrfCallback, this);
            if (result != 0) {
                std::lock_guard<std::mutex> lock(hackrfMutex);
                hackrfRunning = false;
                lastError = hackRfLastErrorMessage();
            }
            return result;
        }
        if (backend == BackendKind::RtlTcp) {
            if (rtlSocket.state() == QAbstractSocket::ConnectedState) return 0;
            rtlSocket.connectToHost(rtlHost, rtlPort);
            if (!rtlSocket.waitForConnected(3000)) {
                lastError = rtlSocket.errorString();
                return -1;
            }
            while (rtlSocket.bytesAvailable() < 12 && rtlSocket.waitForReadyRead(1000)) {}
            if (rtlSocket.bytesAvailable() < 12 || !rtlSocket.peek(4).startsWith("RTL0")) {
                lastError = QStringLiteral("rtl_tcp did not send its 12-byte RTL0 header");
                stop();
                return -1;
            }
            rtlSocket.read(12);
            if (sendRtlCommand(0x01, static_cast<quint32>(requestedCenter)) != 0 ||
                sendRtlCommand(0x02, static_cast<quint32>(requestedRate)) != 0 ||
                sendRtlCommand(0x03, gainDb > 0.0 ? 1U : 0U) != 0 ||
                (gainDb > 0.0 && sendRtlCommand(0x04, static_cast<quint32>(gainDb * 10)) != 0)) {
                stop();
                return -1;
            }
            rtlBuffer.clear();
            return 0;
        }
        if (backend == BackendKind::Soapy) {
            if (soapyStream) return 0;
            if (gainDb > 0.0 && setSoapySdrGainSafely(soapy, gainDb, channel) != 0) {
                lastError = soapySdrLastErrorMessage();
                return -1;
            }
            soapyStream = setupSoapySdrRxStreamSafely(soapy, channel);
            if (!soapyStream) { lastError = soapySdrLastErrorMessage(); return -1; }
            if (activateSoapySdrStreamSafely(soapy, soapyStream) == 0) return 0;
            lastError = soapySdrLastErrorMessage();
            closeSoapySdrStreamSafely(soapy, soapyStream);
            soapyStream = nullptr;
            return -1;
        }
        if (backend == BackendKind::Uhd) {
            uhdRunning = uhd.open(static_cast<int>(index), channel, requestedRate,
                                  requestedCenter, gainDb, antenna, fpgaImage, &lastError);
            return uhdRunning ? 0 : -1;
        }
        const int result = fobos.startSync(count);
        fobosRunning = result == 0;
        return result;
    }
    int read(float *samples, unsigned int *count) {
        if (backend == BackendKind::Virtual) {
            *count = virtualSource.read(samples, readCapacity);
            if (!*count) QThread::msleep(2);
            return 0;
        }
        if (backend == BackendKind::File) {
            if (!replayRunning) return -1;
            const qint64 bytes = replayFile.read(reinterpret_cast<char *>(samples),
                                                   static_cast<qint64>(readCapacity) * 8);
            if (bytes < 0 || bytes % 8 != 0) {
                lastError = replayFile.errorString();
                return -1;
            }
            *count = static_cast<unsigned int>(bytes / 8);
            replaySamples += *count;
            replayFinished = replayFile.atEnd();
            const qint64 targetMs = static_cast<qint64>(replaySamples * 1000.0 / requestedRate);
            while (replayClock.elapsed() + 2 < targetMs)
                QThread::msleep(static_cast<unsigned long>((std::min)(20LL, targetMs - replayClock.elapsed())));
            return 0;
        }
        if (backend == BackendKind::RtlNative) {
            if (!rtlNativeRunning) return -1;
            int bytesRead = 0;
            const int result = readRtlSdrSyncSafely(rtlNative, rtlNativeBuffer.data(),
                                                      static_cast<int>(rtlNativeBuffer.size()), &bytesRead);
            if (result != 0) {
                lastError = QStringLiteral("RTL-SDR USB read error %1").arg(result);
                return result;
            }
            const int floatCount = (std::max)(0, bytesRead & ~1);
            for (int i = 0; i < floatCount; ++i)
                samples[i] = (static_cast<float>(rtlNativeBuffer[static_cast<std::size_t>(i)]) - 127.5f) / 127.5f;
            *count = static_cast<unsigned int>(floatCount / 2);
            return 0;
        }
        if (backend == BackendKind::HackRf) {
            std::unique_lock<std::mutex> lock(hackrfMutex);
            hackrfReady.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return hackrfAvailable >= 2 || !hackrfRunning;
            });
            if (hackrfAvailable < 2) {
                *count = 0;
                const bool running = hackrfRunning;
                lock.unlock();
                if (running && isHackRfStreamingSafely(hackrf)) return 0;
                lastError = QStringLiteral("HackRF RX stream stopped");
                return -1;
            }
            const std::size_t floats = (std::min)(hackrfAvailable / 2,
                static_cast<std::size_t>(readCapacity)) * 2;
            const std::size_t first = (std::min)(floats, hackrfRing.size() - hackrfRead);
            for (std::size_t i = 0; i < first; ++i)
                samples[i] = static_cast<float>(hackrfRing[hackrfRead + i]) / 128.0f;
            for (std::size_t i = first; i < floats; ++i)
                samples[i] = static_cast<float>(hackrfRing[i - first]) / 128.0f;
            hackrfRead = (hackrfRead + floats) % hackrfRing.size();
            hackrfAvailable -= floats;
            *count = static_cast<unsigned int>(floats / 2);
            return 0;
        }
        if (backend == BackendKind::RtlTcp) {
            if (rtlSocket.state() != QAbstractSocket::ConnectedState) {
                lastError = QStringLiteral("rtl_tcp disconnected");
                return -1;
            }
            const int targetBytes = static_cast<int>(readCapacity * 2);
            if (rtlBuffer.size() < targetBytes) {
                if (rtlSocket.bytesAvailable() == 0) rtlSocket.waitForReadyRead(200);
                rtlBuffer += rtlSocket.read(targetBytes - rtlBuffer.size());
            }
            if (rtlBuffer.size() < 2) { *count = 0; return 0; }
            const int availableSamples = (std::min)(static_cast<int>(readCapacity), rtlBuffer.size() / 2);
            const auto *raw = reinterpret_cast<const unsigned char *>(rtlBuffer.constData());
            for (int i = 0; i < availableSamples * 2; ++i)
                samples[i] = (static_cast<float>(raw[i]) - 127.5f) / 127.5f;
            rtlBuffer.remove(0, availableSamples * 2);
            *count = static_cast<unsigned int>(availableSamples);
            return 0;
        }
        if (backend == BackendKind::Soapy) {
            const int result = readSoapySdrStreamSafely(soapy, soapyStream, samples, readCapacity, 200000);
            if (result == -1) { *count = 0; return 0; }
            if (result >= 0) { *count = static_cast<unsigned int>(result); return 0; }
            lastError = soapySdrLastErrorMessage();
            return result;
        }
        if (backend == BackendKind::Uhd) {
            const int result = uhd.receive(samples, readCapacity, &lastError);
            if (result >= 0) { *count = static_cast<unsigned int>(result); return 0; }
            return result;
        }
        return fobos.readSync(samples, count);
    }
    int stop() {
        if (backend == BackendKind::Virtual) { virtualSource.stop(); return 0; }
        if (backend == BackendKind::File) {
            replayRunning = false;
            return 0;
        }
        if (backend == BackendKind::RtlNative) {
            rtlNativeRunning = false;
            return 0;
        }
        if (backend == BackendKind::HackRf) {
            {
                std::lock_guard<std::mutex> lock(hackrfMutex);
                if (!hackrfRunning) return 0;
                hackrfRunning = false;
            }
            hackrfReady.notify_all();
            const int result = isHackRfStreamingSafely(hackrf) ? stopHackRfRxSafely(hackrf) : 0;
            if (result != 0) lastError = hackRfLastErrorMessage();
            return result;
        }
        if (backend == BackendKind::RtlTcp) {
            rtlSocket.abort();
            rtlBuffer.clear();
            return 0;
        }
        if (backend == BackendKind::Soapy) {
            if (!soapyStream) return 0;
            const int result = deactivateSoapySdrStreamSafely(soapy, soapyStream);
            closeSoapySdrStreamSafely(soapy, soapyStream);
            soapyStream = nullptr;
            if (result != 0) lastError = soapySdrLastErrorMessage();
            return result;
        }
        if (backend == BackendKind::Uhd) {
            uhd.close();
            uhdRunning = false;
            return 0;
        }
        if (!fobosRunning) return 0;
        const int result = fobos.stopSync();
        if (result == 0) fobosRunning = false;
        return result;
    }
    QString errorText(int code) const {
        return backend == BackendKind::Fobos ? QString::fromLatin1(fobos_rx_error_name(code))
                                              : lastError;
    }
    void setReadCapacity(unsigned int count) { readCapacity = count; }
    quint64 droppedHackRfSamples() const { return hackrfDropped.load(std::memory_order_relaxed); }
private:
    int sendRtlCommand(quint8 command, quint32 parameter) {
        char packet[5] = {static_cast<char>(command),
                          static_cast<char>((parameter >> 24) & 0xff),
                          static_cast<char>((parameter >> 16) & 0xff),
                          static_cast<char>((parameter >> 8) & 0xff),
                          static_cast<char>(parameter & 0xff)};
        if (rtlSocket.write(packet, sizeof(packet)) != sizeof(packet) ||
            !rtlSocket.waitForBytesWritten(1000)) {
            lastError = rtlSocket.errorString();
            return -1;
        }
        return 0;
    }
    unsigned int readCapacity = 65536;
    bool uhdRunning = false;
    bool fobosRunning = false;
    bool rtlNativeRunning = false;
};

QJsonObject receiverError(const Device &device, const char *operation, int code) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("operation"), QString::fromLatin1(operation)},
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), device.errorText(code)}};
}

int listDevices() {
    const int standardCount = FobosReceiverCore::deviceCount(ApiKind::Standard);
    const int agileCount = FobosReceiverCore::deviceCount(ApiKind::Agile);
    printJson({{QStringLiteral("standard"), standardCount},
               {QStringLiteral("agile"), agileCount}});
    for (const ApiKind api : {ApiKind::Standard, ApiKind::Agile}) {
        const int count = api == ApiKind::Agile ? agileCount : standardCount;
        for (int i = 0; i < count; ++i) {
        Device device;
        const int code = device.open(api, static_cast<unsigned int>(i));
        if (code != FOBOS_ERR_OK) { printJson(fobosError("open", code)); continue; }
        char hw[256] = {}, fw[256] = {}, maker[256] = {}, product[256] = {}, serial[256] = {};
        const int info = device.boardInfo(hw, fw, maker, product, serial);
        QJsonObject result{{QStringLiteral("api"), apiName(api)},
                           {QStringLiteral("index"), i}, {QStringLiteral("ok"), info == FOBOS_ERR_OK}};
        if (info == FOBOS_ERR_OK) {
            result.insert(QStringLiteral("product"), QString::fromLocal8Bit(product));
            result.insert(QStringLiteral("hardware"), QString::fromLocal8Bit(hw));
            result.insert(QStringLiteral("firmware"), QString::fromLocal8Bit(fw));
            // Serial is intentionally omitted from standard diagnostic output.
        }
        printJson(result);
        }
    }
    return standardCount < 0 && agileCount < 0 ? 1 : 0;
}

int listRates(int index, ApiKind api) {
    Device device;
    const int code = device.open(api, static_cast<unsigned int>(index));
    if (code != FOBOS_ERR_OK) { printJson(fobosError("open", code)); return 1; }
    double rates[100] = {};
    unsigned int count = 100;
    const int result = device.sampleRates(rates, &count);
    if (result != FOBOS_ERR_OK) { printJson(fobosError("samplerates", result)); return 1; }
    QJsonArray values;
    for (unsigned int i = 0; i < count; ++i) values.append(rates[i]);
    printJson({{QStringLiteral("api"), apiName(api)},
               {QStringLiteral("device"), index}, {QStringLiteral("sampleRatesHz"), values}});
    return 0;
}

int listExternalDevices(BackendKind backend) {
    if (backend == BackendKind::Virtual) {
        printJson({{"backend", "virtual"}, {"index", 0},
                   {"label", "Virtual CF32 IQ listener (local IPC or TCP)"}});
        return 0;
    }
    if (backend == BackendKind::File) {
        printJson({{QStringLiteral("backend"), backendName(backend)},
                   {QStringLiteral("index"), 0},
                   {QStringLiteral("label"), QStringLiteral("CF32LE recording; use --iq-file, --freq and --rate")}});
        return 0;
    }
    if (backend == BackendKind::HackRf) {
        HackRfRuntimeStatus status;
        const auto devices = enumerateHackRfDevices(&status);
        printJson({{QStringLiteral("backend"), backendName(backend)},
                   {QStringLiteral("count"), devices.size()},
                   {QStringLiteral("library"), status.loadedPath},
                   {QStringLiteral("error"), status.errorMessage}});
        for (const auto &device : devices)
            printJson({{QStringLiteral("backend"), backendName(backend)},
                       {QStringLiteral("index"), device.nativeIndex},
                       {QStringLiteral("label"), device.label}});
        return devices.isEmpty() ? 1 : 0;
    }
    if (backend == BackendKind::RtlNative) {
        QString libraryPath, error;
        const bool libraryReady = rtlSdrLibraryAvailable(&libraryPath, &error);
        const auto devices = libraryReady ? enumerateRtlSdrDevices() : QVector<RtlSdrDeviceInfo>{};
        printJson({{QStringLiteral("backend"), backendName(backend)},
                   {QStringLiteral("count"), devices.size()},
                   {QStringLiteral("library"), libraryPath},
                   {QStringLiteral("error"), error}});
        for (const auto &device : devices)
            printJson({{QStringLiteral("backend"), backendName(backend)},
                       {QStringLiteral("index"), device.nativeIndex},
                       {QStringLiteral("label"), device.label}});
        return devices.isEmpty() ? 1 : 0;
    }
    if (backend == BackendKind::RtlTcp) {
        printJson({{QStringLiteral("backend"), backendName(backend)},
                   {QStringLiteral("index"), 0},
                   {QStringLiteral("label"), QStringLiteral("rtl_tcp network endpoint; use --rtl-host and --rtl-port")}});
        return 0;
    }
    if (backend == BackendKind::Soapy) {
        const auto devices = enumerateSoapySdrDevices();
        printJson({{QStringLiteral("backend"), backendName(backend)},
                   {QStringLiteral("count"), devices.size()}});
        for (const auto &device : devices)
            printJson({{QStringLiteral("backend"), backendName(backend)},
                       {QStringLiteral("index"), device.nativeIndex},
                       {QStringLiteral("label"), device.label},
                       {QStringLiteral("driver"), device.driver}});
        return devices.isEmpty() ? 1 : 0;
    }
    QString error;
    const QStringList devices = enumerateUhdDeviceArgs(&error);
    printJson({{QStringLiteral("backend"), backendName(backend)},
               {QStringLiteral("count"), devices.size()},
               {QStringLiteral("error"), error}});
    for (int i = 0; i < devices.size(); ++i)
        printJson({{QStringLiteral("backend"), backendName(backend)},
                   {QStringLiteral("index"), i},
                   {QStringLiteral("deviceArgs"), devices[i]}});
    return devices.isEmpty() ? 1 : 0;
}

int listExternalRates(BackendKind backend, int index) {
    if (backend == BackendKind::Virtual) {
        printJson({{"backend", "virtual"}, {"minimumRateHz", 8000}, {"maximumRateHz", 100000000},
            {"note", "Set the exact publisher sample rate; this receiver does not resample"}});
        return 0;
    }
    if (backend == BackendKind::File)
        return fail(QStringLiteral("Use the sample rate stored with the CF32LE recording"));
    if (backend == BackendKind::HackRf) {
        printJson({{QStringLiteral("backend"), backendName(backend)},
                   {QStringLiteral("device"), index},
                   {QStringLiteral("minimumSampleRateHz"), 2000000},
                   {QStringLiteral("maximumSampleRateHz"), 20000000},
                   {QStringLiteral("note"), QStringLiteral("Choose --rate explicitly; libhackrf validates the actual hardware rate")}});
        return 0;
    }
    if (backend == BackendKind::RtlNative)
        return fail(QStringLiteral("RTL-SDR rates depend on the tuner and driver; choose --rate explicitly"));
    if (backend == BackendKind::RtlTcp)
        return fail(QStringLiteral("rtl_tcp rates depend on the remote tuner; choose --rate explicitly"));
    if (backend == BackendKind::Soapy) {
        const auto capabilities = querySoapySdrDeviceCapabilities(index);
        if (!capabilities.valid) return fail(capabilities.error);
        QJsonArray rates;
        for (double rate : capabilities.rxSampleRatesHz) rates.append(rate);
        printJson({{QStringLiteral("backend"), backendName(backend)},
                   {QStringLiteral("device"), index},
                   {QStringLiteral("sampleRatesHz"), rates}});
        return 0;
    }
    return fail(QStringLiteral("UHD has no fixed rate list in this backend; choose --rate and check the driver's accepted rate"));
}

QJsonObject sendViewRequest(const QString &id, const QJsonObject &request) {
    QLocalSocket socket;
    socket.connectToServer(QStringLiteral("obrii-cli-view-") + id);
    if (!socket.waitForConnected(400))
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("error"), QStringLiteral("View is not running: %1").arg(id)}};
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(1000))
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("error"), QStringLiteral("View did not accept the command")}};
    QElapsedTimer timeout;
    timeout.start();
    while (!socket.canReadLine()) {
        const int remaining = 1500 - static_cast<int>(timeout.elapsed());
        if (remaining <= 0 || !socket.waitForReadyRead(remaining))
            return {{QStringLiteral("ok"), false},
                    {QStringLiteral("error"), QStringLiteral("View did not reply")}};
    }
    return QJsonDocument::fromJson(socket.readLine(8192)).object();
}

int viewCommand(const QStringList &args, const QCommandLineParser &parser) {
    if (args.size() < 2) return fail(QStringLiteral("view requires open KIND, set, status, snapshot PATH or close"));
    const QString action = args[1];
    const QString id = parser.value(QStringLiteral("view-id"));
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,48}$")).match(id).hasMatch())
        return fail(QStringLiteral("--view-id must contain 1..48 letters, digits, '_' or '-'"));
    const auto patch = [&parser]() -> QJsonObject {
        QJsonObject values;
        bool okay = false;
        for (const QString &name : {QStringLiteral("min-hz"), QStringLiteral("max-hz")}) {
            if (!parser.isSet(name)) continue;
            const double value = frequency(parser.value(name), &okay);
            if (!okay) return {{QStringLiteral("invalid"), name}};
            values.insert(name == QStringLiteral("min-hz") ? QStringLiteral("minHz")
                                                          : QStringLiteral("maxHz"), value);
        }
        for (const QString &name : {QStringLiteral("min-db"), QStringLiteral("max-db")}) {
            if (!parser.isSet(name)) continue;
            const double value = parser.value(name).toDouble(&okay);
            if (!okay || !std::isfinite(value)) return {{QStringLiteral("invalid"), name}};
            values.insert(name == QStringLiteral("min-db") ? QStringLiteral("minDb")
                                                          : QStringLiteral("maxDb"), value);
        }
        if (parser.isSet(QStringLiteral("paused"))) values.insert(QStringLiteral("paused"), true);
        if (parser.isSet(QStringLiteral("resume"))) values.insert(QStringLiteral("paused"), false);
        if (parser.isSet(QStringLiteral("host"))) values.insert(QStringLiteral("host"), parser.value(QStringLiteral("host")));
        if (parser.isSet(QStringLiteral("network-port"))) {
            const int port = parser.value(QStringLiteral("network-port")).toInt(&okay);
            if (!okay || port < 1 || port > 65535) return {{QStringLiteral("invalid"), QStringLiteral("network-port")}};
            values.insert(QStringLiteral("port"), port);
        }
        return values;
    };
    if (action == QStringLiteral("open")) {
        if (args.size() != 3) return fail(QStringLiteral("view open requires spectrum, waterfall, both, level or scale"));
        const QString kind = args[2];
        if (!QStringList{QStringLiteral("spectrum"), QStringLiteral("waterfall"),
                         QStringLiteral("both"), QStringLiteral("level"), QStringLiteral("scale")}.contains(kind))
            return fail(QStringLiteral("Unknown view kind"));
        const QJsonObject values = patch();
        if (values.contains(QStringLiteral("invalid")))
            return fail(QStringLiteral("Invalid view option: %1").arg(values.value(QStringLiteral("invalid")).toString()));
        if (values.contains(QStringLiteral("minHz")) != values.contains(QStringLiteral("maxHz")))
            return fail(QStringLiteral("view open needs both --min-hz and --max-hz"));
        if (sendViewRequest(id, {{QStringLiteral("action"), QStringLiteral("status")}})
                .value(QStringLiteral("ok")).toBool())
            return fail(QStringLiteral("View ID is already in use"));
        bool okay = false;
        const int port = parser.value(QStringLiteral("network-port")).toInt(&okay);
        if (!okay || port < 1 || port > 65535) return fail(QStringLiteral("Invalid network port"));
#ifdef _WIN32
        const QString executable = QStringLiteral("ObriiCliView.exe");
#else
        const QString executable = QStringLiteral("ObriiCliView");
#endif
        const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(executable);
        if (!QFile::exists(path)) return fail(QStringLiteral("ObriiCliView is not installed beside ObriiCli"));
        const QStringList launch{QStringLiteral("--id"), id, QStringLiteral("--kind"), kind,
                                 QStringLiteral("--host"), parser.value(QStringLiteral("host")),
                                 QStringLiteral("--port"), QString::number(port)};
        QProcess child;
        child.setProgram(path);
        child.setArguments(launch);
        child.setWorkingDirectory(QCoreApplication::applicationDirPath());
        child.setStandardInputFile(QProcess::nullDevice());
        child.setStandardOutputFile(QProcess::nullDevice());
        child.setStandardErrorFile(QProcess::nullDevice());
        if (!child.startDetached())
            return fail(QStringLiteral("Could not start ObriiCliView"));
        QJsonObject reply;
        for (int i = 0; i < 40; ++i) {
            QThread::msleep(50);
            reply = sendViewRequest(id, {{QStringLiteral("action"), QStringLiteral("status")}});
            if (reply.value(QStringLiteral("ok")).toBool()) break;
        }
        if (!reply.value(QStringLiteral("ok")).toBool()) return fail(QStringLiteral("View did not start; check desktop/display availability"));
        if (!values.isEmpty()) reply = sendViewRequest(id, {{QStringLiteral("action"), QStringLiteral("set")},
                                                       {QStringLiteral("settings"), values}});
        if (!reply.value(QStringLiteral("ok")).toBool())
            sendViewRequest(id, {{QStringLiteral("action"), QStringLiteral("close")}});
        printJson(reply);
        return reply.value(QStringLiteral("ok")).toBool() ? 0 : 1;
    }
    QJsonObject request{{QStringLiteral("action"), action}};
    if (action == QStringLiteral("set")) {
        const QJsonObject values = patch();
        if (values.contains(QStringLiteral("invalid"))) return fail(QStringLiteral("Invalid view option: %1").arg(values.value(QStringLiteral("invalid")).toString()));
        if (values.isEmpty()) return fail(QStringLiteral("view set requires a changed parameter"));
        request.insert(QStringLiteral("settings"), values);
    } else if (action == QStringLiteral("snapshot")) {
        if (args.size() != 3 || args[2].isEmpty()) return fail(QStringLiteral("view snapshot requires a PNG path"));
        request.insert(QStringLiteral("path"), QFileInfo(args[2]).absoluteFilePath());
    } else if (action != QStringLiteral("status") && action != QStringLiteral("close"))
        return fail(QStringLiteral("view requires open KIND, set, status, snapshot PATH or close"));
    const QJsonObject reply = sendViewRequest(id, request);
    printJson(reply);
    return reply.value(QStringLiteral("ok")).toBool() ? 0 : 1;
}

QJsonObject digitalControl(CliTextDecoder &decoder, const QJsonObject &request) {
    const QString operation = request.value(QStringLiteral("operation")).toString();
    if (operation == QStringLiteral("status"))
        return {{QStringLiteral("ok"), true}, {QStringLiteral("digital"), decoder.status()}};
    if (operation == QStringLiteral("reset")) {
        decoder.reset();
        return {{QStringLiteral("ok"), true}, {QStringLiteral("digital"), decoder.status()}};
    }
    if (!decoder.enabled())
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("error"), QStringLiteral("Start with --decode cw or --decode psk31")}};
    auto settings = decoder.settings();
    const QJsonValue value = request.value(QStringLiteral("value"));
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    const auto badValue = [] {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), QStringLiteral("Invalid digital decoder setting")}};
    };
    if (operation == QStringLiteral("tone") && std::isfinite(number) && number >= 100.0 && number <= 10000.0)
        settings.toneHz = number;
    else if (settings.mode == CliTextDecoder::Mode::Cw && operation == QStringLiteral("wpm") &&
             std::isfinite(number) && number == std::floor(number) && number >= 5 && number <= 60)
        settings.cwWpm = static_cast<int>(number);
    else if (settings.mode == CliTextDecoder::Mode::Cw && operation == QStringLiteral("selectivity") &&
             std::isfinite(number) && number == std::floor(number) && number >= 1 && number <= 10)
        settings.cwSelectivity = static_cast<int>(number);
    else if (settings.mode == CliTextDecoder::Mode::Cw && operation == QStringLiteral("cw-squelch") &&
             std::isfinite(number) && number >= 0.0 && number <= 30.0)
        settings.cwSquelchDb = number;
    else if (settings.mode == CliTextDecoder::Mode::Cw && operation == QStringLiteral("alphabet") &&
             value.isString() && QStringList{QStringLiteral("latin"), QStringLiteral("ukrainian"),
             QStringLiteral("both")}.contains(value.toString()))
        settings.cwAlphabet = value.toString() == QStringLiteral("latin") ? 0 :
                              value.toString() == QStringLiteral("ukrainian") ? 1 : 2;
    else if (settings.mode == CliTextDecoder::Mode::Psk31 && operation == QStringLiteral("psk-squelch") &&
             std::isfinite(number) && number == std::floor(number) && number >= 1 && number <= 100)
        settings.pskSquelch = static_cast<int>(number);
    else if (settings.mode == CliTextDecoder::Mode::Psk31 && operation == QStringLiteral("afc") && value.isBool())
        settings.pskAfc = value.toBool();
    else return badValue();
    decoder.configure(settings);
    decoder.reset();
    return {{QStringLiteral("ok"), true}, {QStringLiteral("digital"), decoder.status()}};
}

QJsonObject filterControl(QString &json, const QJsonObject &request) {
    const QString operation = request.value(QStringLiteral("operation")).toString();
    const auto error = [](const QString &message) {
        return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
    };
    QVector<AudioFilterStage> stages = decodeAudioFilterChain(json);
    if (operation == QStringLiteral("list")) {
    } else if (operation == QStringLiteral("clear")) {
        stages.clear();
    } else if (operation == QStringLiteral("load")) {
        QFile file(request.value(QStringLiteral("path")).toString());
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
            return error(QStringLiteral("Cannot read filter file or it exceeds 64 KiB"));
        const QByteArray bytes = file.readAll();
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isArray() ||
            document.array().size() > 32)
            return error(QStringLiteral("Expected an audio filter JSON array with at most 32 stages"));
        for (const QJsonValue &value : document.array()) {
            if (!value.isObject()) return error(QStringLiteral("Invalid audio filter stage"));
            bool kindOk = false;
            audioFilterKindFromId(value.toObject().value(QStringLiteral("type")).toString(), &kindOk);
            if (!kindOk) return error(QStringLiteral("Unknown audio filter type"));
        }
        bool valid = false;
        stages = decodeAudioFilterChain(QString::fromUtf8(bytes), &valid);
        if (!valid || stages.size() != document.array().size())
            return error(QStringLiteral("Invalid audio filter file"));
    } else if (operation == QStringLiteral("save")) {
        QFile file(request.value(QStringLiteral("path")).toString());
        const QByteArray bytes = encodeAudioFilterChain(stages).toUtf8();
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size())
            return error(QStringLiteral("Cannot create filter file: %1").arg(file.errorString()));
    } else if (operation == QStringLiteral("add")) {
        bool valid = false;
        const AudioFilterKind kind = audioFilterKindFromId(
            request.value(QStringLiteral("type")).toString(), &valid);
        if (!valid || stages.size() >= 32) return error(QStringLiteral("Invalid filter kind or chain is full"));
        stages.push_back(defaultAudioFilterStage(kind));
    } else {
        const int index = request.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= stages.size()) return error(QStringLiteral("Filter index out of range (zero-based)"));
        if (operation == QStringLiteral("remove")) stages.remove(index);
        else if (operation == QStringLiteral("move")) {
            const int target = index + request.value(QStringLiteral("direction")).toInt();
            if (target < 0 || target >= stages.size()) return error(QStringLiteral("Filter cannot move further"));
            stages.swapItemsAt(index, target);
        } else if (operation == QStringLiteral("enable")) {
            stages[index].enabled = request.value(QStringLiteral("enabled")).toBool();
        } else if (operation == QStringLiteral("set")) {
            const QString field = request.value(QStringLiteral("field")).toString();
            const QStringList numericFields{QStringLiteral("frequencyHz"), QStringLiteral("frequency2Hz"),
                QStringLiteral("q"), QStringLiteral("gainDb"), QStringLiteral("thresholdDb"),
                QStringLiteral("ratio"), QStringLiteral("attackMs"), QStringLiteral("releaseMs"),
                QStringLiteral("timeConstantUs"), QStringLiteral("bandwidthHz"), QStringLiteral("amount")};
            if (!numericFields.contains(field) || !request.value(QStringLiteral("value")).isDouble() ||
                !std::isfinite(request.value(QStringLiteral("value")).toDouble()))
                return error(QStringLiteral("Unknown or nonnumeric filter parameter"));
            QJsonArray array = QJsonDocument::fromJson(encodeAudioFilterChain(stages).toUtf8()).array();
            QJsonObject object = array[index].toObject();
            object.insert(field, request.value(QStringLiteral("value")));
            array[index] = object;
            stages = decodeAudioFilterChain(QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
        } else return error(QStringLiteral("Unknown filter operation"));
    }
    if (operation != QStringLiteral("save") && operation != QStringLiteral("list"))
        json = encodeAudioFilterChain(stages);
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("filters"), QJsonDocument::fromJson(encodeAudioFilterChain(stages).toUtf8()).array()}};
}

int remoteCommand(const QString &host, quint16 port, const QStringList &words, const QString &moduleProfile = {}) {
    if (words.isEmpty()) return fail(QStringLiteral("control requires commands, audit, request FILE.json, agent, status, metrics, spectrum, trace, traces, science, measure, tone, digital or receiver/audio commands"));
    QJsonObject command{{QStringLiteral("action"), words.front()}};
    bool okay = false;
    if (words.front() == QStringLiteral("request")) {
        if (words.size() != 2) return fail(QStringLiteral("control request FILE.json"));
        QFile file(words[1]);
        QJsonParseError error;
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
            return fail(QStringLiteral("Command JSON must be readable and at most 64 KiB"));
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject() || !document.object().contains("command"))
            return fail(QStringLiteral("Expected {command,args,id?,targetInstanceId?}"));
        command = document.object();
    } else if (words.front() == QStringLiteral("commands") || words.front() == QStringLiteral("audit")) {
        CommandDispatcher parser(QStringLiteral("client"));
        command = parser.parseText(words.join(QLatin1Char(' ')));
        if (command.value("ok") == QJsonValue(false)) return fail(command.value("error").toString());
    } else if (words.front() == QStringLiteral("agent")) {
        if (words.size() < 2 || words.size() > 3) return fail(QStringLiteral("control agent start [PORT]|status|stop|control on|off"));
        command.insert("operation", words[1]);
        if (words[1] == QLatin1String("start")) {
            const int p = words.size() == 3 ? words[2].toInt(&okay) : 0;
            if (words.size() == 3 && (!okay || p < 0 || p > 65535)) return fail(QStringLiteral("Invalid local agent port"));
            command.insert("port", p);
        } else if (words[1] == QLatin1String("control") && words.size() == 3 &&
                   (words[2] == QLatin1String("on") || words[2] == QLatin1String("off")))
            command.insert("enabled", words[2] == QLatin1String("on"));
        else if ((words[1] != QLatin1String("status") && words[1] != QLatin1String("stop")) || words.size() != 2)
            return fail(QStringLiteral("control agent start [PORT]|status|stop|control on|off"));
    } else if (words.front() == QStringLiteral("record")) {
        if (words.size() == 4 && words[1] == QStringLiteral("start")) {
            command.insert("operation", "start"); command.insert("type", words[2]);
            command.insert("path", QFileInfo(words[3]).absoluteFilePath());
        } else if ((words.size() == 2 && words[1] == QStringLiteral("status")) ||
                   ((words.size() == 2 || words.size() == 3) && words[1] == QStringLiteral("stop"))) {
            command.insert("operation", words[1]);
            if (words.size() == 3) command.insert("type", words[2]);
        } else return fail(QStringLiteral("control record start cf32|ci8|channel|audio|spectrum PATH; record stop [TYPE]; record status"));
    } else if (words.front() == QStringLiteral("gnss") || words.front() == QStringLiteral("research")) {
        if (words.size() == 3 && (words[1] == QStringLiteral("start") || words[1] == QStringLiteral("request") ||
                                  words[1] == QStringLiteral("capture"))) {
            command.insert(QStringLiteral("operation"), words[1]);
            command.insert(QStringLiteral("path"), QFileInfo(words[2]).absoluteFilePath());
        } else if (words.size() == 2 && QStringList{"status", "eof", "stop", "reset", "cancel", "capabilities", "clear"}.contains(words[1]))
            command.insert(QStringLiteral("operation"), words[1]);
        else return fail(QStringLiteral("control gnss start FILE.json|status|eof|stop; control research request|capture FILE.json|status|cancel|clear"));
    } else if (words.front() == QStringLiteral("web")) {
        if (words.size() == 2 && (words[1] == QStringLiteral("status") || words[1] == QStringLiteral("stop")))
            command.insert("operation", words[1]);
        else if (words.size() == 4 && words[1] == QStringLiteral("start")) {
            const int port = words[3].toInt(&okay);
            if (!okay || port < 1 || port > 65535) return fail(QStringLiteral("Invalid web port"));
            command.insert("operation", "start"); command.insert("address", words[2]); command.insert("port", port);
        } else return fail(QStringLiteral("control web start IP PORT|status|stop"));
    } else if (words.front() == QStringLiteral("calibration")) {
        if (words.size() == 3 && words[1] == QStringLiteral("request")) {
            command.insert(QStringLiteral("operation"), QStringLiteral("request"));
            command.insert(QStringLiteral("path"), QFileInfo(words[2]).absoluteFilePath());
        } else if (words.size() == 2 && QStringList{"select", "status", "cancel"}.contains(words[1]))
            command.insert(QStringLiteral("operation"), words[1]);
        else return fail(QStringLiteral("control calibration select|status|cancel|request FILE.json"));
    } else if (words.front() == QStringLiteral("preset") || words.front() == QStringLiteral("experiment")) {
        if (words.size() != 3 || words[1] != QStringLiteral("save") ||
            !words[2].endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
            return fail(QStringLiteral("control preset|experiment save FILE.json"));
        command.insert(QStringLiteral("operation"), QStringLiteral("save"));
        command.insert(QStringLiteral("path"), QFileInfo(words[2]).absoluteFilePath());
    } else if (words.front() == QStringLiteral("module")) {
        if (!moduleProfile.isEmpty()) {
            if (words.size() < 3 || words[1] != QStringLiteral("start") || !CliDigitalProfile::supports(words[2]))
                return fail(QStringLiteral("--profile applies only to module start dmr|rtty|ft8|ft4|m17"));
            command.insert(QStringLiteral("path"), QFileInfo(moduleProfile).absoluteFilePath());
        }
        if (words.size() < 2 || words.size() > 4)
            return fail(QStringLiteral("module list|status [NAME]|start NAME [OPTION]|stop NAME|snapshot NAME FILE.png"));
        const QString operation = words[1];
        if (!QStringList{QStringLiteral("list"), QStringLiteral("status"),
                         QStringLiteral("start"), QStringLiteral("stop"),
                         QStringLiteral("snapshot")}.contains(operation))
            return fail(QStringLiteral("Unknown module operation"));
        if ((operation == QStringLiteral("list") && words.size() != 2) ||
            (operation == QStringLiteral("status") && words.size() > 3) ||
            (operation == QStringLiteral("stop") && words.size() != 3) ||
            (operation == QStringLiteral("snapshot") && words.size() != 4) ||
            (operation == QStringLiteral("start") && (words.size() < 3 || words.size() > 4)))
            return fail(QStringLiteral("module list|status [NAME]|start NAME [OPTION]|stop NAME|snapshot NAME FILE.png"));
        command.insert(QStringLiteral("operation"), operation);
        if (words.size() >= 3) command.insert(QStringLiteral("name"), words[2]);
        if (words.size() == 4) {
            if (operation == QStringLiteral("snapshot")) {
                if (!words[3].endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
                    return fail(QStringLiteral("Snapshot filename must end in .png"));
                command.insert(QStringLiteral("path"), QFileInfo(words[3]).absoluteFilePath());
            } else if (operation == QStringLiteral("start") &&
                       words[2] == QStringLiteral("monitor")) {
                command.insert(QStringLiteral("backend"), QFileInfo(words[3]).absoluteFilePath());
            } else command.insert(QStringLiteral("backend"), words[3]);
        }
    } else if (words.front() == QStringLiteral("vfo")) {
        if (words.size() < 2) return fail(QStringLiteral("control vfo requires an operation"));
        const QString operation = words[1];
        command.insert(QStringLiteral("operation"), operation);
        if (operation == QStringLiteral("list")) {
            if (words.size() != 2) return fail(QStringLiteral("control vfo list takes no arguments"));
        } else {
            if (words.size() < 3) return fail(QStringLiteral("VFO ID is required"));
            const int id = words[2].toInt(&okay);
            if (!okay || id < 1 || id > 4) return fail(QStringLiteral("VFO ID must be 1..4"));
            command.insert(QStringLiteral("id"), id);
            if (operation == QStringLiteral("add")) {
                if (words.size() != 6) return fail(QStringLiteral("control vfo add ID FREQ MODE BANDWIDTH"));
                const double hz = frequency(words[3], &okay);
                if (!okay) return fail(QStringLiteral("Invalid VFO frequency"));
                const int mode = audioModeFromName(words[4].toLower());
                if (mode < 0) return fail(QStringLiteral("Invalid VFO mode"));
                const double bandwidth = frequency(words[5], &okay);
                if (!okay) return fail(QStringLiteral("Invalid VFO bandwidth"));
                command.insert(QStringLiteral("frequencyHz"), hz);
                command.insert(QStringLiteral("modulationType"), mode);
                command.insert(QStringLiteral("bandwidthHz"), bandwidth);
            } else if (operation == QStringLiteral("tune") || operation == QStringLiteral("bandwidth")) {
                if (words.size() != 4) return fail(QStringLiteral("control vfo tune|bandwidth ID HZ"));
                const double hz = frequency(words[3], &okay);
                if (!okay) return fail(QStringLiteral("Invalid VFO value"));
                command.insert(operation == QStringLiteral("tune") ? QStringLiteral("frequencyHz") :
                    QStringLiteral("bandwidthHz"), hz);
            } else if (operation == QStringLiteral("demod")) {
                if (words.size() != 4) return fail(QStringLiteral("control vfo demod ID MODE"));
                const int mode = audioModeFromName(words[3].toLower());
                if (mode < 0) return fail(QStringLiteral("Invalid VFO mode"));
                command.insert(QStringLiteral("modulationType"), mode);
            } else if (operation == QStringLiteral("filters")) {
                if (words.size() != 4) return fail(QStringLiteral("control vfo filters ID FILE.json"));
                QString json;
                const QJsonObject result = filterControl(json,
                    {{QStringLiteral("operation"), QStringLiteral("load")},
                     {QStringLiteral("path"), QFileInfo(words[3]).absoluteFilePath()}});
                if (!result.value(QStringLiteral("ok")).toBool())
                    return fail(result.value(QStringLiteral("error")).toString());
                command.insert(QStringLiteral("json"), json);
            } else if ((operation == QStringLiteral("status") || operation == QStringLiteral("remove")) &&
                       words.size() == 3) {
            } else return fail(QStringLiteral("Unknown VFO operation"));
        }
    } else if (words.front() == QStringLiteral("filters")) {
        if (words.size() < 2) return fail(QStringLiteral("control filters requires an operation"));
        const QString operation = words[1];
        command.insert(QStringLiteral("operation"), operation);
        if ((operation == QStringLiteral("list") || operation == QStringLiteral("clear")) && words.size() == 2) {
        } else if ((operation == QStringLiteral("load") || operation == QStringLiteral("save")) && words.size() == 3) {
            command.insert(QStringLiteral("path"), QFileInfo(words[2]).absoluteFilePath());
        } else if (operation == QStringLiteral("add") && words.size() == 3) {
            command.insert(QStringLiteral("type"), words[2]);
        } else if (operation == QStringLiteral("remove") && words.size() == 3 ||
                   (operation == QStringLiteral("move") || operation == QStringLiteral("enable")) && words.size() == 4 ||
                   operation == QStringLiteral("set") && words.size() == 5) {
            const int index = words[2].toInt(&okay);
            if (!okay || index < 0 || index >= 32) return fail(QStringLiteral("Filter index must be 0..31"));
            command.insert(QStringLiteral("index"), index);
            if (operation == QStringLiteral("move")) {
                if (words[3] != QStringLiteral("up") && words[3] != QStringLiteral("down"))
                    return fail(QStringLiteral("Filter move requires up or down"));
                command.insert(QStringLiteral("direction"), words[3] == QStringLiteral("up") ? -1 : 1);
            } else if (operation == QStringLiteral("enable")) {
                if (words[3] != QStringLiteral("on") && words[3] != QStringLiteral("off"))
                    return fail(QStringLiteral("Filter enable requires on or off"));
                command.insert(QStringLiteral("enabled"), words[3] == QStringLiteral("on"));
            } else if (operation == QStringLiteral("set")) {
                const double value = words[4].toDouble(&okay);
                if (!okay || !std::isfinite(value)) return fail(QStringLiteral("Invalid filter value"));
                command.insert(QStringLiteral("field"), words[3]);
                command.insert(QStringLiteral("value"), value);
            }
        } else return fail(QStringLiteral("control filters list|clear|load FILE|save FILE|add TYPE|remove INDEX|move INDEX up|down|enable INDEX on|off|set INDEX FIELD VALUE"));
    } else if (words.front() == QStringLiteral("scan")) {
        if (words.size() < 2 || words.size() > 3)
            return fail(QStringLiteral("control scan load FILE|start|stop|resume|status"));
        const QString operation = words[1];
        if (operation == QStringLiteral("load") && words.size() == 3)
            command.insert(QStringLiteral("path"), QFileInfo(words[2]).absoluteFilePath());
        else if (!QStringList{QStringLiteral("start"), QStringLiteral("stop"),
                 QStringLiteral("resume"), QStringLiteral("status")}.contains(operation) || words.size() != 2)
            return fail(QStringLiteral("control scan load FILE|start|stop|resume|status"));
        command.insert(QStringLiteral("operation"), operation);
    } else if (words.front() == QStringLiteral("trigger")) {
        if (words.size() < 2 || words.size() > 3)
            return fail(QStringLiteral("control trigger load FILE|arm|disarm|status"));
        const QString operation = words[1];
        if (operation == QStringLiteral("load") && words.size() == 3)
            command.insert(QStringLiteral("path"), QFileInfo(words[2]).absoluteFilePath());
        else if (!QStringList{QStringLiteral("arm"), QStringLiteral("disarm"),
                 QStringLiteral("status")}.contains(operation) || words.size() != 2)
            return fail(QStringLiteral("control trigger load FILE|arm|disarm|status"));
        command.insert(QStringLiteral("operation"), operation);
    } else if (words.front() == QStringLiteral("quality")) {
        if (words.size() < 2 || words.size() > 3)
            return fail(QStringLiteral("control quality capture [BLOCKS]|status|cancel"));
        const QString operation = words[1];
        if (operation == QStringLiteral("capture")) {
            const int blocks = words.size() == 3 ? words[2].toInt(&okay) : 20;
            if (words.size() == 3 && !okay || blocks < 1 || blocks > 100)
                return fail(QStringLiteral("Quality capture needs 1..100 blocks"));
            command.insert(QStringLiteral("blocks"), blocks);
        } else if ((operation != QStringLiteral("status") && operation != QStringLiteral("cancel")) ||
                   words.size() != 2)
            return fail(QStringLiteral("control quality capture [BLOCKS]|status|cancel"));
        command.insert(QStringLiteral("operation"), operation);
    } else if (words.front() == QStringLiteral("zero")) {
        if (words.size() < 2) return fail(QStringLiteral("control zero start START_HZ END_HZ [MAX_POINTS]|stop|status|read [OFFSET [LIMIT]]|export FILE.csv"));
        const QString operation = words[1];
        command.insert(QStringLiteral("operation"), operation);
        if (operation == QStringLiteral("start") && (words.size() == 4 || words.size() == 5)) {
            const double start = frequency(words[2], &okay);
            if (!okay) return fail(QStringLiteral("Invalid zero-span start frequency"));
            const double end = frequency(words[3], &okay);
            if (!okay) return fail(QStringLiteral("Invalid zero-span end frequency"));
            command.insert(QStringLiteral("startHz"), start);
            command.insert(QStringLiteral("endHz"), end);
            if (words.size() == 5) {
                const int limit = words[4].toInt(&okay);
                if (!okay || limit < 1 || limit > 100000) return fail(QStringLiteral("Zero-span max points must be 1..100000"));
                command.insert(QStringLiteral("maxPoints"), limit);
            }
        } else if (operation == QStringLiteral("read") && words.size() >= 2 && words.size() <= 4) {
            if (words.size() >= 3) {
                const int offset = words[2].toInt(&okay);
                if (!okay || offset < 0) return fail(QStringLiteral("Invalid zero-span offset"));
                command.insert(QStringLiteral("offset"), offset);
            }
            if (words.size() == 4) {
                const int limit = words[3].toInt(&okay);
                if (!okay || limit < 1 || limit > 1000) return fail(QStringLiteral("Zero-span read limit must be 1..1000"));
                command.insert(QStringLiteral("limit"), limit);
            }
        } else if (operation == QStringLiteral("export") && words.size() == 3) {
            if (!words[2].endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive))
                return fail(QStringLiteral("Zero-span export path must end in .csv"));
            command.insert(QStringLiteral("path"), QFileInfo(words[2]).absoluteFilePath());
        } else if ((operation != QStringLiteral("status") && operation != QStringLiteral("stop")) || words.size() != 2)
            return fail(QStringLiteral("control zero start START_HZ END_HZ [MAX_POINTS]|stop|status|read [OFFSET [LIMIT]]|export FILE.csv"));
    } else if (words.front() == QStringLiteral("multisdr")) {
        const QString usage = QStringLiteral("control multisdr devices|status|mosaic|add ID BACKEND INDEX FREQ RATE [FFT]|start ID|stop ID|remove ID|tune ID FREQ|spectrum ID");
        if (words.size() < 2) return fail(usage);
        const QString operation = words[1];
        command.insert(QStringLiteral("operation"), operation);
        if (QStringList{QStringLiteral("devices"), QStringLiteral("status"),
                        QStringLiteral("mosaic")}.contains(operation)) {
            if (words.size() != 2) return fail(usage);
        } else {
            if (words.size() < 3) return fail(usage);
            const int id = words[2].toInt(&okay);
            if (!okay || id < 2 || id > 8) return fail(QStringLiteral("Multi-SDR ID must be RX2..RX8"));
            command.insert(QStringLiteral("id"), id);
            if (operation == QStringLiteral("add")) {
                if (words.size() != 7 && words.size() != 8) return fail(usage);
                const int index = words[4].toInt(&okay);
                if (!okay) return fail(QStringLiteral("Invalid receiver index"));
                command.insert(QStringLiteral("backend"), words[3].toLower());
                command.insert(QStringLiteral("deviceIndex"), index);
                const double center = frequency(words[5], &okay);
                if (!okay) return fail(QStringLiteral("Invalid center frequency"));
                command.insert(QStringLiteral("centerHz"), center);
                const double rate = frequency(words[6], &okay);
                if (!okay) return fail(QStringLiteral("Invalid sample rate"));
                command.insert(QStringLiteral("sampleRateHz"), rate);
                if (words.size() == 8) {
                    const int fft = words[7].toInt(&okay);
                    if (!okay) return fail(QStringLiteral("Invalid FFT length"));
                    command.insert(QStringLiteral("fftLength"), fft);
                }
            } else if (operation == QStringLiteral("tune")) {
                if (words.size() != 4) return fail(usage);
                const double center = frequency(words[3], &okay);
                if (!okay) return fail(QStringLiteral("Invalid center frequency"));
                command.insert(QStringLiteral("centerHz"), center);
            } else if (!QStringList{QStringLiteral("start"), QStringLiteral("stop"),
                                    QStringLiteral("remove"), QStringLiteral("spectrum")}.contains(operation) ||
                       words.size() != 3) return fail(usage);
        }
    } else if (words.front() == QStringLiteral("graph")) {
        const QString usage = QStringLiteral("control graph load FILE.json|save FILE.json|start|stop|status|blocks|trace FFT_ID|set ID FIELD VALUE|record start ID DIR|record stop ID|record status");
        if (words.size() < 2 || words.size() > 5) return fail(usage);
        const QString operation = words[1];
        command.insert(QStringLiteral("operation"), operation);
        if ((operation == QStringLiteral("load") || operation == QStringLiteral("request")) && words.size() == 3)
            command.insert(QStringLiteral("path"), QFileInfo(words[2]).absoluteFilePath());
        else if (operation == QStringLiteral("save") && words.size() == 3) {
            if (!words[2].endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) return fail(usage);
            command.insert(QStringLiteral("path"), QFileInfo(words[2]).absoluteFilePath());
        }
        else if (operation == QStringLiteral("trace") && words.size() == 3)
            command.insert(QStringLiteral("id"), words[2]);
        else if (operation == QStringLiteral("set") && words.size() == 5) {
            command.insert(QStringLiteral("id"), words[2]);
            command.insert(QStringLiteral("field"), words[3]);
            if (words[4] == QStringLiteral("true") || words[4] == QStringLiteral("false"))
                command.insert(QStringLiteral("value"), words[4] == QStringLiteral("true"));
            else {
                double value = words[4].toDouble(&okay);
                if (!okay && words[3].endsWith(QStringLiteral("Hz")))
                    value = frequency(words[4], &okay);
                if (!okay || !std::isfinite(value)) return fail(QStringLiteral("Graph value must be a number or true/false"));
                command.insert(QStringLiteral("value"), value);
            }
        } else if (operation == QStringLiteral("record") && words.size() >= 3) {
            const QString mode = words[2];
            command.insert(QStringLiteral("recordOperation"), mode);
            if (mode == QStringLiteral("start") && words.size() == 5) {
                command.insert(QStringLiteral("id"), words[3]);
                command.insert(QStringLiteral("directory"), QFileInfo(words[4]).absoluteFilePath());
            } else if (mode == QStringLiteral("stop") && words.size() == 4)
                command.insert(QStringLiteral("id"), words[3]);
            else if (mode != QStringLiteral("status") || words.size() != 3) return fail(usage);
        }
        else if ((operation != QStringLiteral("start") && operation != QStringLiteral("stop") &&
                  operation != QStringLiteral("status") && operation != QStringLiteral("blocks")) || words.size() != 2)
            return fail(usage);
    } else if (words.front() == QStringLiteral("tune") || words.front() == QStringLiteral("listen")) {
        if (words.size() != 2) return fail(QStringLiteral("control tune/listen requires a frequency"));
        const double value = frequency(words[1], &okay);
        if (!okay) return fail(QStringLiteral("Invalid frequency"));
        command.insert(QStringLiteral("frequencyHz"), value);
    } else if (words.front() == QStringLiteral("mode")) {
        if (words.size() != 2) return fail(QStringLiteral("control mode requires server, channel, or full"));
        const int mode = words[1] == QStringLiteral("server") ? 0 :
                         words[1] == QStringLiteral("channel") ? 1 :
                         words[1] == QStringLiteral("full") ? 2 : -1;
        if (mode < 0) return fail(QStringLiteral("Mode must be server, channel, or full"));
        command.insert(QStringLiteral("processingMode"), mode);
    } else if (words.front() == QStringLiteral("demod")) {
        if (words.size() != 2) return fail(QStringLiteral("control demod requires am, nfm, wfm, usb, lsb or cw"));
        const int modulation = audioModeFromName(words[1].toLower());
        if (modulation < 0) return fail(QStringLiteral("Unsupported audio mode"));
        command.insert(QStringLiteral("modulationType"), modulation);
    } else if (words.front() == QStringLiteral("bandwidth")) {
        if (words.size() != 2) return fail(QStringLiteral("control bandwidth requires a frequency"));
        const double hz = frequency(words[1], &okay);
        if (!okay) return fail(QStringLiteral("Invalid audio bandwidth"));
        command.insert(QStringLiteral("bandwidthHz"), hz);
    } else if (words.front() == QStringLiteral("audio")) {
        if (words.size() != 2 || (words[1] != QStringLiteral("on") && words[1] != QStringLiteral("off")))
            return fail(QStringLiteral("control audio requires on or off"));
        command.insert(QStringLiteral("enabled"), words[1] == QStringLiteral("on"));
    } else if (words.front() == QStringLiteral("gain")) {
        if (words.size() != 3) return fail(QStringLiteral("control gain requires LNA and VGA"));
        int lna = words[1].toInt(&okay);
        if (!okay || lna < 0 || lna > 2) return fail(QStringLiteral("LNA must be 0..2"));
        int vga = words[2].toInt(&okay);
        if (!okay || vga < 0 || vga > 15) return fail(QStringLiteral("VGA must be 0..15"));
        command.insert(QStringLiteral("lna"), lna);
        command.insert(QStringLiteral("vga"), vga);
    } else if (words.front() == QStringLiteral("measure")) {
        QStringList args = words.mid(1);
        QString source = QStringLiteral("live");
        if (!args.isEmpty() && QStringList{QStringLiteral("live"), QStringLiteral("max"),
            QStringLiteral("min"), QStringLiteral("avg"), QStringLiteral("detected"),
            QStringLiteral("p50"), QStringLiteral("p90"), QStringLiteral("p99")}.contains(args.front()))
            source = args.takeFirst();
        if (args.size() != 0 && args.size() != 2)
            return fail(QStringLiteral("control measure [live|max|min|avg|detected|p50|p90|p99] [START_HZ END_HZ]"));
        command.insert(QStringLiteral("source"), source);
        if (args.size() == 2) {
            const double startHz = frequency(args[0], &okay);
            if (!okay) return fail(QStringLiteral("Invalid measurement start frequency"));
            const double endHz = frequency(args[1], &okay);
            if (!okay || endHz <= startHz) return fail(QStringLiteral("Invalid measurement end frequency"));
            command.insert(QStringLiteral("startHz"), startHz);
            command.insert(QStringLiteral("endHz"), endHz);
        }
    } else if (words.front() == QStringLiteral("digital")) {
        if (words.size() != 2 && words.size() != 3)
            return fail(QStringLiteral("control digital requires status, reset or a setting and value"));
        command.insert(QStringLiteral("operation"), words[1]);
        if (words.size() == 3) {
            if (words[1] == QStringLiteral("alphabet")) command.insert(QStringLiteral("value"), words[2]);
            else if (words[1] == QStringLiteral("afc")) {
                if (words[2] != QStringLiteral("on") && words[2] != QStringLiteral("off"))
                    return fail(QStringLiteral("digital afc requires on or off"));
                command.insert(QStringLiteral("value"), words[2] == QStringLiteral("on"));
            } else {
                const double value = words[1] == QStringLiteral("tone") ? frequency(words[2], &okay)
                    : words[2].toDouble(&okay);
                if (!okay || !std::isfinite(value)) return fail(QStringLiteral("Invalid digital value"));
                command.insert(QStringLiteral("value"), value);
            }
        } else if (words[1] != QStringLiteral("status") && words[1] != QStringLiteral("reset"))
            return fail(QStringLiteral("Digital setting requires a value"));
    } else if (words.front() == QStringLiteral("science")) {
        const QString usage = QStringLiteral("control science status|detector MODE [FRAMES]|vbw HZ|average FRAMES|percentiles on|off|marker list|set N HZ|clear N|pair N N|peak N|export SOURCE FILE.csv");
        if (words.size() < 2) return fail(usage);
        const QString operation = words[1].toLower();
        command.insert(QStringLiteral("operation"), operation);
        if (operation == QStringLiteral("detector") && (words.size() == 3 || words.size() == 4)) {
            const QString mode = words[2].toLower();
            if (!QStringList{QStringLiteral("sample"), QStringLiteral("positive"), QStringLiteral("negative"),
                QStringLiteral("rms"), QStringLiteral("average"), QStringLiteral("median"),
                QStringLiteral("quasi")}.contains(mode)) return fail(usage);
            command.insert(QStringLiteral("mode"), mode);
            bool framesOkay = true;
            const int frames = words.size() == 4 ? words[3].toInt(&framesOkay) : 8;
            if (!framesOkay || frames < 1 || frames > 256) return fail(QStringLiteral("Detector frames must be 1..256"));
            command.insert(QStringLiteral("frames"), frames);
        } else if (operation == QStringLiteral("vbw") && words.size() == 3) {
            const double hz = words[2].toDouble(&okay);
            if (!okay || !std::isfinite(hz) || hz < 0.0 || hz > 10000.0)
                return fail(QStringLiteral("VBW must be 0..10000 Hz (0 disables it)"));
            command.insert(QStringLiteral("hz"), hz);
        } else if (operation == QStringLiteral("average") && words.size() == 3) {
            const int frames = words[2].toInt(&okay);
            if (!okay || frames < 0 || frames > 10000)
                return fail(QStringLiteral("Average frame count must be 0..10000 (0 uses exponential average)"));
            command.insert(QStringLiteral("frames"), frames);
        } else if (operation == QStringLiteral("percentiles") && words.size() == 3 &&
                   (words[2] == QStringLiteral("on") || words[2] == QStringLiteral("off"))) {
            command.insert(QStringLiteral("enabled"), words[2] == QStringLiteral("on"));
        } else if (operation == QStringLiteral("export") && words.size() == 4) {
            const QString source = words[2].toLower();
            if (!QStringList{QStringLiteral("live"), QStringLiteral("max"), QStringLiteral("min"),
                QStringLiteral("avg"), QStringLiteral("detected"), QStringLiteral("p50"),
                QStringLiteral("p90"), QStringLiteral("p99")}.contains(source) ||
                !words[3].endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive)) return fail(usage);
            command.insert(QStringLiteral("source"), source);
            command.insert(QStringLiteral("path"), QFileInfo(words[3]).absoluteFilePath());
        } else if (operation == QStringLiteral("marker") && words.size() >= 3) {
            const QString markerOperation = words[2].toLower();
            command.insert(QStringLiteral("markerOperation"), markerOperation);
            if (markerOperation == QStringLiteral("list") && words.size() == 3) {}
            else if (QStringList{QStringLiteral("set"), QStringLiteral("clear"),
                      QStringLiteral("peak")}.contains(markerOperation) &&
                     words.size() == (markerOperation == QStringLiteral("set") ? 5 : 4)) {
                const int index = words[3].toInt(&okay);
                if (!okay || index < 1 || index > 10) return fail(QStringLiteral("Marker number must be 1..10"));
                command.insert(QStringLiteral("index"), index);
                if (markerOperation == QStringLiteral("set")) {
                    const double hz = frequency(words[4], &okay);
                    if (!okay) return fail(QStringLiteral("Invalid marker frequency"));
                    command.insert(QStringLiteral("frequencyHz"), hz);
                }
            } else if (markerOperation == QStringLiteral("pair") && words.size() == 5) {
                const int first = words[3].toInt(&okay);
                if (!okay || first < 1 || first > 10) return fail(QStringLiteral("First marker must be 1..10"));
                const int second = words[4].toInt(&okay);
                if (!okay || second < 1 || second > 10 || second == first)
                    return fail(QStringLiteral("Second marker must be a different number from 1..10"));
                command.insert(QStringLiteral("first"), first);
                command.insert(QStringLiteral("second"), second);
            } else return fail(usage);
        } else if (operation != QStringLiteral("status") || words.size() != 2) return fail(usage);
    } else if (words.front() == QStringLiteral("trace")) {
        if ((words.size() != 1 && words.size() != 2 && words.size() != 4) ||
            (words.size() >= 2 && !QStringList{QStringLiteral("live"), QStringLiteral("max"),
                QStringLiteral("min"), QStringLiteral("avg"), QStringLiteral("detected"),
                QStringLiteral("p50"), QStringLiteral("p90"), QStringLiteral("p99")}.contains(words[1])))
            return fail(QStringLiteral("control trace accepts live, max, min, avg, detected, p50, p90 or p99"));
        command.insert(QStringLiteral("source"), words.size() >= 2 ? words[1] : QStringLiteral("live"));
        if (words.size() == 4) {
            const int offset = words[2].toInt(&okay);
            if (!okay || offset < 0) return fail(QStringLiteral("Trace offset must be nonnegative"));
            const int count = words[3].toInt(&okay);
            if (!okay || count < 1 || count > 65536) return fail(QStringLiteral("Trace page must contain 1..65536 bins"));
            command.insert(QStringLiteral("offset"), offset);
            command.insert(QStringLiteral("count"), count);
        }
    } else if (words.front() == QStringLiteral("traces")) {
        if (words.size() != 2 || !QStringList{QStringLiteral("start"), QStringLiteral("stop"),
            QStringLiteral("reset"), QStringLiteral("status")}.contains(words[1]))
            return fail(QStringLiteral("control traces requires start, stop, reset or status"));
        command.insert(QStringLiteral("operation"), words[1]);
    } else if (words.front() == QStringLiteral("tone")) {
        QStringList args = words.mid(1);
        QString source = QStringLiteral("live");
        if (!args.isEmpty() && QStringList{QStringLiteral("live"), QStringLiteral("max"),
            QStringLiteral("min"), QStringLiteral("avg"), QStringLiteral("detected"),
            QStringLiteral("p50"), QStringLiteral("p90"), QStringLiteral("p99")}.contains(args.front()))
            source = args.takeFirst();
        if (args.size() > 3) return fail(QStringLiteral("control tone [live|max|min|avg|detected|p50|p90|p99] [FUNDAMENTAL_HZ [HALF_WIDTH_HZ [LAST_HARMONIC]]]"));
        command.insert(QStringLiteral("source"), source);
        if (args.size() >= 1) {
            const double fundamental = frequency(args[0], &okay);
            if (!okay) return fail(QStringLiteral("Invalid tone fundamental frequency"));
            command.insert(QStringLiteral("fundamentalHz"), fundamental);
        }
        if (args.size() >= 2) {
            const double width = frequency(args[1], &okay);
            if (!okay) return fail(QStringLiteral("Invalid tone integration half-width"));
            command.insert(QStringLiteral("halfWidthHz"), width);
        }
        if (args.size() == 3) {
            const int last = args[2].toInt(&okay);
            if (!okay || last < 2 || last > 20) return fail(QStringLiteral("Last harmonic must be 2..20"));
            command.insert(QStringLiteral("lastHarmonic"), last);
        }
    } else if (words.front() != QStringLiteral("status") &&
               words.front() != QStringLiteral("capabilities") &&
               words.front() != QStringLiteral("metrics") &&
               words.front() != QStringLiteral("spectrum") &&
               words.front() != QStringLiteral("start") &&
               words.front() != QStringLiteral("stop") &&
               words.front() != QStringLiteral("quit")) {
        return fail(QStringLiteral("Unknown control command"));
    }
    QTcpSocket socket;
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(3000)) return fail(socket.errorString());
    socket.write(QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(3000))
        return fail(QStringLiteral("Control server did not reply"));
    QElapsedTimer timeout;
    timeout.start();
    while (!socket.canReadLine()) {
        const int remaining = 3000 - static_cast<int>(timeout.elapsed());
        if (remaining <= 0 || !socket.waitForReadyRead(remaining))
            return fail(QStringLiteral("Control server did not reply"));
    }
    QTextStream out(stdout);
    const QByteArray response = socket.readLine(4 * 1024 * 1024).trimmed();
    out << response << Qt::endl;
    return QJsonDocument::fromJson(response).object().value(QStringLiteral("ok")).toBool() ? 0 : 1;
}

struct RunOptions {
    BackendKind backend = BackendKind::Fobos;
    ApiKind api = ApiKind::Standard;
    int inputMode = INPUT_RF;
    int index = 0;
    int lna = 0;
    int vga = 0;
    double gainDb = 0.0;
    int rxChannel = 0;
    QString antenna;
    QString fpgaImage;
    QString iqFile;
    VirtualSignalEndpoint virtualRxEndpoint;
    qint64 virtualRxQueueBytes = 16 * 1024 * 1024;
    QString rtlHost = QStringLiteral("127.0.0.1");
    quint16 rtlPort = 1234;
    int blockSamples = 65536;
    double centerHz = 100e6;
    double sampleRateHz = 8e6;
    double seconds = 0.0;
    QString output;
    QString audioFile;
    QString audioFilterPath;
    CliTextDecoder::Settings decoder;
    QString spectrumFramesFile;
    QString channelIqBase;
    QString fullIq8Base;
    int spectrumIntervalMs = 50;
    bool audioStdout = false;
    bool moduleVoiceStdout = false;
    bool audioPlay = false;
    bool spectrum = false;
    bool progress = false;
    int fftSize = 4096;
    double hzPerBin = 0.0;
    int audioMode = MOD_NFM;
    double audioBandwidthHz = 12500.0;
    double listeningHz = 0.0;
    QString virtualHost;
    QString virtualNodeId;
    quint16 virtualPort = 17661;
    quint16 controlPort = 17675;
    bool serve = false;
    bool networkAudioEnabled = true;
    QString bindAddress = QStringLiteral("127.0.0.1");
    quint16 networkPort = 21090;
    quint16 webPort = 0;
    QString webBind = QStringLiteral("127.0.0.1");
    QJsonObject preset;
};

QJsonArray profileLaunch(const RunOptions &options, double centerHz, double rateHz,
                         double listenHz, double bandwidthHz, int modulation,
                         int lna, int vga, bool networkAudioEnabled,
                         const CliTextDecoder::Settings &decoder, bool spectrumNeeded) {
    QJsonArray args;
    const auto value = [&args](const QString &name, const QString &setting) {
        args.append(name); args.append(setting);
    };
    const auto hz = [](double number) { return QString::number(number, 'f', 6); };
    value(QStringLiteral("--backend"), backendName(options.backend));
    value(QStringLiteral("--device"), QString::number(options.index));
    if (options.backend == BackendKind::Fobos)
        value(QStringLiteral("--api"), apiName(options.api));
    if (options.backend == BackendKind::File)
        value(QStringLiteral("--iq-file"), QFileInfo(options.iqFile).absoluteFilePath());
    if (options.backend == BackendKind::Virtual) {
        value(QStringLiteral("--virtual-rx-transport"), options.virtualRxEndpoint.mode == VirtualSignalTransportMode::Tcp ? QStringLiteral("tcp") : QStringLiteral("local"));
        value(QStringLiteral("--virtual-name"), options.virtualRxEndpoint.localName);
        value(QStringLiteral("--virtual-rx-bind"), options.virtualRxEndpoint.host);
        value(QStringLiteral("--virtual-rx-port"), QString::number(options.virtualRxEndpoint.port));
        value(QStringLiteral("--virtual-rx-queue-mib"), QString::number(options.virtualRxQueueBytes / (1024 * 1024)));
    }
    if (options.backend == BackendKind::RtlTcp) {
        value(QStringLiteral("--rtl-host"), options.rtlHost);
        value(QStringLiteral("--rtl-port"), QString::number(options.rtlPort));
    }
    if (options.backend != BackendKind::Fobos && options.backend != BackendKind::File && options.backend != BackendKind::Virtual) {
        value(QStringLiteral("--gain-db"), hz(options.gainDb));
        if (options.backend == BackendKind::Soapy || options.backend == BackendKind::Uhd) {
            value(QStringLiteral("--rx-channel"), QString::number(options.rxChannel));
            if (!options.antenna.isEmpty()) value(QStringLiteral("--antenna"), options.antenna);
            if (!options.fpgaImage.isEmpty()) value(QStringLiteral("--fpga-image"), options.fpgaImage);
        }
    }
    value(QStringLiteral("--input"), inputName(options.inputMode));
    value(QStringLiteral("--freq"), hz(options.inputMode == INPUT_RF ? centerHz : listenHz));
    value(QStringLiteral("--rate"), hz(rateHz));
    if (options.backend == BackendKind::Fobos) {
        value(QStringLiteral("--lna"), QString::number(lna));
        value(QStringLiteral("--vga"), QString::number(vga));
    }
    value(QStringLiteral("--listen"), hz(listenHz));
    const QString mode = modulation == MOD_AM ? QStringLiteral("am") :
        modulation == MOD_WFM ? QStringLiteral("wfm") :
        modulation == MOD_USB ? QStringLiteral("usb") :
        modulation == MOD_LSB ? QStringLiteral("lsb") :
        modulation == MOD_CW ? QStringLiteral("cw") : QStringLiteral("nfm");
    value(QStringLiteral("--mode"), mode);
    value(QStringLiteral("--bandwidth"), hz(bandwidthHz));
    value(QStringLiteral("--block-samples"), QString::number(options.blockSamples));
    if (spectrumNeeded || options.serve) {
        if (options.hzPerBin > 0.0) value(QStringLiteral("--hz-per-bin"), hz(options.hzPerBin));
        else value(QStringLiteral("--fft"), QString::number(options.fftSize));
    }
    value(QStringLiteral("--spectrum-ms"), QString::number(options.spectrumIntervalMs));
    if (spectrumNeeded) args.append(QStringLiteral("--spectrum"));
    if (options.progress) args.append(QStringLiteral("--progress"));
    if (options.serve) {
        args.append(QStringLiteral("--serve"));
        value(QStringLiteral("--bind"), options.bindAddress);
        value(QStringLiteral("--network-port"), QString::number(options.networkPort));
        if (!networkAudioEnabled) args.append(QStringLiteral("--no-audio"));
    }
    if (options.audioPlay) args.append(QStringLiteral("--audio-play"));
    if (options.webPort) {
        value(QStringLiteral("--web-port"), QString::number(options.webPort));
        value(QStringLiteral("--web-bind"), options.webBind);
    }
    if (options.audioStdout) args.append(QStringLiteral("--audio-stdout"));
    if (options.moduleVoiceStdout) args.append(QStringLiteral("--module-voice-stdout"));
    if (!options.virtualHost.isEmpty()) {
        value(QStringLiteral("--virtual-host"), options.virtualHost);
        value(QStringLiteral("--virtual-port"), QString::number(options.virtualPort));
        if (!options.virtualNodeId.isEmpty())
            value(QStringLiteral("--virtual-node-id"), options.virtualNodeId);
    }
    value(QStringLiteral("--control-port"), QString::number(options.controlPort));
    value(QStringLiteral("--decode"), CliTextDecoder::modeName(decoder.mode));
    value(QStringLiteral("--decode-tone"), hz(decoder.toneHz));
    value(QStringLiteral("--cw-wpm"), QString::number(decoder.cwWpm));
    value(QStringLiteral("--cw-selectivity"), QString::number(decoder.cwSelectivity));
    value(QStringLiteral("--cw-squelch-db"), hz(decoder.cwSquelchDb));
    value(QStringLiteral("--cw-alphabet"), QStringList{QStringLiteral("latin"),
        QStringLiteral("ukrainian"), QStringLiteral("both")}.value(decoder.cwAlphabet));
    value(QStringLiteral("--psk-squelch"), QString::number(decoder.pskSquelch));
    value(QStringLiteral("--psk-afc"), decoder.pskAfc ? QStringLiteral("on") : QStringLiteral("off"));
    return args;
}

int runClient(QCoreApplication &app, const QString &host, quint16 port,
              quint16 controlPort, double seconds, bool audioStdout,
              bool moduleVoiceStdout, bool audioPlay, const QString &audioFile, bool reportSpectrum,
              bool reportProgress, bool startRemote,
              const CliTextDecoder::Settings &decoderSettings, const QString &filterPath) {
    binaryStdout = audioStdout || moduleVoiceStdout;
#ifdef _WIN32
    if (binaryStdout && _setmode(_fileno(stdout), _O_BINARY) < 0)
        return fail(QStringLiteral("Cannot set binary stdout mode"));
#endif
    CliNetworkBridge bridge(&app);
    RemoteAudioPlayer audioPlayer(&app);
    CliPcmRecorder audioRecorder;
    CliTextDecoder textDecoder;
    textDecoder.configure(decoderSettings);
    QString clientFilterJson = QStringLiteral("[]");
    AudioFilterChain clientFilters;
    std::vector<short> filteredSamples;
    if (!filterPath.isEmpty()) {
        const QJsonObject loaded = filterControl(clientFilterJson,
            {{QStringLiteral("operation"), QStringLiteral("load")},
             {QStringLiteral("path"), filterPath}});
        if (!loaded.value(QStringLiteral("ok")).toBool())
            return fail(loaded.value(QStringLiteral("error")).toString());
        clientFilters.configure(clientFilterJson, 48000.0);
    }
    CliModuleManager modules(&app);
    CliVfoManager vfos(0.0, 0.0);
    CliIqQuality quality;
    CliGnssSession gnss(&app);
    gnss.onEvent = [](const QJsonObject &event) { printJson(event); };
    quint64 gnssSequence = 0;
    int remoteProcessingMode = 0;
    modules.onEvent = [](const QJsonObject &event) { printJson(event); };
    modules.onVoicePcm = [&](const QByteArray &pcm) {
        if (moduleVoiceStdout) writeModuleVoice(pcm);
        if (audioPlay) audioPlayer.playNetworkPcmFrame(pcm);
    };
    QString recorderError;
    if (!audioRecorder.open(audioFile, &recorderError)) return fail(recorderError);
    if (audioPlay) audioPlayer.setMaximumQueuedMs(200);
    quint64 audioBytes = 0, audioFrames = 0, spectrumFrames = 0;
    quint64 iqBytes = 0, iqFrames = 0, iqAudioErrors = 0;
    std::unique_ptr<CliAudioOutput> iqAudio;
    std::unique_ptr<CliSpectrumMeter> iqSpectrum;
    std::vector<float> iqFloatSamples;
    QString iqAudioShape;
    QJsonObject latestSpectrum;
    QElapsedTimer elapsed, spectrumReport, reconnect;
    elapsed.start(); spectrumReport.start(); reconnect.start();
    QElapsedTimer progressWindow;
    progressWindow.start();
    quint64 previousIqBytes = 0, previousAudioBytes = 0;
    bool startedRemote = false, quit = false;
    bridge.onStatus = [](const QString &status) {
        printJson({{QStringLiteral("event"), QStringLiteral("networkStatus")},
                   {QStringLiteral("status"), status}});
    };
    bridge.onClientState = [&](const QJsonObject &state) {
        const QJsonObject settings = state.value(QStringLiteral("settings")).toObject();
        remoteProcessingMode = state.value(QStringLiteral("processingMode")).toInt(0);
        if (remoteProcessingMode != 2) gnss.command({{"operation", "stop"}}, 0, 0);
        vfos.setSource(settings.value(QStringLiteral("centerFrequency")).toDouble(),
                       settings.value(QStringLiteral("sampleRate")).toDouble());
        modules.setListeningHz(settings.value(QStringLiteral("listeningFrequency")).toDouble());
        printJson({{QStringLiteral("event"), QStringLiteral("serverState")},
                   {QStringLiteral("centerFrequencyHz"), settings.value(QStringLiteral("centerFrequency"))},
                   {QStringLiteral("listeningFrequencyHz"), settings.value(QStringLiteral("listeningFrequency"))},
                   {QStringLiteral("sampleRateHz"), settings.value(QStringLiteral("sampleRate"))},
                   {QStringLiteral("processingMode"), state.value(QStringLiteral("processingMode"))},
                   {QStringLiteral("canControl"), bridge.clientHasControl()}});
        if (startRemote && !startedRemote && bridge.clientHasControl()) {
            startedRemote = bridge.sendClientControl(QStringLiteral("start"));
        }
    };
    bridge.onClientSpectrum = [&](const QJsonObject &frame) {
        ++spectrumFrames;
        const QJsonArray frequencies = frame.value(QStringLiteral("frequencies")).toArray();
        const QJsonArray magnitudes = frame.value(QStringLiteral("magnitudes")).toArray();
        const int count = (std::min)(frequencies.size(), magnitudes.size());
        if (count <= 0) return;
        if (modules.needsSpectrum() && count > 1) {
            std::vector<float> levels;
            levels.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i)
                levels.push_back(static_cast<float>(magnitudes[i].toDouble(-200.0)));
            const double firstHz = frequencies.first().toDouble();
            const double lastHz = frequencies[count - 1].toDouble();
            const double rateHz = (lastHz - firstHz) * count / (count - 1);
            modules.submitSpectrum(levels, (firstHz + lastHz) * 0.5, rateHz);
        }
        int peak = 0;
        for (int i = 1; i < count; ++i)
            if (magnitudes[i].toDouble(-160.0) > magnitudes[peak].toDouble(-160.0)) peak = i;
        latestSpectrum = {{QStringLiteral("centerFrequencyHz"), frame.value(QStringLiteral("centerFrequency"))},
                          {QStringLiteral("peakFrequencyHz"), frequencies[peak]},
                          {QStringLiteral("peakDbfs"), magnitudes[peak]},
                          {QStringLiteral("bins"), count}};
        if (reportSpectrum && spectrumReport.elapsed() >= 1000) {
            QJsonObject line = latestSpectrum;
            line.insert(QStringLiteral("event"), QStringLiteral("remoteSpectrum"));
            printJson(line);
            spectrumReport.restart();
        }
    };
    const auto outputPcm = [&](const QByteArray &inputPcm) {
        QByteArray pcm = inputPcm;
        if (pcm.isEmpty()) return;
        if (!clientFilters.empty() && pcm.size() % 2 == 0) {
            filteredSamples.resize(static_cast<std::size_t>(pcm.size() / 2));
            std::memcpy(filteredSamples.data(), pcm.constData(), static_cast<std::size_t>(pcm.size()));
            clientFilters.process(filteredSamples);
            std::memcpy(pcm.data(), filteredSamples.data(), static_cast<std::size_t>(pcm.size()));
        }
        textDecoder.process(pcm, [](const QJsonObject &line) { printJson(line); });
        modules.submitPcm(pcm);
        ++audioFrames;
        audioBytes += static_cast<quint64>(pcm.size());
        if (audioPlay && !modules.needsChannelIq()) audioPlayer.playNetworkPcmFrame(pcm);
        QString error;
        if (!audioRecorder.append(pcm, &error)) {
            fail(error);
            quit = true;
        }
        if (audioStdout &&
            (std::fwrite(pcm.constData(), 1, static_cast<std::size_t>(pcm.size()), stdout) !=
                 static_cast<std::size_t>(pcm.size()) || std::fflush(stdout) != 0))
            quit = true;
    };
    bridge.onClientAudio = outputPcm;
    bridge.onClientIq = [&](const QJsonObject &frame, const QByteArray &iq) {
        ++iqFrames;
        iqBytes += static_cast<quint64>(iq.size());
        const bool needDemod = audioPlay || audioStdout || !audioFile.isEmpty() ||
                               textDecoder.enabled() || modules.needsAudio() || modules.needsChannelIq();
        const bool needIq = needDemod || vfos.hasListeners() || quality.pending() || gnss.ready();
        if (iq.isEmpty() || (!needIq &&
            (!reportSpectrum || frame.value(QStringLiteral("sampleFormat")).toString() !=
                QStringLiteral("iq_s8_interleaved") || spectrumReport.elapsed() < 1000))) return;
        const QString format = frame.value(QStringLiteral("sampleFormat")).toString();
        const bool channelized = format == QStringLiteral("channel_iq_s16le");
        const bool full = format == QStringLiteral("iq_s8_interleaved");
        const int bytesPerSample = channelized ? 4 : 2;
        const double rate = frame.value(QStringLiteral("sampleRate")).toDouble();
        const double center = frame.value(QStringLiteral("centerFrequency")).toDouble();
        const double listen = frame.value(QStringLiteral("listeningFrequency")).toDouble(center);
        const double bandwidth = frame.value(QStringLiteral("bandwidth")).toDouble();
        const int modulation = frame.value(QStringLiteral("modulationType")).toInt();
        if ((!channelized && !full) || iq.size() % bytesPerSample != 0 ||
            !std::isfinite(rate) || rate < 48000.0 || !std::isfinite(center) ||
            !std::isfinite(listen) || !std::isfinite(bandwidth) || bandwidth <= 0.0) {
            ++iqAudioErrors;
            return;
        }
        const QString shape = QStringLiteral("%1:%2:%3:%4:%5:%6")
            .arg(format).arg(rate, 0, 'f', 0).arg(center, 0, 'f', 0)
            .arg(listen, 0, 'f', 0).arg(bandwidth, 0, 'f', 0).arg(modulation);
        if (needDemod) {
            if (!iqAudio || shape != iqAudioShape) {
                iqAudio = std::make_unique<CliAudioOutput>();
                QString error;
                if (!iqAudio->open({}, false, INPUT_RF,
                                   modulation == MOD_DMR ? MOD_NFM : modulation, bandwidth, rate,
                                   center, listen, false, &error, outputPcm)) {
                    ++iqAudioErrors;
                    iqAudio.reset();
                    iqAudioShape.clear();
                    return;
                }
                iqAudio->setChannelIqCallback([&](const float *samples, int count, double iqRate) {
                    modules.submitChannelIq(samples, count, iqRate);
                });
                iqAudioShape = shape;
            }
        }
        iqFloatSamples.resize(static_cast<std::size_t>(iq.size() / (channelized ? 2 : 1)));
        if (channelized) {
            const auto *raw = reinterpret_cast<const unsigned char *>(iq.constData());
            for (std::size_t i = 0; i < iqFloatSamples.size(); ++i) {
                const auto value = static_cast<qint16>(raw[i * 2] | (raw[i * 2 + 1] << 8));
                iqFloatSamples[i] = static_cast<float>(value) / 32767.0f;
            }
        } else {
            const auto *raw = reinterpret_cast<const signed char *>(iq.constData());
            for (std::size_t i = 0; i < iqFloatSamples.size(); ++i)
                iqFloatSamples[i] = static_cast<float>(raw[i]) / 127.0f;
        }
        if (full && vfos.hasListeners()) {
            vfos.setSource(center, rate);
            vfos.process(iqFloatSamples.data(), iqFloatSamples.size() / 2);
        }
        if (full) {
            if (gnss.ready()) {
                bool valid = false;
                const auto sequence = frame.value(QStringLiteral("sequence")).toString().toULongLong(&valid);
                const bool gap = !valid || (gnssSequence && sequence != gnssSequence + 1);
                gnss.submitIq(iqFloatSamples.data(), static_cast<int>(iqFloatSamples.size() / 2), rate, center, gap);
                gnssSequence = sequence;
            }
            const QJsonObject result = quality.submit(iqFloatSamples.data(),
                static_cast<int>(iqFloatSamples.size() / 2), rate);
            if (!result.isEmpty()) printJson(result);
        }
        if (iqAudio) {
            QString error;
            const bool decodePcm = audioPlay || audioStdout || !audioFile.isEmpty() ||
                                   textDecoder.enabled() || modules.needsAudio();
            if (!iqAudio->process(iqFloatSamples.data(), iqFloatSamples.size() / 2,
                                  &error, decodePcm)) {
                ++iqAudioErrors;
                iqAudio.reset();
                iqAudioShape.clear();
            }
        }
        if (reportSpectrum && full && spectrumReport.elapsed() >= 1000) {
            if (!iqSpectrum) iqSpectrum = std::make_unique<CliSpectrumMeter>(4096);
            if (iqSpectrum->valid()) {
                QJsonObject line = iqSpectrum->analyze(iqFloatSamples.data(),
                    static_cast<int>(iqFloatSamples.size() / 2), rate, center);
                if (!line.isEmpty()) {
                    line.insert(QStringLiteral("event"), QStringLiteral("remoteIqSpectrum"));
                    latestSpectrum = line;
                    printJson(line);
                    spectrumReport.restart();
                }
            }
        }
    };

    QTcpServer localControl;
    if (!localControl.listen(QHostAddress::LocalHost, controlPort))
        return fail(localControl.errorString());
    CommandDispatcher dispatcher(QStringLiteral("cli-client"));
    CommandControlServer agentApi(&dispatcher);
    const auto executeControl = [&](const QJsonObject &input) -> QJsonObject {
                const QString action = input.value(QStringLiteral("action")).toString();
                QJsonObject reply{{QStringLiteral("ok"), true}};
                if (action == QStringLiteral("agent")) {
                    reply = cliAgentCommand(agentApi, input);
                } else if (action == QStringLiteral("capabilities")) {
                    reply = cliCapabilities();
                } else if (action == QStringLiteral("status") || action == QStringLiteral("metrics")) {
                    reply.insert(QStringLiteral("connected"), bridge.ready());
                    reply.insert(QStringLiteral("network"), bridge.metrics());
                    reply.insert(QStringLiteral("audioFrames"), static_cast<double>(audioFrames));
                    reply.insert(QStringLiteral("audioBytes"), static_cast<double>(audioBytes));
                    reply.insert(QStringLiteral("spectrumFrames"), static_cast<double>(spectrumFrames));
                    reply.insert(QStringLiteral("iqFrames"), static_cast<double>(iqFrames));
                    reply.insert(QStringLiteral("iqBytes"), static_cast<double>(iqBytes));
                    reply.insert(QStringLiteral("iqAudioErrors"), static_cast<double>(iqAudioErrors));
                    reply.insert(QStringLiteral("gnss"), gnss.status());
                    reply.insert(QStringLiteral("uptimeSeconds"), elapsed.elapsed() / 1000.0);
                    const QJsonObject state = bridge.clientState().value(QStringLiteral("settings")).toObject();
                    reply.insert(QStringLiteral("centerFrequencyHz"), state.value(QStringLiteral("centerFrequency")));
                    reply.insert(QStringLiteral("listeningFrequencyHz"), state.value(QStringLiteral("listeningFrequency")));
                } else if (action == QStringLiteral("gnss")) {
                    QJsonObject request = optionalToolRequest(input);
                    if (input.value("operation") == QJsonValue("start")) request.insert("operation", "start");
                    const auto settings = bridge.clientState().value("settings").toObject();
                    if (request.value("operation") == QJsonValue("start") && remoteProcessingMode != 2)
                        reply = {{"ok", false}, {"error", "Live GNSS requires full-band IQ mode, not audio or channel IQ"}};
                    else {
                        if (request.value("operation") == QJsonValue("start")) gnssSequence = 0;
                        reply = gnss.command(request, settings.value("sampleRate").toDouble(), settings.value("centerFrequency").toDouble());
                    }
                } else if (action == QStringLiteral("spectrum")) {
                    if (latestSpectrum.isEmpty())
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("No server spectrum frame yet")}};
                    else reply.insert(QStringLiteral("spectrum"), latestSpectrum);
                } else if (action == QStringLiteral("digital")) {
                    reply = digitalControl(textDecoder, input);
                } else if (action == QStringLiteral("filters")) {
                    const QString previous = clientFilterJson;
                    reply = filterControl(clientFilterJson, input);
                    if (reply.value(QStringLiteral("ok")).toBool() && previous != clientFilterJson)
                        clientFilters.configure(clientFilterJson, 48000.0);
                } else if (action == QStringLiteral("vfo")) {
                    if (remoteProcessingMode != 2 && input.value(QStringLiteral("operation")) != QStringLiteral("list"))
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Additional VFOs require control mode full (full-band IQ)")}};
                    else reply = vfos.command(input);
                } else if (action == QStringLiteral("scan")) {
                    reply = {{QStringLiteral("ok"), false},
                             {QStringLiteral("error"), QStringLiteral("Scanner currently runs on the receiver host CLI")}};
                } else if (action == QStringLiteral("trigger")) {
                    reply = {{QStringLiteral("ok"), false},
                             {QStringLiteral("error"), QStringLiteral("IQ trigger currently runs on the receiver host CLI")}};
                } else if (action == QStringLiteral("zero")) {
                    reply = {{QStringLiteral("ok"), false},
                             {QStringLiteral("error"), QStringLiteral("Zero-span capture currently runs on the receiver host CLI")}};
                } else if (action == QStringLiteral("graph")) {
                    reply = {{QStringLiteral("ok"), false},
                             {QStringLiteral("error"), QStringLiteral("DSP graph currently runs on the receiver host CLI")}};
                } else if (action == QStringLiteral("quality")) {
                    const QString operation = input.value(QStringLiteral("operation")).toString();
                    if (operation == QStringLiteral("capture") && remoteProcessingMode != 2)
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Client IQ quality requires control mode full")}};
                    else if (operation == QStringLiteral("capture"))
                        reply = quality.request(input.value(QStringLiteral("blocks")).toInt());
                    else if (operation == QStringLiteral("cancel")) {
                        quality.cancel();
                        reply.insert(QStringLiteral("quality"), quality.status());
                    } else reply.insert(QStringLiteral("quality"), quality.status());
                } else if (action == QStringLiteral("module")) {
                    reply = modules.command(input);
                } else if (action == QStringLiteral("quit")) {
                    quit = true;
                } else if (action == QStringLiteral("mode")) {
                    if (!bridge.requestClientProcessingMode(input.value(QStringLiteral("processingMode")).toInt(-1)))
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Cannot change remote processing mode")}};
                } else if (action == QStringLiteral("start") || action == QStringLiteral("stop")) {
                    if (!bridge.sendClientControl(action))
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("No remote control permission or connection")}};
                } else if (action == QStringLiteral("tune") || action == QStringLiteral("listen")) {
                    const double hz = input.value(QStringLiteral("frequencyHz")).toDouble();
                    if (!std::isfinite(hz) || hz <= 0.0 || !bridge.clientHasControl())
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Invalid frequency or observer role")}};
                    else {
                        QJsonObject patch;
                        if (action == QStringLiteral("tune")) patch.insert(QStringLiteral("centerFrequency"), hz);
                        patch.insert(QStringLiteral("listeningFrequency"), hz);
                        if (!bridge.sendClientControl(QStringLiteral("setParameters"), patch))
                            reply = {{QStringLiteral("ok"), false},
                                     {QStringLiteral("error"), QStringLiteral("Network command could not be sent")}};
                    }
                } else if (action == QStringLiteral("gain")) {
                    if (!bridge.clientHasControl() || !bridge.sendClientControl(
                        QStringLiteral("setParameters"),
                        {{QStringLiteral("lnaGain"), input.value(QStringLiteral("lna"))},
                         {QStringLiteral("vgaGain"), input.value(QStringLiteral("vga"))}}))
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Network command could not be sent")}};
                } else if (action == QStringLiteral("demod") ||
                           action == QStringLiteral("bandwidth") ||
                           action == QStringLiteral("audio")) {
                    QJsonObject patch;
                    if (action == QStringLiteral("demod")) {
                        const int mode = input.value(QStringLiteral("modulationType")).toInt(-1);
                        patch.insert(QStringLiteral("modulationType"), mode);
                        patch.insert(QStringLiteral("bandwidth"), defaultAudioBandwidth(mode));
                    } else if (action == QStringLiteral("bandwidth")) {
                        patch.insert(QStringLiteral("bandwidth"), input.value(QStringLiteral("bandwidthHz")));
                    } else {
                        patch.insert(QStringLiteral("audioEnabled"), input.value(QStringLiteral("enabled")));
                    }
                    if (!bridge.clientHasControl() ||
                        !bridge.sendClientControl(QStringLiteral("setParameters"), patch))
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Network command could not be sent")}};
                } else {
                    reply = {{QStringLiteral("ok"), false},
                             {QStringLiteral("error"), QStringLiteral("Unknown action")}};
                }
                return reply;
    };
    registerCliCommandHandlers(dispatcher, executeControl, false);
    QObject::connect(&localControl, &QTcpServer::newConnection, &app, [&] {
        while (localControl.hasPendingConnections()) {
            QTcpSocket *socket = localControl.nextPendingConnection();
            socket->setReadBufferSize(65537);
            auto *deadline = new QTimer(socket); deadline->setSingleShot(true); deadline->start(3000);
            QObject::connect(deadline, &QTimer::timeout, socket, &QTcpSocket::abort);
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, deadline] {
                if (socket->bytesAvailable() > 65536) { socket->abort(); return; }
                if (!socket->canReadLine()) return;
                deadline->stop();
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(socket->readLine(65537), &error);
                QJsonObject reply;
                if (error.error != QJsonParseError::NoError || !document.isObject())
                    reply = CommandDispatcher::failure("invalid_json", "Expected a JSON object");
                else {
                    const auto input = document.object();
                    const QJsonObject request = input.contains("command") ? input :
                        QJsonObject{{"command", "cli.legacy"}, {"args", QJsonObject{{"request", input}}}};
                    reply = cliCommandWireReply(dispatcher.execute(request, CommandDispatcher::Context::human()),
                                                !input.contains("command"));
                }
                socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                socket->flush();
                socket->disconnectFromHost();
            });
        }
    });
    bridge.connectClient(host, port);
    std::signal(SIGINT, interruptHandler);
    std::signal(SIGTERM, interruptHandler);
    QTimer timer;
    timer.setInterval(100);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (reportProgress && progressWindow.elapsed() >= 1000) {
            const double windowSeconds = progressWindow.restart() / 1000.0;
            printJson({{QStringLiteral("event"), QStringLiteral("clientProgress")},
                       {QStringLiteral("connected"), bridge.ready()},
                       {QStringLiteral("audioFrames"), static_cast<double>(audioFrames)},
                       {QStringLiteral("iqFrames"), static_cast<double>(iqFrames)},
                       {QStringLiteral("iqAudioErrors"), static_cast<double>(iqAudioErrors)},
                       {QStringLiteral("iqBytesPerSecond"), windowSeconds > 0.0
                            ? (iqBytes - previousIqBytes) / windowSeconds : 0.0},
                       {QStringLiteral("audioBytesPerSecond"), windowSeconds > 0.0
                            ? (audioBytes - previousAudioBytes) / windowSeconds : 0.0},
                       {QStringLiteral("network"), bridge.metrics()}});
            previousIqBytes = iqBytes;
            previousAudioBytes = audioBytes;
        }
        if (quit || interrupted || (seconds > 0.0 && elapsed.elapsed() >= seconds * 1000.0))
            app.quit();
        if (!bridge.ready() && reconnect.elapsed() >= 3000) {
            bridge.connectClient(host, port);
            reconnect.restart();
            startedRemote = false;
        }
    });
    timer.start();
    const int result = app.exec();
    audioPlayer.stop();
    if (!audioRecorder.close(&recorderError)) return fail(recorderError);
    printJson({{QStringLiteral("event"), QStringLiteral("clientStopped")},
               {QStringLiteral("audioFrames"), static_cast<double>(audioFrames)},
               {QStringLiteral("spectrumFrames"), static_cast<double>(spectrumFrames)},
               {QStringLiteral("iqFrames"), static_cast<double>(iqFrames)}});
    return result;
}

int runReceiver(QCoreApplication &app, const RunOptions &options) {
    if (!options.spectrumFramesFile.isEmpty() && options.fftSize > 2097152)
        return fail(QStringLiteral("Spectrum file format supports at most 2097152 exact bins; lower FFT or record full IQ instead"));
    if (QSysInfo::ByteOrder != QSysInfo::LittleEndian)
        return fail(QStringLiteral("CF32LE output requires a little-endian host"));
    if (options.inputMode != INPUT_RF && !options.virtualHost.isEmpty())
        return fail(QStringLiteral("Direct-sampling HF channels cannot use the complex-IQ virtual receiver yet"));
    if (options.inputMode != INPUT_RF && options.spectrum)
        return fail(QStringLiteral("CLI spectrum diagnostics currently support the RF input only"));
    if (options.inputMode != INPUT_RF && options.serve)
        return fail(QStringLiteral("GUI-compatible CLI server currently supports the RF input only"));
    if (options.backend != BackendKind::Fobos && options.inputMode != INPUT_RF)
        return fail(QStringLiteral("External CLI backends currently support RF input only"));
    Device device(options.backend);
    device.virtualEndpoint = options.virtualRxEndpoint;
    device.virtualQueueBytes = options.virtualRxQueueBytes;
    device.channel = options.rxChannel;
    device.gainDb = options.gainDb;
    device.antenna = options.antenna;
    device.fpgaImage = options.fpgaImage;
    device.replayPath = options.iqFile;
    device.rtlHost = options.rtlHost;
    device.rtlPort = options.rtlPort;
    int code = device.open(options.api, static_cast<unsigned int>(options.index));
    if (code != FOBOS_ERR_OK) { printJson(receiverError(device, "open", code)); return 1; }
    code = device.directSampling(options.inputMode == INPUT_RF ? 0U : 1U);
    if (code != FOBOS_ERR_OK) { printJson(receiverError(device, "input", code)); return 1; }
    double actualRate = 0.0, actualCenter = 0.0;
    code = device.sampleRate(options.sampleRateHz, &actualRate);
    if (code != FOBOS_ERR_OK) { printJson(receiverError(device, "sample_rate", code)); return 1; }
    if (options.inputMode == INPUT_RF) {
        code = device.frequency(options.centerHz, &actualCenter);
        if (code != FOBOS_ERR_OK) { printJson(receiverError(device, "frequency", code)); return 1; }
    } else if (options.centerHz >= actualRate / 2) {
        return fail(QStringLiteral("HF listening frequency must be below half the sample rate"));
    }
    code = device.lna(static_cast<unsigned int>(options.lna));
    if (code != FOBOS_ERR_OK) { printJson(receiverError(device, "lna", code)); return 1; }
    code = device.vga(static_cast<unsigned int>(options.vga));
    if (code != FOBOS_ERR_OK) { printJson(receiverError(device, "vga", code)); return 1; }

    std::unique_ptr<CliNetworkBridge> bridge;
    if (options.serve) bridge = std::make_unique<CliNetworkBridge>(&app);
    RemoteAudioPlayer localAudioPlayer(&app);
    if (options.audioPlay) localAudioPlayer.setMaximumQueuedMs(200);
    int modulation = options.audioMode;
    double audioBandwidthHz = options.audioBandwidthHz;
    double listeningHz = options.listeningHz > 0.0 ? options.listeningHz :
                         options.inputMode == INPUT_RF ? actualCenter : options.centerHz;
    bool networkAudioEnabled = options.networkAudioEnabled;
    QString audioFilterJson = QStringLiteral("[]");
    if (!options.audioFilterPath.isEmpty()) {
        const QJsonObject loaded = filterControl(audioFilterJson,
            {{QStringLiteral("operation"), QStringLiteral("load")},
             {QStringLiteral("path"), options.audioFilterPath}});
        if (!loaded.value(QStringLiteral("ok")).toBool())
            return fail(loaded.value(QStringLiteral("error")).toString());
    }
    CliVfoManager vfos(actualCenter, actualRate);
    CliFrequencyScanner scanner;
    CliIqTrigger trigger;
    CliIqQuality quality;
    CliGnssSession gnss(&app);
    gnss.onEvent = [](const QJsonObject &event) { printJson(event); };
    CliResearchTools research;
    QJsonObject researchCapture;
    CliRuntimeRecorder runtimeRecorder;
    CliZeroSpan zeroSpan;
    CliDspGraph graph;
    CliWebService web(&app);
    const QString primaryBackend = options.backend == BackendKind::Fobos
        ? (options.api == ApiKind::Agile ? QStringLiteral("fobos-agile") : QStringLiteral("fobos-standard"))
        : options.backend == BackendKind::RtlNative ? QStringLiteral("rtl-sdr")
        : options.backend == BackendKind::Soapy ? QStringLiteral("soapy-sdr")
        : backendName(options.backend);
    CliMultiSdr multi(primaryBackend, options.index);
    graph.setAuxiliarySourceLookup([&multi](const DspSourceIdentity &source, double *center, double *rate) {
        return multi.hasSource(source, center, rate);
    });
    CliTextDecoder textDecoder;
    textDecoder.configure(options.decoder);
    CliModuleManager modules(&app);
    modules.setListeningHz(listeningHz);
    modules.onEvent = [](const QJsonObject &event) { printJson(event); };
    modules.onVoicePcm = [&](const QByteArray &pcm) {
        if (options.moduleVoiceStdout) writeModuleVoice(pcm);
        if (options.audioPlay) localAudioPlayer.playNetworkPcmFrame(pcm);
    };
    std::unique_ptr<CliAudioOutput> audio;
    const auto ensureAudio = [&]() -> QString {
        if (audio) return {};
        audio = std::make_unique<CliAudioOutput>();
        QString audioError;
        const bool followCenter = options.inputMode == INPUT_RF && options.listeningHz == 0.0;
        if (!audio->open(options.audioFile, options.audioStdout, options.inputMode, modulation,
                         audioBandwidthHz, actualRate, actualCenter,
                         listeningHz, followCenter, &audioError,
                         [bridgePtr = bridge.get(), &networkAudioEnabled, &textDecoder,
                          &modules, &localAudioPlayer, &runtimeRecorder, play = options.audioPlay](const QByteArray &pcm) {
                             if (runtimeRecorder.needsAudio()) runtimeRecorder.submitAudio(pcm);
                             textDecoder.process(pcm, [](const QJsonObject &line) { printJson(line); });
                             modules.submitPcm(pcm);
                             if (play && !modules.needsChannelIq())
                                 localAudioPlayer.playNetworkPcmFrame(pcm);
                             if (bridgePtr && networkAudioEnabled) bridgePtr->sendAudio(pcm);
                         })) {
            audio.reset();
            return audioError;
        }
        audio->setChannelIqCallback([&modules](const float *samples, int count, double iqRate) {
            modules.submitChannelIq(samples, count, iqRate);
        });
        audio->setFilterChain(audioFilterJson);
        return {};
    };
    if (!options.audioFile.isEmpty() || options.audioStdout || options.audioPlay || options.serve ||
        textDecoder.enabled()) {
        const QString audioError = ensureAudio();
        if (!audioError.isEmpty()) return fail(audioError);
    }
    std::unique_ptr<CliAsyncSpectrum> spectrum;
    CliCalibration calibration;
    if (options.spectrum || options.serve || !options.spectrumFramesFile.isEmpty()) {
        spectrum = std::make_unique<CliAsyncSpectrum>(options.fftSize);
        if (!spectrum->valid()) return fail(QStringLiteral("Cannot create the diagnostic FFT plan"));
    }
    QJsonObject latestSpectrum;

    RadioSettings recordSettings;
    recordSettings.inputMode = options.inputMode;
    recordSettings.sampleRate = actualRate;
    recordSettings.centerFrequency = actualCenter;
    recordSettings.actualFrequency = actualCenter;
    recordSettings.listeningFrequency = listeningHz;
    recordSettings.bandwidth = audioBandwidthHz;
    recordSettings.modulationType = modulation;
    recordSettings.fftLength = options.fftSize;
    recordSettings.lnaGain = options.lna;
    recordSettings.vgaGain = options.vga;
    const QJsonObject recorderMetadata{{QStringLiteral("backend"), backendName(options.backend)},
                                       {QStringLiteral("deviceIndex"), options.index},
                                       {QStringLiteral("lna"), options.lna},
                                       {QStringLiteral("vga"), options.vga},
                                       {QStringLiteral("gainDb"), options.gainDb},
                                       {QStringLiteral("fftLength"), options.fftSize},
                                       {QStringLiteral("bandwidthHz"), audioBandwidthHz}};
    SpectrumFrameRecorder spectrumRecorder;
    SpectrumIqEventRecorder channelRecorder, fullIq8Recorder;
    spectrumRecorder.setExtendedMetadata(recorderMetadata);
    channelRecorder.setExtendedMetadata(recorderMetadata);
    fullIq8Recorder.setExtendedMetadata(recorderMetadata);
    IqChannelizer recordChannelizer;
    std::vector<float> channelIq;
    QByteArray quantizedIq;
    const bool startupRecordingLocked = !options.output.isEmpty() || !options.spectrumFramesFile.isEmpty() ||
        !options.channelIqBase.isEmpty() || !options.fullIq8Base.isEmpty();
    const auto recordingLocked = [&] { return startupRecordingLocked || runtimeRecorder.active(); };
    QString recordError;
    if (!options.spectrumFramesFile.isEmpty() &&
        !spectrumRecorder.start(recordSettings, 100.0, options.fftSize, 0, &recordError,
                                options.spectrumFramesFile)) return fail(recordError);
    if (!options.fullIq8Base.isEmpty() &&
        !fullIq8Recorder.start(options.fullIq8Base, SpectrumIqEventRecorder::Mode::FullIqS8,
                               recordSettings, actualRate, 0, 0, &recordError)) return fail(recordError);

    QFile capture(options.output);
    if (!options.output.isEmpty()) {
        if (!capture.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail(capture.errorString());
        QFile metadata(options.output + QStringLiteral(".json"));
        if (!metadata.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail(metadata.errorString());
        const QJsonObject meta{{QStringLiteral("format"), QStringLiteral("cf32le")},
                               {QStringLiteral("receiver"), backendName(options.backend)},
                               {QStringLiteral("api"), options.backend == BackendKind::Fobos
                                   ? apiName(options.api) : backendName(options.backend)},
                               {QStringLiteral("input"), inputName(options.inputMode)},
                               {QStringLiteral("sampleRate"), actualRate},
                               {QStringLiteral("lna"), options.lna},
                               {QStringLiteral("vga"), options.vga},
                               {QStringLiteral("gainDb"), options.gainDb},
                               {QStringLiteral("centerFrequency"), actualCenter},
                               {QStringLiteral("requestedFrequency"), options.centerHz},
                               {QStringLiteral("listeningFrequency"), options.listeningHz > 0.0 ? options.listeningHz : options.centerHz},
                               {QStringLiteral("startUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
        const QByteArray metadataBytes = QJsonDocument(meta).toJson(QJsonDocument::Indented);
        if (metadata.write(metadataBytes) != metadataBytes.size())
            return fail(metadata.errorString());
    }

    bool stop = false, receiverRunning = false, discontinuity = true, fileAtEof = false;
    std::vector<float> orderedTrace;
    std::vector<float> traceBinIndices;
    SpectrumScienceAnalyzer accumulatedTraces;
    bool tracesEnabled = false;
    quint64 accumulatedFrames = 0;
    QString detectorName = QStringLiteral("sample");
    int detectorFrames = 8, averageFrames = 0;
    double vbwHz = 0.0;
    bool percentilesEnabled = false;
    std::array<double, 10> markerHz;
    markerHz.fill(std::numeric_limits<double>::quiet_NaN());
    int markerFirst = 0, markerSecond = 1;
    int currentLna = options.lna, currentVga = options.vga;
    const auto calibrationKey = [&] {
        ReceiverNoiseBaselineKey key;
        key.receiver.backendId = primaryBackend;
        key.receiver.inputMode = options.inputMode;
        key.receiver.sampleRateHz = actualRate;
        key.receiver.lnaGain = options.backend == BackendKind::Fobos ? currentLna : -1;
        key.receiver.vgaGain = options.backend == BackendKind::Fobos ? currentVga : -1;
        key.receiver.rtlGainTenthsDb = options.backend == BackendKind::RtlNative || options.backend == BackendKind::RtlTcp
            ? static_cast<int>(std::llround(options.gainDb * 10)) : -1;
        key.centerFrequencyHz = actualCenter; key.fftLength = options.fftSize;
        key.fftWindowType = FFT_WINDOW_HANN;
        return key;
    };
    const auto runtimeSettings = [&] {
        RadioSettings settings = recordSettings;
        settings.sampleRate = actualRate; settings.centerFrequency = settings.actualFrequency = actualCenter;
        settings.listeningFrequency = listeningHz; settings.bandwidth = audioBandwidthHz;
        settings.modulationType = modulation; settings.lnaGain = currentLna; settings.vgaGain = currentVga;
        settings.uhdRxGainDb = options.gainDb;
        settings.rtlTunerGainTenthsDb = static_cast<int>(std::lround(options.gainDb * 10));
        settings.deviceIndex = options.index;
        return settings;
    };
    quint64 samples = 0;
    const auto restorePreset = [&]() -> QString {
        if (options.preset.isEmpty()) return {};
        const QJsonObject state = options.preset.value(QStringLiteral("state")).toObject();
        const QJsonArray filters = state.value(QStringLiteral("filters")).toArray();
        const QString requestedFilters = QString::fromUtf8(QJsonDocument(filters).toJson(QJsonDocument::Compact));
        const QVector<AudioFilterStage> parsedFilters = decodeAudioFilterChain(requestedFilters);
        if (parsedFilters.size() != filters.size())
            return QStringLiteral("Preset contains unsupported audio filters");
        audioFilterJson = encodeAudioFilterChain(parsedFilters);
        if (audio) audio->setFilterChain(audioFilterJson);

        const QJsonObject science = state.value(QStringLiteral("science")).toObject();
        const QString mode = science.value(QStringLiteral("detector")).toString();
        const QStringList modes{QStringLiteral("sample"), QStringLiteral("positive"),
            QStringLiteral("negative"), QStringLiteral("rms"), QStringLiteral("average"),
            QStringLiteral("median"), QStringLiteral("quasi")};
        const int frames = science.value(QStringLiteral("detectorFrames")).toInt(-1);
        const int average = science.value(QStringLiteral("averageFrames")).toInt(-1);
        const double vbw = science.value(QStringLiteral("vbwHz")).toDouble(-1.0);
        const QJsonArray markers = science.value(QStringLiteral("markers")).toArray();
        const QJsonArray pair = science.value(QStringLiteral("pair")).toArray();
        if (!modes.contains(mode) || frames < 1 || frames > 256 || average < 0 || average > 10000 ||
            !std::isfinite(vbw) || vbw < 0.0 || vbw > 10000.0 || markers.size() != 10 ||
            pair.size() != 2 || pair[0].toInt() < 1 || pair[0].toInt() > 10 ||
            pair[1].toInt() < 1 || pair[1].toInt() > 10 || pair[0] == pair[1] ||
            !science.value(QStringLiteral("tracesEnabled")).isBool() ||
            !science.value(QStringLiteral("percentilesEnabled")).isBool())
            return QStringLiteral("Invalid science settings in CLI preset");
        const double spanStart = spectrumFirstHz(actualCenter, actualRate, options.fftSize);
        const double spanEnd = actualCenter + actualRate * 0.5;
        for (int i = 0; i < 10; ++i) {
            if (markers[i].isNull()) continue;
            if (!markers[i].isDouble() || !std::isfinite(markers[i].toDouble()) ||
                markers[i].toDouble() < spanStart || markers[i].toDouble() > spanEnd)
                return QStringLiteral("Preset marker is outside the current IQ span");
        }
        detectorName = mode;
        detectorFrames = frames;
        averageFrames = average;
        vbwHz = vbw;
        percentilesEnabled = science.value(QStringLiteral("percentilesEnabled")).toBool();
        tracesEnabled = science.value(QStringLiteral("tracesEnabled")).toBool();
        markerFirst = pair[0].toInt() - 1;
        markerSecond = pair[1].toInt() - 1;
        for (int i = 0; i < 10; ++i)
            if (markers[i].isDouble()) markerHz[static_cast<std::size_t>(i)] = markers[i].toDouble();
        accumulatedTraces.setDetector(modes.indexOf(mode), frames);
        accumulatedTraces.setAverageFrameCount(average);
        accumulatedTraces.setVbwHz(vbw);
        accumulatedTraces.setPercentileTraces(percentilesEnabled, percentilesEnabled, percentilesEnabled);
        if (tracesEnabled) {
            if (!spectrum) return QStringLiteral("Preset traces require --spectrum or --serve");
            accumulatedTraces.setTraceEnabled(true, true, true);
            accumulatedTraces.setAverageTimeSeconds(2.0);
        }

        for (const QJsonValue &entry : state.value(QStringLiteral("vfos")).toArray()) {
            const QJsonObject vfo = entry.toObject();
            QJsonObject request = vfo;
            request.insert(QStringLiteral("operation"), QStringLiteral("add"));
            const QJsonObject result = vfos.command(request);
            if (!result.value(QStringLiteral("ok")).toBool())
                return QStringLiteral("Preset VFO: ") + result.value(QStringLiteral("error")).toString();
            const QJsonArray stages = vfo.value(QStringLiteral("filters")).toArray();
            if (!stages.isEmpty()) {
                const QString json = QString::fromUtf8(QJsonDocument(stages).toJson(QJsonDocument::Compact));
                if (decodeAudioFilterChain(json).size() != stages.size())
                    return QStringLiteral("Preset VFO has unsupported filters");
                const QJsonObject applied = vfos.command({{QStringLiteral("operation"), QStringLiteral("filters")},
                    {QStringLiteral("id"), vfo.value(QStringLiteral("id"))}, {QStringLiteral("json"), json}});
                if (!applied.value(QStringLiteral("ok")).toBool())
                    return QStringLiteral("Preset VFO filters could not be applied");
            }
        }
        for (const QJsonValue &entry : state.value(QStringLiteral("multisdr")).toArray()) {
            QJsonObject request = entry.toObject();
            request.insert(QStringLiteral("operation"), QStringLiteral("add"));
            const QJsonObject result = multi.command(request, actualCenter, actualRate, false);
            if (!result.value(QStringLiteral("ok")).toBool())
                return QStringLiteral("Preset Multi-SDR: ") + result.value(QStringLiteral("error")).toString();
        }
        if (multi.hasSlots() && !spectrum) {
            spectrum = std::make_unique<CliAsyncSpectrum>(options.fftSize);
            if (!spectrum->valid()) return QStringLiteral("Preset Multi-SDR needs a valid FFT plan");
        }
        if (state.value(QStringLiteral("graph")).isObject()) {
            const QJsonObject result = graph.command({{QStringLiteral("operation"), QStringLiteral("restoreDefinition")},
                {QStringLiteral("definition"), state.value(QStringLiteral("graph"))}}, actualCenter, actualRate, false);
            if (!result.value(QStringLiteral("ok")).toBool())
                return QStringLiteral("Preset DSP graph: ") + result.value(QStringLiteral("error")).toString();
        }
        if (state.value(QStringLiteral("scan")).isObject()) {
            const QJsonObject result = scanner.command({{QStringLiteral("operation"), QStringLiteral("restoreConfiguration")},
                {QStringLiteral("configuration"), state.value(QStringLiteral("scan"))}});
            if (!result.value(QStringLiteral("ok")).toBool())
                return QStringLiteral("Preset scanner: ") + result.value(QStringLiteral("error")).toString();
        }
        if (state.value(QStringLiteral("trigger")).isObject()) {
            const QJsonObject result = trigger.command({{QStringLiteral("operation"), QStringLiteral("restoreConfiguration")},
                {QStringLiteral("configuration"), state.value(QStringLiteral("trigger"))}}, actualCenter, actualRate);
            if (!result.value(QStringLiteral("ok")).toBool())
                return QStringLiteral("Preset trigger: ") + result.value(QStringLiteral("error")).toString();
        }
        return {};
    };
    const QString restoreError = restorePreset();
    if (!restoreError.isEmpty()) return fail(restoreError);

    const auto makeProfile = [&](bool withResults) {
        QJsonArray markers;
        for (double hz : markerHz)
            markers.append(std::isfinite(hz) ? QJsonValue(hz) : QJsonValue(QJsonValue::Null));
        QJsonObject science{{QStringLiteral("tracesEnabled"), tracesEnabled},
            {QStringLiteral("detector"), detectorName},
            {QStringLiteral("detectorFrames"), detectorFrames},
            {QStringLiteral("averageFrames"), averageFrames},
            {QStringLiteral("vbwHz"), vbwHz},
            {QStringLiteral("percentilesEnabled"), percentilesEnabled},
            {QStringLiteral("markers"), markers},
            {QStringLiteral("pair"), QJsonArray{markerFirst + 1, markerSecond + 1}}};
        QJsonArray auxiliary;
        const QJsonArray sources = multi.status(actualCenter, actualRate, receiverRunning)
            .value(QStringLiteral("sources")).toArray();
        for (int i = 1; i < sources.size(); ++i) {
            const QJsonObject source = sources[i].toObject();
            auxiliary.append(QJsonObject{{QStringLiteral("id"), source.value(QStringLiteral("id"))},
                {QStringLiteral("backend"), source.value(QStringLiteral("backend"))},
                {QStringLiteral("deviceIndex"), source.value(QStringLiteral("deviceIndex"))},
                {QStringLiteral("centerHz"), source.value(QStringLiteral("centerHz"))},
                {QStringLiteral("sampleRateHz"), source.value(QStringLiteral("sampleRateHz"))},
                {QStringLiteral("fftLength"), source.value(QStringLiteral("fftLength"))}});
        }
        QJsonObject state{{QStringLiteral("science"), science},
            {QStringLiteral("filters"), QJsonDocument::fromJson(audioFilterJson.toUtf8()).array()},
            {QStringLiteral("vfos"), vfos.configuration()},
            {QStringLiteral("multisdr"), auxiliary}};
        if (!scanner.configuration().isEmpty())
            state.insert(QStringLiteral("scan"), scanner.configuration());
        if (!trigger.configuration().isEmpty())
            state.insert(QStringLiteral("trigger"), trigger.configuration());
        if (!graph.definitionSnapshot().isEmpty())
            state.insert(QStringLiteral("graph"), graph.definitionSnapshot());
        QJsonObject source{{QStringLiteral("backend"), backendName(options.backend)},
            {QStringLiteral("deviceIndex"), options.index},
            {QStringLiteral("sampleRateHz"), actualRate},
            {QStringLiteral("centerFrequencyHz"), actualCenter},
            {QStringLiteral("iqReplayable"), options.backend == BackendKind::File}};
        if (options.backend == BackendKind::File) {
            const QFileInfo file(options.iqFile);
            source.insert(QStringLiteral("iqFile"), file.absoluteFilePath());
            source.insert(QStringLiteral("iqBytes"), static_cast<double>(file.size()));
            source.insert(QStringLiteral("iqModifiedUtc"), file.lastModified().toUTC().toString(Qt::ISODateWithMs));
        }
        if (!options.output.isEmpty()) {
            source.insert(QStringLiteral("activeRecordingPath"), QFileInfo(options.output).absoluteFilePath());
            source.insert(QStringLiteral("activeRecordingComplete"), false);
        }
        QJsonObject profile{{QStringLiteral("format"), ProfileFormat},
            {QStringLiteral("version"), 1},
            {QStringLiteral("kind"), withResults ? QStringLiteral("experiment") : QStringLiteral("preset")},
            {QStringLiteral("savedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("launch"), profileLaunch(options, actualCenter, actualRate, listeningHz,
                audioBandwidthHz, modulation, currentLna, currentVga, networkAudioEnabled,
                textDecoder.settings(), options.spectrum || tracesEnabled)},
            {QStringLiteral("state"), state}, {QStringLiteral("source"), source}};
        if (withResults) {
            QJsonObject results{{QStringLiteral("samplesReceived"), static_cast<double>(samples)},
                {QStringLiteral("fftWorker"), spectrum ? spectrum->status() : QJsonObject{}},
                {QStringLiteral("runtimeRecording"), runtimeRecorder.status()},
                {QStringLiteral("spectrum"), latestSpectrum},
                {QStringLiteral("quality"), quality.status()},
                {QStringLiteral("zeroSpan"), zeroSpan.status()},
                {QStringLiteral("scan"), scanner.status()},
                {QStringLiteral("trigger"), trigger.status()},
                {QStringLiteral("modules"), modules.command({{QStringLiteral("operation"), QStringLiteral("list")}})
                    .value(QStringLiteral("modules"))}};
            const int totalZeroPoints = zeroSpan.status().value(QStringLiteral("points")).toInt();
            QJsonArray zeroPoints;
            const int savedZeroPoints = (std::min)(totalZeroPoints, 10000);
            for (int offset = 0; offset < savedZeroPoints; offset += 1000) {
                const QJsonArray page = zeroSpan.command({{QStringLiteral("operation"), QStringLiteral("read")},
                    {QStringLiteral("offset"), offset},
                    {QStringLiteral("limit"), (std::min)(1000, savedZeroPoints - offset)}},
                    actualCenter, actualRate).value(QStringLiteral("points")).toArray();
                for (const QJsonValue &point : page) zeroPoints.append(point);
            }
            results.insert(QStringLiteral("zeroSpanHistory"), QJsonObject{
                {QStringLiteral("totalPoints"), totalZeroPoints},
                {QStringLiteral("savedPoints"), zeroPoints.size()},
                {QStringLiteral("truncated"), totalZeroPoints > savedZeroPoints},
                {QStringLiteral("points"), zeroPoints}});
            QJsonObject traces;
            const auto addTrace = [&](const QString &name, const std::vector<float> &levels) {
                if (levels.empty()) return;
                QJsonArray values;
                for (float level : levels) values.append(level);
                traces.insert(name, QJsonObject{{QStringLiteral("startHz"), spectrumFirstHz(actualCenter, actualRate, options.fftSize)},
                    {QStringLiteral("binHz"), actualRate / levels.size()},
                    {QStringLiteral("levelsDbfs"), values}});
            };
            addTrace(QStringLiteral("live"), orderedTrace);
            if (tracesEnabled) {
                addTrace(QStringLiteral("max"), accumulatedTraces.maxHoldTrace());
                addTrace(QStringLiteral("min"), accumulatedTraces.minHoldTrace());
                addTrace(QStringLiteral("avg"), accumulatedTraces.averageTrace());
                addTrace(QStringLiteral("detected"), accumulatedTraces.levels());
                if (percentilesEnabled) {
                    addTrace(QStringLiteral("p50"), accumulatedTraces.percentile50Trace());
                    addTrace(QStringLiteral("p90"), accumulatedTraces.percentile90Trace());
                    addTrace(QStringLiteral("p99"), accumulatedTraces.percentile99Trace());
                }
            }
            results.insert(QStringLiteral("traces"), traces);
            profile.insert(QStringLiteral("results"), results);
        }
        return profile;
    };
    const auto traceForSource = [&](const QString &source) -> const std::vector<float> * {
        if (source == QStringLiteral("live")) return &orderedTrace;
        if (!tracesEnabled) return nullptr;
        if (source == QStringLiteral("max")) return &accumulatedTraces.maxHoldTrace();
        if (source == QStringLiteral("min")) return &accumulatedTraces.minHoldTrace();
        if (source == QStringLiteral("avg")) return &accumulatedTraces.averageTrace();
        if (source == QStringLiteral("detected")) return &accumulatedTraces.levels();
        if (source == QStringLiteral("p50")) return &accumulatedTraces.percentile50Trace();
        if (source == QStringLiteral("p90")) return &accumulatedTraces.percentile90Trace();
        if (source == QStringLiteral("p99")) return &accumulatedTraces.percentile99Trace();
        return nullptr;
    };
    quint64 tuneSettlingSamples = 0, discardedTuneSamples = 0;
    quint64 droppedVirtualFrames = 0;
    quint32 sequence = 0;
    QElapsedTimer elapsed; elapsed.start();
    QElapsedTimer rateWindow; rateWindow.start();
    quint64 previousRateSamples = 0;
    double recentReadRateMsps = 0.0;
    QTcpServer control;
    if (!control.listen(QHostAddress::LocalHost, options.controlPort)) return fail(control.errorString());
    QTcpSocket virtualSink;
    qint64 virtualSampleEpochNs = 0;
    qint64 virtualSampleCursor = 0;
    QElapsedTimer reconnect; reconnect.start();
    if (!options.virtualHost.isEmpty()) virtualSink.connectToHost(options.virtualHost, options.virtualPort);
    QObject::connect(&virtualSink, &QTcpSocket::connected, &app, [&] { discontinuity = true; });
    CommandDispatcher dispatcher(QStringLiteral("cli"));
    CommandControlServer agentApi(&dispatcher);
    const auto executeControl = [&](const QJsonObject &input) -> QJsonObject {
                const QString action = input.value(QStringLiteral("action")).toString();
                QJsonObject reply{{QStringLiteral("ok"), true}};
                if (action == QStringLiteral("agent")) {
                    reply = cliAgentCommand(agentApi, input);
                } else if (action == QStringLiteral("capabilities")) {
                    reply = cliCapabilities();
                } else if (action == QStringLiteral("record")) {
                    const auto operation = input.value("operation").toString();
                    const auto type = input.value("type").toString();
                    if (operation == QStringLiteral("start") && (!receiverRunning || scanner.running() || trigger.active()))
                        reply = {{"ok", false}, {"error", "Runtime recording needs running RX without scanning or armed IQ trigger"}};
                    else if (operation == QStringLiteral("start") && type == QStringLiteral("spectrum") && options.fftSize > 2097152)
                        reply = {{"ok", false}, {"error", "Spectrum file format supports at most 2097152 exact bins; lower FFT or record full IQ instead"}};
                    else {
                        QString error;
                        if (operation == QStringLiteral("start") && type == QStringLiteral("audio")) error = ensureAudio();
                        if (operation == QStringLiteral("start") && type == QStringLiteral("spectrum") && !spectrum)
                            spectrum = std::make_unique<CliAsyncSpectrum>(options.fftSize);
                        if (!error.isEmpty()) reply = {{"ok", false}, {"error", error}};
                        else {
                            reply = runtimeRecorder.command(input, runtimeSettings());
                            if (operation == QStringLiteral("start") && reply.value("ok").toBool() && type == QStringLiteral("spectrum") && spectrum)
                                spectrum->reset();
                        }
                    }
                } else if (action == QStringLiteral("gnss")) {
                    QJsonObject request = optionalToolRequest(input);
                    if (input.value("operation") == QJsonValue("start")) request.insert("operation", "start");
                    if (request.value("operation") == QJsonValue("start") && (!receiverRunning || options.inputMode != INPUT_RF || scanner.running()))
                        reply = {{"ok", false}, {"error", "GNSS needs running complex RF IQ with scanning disabled"}};
                    else reply = gnss.command(request, actualRate, actualCenter);
                } else if (action == QStringLiteral("research")) {
                    const auto request = optionalToolRequest(input);
                    if (input.value("operation") == QJsonValue("cancel")) {
                        researchCapture = {}; reply.insert("pending", false);
                    } else if (input.value("operation") == QJsonValue("capture")) {
                        const QString op = request.value("operation").toString();
                        if (!receiverRunning || options.inputMode != INPUT_RF ||
                            !QStringList{"iq.analyze", "constellation.analyze", "eye.analyze", "iq.phase.analyze",
                                "spectrum.analyze", "density.append", "comb.analyze"}.contains(op))
                            reply = {{"ok", false}, {"error", "Capture requires running RF IQ and an IQ analysis operation"}};
                        else if (QStringList{"spectrum.analyze", "density.append", "comb.analyze"}.contains(op)) {
                            if (orderedTrace.size() < 2 || orderedTrace.size() > CliResearchTools::MaximumSpectrumBins)
                                reply = {{"ok", false}, {"error", "Enable spectrum first; research snapshots require 2..65536 full-resolution FFT bins"}};
                            else {
                                QJsonObject snapshot = request;
                                QJsonArray frequencies, levels;
                                for (std::size_t i = 0; i < orderedTrace.size(); ++i) {
                                    frequencies.append(spectrumFirstHz(actualCenter, actualRate, options.fftSize) + i * actualRate / options.fftSize);
                                    levels.append(orderedTrace[i]);
                                }
                                snapshot.insert("frequenciesHz", frequencies); snapshot.insert("levelsDb", levels);
                                snapshot.insert("metadata", QJsonObject{{"source", primaryBackend}, {"sampleRateHz", actualRate},
                                    {"centerHz", actualCenter}, {"amplitudeUnit", "dBFS"}});
                                reply = research.command(snapshot);
                            }
                        } else { researchCapture = request; reply.insert("pending", true); }
                    } else reply = research.command(request);
                } else if (action == QStringLiteral("web")) {
                    if (options.inputMode != INPUT_RF) reply = {{"ok", false}, {"error", "Browser VFOs require complex RF IQ"}};
                    else {
                        QJsonObject request = input;
                        request.insert("action", input.value("operation"));
                        reply = web.command(request);
                    }
                } else if (action == QStringLiteral("calibration")) {
                    QJsonObject request = input;
                    const QString operation = input.value(QStringLiteral("operation")).toString();
                    if (operation == QStringLiteral("select")) {
                        const auto key = calibrationKey();
                        request = {{"operation", "profile.select"}, {"key", QJsonObject{
                            {"receiver", key.receiver.toJson()}, {"centerFrequencyHz", key.centerFrequencyHz},
                            {"fftLength", key.fftLength}, {"fftWindowType", key.fftWindowType}}}};
                    } else if (operation == QStringLiteral("cancel")) request.insert("operation", "capture.cancel");
                    bool valid = true;
                    if (operation == QStringLiteral("request")) {
                        QFile file(input.value(QStringLiteral("path")).toString());
                        QJsonParseError parseError;
                        const bool opened = file.open(QIODevice::ReadOnly);
                        const QJsonDocument document = opened && file.size() <= 4 * 1024 * 1024
                            ? QJsonDocument::fromJson(file.readAll(), &parseError) : QJsonDocument{};
                        valid = !document.isNull() && document.isObject() && parseError.error == QJsonParseError::NoError;
                        if (valid) request = document.object();
                    }
                    reply = valid ? calibration.command(request)
                        : QJsonObject{{"ok", false}, {"error", "Expected a calibration request JSON object, maximum 4 MiB"}};
                    if (calibration.capturing()) {
                        if (!spectrum) spectrum = std::make_unique<CliAsyncSpectrum>(options.fftSize);
                        if (!spectrum->valid()) {
                            calibration.command({{"operation", "capture.cancel"}});
                            reply = {{"ok", false}, {"error", "Cannot create calibration FFT"}};
                        }
                    }
                } else if (action == QStringLiteral("preset") || action == QStringLiteral("experiment")) {
                    const QString path = input.value(QStringLiteral("path")).toString();
                    if (input.value(QStringLiteral("operation")) != QStringLiteral("save") ||
                        !path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
                        reply = {{QStringLiteral("ok"), false},
                            {QStringLiteral("error"), QStringLiteral("Use preset|experiment save FILE.json")}};
                    else {
                        if (action == QStringLiteral("experiment") && orderedTrace.size() > 65536) {
                            return QJsonObject{{"ok", false},
                                {"error", "Experiment JSON is limited to 65536 bins; use science export or spectrum-frame recording for larger FFTs"}};
                        }
                        const QJsonObject profile = makeProfile(action == QStringLiteral("experiment"));
                        QString profileError;
                        if (!writeProfile(path, profile, &profileError))
                            reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), profileError}};
                        else reply.insert(QStringLiteral("path"), path);
                    }
                } else if (action == QStringLiteral("status")) {
                    reply.insert(QStringLiteral("running"), receiverRunning);
                    if (spectrum) reply.insert(QStringLiteral("fftWorker"), spectrum->status());
                    reply.insert(QStringLiteral("input"), inputName(options.inputMode));
                    reply.insert(QStringLiteral("sampleRateHz"), actualRate);
                    reply.insert(QStringLiteral("centerFrequencyHz"), actualCenter);
                    reply.insert(QStringLiteral("modulationType"), modulation);
                    reply.insert(QStringLiteral("audioBandwidthHz"), audioBandwidthHz);
                    reply.insert(QStringLiteral("networkAudioEnabled"), networkAudioEnabled);
                    reply.insert(QStringLiteral("samples"), static_cast<double>(samples));
                    reply.insert(QStringLiteral("seconds"), samples / actualRate);
                    reply.insert(QStringLiteral("readRateMsps"), elapsed.elapsed() > 0
                                     ? samples / (elapsed.elapsed() * 1000.0) : 0.0);
                    reply.insert(QStringLiteral("recentReadRateMsps"), receiverRunning ? recentReadRateMsps : 0.0);
                    reply.insert(QStringLiteral("droppedVirtualFrames"), static_cast<double>(droppedVirtualFrames));
                    reply.insert(QStringLiteral("discardedTuneSamples"), static_cast<double>(discardedTuneSamples));
                    if (options.backend == BackendKind::HackRf)
                        reply.insert(QStringLiteral("hackrfQueueDroppedSamples"),
                                     static_cast<double>(device.droppedHackRfSamples()));
                    reply.insert(QStringLiteral("virtualConnected"), virtualSink.state() == QAbstractSocket::ConnectedState);
                    if (options.backend == BackendKind::Virtual)
                        reply.insert(QStringLiteral("virtualRx"), device.virtualSource.status());
                    reply.insert(QStringLiteral("recordingLocked"), recordingLocked());
                    reply.insert(QStringLiteral("recording"), runtimeRecorder.status());
                    reply.insert(QStringLiteral("scan"), scanner.status());
                    reply.insert(QStringLiteral("trigger"), trigger.status());
                    reply.insert(QStringLiteral("quality"), quality.status());
                    reply.insert(QStringLiteral("gnss"), gnss.status());
                    reply.insert(QStringLiteral("researchCapturePending"), !researchCapture.isEmpty());
                    reply.insert(QStringLiteral("zeroSpan"), zeroSpan.status());
                    reply.insert(QStringLiteral("graph"), graph.status());
                    reply.insert(QStringLiteral("multisdr"), multi.status(actualCenter, actualRate, receiverRunning));
                    reply.insert(QStringLiteral("vfoCount"), vfos.command(
                        {{QStringLiteral("operation"), QStringLiteral("list")}})
                        .value(QStringLiteral("vfos")).toArray().size());
                    reply.insert(QStringLiteral("spectrumFramesWritten"), static_cast<double>(spectrumRecorder.frameCount()));
                    reply.insert(QStringLiteral("channelIqBytesWritten"), static_cast<double>(channelRecorder.bytesWritten()));
                    reply.insert(QStringLiteral("fullIq8BytesWritten"), static_cast<double>(fullIq8Recorder.bytesWritten()));
                    if (audio) reply.insert(QStringLiteral("listeningFrequencyHz"), audio->listeningHz());
                    if (bridge) reply.insert(QStringLiteral("network"), bridge->metrics());
                } else if (action == QStringLiteral("metrics")) {
                    if (spectrum) reply.insert(QStringLiteral("fftWorker"), spectrum->status());
                    reply.insert(QStringLiteral("recording"), runtimeRecorder.status());
                    if (options.backend == BackendKind::Virtual)
                        reply.insert(QStringLiteral("virtualRx"), device.virtualSource.status());
                    reply.insert(QStringLiteral("running"), receiverRunning);
                    reply.insert(QStringLiteral("uptimeSeconds"), elapsed.elapsed() / 1000.0);
                    reply.insert(QStringLiteral("gnss"), gnss.status());
                    reply.insert(QStringLiteral("researchCapturePending"), !researchCapture.isEmpty());
                    reply.insert(QStringLiteral("readRateMsps"), elapsed.elapsed() > 0
                                     ? samples / (elapsed.elapsed() * 1000.0) : 0.0);
                    reply.insert(QStringLiteral("recentReadRateMsps"), receiverRunning ? recentReadRateMsps : 0.0);
                    reply.insert(QStringLiteral("samples"), static_cast<double>(samples));
                    reply.insert(QStringLiteral("discardedTuneSamples"), static_cast<double>(discardedTuneSamples));
                    reply.insert(QStringLiteral("droppedVirtualFrames"), static_cast<double>(droppedVirtualFrames));
                    if (options.backend == BackendKind::HackRf)
                        reply.insert(QStringLiteral("hackrfQueueDroppedSamples"),
                                     static_cast<double>(device.droppedHackRfSamples()));
                    if (bridge) reply.insert(QStringLiteral("network"), bridge->metrics());
                } else if (action == QStringLiteral("spectrum")) {
                    if (latestSpectrum.isEmpty())
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Start run with --spectrum and wait for the first report")}};
                    else reply.insert(QStringLiteral("spectrum"), latestSpectrum);
                } else if (action == QStringLiteral("digital")) {
                    reply = digitalControl(textDecoder, input);
                } else if (action == QStringLiteral("filters")) {
                    const QString previous = audioFilterJson;
                    reply = filterControl(audioFilterJson, input);
                    if (reply.value(QStringLiteral("ok")).toBool() && previous != audioFilterJson && audio)
                        audio->setFilterChain(audioFilterJson);
                } else if (action == QStringLiteral("vfo")) {
                    if (scanner.running() && input.value(QStringLiteral("operation")) == QStringLiteral("add"))
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Stop scanning before adding a VFO")}};
                    else reply = vfos.command(input);
                } else if (action == QStringLiteral("scan")) {
                    const QString operation = input.value(QStringLiteral("operation")).toString();
                    if (operation == QStringLiteral("start") &&
                        (recordingLocked() || trigger.active() || zeroSpan.running() || graph.running() || !options.audioFile.isEmpty() || options.backend == BackendKind::File ||
                         options.inputMode != INPUT_RF || vfos.hasListeners()))
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Scanning requires live RF without IQ recording or active extra VFO audio")}};
                    else {
                        reply = scanner.command(input);
                        if (reply.value(QStringLiteral("ok")).toBool() &&
                            operation == QStringLiteral("start") && !spectrum) {
                            spectrum = std::make_unique<CliAsyncSpectrum>(options.fftSize);
                            if (!spectrum->valid()) {
                                scanner.stop();
                                spectrum.reset();
                                reply = {{QStringLiteral("ok"), false},
                                         {QStringLiteral("error"), QStringLiteral("Cannot create scanner FFT plan")}};
                            }
                        }
                    }
                } else if (action == QStringLiteral("trigger")) {
                    const QString operation = input.value(QStringLiteral("operation")).toString();
                    if (operation == QStringLiteral("arm") &&
                        (recordingLocked() || scanner.running() || options.inputMode != INPUT_RF))
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Trigger requires RF without scanning or another IQ recording")}};
                    else {
                        reply = trigger.command(input, actualCenter, actualRate);
                        if (reply.value(QStringLiteral("ok")).toBool() &&
                            operation == QStringLiteral("arm") && !spectrum) {
                            spectrum = std::make_unique<CliAsyncSpectrum>(options.fftSize);
                            if (!spectrum->valid()) {
                                trigger.disarm();
                                spectrum.reset();
                                reply = {{QStringLiteral("ok"), false},
                                         {QStringLiteral("error"), QStringLiteral("Cannot create trigger FFT plan")}};
                            }
                        }
                    }
                } else if (action == QStringLiteral("quality")) {
                    const QString operation = input.value(QStringLiteral("operation")).toString();
                    if (operation == QStringLiteral("capture"))
                        reply = quality.request(input.value(QStringLiteral("blocks")).toInt());
                    else if (operation == QStringLiteral("cancel")) {
                        quality.cancel();
                        reply.insert(QStringLiteral("quality"), quality.status());
                    } else reply.insert(QStringLiteral("quality"), quality.status());
                } else if (action == QStringLiteral("zero")) {
                    const QString operation = input.value(QStringLiteral("operation")).toString();
                    if (operation == QStringLiteral("start") && scanner.running())
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Stop scanning before zero-span capture")}};
                    else {
                        reply = zeroSpan.command(input, actualCenter, actualRate);
                        if (reply.value(QStringLiteral("ok")).toBool() &&
                            operation == QStringLiteral("start") && !spectrum) {
                            spectrum = std::make_unique<CliAsyncSpectrum>(options.fftSize);
                            if (!spectrum->valid()) {
                                zeroSpan.stop();
                                spectrum.reset();
                                reply = {{QStringLiteral("ok"), false},
                                         {QStringLiteral("error"), QStringLiteral("Cannot create zero-span FFT plan")}};
                            }
                        }
                    }
                } else if (action == QStringLiteral("graph")) {
                    const QString operation = input.value(QStringLiteral("operation")).toString();
                    QJsonObject request = input;
                    bool valid = true;
                    if (operation == QStringLiteral("request")) {
                        QFile file(input.value("path").toString());
                        QJsonParseError error;
                        const auto document = file.open(QIODevice::ReadOnly) && file.size() <= 65536
                            ? QJsonDocument::fromJson(file.readAll(), &error) : QJsonDocument{};
                        valid = document.isObject() && error.error == QJsonParseError::NoError;
                        if (valid) request = document.object();
                    }
                    if (!valid) reply = {{"ok", false}, {"error", "Expected graph request JSON object, maximum 64 KiB"}};
                    else if (request.value("operation") == QStringLiteral("start") && scanner.running())
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Stop scanning before starting a DSP graph")}};
                    else reply = graph.command(request, actualCenter, actualRate, receiverRunning);
                } else if (action == QStringLiteral("multisdr")) {
                    reply = multi.command(input, actualCenter, actualRate, receiverRunning);
                    if (reply.value(QStringLiteral("ok")).toBool() &&
                        input.value(QStringLiteral("operation")) == QStringLiteral("add") && !spectrum) {
                        spectrum = std::make_unique<CliAsyncSpectrum>(options.fftSize);
                        if (!spectrum->valid()) {
                            multi.command({{QStringLiteral("operation"), QStringLiteral("remove")},
                                           {QStringLiteral("id"), input.value(QStringLiteral("id"))}},
                                          actualCenter, actualRate, receiverRunning);
                            spectrum.reset();
                            reply = {{QStringLiteral("ok"), false},
                                     {QStringLiteral("error"), QStringLiteral("Cannot create primary Multi-SDR FFT plan")}};
                        }
                    }
                } else if (action == QStringLiteral("module")) {
                    reply = modules.command(input);
                    if (reply.value(QStringLiteral("ok")).toBool() &&
                        input.value(QStringLiteral("operation")).toString() == QStringLiteral("start")) {
                        QString moduleError;
                        if (input.value(QStringLiteral("name")) == QStringLiteral("monitor") && !spectrum) {
                            spectrum = std::make_unique<CliAsyncSpectrum>(options.fftSize);
                            if (!spectrum->valid()) {
                                spectrum.reset();
                                moduleError = QStringLiteral("Cannot create the monitor FFT plan");
                            }
                        } else if (input.value(QStringLiteral("name")) != QStringLiteral("monitor")) {
                            moduleError = ensureAudio();
                        }
                        if (!moduleError.isEmpty()) {
                            modules.command({{QStringLiteral("operation"), QStringLiteral("stop")},
                                             {QStringLiteral("name"), input.value(QStringLiteral("name"))}});
                            reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), moduleError}};
                        }
                    }
                } else if (action == QStringLiteral("science")) {
                    const QString operation = input.value(QStringLiteral("operation")).toString();
                    const QString markerOperation = input.value(QStringLiteral("markerOperation")).toString();
                    const double spanStart = spectrumFirstHz(actualCenter, actualRate, options.fftSize);
                    const double spanEnd = actualCenter + actualRate * 0.5;
                    if (operation == QStringLiteral("detector")) {
                        const QString mode = input.value(QStringLiteral("mode")).toString();
                        const QStringList modes{QStringLiteral("sample"), QStringLiteral("positive"),
                            QStringLiteral("negative"), QStringLiteral("rms"), QStringLiteral("average"),
                            QStringLiteral("median"), QStringLiteral("quasi")};
                        const int frames = input.value(QStringLiteral("frames")).toInt();
                        if (!modes.contains(mode) || frames < 1 || frames > 256)
                            reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Invalid detector mode or frame count")}};
                        else {
                            detectorName = mode;
                            detectorFrames = frames;
                            accumulatedTraces.setDetector(modes.indexOf(mode), frames);
                        }
                    } else if (operation == QStringLiteral("vbw")) {
                        const double hz = input.value(QStringLiteral("hz")).toDouble(-1.0);
                        if (!std::isfinite(hz) || hz < 0.0 || hz > 10000.0)
                            reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("VBW must be 0..10000 Hz")}};
                        else { vbwHz = hz; accumulatedTraces.setVbwHz(hz); }
                    } else if (operation == QStringLiteral("average")) {
                        const int frames = input.value(QStringLiteral("frames")).toInt(-1);
                        if (frames < 0 || frames > 10000)
                            reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Average frames must be 0..10000")}};
                        else { averageFrames = frames; accumulatedTraces.setAverageFrameCount(frames); }
                    } else if (operation == QStringLiteral("percentiles")) {
                        percentilesEnabled = input.value(QStringLiteral("enabled")).toBool();
                        accumulatedTraces.setPercentileTraces(percentilesEnabled, percentilesEnabled, percentilesEnabled);
                    } else if (operation == QStringLiteral("export")) {
                        const QString source = input.value(QStringLiteral("source")).toString();
                        const std::vector<float> *levels = traceForSource(source);
                        const QString path = input.value(QStringLiteral("path")).toString();
                        if (!levels || levels->empty() || !path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive))
                            reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Trace unavailable or CSV path invalid")}};
                        else {
                            QFile csv(path);
                            if (!csv.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text))
                                reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), csv.errorString()}};
                            else {
                                QTextStream stream(&csv);
                                stream.setLocale(QLocale::c());
                                stream << "frequency_hz,level_dbfs\n";
                                const double binHz = actualRate / levels->size();
                                for (std::size_t bin = 0; bin < levels->size(); ++bin)
                                    stream << QString::number(spanStart + bin * binHz, 'f', 6)
                                           << ',' << QString::number((*levels)[bin], 'f', 6) << '\n';
                                stream.flush();
                                if (stream.status() != QTextStream::Ok || csv.error() != QFile::NoError) {
                                    reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("CSV write failed")}};
                                    csv.close();
                                    csv.remove();
                                } else {
                                    csv.close();
                                    reply.insert(QStringLiteral("path"), QFileInfo(path).absoluteFilePath());
                                    reply.insert(QStringLiteral("source"), source);
                                    reply.insert(QStringLiteral("bins"), static_cast<int>(levels->size()));
                                    reply.insert(QStringLiteral("binHz"), binHz);
                                }
                            }
                        }
                    } else if (operation == QStringLiteral("marker")) {
                        const int index = input.value(QStringLiteral("index")).toInt() - 1;
                        if (markerOperation == QStringLiteral("set")) {
                            const double hz = input.value(QStringLiteral("frequencyHz")).toDouble();
                            if (index < 0 || index >= 10 || !std::isfinite(hz) || hz < spanStart || hz > spanEnd)
                                reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Marker must lie inside the current IQ span")}};
                            else markerHz[static_cast<std::size_t>(index)] = hz;
                        } else if (markerOperation == QStringLiteral("clear")) {
                            if (index < 0 || index >= 10)
                                reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Marker number must be 1..10")}};
                            else markerHz[static_cast<std::size_t>(index)] = std::numeric_limits<double>::quiet_NaN();
                        } else if (markerOperation == QStringLiteral("peak")) {
                            if (index < 0 || index >= 10 || orderedTrace.empty())
                                reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Peak search needs a live spectrum")}};
                            else {
                                const auto peak = std::max_element(orderedTrace.begin(), orderedTrace.end());
                                markerHz[static_cast<std::size_t>(index)] = spanStart +
                                    (peak - orderedTrace.begin()) * actualRate / orderedTrace.size();
                            }
                        } else if (markerOperation == QStringLiteral("pair")) {
                            const int first = input.value(QStringLiteral("first")).toInt() - 1;
                            const int second = input.value(QStringLiteral("second")).toInt() - 1;
                            if (first < 0 || first >= 10 || second < 0 || second >= 10 || first == second)
                                reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Invalid marker pair")}};
                            else { markerFirst = first; markerSecond = second; }
                        } else if (markerOperation != QStringLiteral("list"))
                            reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Unknown marker operation")}};
                    } else if (operation != QStringLiteral("status"))
                        reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Unknown science operation")}};
                    if (reply.value(QStringLiteral("ok")).toBool()) {
                        QJsonArray markers;
                        for (int i = 0; i < 10; ++i) {
                            const double hz = markerHz[static_cast<std::size_t>(i)];
                            QJsonObject marker{{QStringLiteral("number"), i + 1},
                                {QStringLiteral("enabled"), std::isfinite(hz)}};
                            if (std::isfinite(hz)) {
                                marker.insert(QStringLiteral("frequencyHz"), hz);
                                if (!orderedTrace.empty() && hz >= spanStart && hz <= spanEnd) {
                                    const auto bin = static_cast<std::size_t>(std::clamp(
                                        (hz - spanStart) * orderedTrace.size() / actualRate,
                                        0.0, static_cast<double>(orderedTrace.size() - 1)));
                                    marker.insert(QStringLiteral("levelDbfs"), orderedTrace[bin]);
                                }
                            }
                            markers.append(marker);
                        }
                        QJsonObject science{{QStringLiteral("tracesEnabled"), tracesEnabled},
                            {QStringLiteral("detector"), detectorName},
                            {QStringLiteral("detectorFrames"), detectorFrames},
                            {QStringLiteral("vbwHz"), vbwHz},
                            {QStringLiteral("averageFrames"), averageFrames},
                            {QStringLiteral("percentilesEnabled"), percentilesEnabled},
                            {QStringLiteral("markers"), markers},
                            {QStringLiteral("pair"), QJsonArray{markerFirst + 1, markerSecond + 1}}};
                        const double firstHz = markerHz[static_cast<std::size_t>(markerFirst)];
                        const double secondHz = markerHz[static_cast<std::size_t>(markerSecond)];
                        if (std::isfinite(firstHz) && std::isfinite(secondHz)) {
                            science.insert(QStringLiteral("deltaFrequencyHz"), secondHz - firstHz);
                            const QJsonObject first = markers[markerFirst].toObject();
                            const QJsonObject second = markers[markerSecond].toObject();
                            if (first.contains(QStringLiteral("levelDbfs")) && second.contains(QStringLiteral("levelDbfs")))
                                science.insert(QStringLiteral("deltaLevelDb"),
                                    second.value(QStringLiteral("levelDbfs")).toDouble() - first.value(QStringLiteral("levelDbfs")).toDouble());
                        }
                        reply.insert(QStringLiteral("science"), science);
                    }
                } else if (action == QStringLiteral("trace")) {
                    const QString source = input.value(QStringLiteral("source")).toString(QStringLiteral("live"));
                    const std::vector<float> *values = traceForSource(source);
                    if (!values || values->empty())
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Requested trace is unavailable; use --spectrum or control traces start")}};
                    else {
                        QJsonArray bins;
                        const int offset = input.value(QStringLiteral("offset")).toInt(0);
                        const int count = input.value(QStringLiteral("count")).toInt(65536);
                        if (offset < 0 || offset >= static_cast<int>(values->size()) || count < 1 || count > 65536) {
                            return QJsonObject{{"ok", false}, {"error", "Trace page outside available bins"}};
                        }
                        const int end = (std::min)(static_cast<int>(values->size()), offset + count);
                        for (int index = offset; index < end; ++index) bins.append((*values)[index]);
                        reply.insert(QStringLiteral("source"), source);
                        reply.insert(QStringLiteral("startHz"), actualCenter - (static_cast<int>(values->size()) / 2) * (actualRate / values->size()) + offset * (actualRate / values->size()));
                        reply.insert(QStringLiteral("binHz"), actualRate / values->size());
                        reply.insert(QStringLiteral("offset"), offset);
                        reply.insert(QStringLiteral("totalBins"), static_cast<int>(values->size()));
                        reply.insert(QStringLiteral("returnedBins"), end - offset);
                        reply.insert(QStringLiteral("truncated"), end < static_cast<int>(values->size()));
                        reply.insert(QStringLiteral("levelsDbfs"), bins);
                    }
                } else if (action == QStringLiteral("traces")) {
                    const QString operation = input.value(QStringLiteral("operation")).toString();
                    if (operation == QStringLiteral("start")) {
                        if (!spectrum) reply = {{QStringLiteral("ok"), false},
                                                {QStringLiteral("error"), QStringLiteral("Start the receiver with --spectrum or --serve")}};
                        else {
                            accumulatedTraces.clear();
                            accumulatedTraces.setTraceEnabled(true, true, true);
                            accumulatedTraces.setAverageTimeSeconds(2.0);
                            accumulatedTraces.setAverageFrameCount(averageFrames);
                            accumulatedTraces.setVbwHz(vbwHz);
                            accumulatedTraces.setPercentileTraces(percentilesEnabled, percentilesEnabled, percentilesEnabled);
                            tracesEnabled = true;
                            accumulatedFrames = 0;
                        }
                    } else if (operation == QStringLiteral("stop")) {
                        tracesEnabled = false;
                        accumulatedTraces.clear();
                        accumulatedFrames = 0;
                    } else if (operation == QStringLiteral("reset")) {
                        accumulatedTraces.clear();
                        if (tracesEnabled) accumulatedTraces.setTraceEnabled(true, true, true);
                        accumulatedFrames = 0;
                    } else if (operation != QStringLiteral("status"))
                        reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Unknown traces command")}};
                    reply.insert(QStringLiteral("enabled"), tracesEnabled);
                    reply.insert(QStringLiteral("frames"), static_cast<double>(accumulatedFrames));
                } else if (action == QStringLiteral("tone")) {
                    const QString source = input.value(QStringLiteral("source")).toString(QStringLiteral("live"));
                    const std::vector<float> *values = traceForSource(source);
                    if (!values || values->empty())
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Requested tone trace is unavailable")}};
                    else {
                        const double firstHz = spectrumFirstHz(actualCenter, actualRate, options.fftSize);
                        const double binHz = actualRate / values->size();
                        const auto peak = std::max_element(values->begin(), values->end());
                        const double automaticHz = firstHz + (peak - values->begin()) * binHz;
                        const double fundamental = input.value(QStringLiteral("fundamentalHz")).toDouble(automaticHz);
                        const double halfWidth = input.value(QStringLiteral("halfWidthHz")).toDouble(binHz * 3.0);
                        const int lastHarmonic = input.value(QStringLiteral("lastHarmonic")).toInt(5);
                        std::vector<float> frequencies;
                        frequencies.reserve(values->size());
                        for (std::size_t i = 0; i < values->size(); ++i)
                            frequencies.push_back(static_cast<float>(firstHz + i * binHz));
                        const TonalSpectrumResult result = analyzeTonalSpectrum(
                            frequencies, *values, fundamental, halfWidth, lastHarmonic);
                        if (!result.valid) reply = {{QStringLiteral("ok"), false},
                                                    {QStringLiteral("error"), QStringLiteral("No isolated tone in this contiguous span; check frequency and integration width")}};
                        else {
                            reply.insert(QStringLiteral("source"), source);
                            reply.insert(QStringLiteral("fundamentalHz"), result.fundamentalHz);
                            reply.insert(QStringLiteral("binHz"), result.binWidthHz);
                            reply.insert(QStringLiteral("snrDb"), result.snrDb);
                            reply.insert(QStringLiteral("sinadDb"), result.sinadDb);
                            if (result.harmonicsMeasured > 0) {
                                reply.insert(QStringLiteral("thdDb"), result.thdDb);
                                reply.insert(QStringLiteral("thdnDb"), result.thdnDb);
                                reply.insert(QStringLiteral("thdPercent"), result.thdPercent);
                                reply.insert(QStringLiteral("thdnPercent"), result.thdnPercent);
                            } else {
                                reply.insert(QStringLiteral("thdDb"), QJsonValue::Null);
                                reply.insert(QStringLiteral("thdnDb"), QJsonValue::Null);
                                reply.insert(QStringLiteral("thdPercent"), QJsonValue::Null);
                                reply.insert(QStringLiteral("thdnPercent"), QJsonValue::Null);
                                reply.insert(QStringLiteral("warning"), QStringLiteral("No harmonics lie inside this spectrum span; THD and THD+N are unavailable"));
                            }
                            reply.insert(QStringLiteral("harmonicsMeasured"), result.harmonicsMeasured);
                            reply.insert(QStringLiteral("harmonicsRequested"), result.harmonicsRequested);
                            reply.insert(QStringLiteral("completeHarmonicSpan"), result.completeHarmonicSpan);
                        }
                    }
                } else if (action == QStringLiteral("measure")) {
                    const QString source = input.value(QStringLiteral("source")).toString(QStringLiteral("live"));
                    const std::vector<float> *values = traceForSource(source);
                    if (!values || values->empty())
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Requested measurement trace is unavailable")}};
                    else {
                        const double spanStart = spectrumFirstHz(actualCenter, actualRate, options.fftSize);
                        const double spanEnd = actualCenter + actualRate * 0.5;
                        const bool selected = input.contains(QStringLiteral("startHz"));
                        const double startHz = input.value(QStringLiteral("startHz")).toDouble(spanStart);
                        const double endHz = input.value(QStringLiteral("endHz")).toDouble(spanEnd);
                        if (!std::isfinite(startHz) || !std::isfinite(endHz) ||
                            startHz < spanStart || endHz > spanEnd || endHz <= startHz)
                            reply = {{QStringLiteral("ok"), false},
                                     {QStringLiteral("error"), QStringLiteral("Measurement range is outside the current IQ span")}};
                        else {
                            std::vector<float> frequencies;
                            frequencies.reserve(values->size());
                            for (std::size_t i = 0; i < values->size(); ++i)
                                frequencies.push_back(static_cast<float>(i));
                            const double binHz = actualRate / values->size();
                            const auto physicalHz = [&](double bin) { return spanStart + bin * binHz; };
                            SpectrumScienceAnalyzer analyzer;
                            analyzer.update(frequencies, *values);
                            if (selected) {
                                analyzer.setMarker(0, (startHz - spanStart) / binHz);
                                analyzer.setMarker(1, (endHz - spanStart) / binHz);
                            }
                            const SpectrumScienceMetrics &m = analyzer.metrics();
                            if (!m.valid) reply = {{QStringLiteral("ok"), false},
                                                   {QStringLiteral("error"), QStringLiteral("Measurement could not be calculated")}};
                            else {
                                reply.insert(QStringLiteral("source"), source);
                                reply.insert(QStringLiteral("rangeStartHz"), physicalHz(m.rangeStartHz));
                                reply.insert(QStringLiteral("rangeEndHz"), physicalHz(m.rangeEndHz));
                                reply.insert(QStringLiteral("binHz"), binHz);
                                reply.insert(QStringLiteral("binCount"), m.binCount);
                                reply.insert(QStringLiteral("peakFrequencyHz"), physicalHz(m.peakFrequencyHz));
                                reply.insert(QStringLiteral("peakDbfs"), m.peakDb);
                                reply.insert(QStringLiteral("noiseFloorDbfs"), m.noiseFloorDb);
                                reply.insert(QStringLiteral("snrEstimateDb"), m.snrDb);
                                reply.insert(QStringLiteral("centroidFrequencyHz"), physicalHz(m.centroidFrequencyHz));
                                reply.insert(QStringLiteral("channelPowerDbfsRelative"), m.channelPowerDb);
                                reply.insert(QStringLiteral("obw90Hz"), m.occupiedBandwidth90Hz * binHz);
                                reply.insert(QStringLiteral("obw95Hz"), m.occupiedBandwidth95Hz * binHz);
                                reply.insert(QStringLiteral("obw99Hz"), m.occupiedBandwidthHz * binHz);
                                reply.insert(QStringLiteral("width3DbHz"), m.width3DbHz * binHz);
                                reply.insert(QStringLiteral("width6DbHz"), m.width6DbHz * binHz);
                                reply.insert(QStringLiteral("width20DbHz"), m.width20DbHz * binHz);
                                if (selected) {
                                    reply.insert(QStringLiteral("acprLowerDb"), m.acprLowerDb);
                                    reply.insert(QStringLiteral("acprUpperDb"), m.acprUpperDb);
                                }
                            }
                        }
                    }
                } else if (action == QStringLiteral("tune")) {
                    scanner.stop();
                    trigger.disarm();
                    quality.cancel();
                    zeroSpan.stop();
                    const double requested = input.value(QStringLiteral("frequencyHz")).toDouble();
                    if (!std::isfinite(requested) || requested <= 0.0) reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Invalid frequency")}};
                    else if (options.inputMode != INPUT_RF) {
                        if (!audio || requested + audioBandwidthHz / 2 >= actualRate / 2)
                            reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("HF hardware center is fixed at zero; select an audio channel within Nyquist")}};
                        else {
                            audio->setListening(requested);
                            listeningHz = requested;
                            modules.setListeningHz(requested);
                            reply.insert(QStringLiteral("listeningFrequencyHz"), requested);
                        }
                    }
                    else if (options.backend == BackendKind::Fobos && requested < 50e6)
                        reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("RF tuning cannot cross below 50 MHz; restart with an HF input")}};
                    else if (recordingLocked())
                        reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Retuning is disabled during IQ or spectrum recording")}};
                    else if (audio && !audio->followsReceiverCenter() &&
                             std::abs(audio->listeningHz() - requested) + audioBandwidthHz / 2 >= actualRate / 2)
                        reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Audio channel would leave the IQ band")}};
                    else {
                        const int result = device.frequency(requested, &actualCenter);
                        if (result != FOBOS_ERR_OK) reply = receiverError(device, "frequency", result);
                        else {
                            reply.insert(QStringLiteral("centerFrequencyHz"), actualCenter);
                            if (audio) {
                                audio->setCenter(actualCenter);
                                listeningHz = audio->listeningHz();
                            }
                            modules.setListeningHz(listeningHz);
                            modules.reset();
                            vfos.setSource(actualCenter, actualRate);
                            tuneSettlingSamples = static_cast<quint64>(actualRate * 0.10);
                            discontinuity = true;
                            latestSpectrum = {};
                            if (spectrum) spectrum->reset();
                            multi.clearPrimarySpectrum();
                            orderedTrace.clear();
                            accumulatedTraces.clear(); accumulatedFrames = 0;
                            if (bridge) bridge->publishState();
                        }
                    }
                } else if (action == QStringLiteral("listen")) {
                    const double requested = input.value(QStringLiteral("frequencyHz")).toDouble();
                    if (!options.channelIqBase.isEmpty() || runtimeRecorder.active())
                        reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Stop channel IQ recording before changing its center")}};
                    else if (!audio || !std::isfinite(requested) || requested <= 0.0 ||
                        std::abs(requested - actualCenter) + audioBandwidthHz / 2 >= actualRate / 2)
                        reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Audio channel unavailable or outside IQ band")}};
                    else {
                        audio->setListening(requested);
                        listeningHz = requested;
                        modules.setListeningHz(requested);
                        reply.insert(QStringLiteral("listeningFrequencyHz"), requested);
                        if (bridge) bridge->publishState();
                    }
                } else if (action == QStringLiteral("gain")) {
                    const int lna = input.value(QStringLiteral("lna")).toInt(-1);
                    const int vga = input.value(QStringLiteral("vga")).toInt(-1);
                    if (recordingLocked())
                        reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Gain is fixed during IQ or spectrum recording")}};
                    else if (lna < 0 || lna > 2 || vga < 0 || vga > 15)
                        reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Invalid gain")}};
                    else {
                        const int a = device.lna(static_cast<unsigned int>(lna));
                        const int b = a == FOBOS_ERR_OK ? device.vga(static_cast<unsigned int>(vga)) : a;
                        if (b != FOBOS_ERR_OK) reply = receiverError(device, "gain", b);
                        else { currentLna = lna; currentVga = vga;
                               if (spectrum) spectrum->reset();
                               latestSpectrum = {}; orderedTrace.clear();
                               accumulatedTraces.clear(); accumulatedFrames = 0;
                               reply.insert(QStringLiteral("lna"), lna); reply.insert(QStringLiteral("vga"), vga); }
                        if (reply.value(QStringLiteral("ok")).toBool() && bridge) bridge->publishState();
                    }
                } else if (action == QStringLiteral("demod")) {
                    const int requested = input.value(QStringLiteral("modulationType")).toInt(-1);
                    const double bandwidth = defaultAudioBandwidth(requested);
                    if (!options.channelIqBase.isEmpty() || runtimeRecorder.active())
                        reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Stop channel IQ recording before changing its mode")}};
                    else if (!audio || (requested != MOD_AM && requested != MOD_NFM &&
                                   requested != MOD_WFM && requested != MOD_USB &&
                                   requested != MOD_LSB && requested != MOD_CW) ||
                        std::abs(listeningHz - actualCenter) + bandwidth / 2 >= actualRate / 2)
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Audio mode is unavailable for this channel")}};
                    else {
                        modulation = requested;
                        audioBandwidthHz = bandwidth;
                        audio->setModulation(modulation);
                        audio->setBandwidth(audioBandwidthHz);
                        reply.insert(QStringLiteral("modulationType"), modulation);
                        reply.insert(QStringLiteral("audioBandwidthHz"), audioBandwidthHz);
                        if (bridge) bridge->publishState();
                    }
                } else if (action == QStringLiteral("bandwidth")) {
                    const double requested = input.value(QStringLiteral("bandwidthHz")).toDouble();
                    if (!options.channelIqBase.isEmpty() || runtimeRecorder.active())
                        reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Stop channel IQ recording before changing its bandwidth")}};
                    else if (!audio || !std::isfinite(requested) || requested <= 0.0 ||
                        std::abs(listeningHz - actualCenter) + requested / 2 >= actualRate / 2)
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Audio bandwidth is outside the IQ band")}};
                    else {
                        audioBandwidthHz = requested;
                        audio->setBandwidth(requested);
                        reply.insert(QStringLiteral("audioBandwidthHz"), requested);
                        if (bridge) bridge->publishState();
                    }
                } else if (action == QStringLiteral("audio")) {
                    if (!bridge || !input.value(QStringLiteral("enabled")).isBool())
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Network audio requires --serve")}};
                    else {
                        networkAudioEnabled = input.value(QStringLiteral("enabled")).toBool();
                        reply.insert(QStringLiteral("networkAudioEnabled"), networkAudioEnabled);
                        bridge->publishState();
                    }
                } else if (action == QStringLiteral("start")) {
                    if (!receiverRunning) {
                        const int result = device.start(static_cast<unsigned int>(options.blockSamples));
                        if (result == FOBOS_ERR_OK) {
                            receiverRunning = true; fileAtEof = false;
                            if (spectrum) spectrum->reset();
                            if (bridge) bridge->publishState();
                        }
                        else reply = receiverError(device, "start", result);
                    }
                } else if (action == QStringLiteral("stop")) {
                    scanner.stop();
                    gnss.command({{"operation", "stop"}}, actualRate, actualCenter);
                    researchCapture = {};
                    trigger.disarm();
                    quality.cancel();
                    zeroSpan.stop();
                    IqBuffer::clear();
                    multi.clearPrimarySpectrum();
                    if (runtimeRecorder.active()) runtimeRecorder.command({{"operation", "stop"}}, runtimeSettings());
                    if (spectrum) spectrum->reset();
                    if (startupRecordingLocked) {
                        stop = true;
                        reply.insert(QStringLiteral("recordingFinalized"), true);
                    } else if (bridge) {
                        if (receiverRunning) {
                            const int result = device.stop();
                            if (result == FOBOS_ERR_OK) { receiverRunning = false; vfos.reset(); bridge->publishState(); }
                            else reply = receiverError(device, "stop", result);
                        }
                    } else stop = true;
                } else if (action == QStringLiteral("quit")) {
                    stop = true;
                } else if (action == QStringLiteral("mode")) {
                    if (!bridge || !bridge->setServerProcessingMode(
                            input.value(QStringLiteral("processingMode")).toInt(-1)))
                        reply = {{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Server processing mode unavailable")}};
                } else {
                    reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Unknown action")}};
                }
                return reply;
    };
    registerCliCommandHandlers(dispatcher, executeControl, true);
    QObject::connect(&control, &QTcpServer::newConnection, &app, [&] {
        while (control.hasPendingConnections()) {
            QTcpSocket *client = control.nextPendingConnection();
            client->setReadBufferSize(65537);
            auto *deadline = new QTimer(client); deadline->setSingleShot(true); deadline->start(3000);
            QObject::connect(deadline, &QTimer::timeout, client, &QTcpSocket::abort);
            QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
            QObject::connect(client, &QTcpSocket::readyRead, client, [&, client, deadline] {
                if (client->bytesAvailable() > 65536) { client->abort(); return; }
                if (!client->canReadLine()) return;
                deadline->stop();
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(client->readLine(65537), &error);
                QJsonObject reply;
                if (error.error != QJsonParseError::NoError || !document.isObject())
                    reply = CommandDispatcher::failure("invalid_json", "Expected a JSON object");
                else {
                    const auto input = document.object();
                    const QJsonObject request = input.contains("command") ? input :
                        QJsonObject{{"command", "cli.legacy"}, {"args", QJsonObject{{"request", input}}}};
                    reply = cliCommandWireReply(dispatcher.execute(request, CommandDispatcher::Context::human()),
                                                !input.contains("command"));
                }
                client->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                client->flush();
                client->disconnectFromHost();
            });
        }
    });

    code = device.start(static_cast<unsigned int>(options.blockSamples));
    if (code != FOBOS_ERR_OK) { printJson(receiverError(device, "start", code)); return 1; }
    receiverRunning = true;
    if (bridge) {
        CliNetworkBridge::ServerCallbacks callbacks;
        callbacks.settings = [&]() {
            return QJsonObject{{QStringLiteral("deviceIndex"), 0},
                               {QStringLiteral("inputMode"), options.inputMode},
                               {QStringLiteral("centerFrequency"), actualCenter},
                               {QStringLiteral("actualFrequency"), actualCenter},
                               {QStringLiteral("listeningFrequency"), listeningHz},
                               {QStringLiteral("sampleRate"), actualRate},
                               {QStringLiteral("bandwidth"), audioBandwidthHz},
                               {QStringLiteral("modulationType"), modulation},
                               {QStringLiteral("fftLength"), options.fftSize},
                               {QStringLiteral("lnaGain"), currentLna},
                               {QStringLiteral("vgaGain"), currentVga},
                               {QStringLiteral("audioEnabled"), networkAudioEnabled},
                               {QStringLiteral("scalePercent"), 100},
                               {QStringLiteral("additionalScaleDivisor"), 1}};
        };
        callbacks.receivers = [&]() {
            return QJsonArray{QJsonObject{{QStringLiteral("deviceIndex"), 0},
                                           {QStringLiteral("label"), backendName(options.backend) +
                                                QStringLiteral(" RX (Obrii CLI)")}}};
        };
        callbacks.setRunning = [&](bool requested, QString *reason) {
            if (requested == receiverRunning) return true;
            if (!requested && startupRecordingLocked) {
                if (reason) *reason = QStringLiteral("Stop the CLI process to finalize this IQ/spectrum recording");
                return false;
            }
            const int result = requested ? device.start(static_cast<unsigned int>(options.blockSamples))
                                         : device.stop();
            if (result != FOBOS_ERR_OK) {
                if (reason) *reason = QStringLiteral("Receiver start/stop failed: %1").arg(device.errorText(result));
                return false;
            }
            receiverRunning = requested;
            if (requested) fileAtEof = false;
            if (!requested && runtimeRecorder.active()) runtimeRecorder.command({{"operation", "stop"}}, runtimeSettings());
            if (!requested) gnss.command({{"operation", "stop"}}, actualRate, actualCenter);
            if (!requested) researchCapture = {};
            if (!requested) scanner.stop();
            if (!requested) trigger.disarm();
            if (!requested) quality.cancel();
            if (!requested) zeroSpan.stop();
            if (!requested) IqBuffer::clear();
            if (!requested) multi.clearPrimarySpectrum();
            vfos.reset();
            discontinuity = true;
            latestSpectrum = {};
            if (spectrum) spectrum->reset();
            orderedTrace.clear();
            accumulatedTraces.clear(); accumulatedFrames = 0;
            return true;
        };
        callbacks.applySettings = [&](const QJsonObject &patch, QString *reason) {
            if (patch.isEmpty()) return true;
            scanner.stop();
            trigger.disarm();
            quality.cancel();
            zeroSpan.stop();
            const int requestedDevice = patch.value(QStringLiteral("deviceIndex")).toInt(0);
            const int requestedInput = patch.value(QStringLiteral("inputMode")).toInt(options.inputMode);
            const double requestedRate = patch.value(QStringLiteral("sampleRate")).toDouble(actualRate);
            const double requestedCenter = patch.value(QStringLiteral("centerFrequency")).toDouble(actualCenter);
            const double requestedListen = patch.value(QStringLiteral("listeningFrequency")).toDouble(listeningHz);
            const double requestedBandwidth = patch.value(QStringLiteral("bandwidth")).toDouble(audioBandwidthHz);
            const int requestedLna = patch.value(QStringLiteral("lnaGain")).toInt(currentLna);
            const int requestedVga = patch.value(QStringLiteral("vgaGain")).toInt(currentVga);
            const int requestedModulation = patch.value(QStringLiteral("modulationType")).toInt(modulation);
            const bool requestedAudio = patch.value(QStringLiteral("audioEnabled")).toBool(networkAudioEnabled);
            if (requestedDevice != 0 || requestedInput != INPUT_RF) {
                *reason = QStringLiteral("This CLI server exposes only its selected RF device");
                return false;
            }
            if (!std::isfinite(requestedRate) || requestedRate < 1e6 ||
                !std::isfinite(requestedCenter) ||
                requestedCenter < (options.backend == BackendKind::Fobos ? 50e6 : 1.0) ||
                !std::isfinite(requestedListen) || requestedListen <= 0.0 ||
                !std::isfinite(requestedBandwidth) || requestedBandwidth <= 0.0 ||
                std::abs(requestedListen - requestedCenter) + requestedBandwidth * 0.5 >= requestedRate * 0.5 ||
                (requestedModulation != MOD_AM && requestedModulation != MOD_NFM &&
                 requestedModulation != MOD_WFM && requestedModulation != MOD_USB &&
                 requestedModulation != MOD_LSB && requestedModulation != MOD_CW) ||
                requestedLna < 0 || requestedLna > 2 || requestedVga < 0 || requestedVga > 15) {
                *reason = QStringLiteral("Requested frequency, bandwidth, rate or gain is outside the receiver range");
                return false;
            }
            const bool rateChanged = std::abs(requestedRate - actualRate) > 0.5;
            if ((!options.channelIqBase.isEmpty() || runtimeRecorder.active()) &&
                (std::abs(requestedListen - listeningHz) > 0.5 ||
                 std::abs(requestedBandwidth - audioBandwidthHz) > 0.5 ||
                 requestedModulation != modulation)) {
                *reason = QStringLiteral("Channel IQ recording fixes its listening frequency, bandwidth and mode");
                return false;
            }
            if (recordingLocked() && (requestedLna != currentLna || requestedVga != currentVga)) {
                *reason = QStringLiteral("Gain is fixed during IQ or spectrum recording");
                return false;
            }
            const bool wasRunning = receiverRunning;
            if (rateChanged) {
                if (options.backend == BackendKind::File || options.backend == BackendKind::Virtual) {
                    *reason = QStringLiteral("File/virtual IQ sample rate is fixed by its source");
                    return false;
                }
                if (recordingLocked()) {
                    *reason = QStringLiteral("Cannot change sample rate during IQ or spectrum recording");
                    return false;
                }
                if (options.backend == BackendKind::Fobos) {
                    double rates[100] = {};
                    unsigned int rateCount = 100;
                    if (device.sampleRates(rates, &rateCount) != FOBOS_ERR_OK ||
                        std::none_of(rates, rates + rateCount, [requestedRate](double value) {
                            return std::abs(value - requestedRate) < 0.5;
                        })) {
                        *reason = QStringLiteral("Sample rate is not supported by this Fobos device");
                        return false;
                    }
                }
                if (receiverRunning) {
                    const int result = device.stop();
                    if (result != FOBOS_ERR_OK) {
                        *reason = QStringLiteral("Could not stop receiver before sample-rate change");
                        return false;
                    }
                    receiverRunning = false;
                }
                const int result = device.sampleRate(requestedRate, &actualRate);
                if (result != FOBOS_ERR_OK) {
                    *reason = QStringLiteral("Receiver rejected sample-rate change: %1").arg(device.errorText(result));
                    return false;
                }
                if (audio) audio->setSampleRate(actualRate);
                vfos.setSource(actualCenter, actualRate);
            }
            if (std::abs(requestedCenter - actualCenter) > 0.5 || rateChanged) {
                if (recordingLocked()) {
                    *reason = QStringLiteral("Cannot retune during IQ or spectrum recording");
                    return false;
                }
                const int result = device.frequency(requestedCenter, &actualCenter);
                if (result != FOBOS_ERR_OK) {
                    *reason = QStringLiteral("Receiver rejected frequency: %1").arg(device.errorText(result));
                    return false;
                }
                tuneSettlingSamples = static_cast<quint64>(actualRate * 0.10);
                latestSpectrum = {};
                if (spectrum) spectrum->reset();
                multi.clearPrimarySpectrum();
                orderedTrace.clear();
                accumulatedTraces.clear(); accumulatedFrames = 0;
                discontinuity = true;
                if (audio) audio->setCenter(actualCenter);
                vfos.setSource(actualCenter, actualRate);
            }
            if (requestedLna != currentLna) {
                const int result = device.lna(static_cast<unsigned int>(requestedLna));
                if (result != FOBOS_ERR_OK) { *reason = QStringLiteral("Receiver rejected LNA gain"); return false; }
                currentLna = requestedLna;
                if (spectrum) spectrum->reset();
                latestSpectrum = {}; orderedTrace.clear();
                accumulatedTraces.clear(); accumulatedFrames = 0;
            }
            if (requestedVga != currentVga) {
                const int result = device.vga(static_cast<unsigned int>(requestedVga));
                if (result != FOBOS_ERR_OK) { *reason = QStringLiteral("Receiver rejected VGA gain"); return false; }
                currentVga = requestedVga;
                if (spectrum) spectrum->reset();
                latestSpectrum = {}; orderedTrace.clear();
                accumulatedTraces.clear(); accumulatedFrames = 0;
            }
            listeningHz = requestedListen;
            audioBandwidthHz = requestedBandwidth;
            modulation = requestedModulation;
            networkAudioEnabled = requestedAudio;
            if (audio) {
                audio->setListening(listeningHz);
                audio->setBandwidth(audioBandwidthHz);
                audio->setModulation(modulation);
            }
            modules.setListeningHz(listeningHz);
            if (rateChanged) modules.reset();
            if (rateChanged && wasRunning) {
                const int result = device.start(static_cast<unsigned int>(options.blockSamples));
                if (result != FOBOS_ERR_OK) {
                    *reason = QStringLiteral("Receiver did not restart after sample-rate change: %1").arg(device.errorText(result));
                    return false;
                }
                receiverRunning = true;
            }
            return true;
        };
        if (!bridge->startServer(options.bindAddress, options.networkPort, std::move(callbacks))) {
            device.stop();
            return fail(QStringLiteral("Cannot start the GUI-compatible network server"));
        }
    }
    printJson({{QStringLiteral("event"), QStringLiteral("started")},
               {QStringLiteral("backend"), backendName(options.backend)},
               {QStringLiteral("api"), options.backend == BackendKind::Fobos
                   ? apiName(options.api) : backendName(options.backend)},
               {QStringLiteral("input"), inputName(options.inputMode)},
               {QStringLiteral("centerFrequencyHz"), actualCenter},
               {QStringLiteral("sampleRateHz"), actualRate},
               {QStringLiteral("control"), QStringLiteral("127.0.0.1:%1").arg(options.controlPort)}});
    std::signal(SIGINT, interruptHandler);
    std::signal(SIGTERM, interruptHandler);
    std::vector<float> block(static_cast<std::size_t>(options.blockSamples) * 2);
    QElapsedTimer report; report.start();
    int exitCode = 0;
    const auto pollSpectrum = [&] {
        if (!spectrum) return;
        CliSpectrumResult result;
        if (!spectrum->poll(result)) return;
        if (!result.error.isEmpty()) { fail(result.error); exitCode = 1; stop = true; return; }
        latestSpectrum = std::move(result.summary);
        orderedTrace = std::move(result.trace);
        if (runtimeRecorder.needsSpectrum() && !orderedTrace.empty())
            runtimeRecorder.submitSpectrum(orderedTrace, spectrumFirstHz(actualCenter, actualRate, options.fftSize),
                actualRate / options.fftSize, runtimeSettings(), false, result.utcMs);
        if (calibration.capturing()) {
            std::vector<float> frequencies(orderedTrace.size());
            const double binHz = actualRate / options.fftSize;
            const double firstHz = spectrumFirstHz(actualCenter, actualRate, options.fftSize);
            for (size_t i = 0; i < frequencies.size(); ++i) frequencies[i] = static_cast<float>(firstHz + i * binHz);
            const auto calibrated = calibration.submitSpectrum(frequencies, orderedTrace, calibrationKey());
            if (!calibrated.isEmpty()) printJson(calibrated);
        }
        if (multi.hasRunningAuxiliary() && !orderedTrace.empty()) multi.updatePrimarySpectrum(orderedTrace, actualCenter, actualRate);
        if (modules.needsSpectrum()) modules.submitSpectrum(orderedTrace, actualCenter, actualRate);
        if (scanner.running()) {
            const QJsonObject hit = scanner.observe(latestSpectrum, actualCenter);
            if (!hit.isEmpty()) {
                const double signalHz = hit.value(QStringLiteral("signalFrequencyHz")).toDouble(actualCenter);
                if (audio && std::abs(signalHz - actualCenter) + audioBandwidthHz * 0.5 < actualRate * 0.5) {
                    audio->setListening(signalHz); listeningHz = signalHz; modules.setListeningHz(signalHz);
                    if (bridge) bridge->publishState();
                }
                printJson(hit);
            }
        }
        if (trigger.needsSpectrum()) {
            const auto fired = trigger.observe(orderedTrace, actualCenter, actualRate);
            if (!fired.isEmpty()) printJson(fired);
        }
        if (zeroSpan.running()) {
            const auto observed = zeroSpan.observe(orderedTrace, actualCenter, actualRate);
            if (!observed.isEmpty()) printJson(observed);
        }
        if (tracesEnabled && !orderedTrace.empty()) {
            if (traceBinIndices.size() != orderedTrace.size()) {
                traceBinIndices.resize(orderedTrace.size());
                for (std::size_t i = 0; i < traceBinIndices.size(); ++i) traceBinIndices[i] = static_cast<float>(i);
            }
            accumulatedTraces.update(traceBinIndices, orderedTrace); ++accumulatedFrames;
        }
        if (bridge && !result.display.empty()) bridge->sendSpectrum(result.display, actualCenter, actualRate, listeningHz,
            audioBandwidthHz, modulation, options.inputMode, options.fftSize);
        if (spectrumRecorder.isRecording() && !orderedTrace.empty()) {
            SpectrumFrameRecord frame;
            frame.utcMs = result.utcMs;
            frame.centerFrequency = actualCenter;
            frame.minFrequency = actualCenter - actualRate * 0.5;
            frame.maxFrequency = actualCenter + actualRate * 0.5;
            frame.fftLength = options.fftSize; frame.magnitudes = orderedTrace;
            if (!spectrumRecorder.appendFrameRecord(frame)) {
                fail(QStringLiteral("Spectrum frame recording write failed")); exitCode = 1; stop = true;
            }
        }
    };
    if (options.webPort && !web.start(options.webBind, options.webPort)) return fail(web.statusText());
    while (!stop && !interrupted) {
        pollSpectrum();
        if (fileAtEof && (!spectrum || !spectrum->busy()) && runtimeRecorder.active())
            runtimeRecorder.command({{"operation", "stop"}}, runtimeSettings());
        for (const auto &notification : runtimeRecorder.takeNotifications()) printJson(notification.toObject());
        if (web.isListening()) {
            RadioSettings webSettings;
            webSettings.inputMode = options.inputMode; webSettings.sampleRate = actualRate;
            webSettings.centerFrequency = webSettings.actualFrequency = actualCenter;
            webSettings.listeningFrequency = listeningHz; webSettings.bandwidth = audioBandwidthHz;
            webSettings.modulationType = modulation;
            web.setSourceState(webSettings, receiverRunning);
        }
        graph.poll();
        if (!receiverRunning) {
            const QJsonObject completed = trigger.submitIq(nullptr, 0);
            if (!completed.isEmpty()) printJson(completed);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(10);
            continue;
        }
        if (scanner.running() && vfos.hasListeners()) {
            scanner.stop();
            printJson({{QStringLiteral("event"), QStringLiteral("scanStopped")},
                       {QStringLiteral("reason"), QStringLiteral("Additional VFO PCM client connected")}});
        }
        double scanCenter = 0.0;
        if (scanner.takeTune(&scanCenter)) {
            const int retuneResult = device.frequency(scanCenter, &actualCenter);
            if (retuneResult != FOBOS_ERR_OK) {
                scanner.stop();
                printJson({{QStringLiteral("event"), QStringLiteral("scanError")},
                           {QStringLiteral("frequencyHz"), scanCenter},
                           {QStringLiteral("error"), device.errorText(retuneResult)}});
            } else {
                scanner.tuned();
                tuneSettlingSamples = static_cast<quint64>(actualRate * 0.10);
                discontinuity = true;
                latestSpectrum = {};
                if (spectrum) spectrum->reset();
                multi.clearPrimarySpectrum();
                orderedTrace.clear();
                accumulatedTraces.clear(); accumulatedFrames = 0;
                listeningHz = actualCenter;
                if (audio) {
                    audio->setCenter(actualCenter);
                    audio->setListening(actualCenter);
                }
                modules.setListeningHz(listeningHz);
                modules.reset();
                vfos.setSource(actualCenter, actualRate);
                if (bridge) bridge->publishState();
            }
        }
        unsigned int count = 0;
        code = device.read(block.data(), &count);
        if (code != FOBOS_ERR_OK) {
            if (runtimeRecorder.needsIq()) runtimeRecorder.submitIq(nullptr, 0, runtimeSettings(), true);
            printJson(receiverError(device, "read", code)); exitCode = 1; break;
        }
        count = (std::min)(count, static_cast<unsigned int>(options.blockSamples));
        if (count && options.backend == BackendKind::Virtual && device.virtualSource.takeDiscontinuity()) {
            if (gnss.ready() && gnss.status().value("sourceSamples").toString() != QStringLiteral("0"))
                gnss.submitIq(nullptr, 0, actualRate, actualCenter, true);
            if (samples && runtimeRecorder.needsIq()) runtimeRecorder.submitIq(nullptr, 0, runtimeSettings(), true);
            IqBuffer::clear();
            if (spectrum) spectrum->reset();
            if (samples && (capture.isOpen() || channelRecorder.isRecording() || fullIq8Recorder.isRecording())) {
                fail(QStringLiteral("Virtual IQ discontinuity during continuous IQ recording; capture stopped to avoid silently joining missing samples"));
                exitCode = 1; break;
            }
            discontinuity = true;
            modules.reset();
            if (audio) audio->setSampleRate(actualRate);
            vfos.setSource(actualCenter, actualRate);
        }
        if (!count) {
            if (options.backend == BackendKind::File && device.replayFinished) {
                fileAtEof = true;
                if (gnss.ready()) gnss.command({{"operation", "eof"}}, actualRate, actualCenter);
                if (bridge) {
                    device.stop();
                    receiverRunning = false;
                    bridge->publishState();
                } else break;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            continue;
        }
        samples += count;
        const unsigned int skip = static_cast<unsigned int>((std::min)(tuneSettlingSamples, quint64(count)));
        tuneSettlingSamples -= skip;
        discardedTuneSamples += skip;
        const float *published = block.data() + static_cast<std::size_t>(skip) * 2;
        const unsigned int publishedCount = count - skip;
        if (!publishedCount) { QCoreApplication::processEvents(QEventLoop::AllEvents, 1); continue; }
        if (gnss.ready()) gnss.submitIq(published, static_cast<int>(publishedCount), actualRate, actualCenter);
        if (runtimeRecorder.needsIq()) runtimeRecorder.submitIq(published, publishedCount, runtimeSettings());
        if (!researchCapture.isEmpty()) {
            QJsonObject request = researchCapture;
            researchCapture = {};
            const auto countValue = request.value("samples");
            const int requested = countValue.toInt(4096);
            if ((!countValue.isUndefined() && (!countValue.isDouble() || countValue.toDouble() != requested)) ||
                requested < 16 || requested > CliResearchTools::MaximumIqSamples) {
                printJson({{"event", "researchCapture"}, {"ok", false}, {"error", "samples must be an integer in 16..65536"}});
            } else {
                const int take = (std::min)(requested, static_cast<int>(publishedCount));
                QJsonArray values;
                for (int i = 0; i < take * 2; ++i) values.append(published[i]);
                request.insert("iq", values);
                request.insert("metadata", QJsonObject{{"source", primaryBackend}, {"sampleRateHz", actualRate},
                    {"centerHz", actualCenter}, {"amplitudeUnit", "normalized"}});
                auto event = research.command(request); event.insert("event", "researchCapture");
                event.insert("capturedSamples", take); printJson(event);
            }
        }
        const QJsonObject qualityResult = quality.submit(published, static_cast<int>(publishedCount), actualRate);
        if (!qualityResult.isEmpty()) printJson(qualityResult);
        if (graph.running()) {
            graph.updateSource(actualCenter, actualRate);
            graph.publishIq(published, publishedCount);
        }
        web.submitIq(published, publishedCount, graph.running());
        const QJsonObject triggerWrite = trigger.submitIq(published, static_cast<int>(publishedCount));
        if (!triggerWrite.isEmpty()) printJson(triggerWrite);
        if (capture.isOpen()) {
            RecordingFormatCore::EncodedIq bytes;
            if (!RecordingFormatCore::encodeIq(published, static_cast<std::size_t>(publishedCount) * 2,
                    RecordingFormatCore::IqEncoding::Cf32Le, quantizedIq, bytes, &recordError) ||
                !RecordingFormatCore::checkedAppend(capture, bytes.data, bytes.size, &recordError)) {
                fail(recordError); exitCode = 1; break;
            }
        }
        if (fullIq8Recorder.isRecording()) {
            RecordingFormatCore::EncodedIq bytes;
            if (!RecordingFormatCore::encodeIq(published, static_cast<std::size_t>(publishedCount) * 2,
                    RecordingFormatCore::IqEncoding::Ci8, quantizedIq, bytes, &recordError,
                    RecordingFormatCore::IqNonFinitePolicy::Zero)) {
                fail(recordError); exitCode = 1; break;
            }
            if (!fullIq8Recorder.appendFrame(quantizedIq, actualRate, static_cast<int>(publishedCount),
                                             QDateTime::currentMSecsSinceEpoch())) {
                fail(QStringLiteral("Full CI8/SigMF recording write failed")); exitCode = 1; break;
            }
        }
        if (!options.channelIqBase.isEmpty()) {
            recordSettings.listeningFrequency = listeningHz;
            recordSettings.bandwidth = audioBandwidthHz;
            const auto channelResult = recordChannelizer.processFloatIq(
                published, static_cast<std::size_t>(publishedCount) * 2,
                recordSettings, channelIq, false);
            if (!channelResult.valid) {
                fail(QStringLiteral("Channel IQ recording rejected the selected band")); exitCode = 1; break;
            }
            if (!channelRecorder.isRecording() &&
                !channelRecorder.start(options.channelIqBase,
                                       SpectrumIqEventRecorder::Mode::ChannelIqS16,
                                       recordSettings, channelResult.outputRate, 0, 0, &recordError)) {
                fail(recordError); exitCode = 1; break;
            }
            if (!channelIq.empty()) {
                RecordingFormatCore::EncodedIq bytes;
                if (!RecordingFormatCore::encodeIq(channelIq.data(), channelIq.size(),
                        RecordingFormatCore::IqEncoding::Ci16Le, quantizedIq, bytes, &recordError,
                        RecordingFormatCore::IqNonFinitePolicy::Zero)) {
                    fail(recordError); exitCode = 1; break;
                }
                if (!channelRecorder.appendFrame(quantizedIq, channelResult.outputRate,
                                                 channelResult.outputSamples,
                                                 QDateTime::currentMSecsSinceEpoch())) {
                    fail(QStringLiteral("Channel IQ WAV recording write failed")); exitCode = 1; break;
                }
            }
        }
        if (audio && (!options.audioFile.isEmpty() || options.audioStdout || options.audioPlay || runtimeRecorder.needsAudio() ||
                      textDecoder.enabled() || modules.needsAudio() || modules.needsChannelIq() ||
                      (bridge && bridge->peerCount() > 0 && bridge->processingMode() == 0 &&
                       networkAudioEnabled))) {
            QString audioError;
            const bool decodePcm = !options.audioFile.isEmpty() || options.audioStdout || runtimeRecorder.needsAudio() ||
                (options.audioPlay && !modules.needsChannelIq()) || textDecoder.enabled() ||
                modules.needsAudio() || (bridge && bridge->peerCount() > 0 &&
                bridge->processingMode() == 0 && networkAudioEnabled);
            if (!audio->process(published, publishedCount, &audioError, decodePcm)) {
                fail(audioError); exitCode = 1; break;
            }
        }
        if (vfos.hasListeners()) vfos.process(published, publishedCount);
        if (bridge && bridge->peerCount() > 0 && bridge->processingMode() != 0) {
            RadioSettings settings;
            settings.inputMode = INPUT_RF;
            settings.sampleRate = actualRate;
            settings.centerFrequency = actualCenter;
            settings.listeningFrequency = listeningHz;
            settings.bandwidth = audioBandwidthHz;
            settings.modulationType = modulation;
            bridge->sendIq(published, static_cast<int>(publishedCount), settings);
        }
        const bool needsSpectrum = spectrum && (calibration.capturing() || options.spectrum || multi.hasRunningAuxiliary() || tracesEnabled || scanner.running() || trigger.needsSpectrum() || zeroSpan.running() || modules.needsSpectrum() || spectrumRecorder.isRecording() || runtimeRecorder.needsSpectrum() ||
                         (bridge && bridge->peerCount() > 0)) &&
            (calibration.capturing() || options.spectrum || multi.hasRunningAuxiliary() || tracesEnabled || scanner.running() || trigger.needsSpectrum() || zeroSpan.running() || modules.needsSpectrum() || spectrumRecorder.isRecording() || runtimeRecorder.needsSpectrum() || !bridge || bridge->processingMode() != 2);
        if (needsSpectrum) {
            const int interval = spectrumRecorder.isRecording() || runtimeRecorder.needsSpectrum() ? options.spectrumIntervalMs :
                calibration.capturing() || multi.hasRunningAuxiliary() || tracesEnabled || scanner.running() || trigger.needsSpectrum() || zeroSpan.running() || modules.needsSpectrum() || (bridge && bridge->peerCount() > 0) ? 50 : 1000;
            spectrum->submit(published, static_cast<int>(publishedCount), actualRate, actualCenter, interval,
                calibration.capturing() || options.spectrum || multi.hasRunningAuxiliary() || tracesEnabled || trigger.needsSpectrum() ||
                zeroSpan.running() || modules.needsSpectrum() || spectrumRecorder.isRecording() || runtimeRecorder.needsSpectrum());
        } else if (spectrum) spectrum->reset();
        if (!options.virtualHost.isEmpty()) {
            if (virtualSink.state() == QAbstractSocket::UnconnectedState && reconnect.elapsed() >= 1000) {
                virtualSink.connectToHost(options.virtualHost, options.virtualPort);
                reconnect.restart();
            }
            if (virtualSink.state() == QAbstractSocket::ConnectedState) {
                const quint32 virtualRate = static_cast<quint32>(std::llround(actualRate));
                if (!virtualSampleEpochNs || discontinuity) {
                    virtualSampleEpochNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
                    virtualSampleCursor = 0;
                }
                VirtualSignalFrame frame;
                frame.sequence = sequence++;
                frame.timestampNs = virtualSampleEpochNs + static_cast<qint64>(
                    std::llround(static_cast<long double>(virtualSampleCursor) * 1000000000.0L / virtualRate));
                frame.streamId = backendName(options.backend) + QStringLiteral("-cli");
                if (!options.virtualNodeId.isEmpty())
                    frame.metadata.insert(QStringLiteral("hubNodeId"), options.virtualNodeId);
                frame.protocolId = QStringLiteral("iq");
                frame.payloadType = VirtualSignalPayloadType::ComplexFloat32;
                frame.sampleRate = virtualRate;
                frame.centerFrequencyHz = static_cast<qint64>(std::llround(actualCenter));
                frame.discontinuity = discontinuity;
                discontinuity = false;
                frame.payload = QByteArray(reinterpret_cast<const char *>(published), static_cast<int>(publishedCount * 8));
                QString error;
                const QByteArray wire = VirtualSignalStreamParser::wrap(frame, &error);
                if (wire.isEmpty() || virtualSink.bytesToWrite() > 4 * 1024 * 1024) {
                    ++droppedVirtualFrames; discontinuity = true;
                } else {
                    virtualSink.write(wire);
                    virtualSampleCursor += static_cast<qint64>(publishedCount);
                }
            }
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        if (report.elapsed() >= 1000) {
            const qint64 windowMs = rateWindow.restart();
            recentReadRateMsps = windowMs > 0
                ? (samples - previousRateSamples) / (windowMs * 1000.0) : 0.0;
            previousRateSamples = samples;
            QJsonObject progress{{QStringLiteral("event"), QStringLiteral("progress")},
                                 {QStringLiteral("samples"), static_cast<double>(samples)},
                                 {QStringLiteral("iqSeconds"), samples / actualRate},
                                 {QStringLiteral("elapsedSeconds"), elapsed.elapsed() / 1000.0},
                                 {QStringLiteral("readRateMsps"), elapsed.elapsed() > 0
                                      ? samples / (elapsed.elapsed() * 1000.0) : 0.0},
                                 {QStringLiteral("recentReadRateMsps"), recentReadRateMsps},
                                 {QStringLiteral("droppedVirtualFrames"), static_cast<double>(droppedVirtualFrames)}};
            if (options.spectrum && !latestSpectrum.isEmpty())
                progress.insert(QStringLiteral("spectrum"), latestSpectrum);
            if (bridge) progress.insert(QStringLiteral("network"), bridge->metrics());
            if (options.spectrum || options.progress) printJson(progress);
            report.restart();
        }
        if (options.seconds > 0 && samples >= static_cast<quint64>(actualRate * options.seconds)) break;
    }
    if (receiverRunning) device.stop();
    control.close();
    while (spectrum && spectrum->busy()) {
        pollSpectrum();
        QThread::msleep(1);
    }
    runtimeRecorder.shutdown();
    for (const auto &notification : runtimeRecorder.takeNotifications()) printJson(notification.toObject());
    if (gnss.ready()) gnss.command({{"operation", "eof"}}, actualRate, actualCenter);
    QElapsedTimer gnssDrain; gnssDrain.start();
    while (gnss.status().value("running").toBool() && gnssDrain.elapsed() < 15000 && !interrupted) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(5);
    }
    gnss.stop();
    web.stop();
    if (capture.isOpen()) capture.close();
    spectrumRecorder.stop();
    channelRecorder.stop();
    fullIq8Recorder.stop();
    if (audio) {
        QString audioError;
        if (!audio->close(&audioError)) { fail(audioError); exitCode = 1; }
    }
    localAudioPlayer.stop();
    printJson({{QStringLiteral("event"), QStringLiteral("stopped")},
               {QStringLiteral("samples"), static_cast<double>(samples)},
               {QStringLiteral("fftWorker"), spectrum ? spectrum->status() : QJsonObject{}},
               {QStringLiteral("spectrum"), latestSpectrum},
               {QStringLiteral("iqSeconds"), samples / actualRate},
               {QStringLiteral("elapsedSeconds"), elapsed.elapsed() / 1000.0}});
    return exitCode;
}
}

int runLocalModule(QCoreApplication &app, const QString &name, int argumentOffset) {
    const QString executable = QCoreApplication::applicationDirPath() +
        QLatin1Char('/') + name +
#ifdef _WIN32
        QLatin1String(".exe");
#else
        QString();
#endif
    if (!QFileInfo::exists(executable))
        return fail(QStringLiteral("%1 is not installed beside ObriiCli").arg(name));
    QProcess process;
    process.setProgram(executable);
    process.setArguments(app.arguments().mid(argumentOffset));
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.start();
    if (!process.waitForStarted(3000)) return fail(process.errorString());
    interrupted = 0;
    std::signal(SIGINT, interruptHandler);
    std::signal(SIGTERM, interruptHandler);
    while (!process.waitForFinished(200)) {
        if (!interrupted) continue;
        if (process.waitForFinished(1500))
            return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : 1;
        process.terminate();
        if (!process.waitForFinished(1000)) {
            process.kill();
            process.waitForFinished(1000);
        }
        return 130;
    }
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : 1;
}

int runComputeService(QCoreApplication &app, const QString &path, double duration) {
    QFile file(path);
    QJsonParseError error;
    const QJsonDocument document = file.open(QIODevice::ReadOnly) && file.size() <= 65536
        ? QJsonDocument::fromJson(file.readAll(), &error) : QJsonDocument{};
    if (!document.isObject() || error.error != QJsonParseError::NoError)
        return fail(QStringLiteral("Expected compute endpoint JSON object, maximum 64 KiB"));
    QJsonObject request = document.object();
    request.insert("operation", "compute"); request.insert("computeOperation", "start");
    CliDspGraph compute;
    const auto reply = compute.command(request, 0, 0, false);
    if (!reply.value("ok").toBool()) { printJson(reply); return 1; }
    interrupted = 0;
    std::signal(SIGINT, interruptHandler); std::signal(SIGTERM, interruptHandler);
    QElapsedTimer elapsed; elapsed.start();
    QJsonObject previous;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        const QJsonObject state = compute.status().value("compute").toObject();
        if (state != previous) {
            QJsonObject event = state; event.insert("event", "computeStatus"); printJson(event); previous = state;
        }
        if (interrupted || (duration > 0 && elapsed.elapsed() >= duration * 1000)) app.quit();
        else if (elapsed.elapsed() > 1000 && !state.value("running").toBool()) app.exit(1);
    });
    timer.start(100);
    const int result = app.exec();
    compute.command({{"operation", "compute"}, {"computeOperation", "stop"}}, 0, 0, false);
    return result;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ObriiCli"));
    if (app.arguments().size() >= 3 && app.arguments()[1] == QStringLiteral("module") &&
        app.arguments()[2] == QStringLiteral("gnss"))
        return runLocalModule(app, QStringLiteral("ObriiCliGnssModule"), 3);
    if (app.arguments().size() >= 3 && app.arguments()[1] == QStringLiteral("module") &&
        app.arguments()[2] == QStringLiteral("tx"))
        return runLocalModule(app, QStringLiteral("ObriiCliTxModule"), 2);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Headless SDR receiver. Commands: devices, rates, doctor [REQUEST.json], run [--serve|--preset FILE], client, research FILE.json, calibration FILE.json, preset list DIR|inspect|summary FILE, experiment inspect|summary|results|trace|measure|zero FILE, module list|start|status|stop|snapshot|tx|gnss, control commands|audit|request FILE.json|agent start [PORT]|agent status|agent control on/off|agent stop|status|metrics|record|spectrum|trace|traces|measure|tone|digital|filters|vfo|scan|trigger|quality|zero|multisdr|graph|gnss|research|preset|experiment|mode|demod|bandwidth|audio|start|stop|quit|tune|listen|gain, view open|set|status|snapshot|close."));
    parser.addHelpOption();
    const auto option = [&parser](const QString &name, const QString &description, const QString &value, const QString &fallback) {
        parser.addOption(QCommandLineOption(name, description, value, fallback));
    };
    option(QStringLiteral("device"), QStringLiteral("Receiver device index"), QStringLiteral("index"), QStringLiteral("0"));
    option(QStringLiteral("backend"), QStringLiteral("Receiver backend: fobos, hackrf, soapy, uhd, rtl, rtl-tcp, file, virtual"),
           QStringLiteral("backend"), QStringLiteral("fobos"));
    option(QStringLiteral("iq-file"), QStringLiteral("CF32LE recording for --backend file"),
           QStringLiteral("path"), QString());
    option(QStringLiteral("rtl-host"), QStringLiteral("rtl_tcp server host"),
           QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    option(QStringLiteral("rtl-port"), QStringLiteral("rtl_tcp server port"),
           QStringLiteral("port"), QStringLiteral("1234"));
    option(QStringLiteral("api"), QStringLiteral("Fobos API: auto, standard, agile"), QStringLiteral("api"), QStringLiteral("auto"));
    option(QStringLiteral("gain-db"), QStringLiteral("Soapy/UHD/RTL/HackRF RX gain in dB; RTL zero uses auto gain"),
           QStringLiteral("dB"), QStringLiteral("0"));
    option(QStringLiteral("rx-channel"), QStringLiteral("Soapy/UHD RX channel index"),
           QStringLiteral("index"), QStringLiteral("0"));
    option(QStringLiteral("antenna"), QStringLiteral("UHD RX antenna name"),
           QStringLiteral("name"), QString());
    option(QStringLiteral("fpga-image"), QStringLiteral("Optional UHD FPGA image path"),
           QStringLiteral("path"), QString());
    option(QStringLiteral("input"), QStringLiteral("Input: auto, rf, hf1, hf2, hf-combined"), QStringLiteral("mode"), QStringLiteral("auto"));
    option(QStringLiteral("freq"), QStringLiteral("RF center or HF listening frequency, Hz or suffix k/M/G"), QStringLiteral("frequency"), QStringLiteral("100M"));
    option(QStringLiteral("rate"), QStringLiteral("Requested sample rate, Hz or suffix k/M/G"), QStringLiteral("rate"), QStringLiteral("8M"));
    option(QStringLiteral("lna"), QStringLiteral("LNA gain 0..2"), QStringLiteral("gain"), QStringLiteral("0"));
    option(QStringLiteral("vga"), QStringLiteral("VGA gain 0..15"), QStringLiteral("gain"), QStringLiteral("0"));
    option(QStringLiteral("seconds"), QStringLiteral("Capture duration; 0 runs until stop"), QStringLiteral("seconds"), QStringLiteral("0"));
    option(QStringLiteral("block-samples"), QStringLiteral("Complex IQ samples per read, 16384..65536"), QStringLiteral("count"), QStringLiteral("65536"));
    option(QStringLiteral("output"), QStringLiteral("Write full CF32LE IQ and JSON metadata"), QStringLiteral("path"), QString());
    option(QStringLiteral("doctor-dir"), QStringLiteral("Doctor inspection directory (no device access)"), QStringLiteral("directory"), QString());
    option(QStringLiteral("doctor-port"), QStringLiteral("Explicit opt-in loopback TCP bind probe; may be repeated"), QStringLiteral("port"), QString());
    parser.addOption(QCommandLineOption(QStringLiteral("doctor-write-test"), QStringLiteral("Explicit temporary write/verify/remove probe in --doctor-dir")));
    option(QStringLiteral("audio-file"), QStringLiteral("Write demodulated 48 kHz mono WAV"), QStringLiteral("path"), QString());
    option(QStringLiteral("audio-filters"), QStringLiteral("Load GUI-compatible audio filter chain JSON"), QStringLiteral("path"), QString());
    option(QStringLiteral("decode"), QStringLiteral("Decode PCM text: off, cw, psk31"), QStringLiteral("mode"), QStringLiteral("off"));
    option(QStringLiteral("decode-tone"), QStringLiteral("CW/PSK31 audio tone frequency"), QStringLiteral("Hz"), QString());
    option(QStringLiteral("cw-wpm"), QStringLiteral("CW initial words per minute, 5..60"), QStringLiteral("wpm"), QStringLiteral("18"));
    option(QStringLiteral("cw-selectivity"), QStringLiteral("CW selectivity, 1..10"), QStringLiteral("level"), QStringLiteral("5"));
    option(QStringLiteral("cw-squelch-db"), QStringLiteral("CW tone-over-noise gate, 0 disables; 0..30 dB"), QStringLiteral("dB"), QStringLiteral("8"));
    option(QStringLiteral("cw-alphabet"), QStringLiteral("CW alphabet: latin, ukrainian, both"), QStringLiteral("alphabet"), QStringLiteral("latin"));
    option(QStringLiteral("psk-squelch"), QStringLiteral("BPSK31 squelch, 1..100"), QStringLiteral("percent"), QStringLiteral("50"));
    option(QStringLiteral("psk-afc"), QStringLiteral("BPSK31 AFC: on, off"), QStringLiteral("state"), QStringLiteral("on"));
    option(QStringLiteral("spectrum-frames"), QStringLiteral("Record replayable Obrii spectrum frames"), QStringLiteral("path"), QString());
    option(QStringLiteral("channel-iq"), QStringLiteral("Record demodulator-channel IQ as stereo PCM16 WAV; path is a base name"), QStringLiteral("base"), QString());
    option(QStringLiteral("full-iq8"), QStringLiteral("Record full-band IQ as CI8 with SigMF metadata; path is a base name"), QStringLiteral("base"), QString());
    option(QStringLiteral("spectrum-ms"), QStringLiteral("Spectrum recording interval, 20..10000 ms"), QStringLiteral("ms"), QStringLiteral("50"));
    option(QStringLiteral("mode"), QStringLiteral("Audio mode: am, nfm, wfm, usb, lsb, cw"), QStringLiteral("mode"), QStringLiteral("nfm"));
    option(QStringLiteral("listen"), QStringLiteral("Audio listening frequency; default follows RX center"), QStringLiteral("frequency"), QString());
    option(QStringLiteral("bandwidth"), QStringLiteral("Audio channel bandwidth; default depends on mode"), QStringLiteral("hertz"), QString());
    parser.addOption(QCommandLineOption(QStringLiteral("spectrum"),
                                        QStringLiteral("Report RF peak frequencies and noise floor once per second")));
    parser.addOption(QCommandLineOption(QStringLiteral("progress"),
                                        QStringLiteral("Print one JSON progress line per second in server/client mode")));
    option(QStringLiteral("fft"), QStringLiteral("Diagnostic/server FFT bins, 512..50000000"),
           QStringLiteral("bins"), QStringLiteral("4096"));
    option(QStringLiteral("hz-per-bin"), QStringLiteral("Choose FFT length from source rate / Hz per bin; excludes --fft"),
           QStringLiteral("Hz"), QString());
    parser.addOption(QCommandLineOption(QStringLiteral("serve"),
                                        QStringLiteral("Serve standard Obrii GUI network clients (server-side processing)")));
    parser.addOption(QCommandLineOption(QStringLiteral("no-audio"),
                                        QStringLiteral("Disable network audio until changed by a GUI client")));
    parser.addOption(QCommandLineOption(QStringLiteral("start"),
                                        QStringLiteral("In client mode, request remote RX start after connecting")));
    option(QStringLiteral("bind"), QStringLiteral("GUI server listen address; use 0.0.0.0 only on a trusted LAN"),
           QStringLiteral("address"), QStringLiteral("127.0.0.1"));
    option(QStringLiteral("network-port"), QStringLiteral("GUI server TCP port"),
           QStringLiteral("port"), QStringLiteral("21090"));
    option(QStringLiteral("web-port"), QStringLiteral("Enable browser multi-user VFO service on this port; 0=off, trusted LAN only"), QStringLiteral("port"), QStringLiteral("0"));
    option(QStringLiteral("web-bind"), QStringLiteral("Browser listener numeric address"), QStringLiteral("IP"), QStringLiteral("127.0.0.1"));
    parser.addOption(QCommandLineOption(QStringLiteral("audio-stdout"),
                                        QStringLiteral("Write raw PCM16LE mono 48 kHz to stdout; status goes to stderr")));
    parser.addOption(QCommandLineOption(QStringLiteral("module-voice-stdout"),
                                        QStringLiteral("Write decoded DMR voice PCM16LE mono 48 kHz to stdout; events go to stderr")));
    parser.addOption(QCommandLineOption(QStringLiteral("audio-play"),
                                        QStringLiteral("Play demodulated PCM on the default local audio device")));
    option(QStringLiteral("virtual-host"), QStringLiteral("Send IQ to a listening Obrii virtual RX"), QStringLiteral("host"), QString());
    option(QStringLiteral("virtual-port"), QStringLiteral("Virtual RX TCP port"), QStringLiteral("port"), QStringLiteral("17661"));
    option(QStringLiteral("virtual-node-id"), QStringLiteral("Stable virtual IQ publisher ID for hub routing"), QStringLiteral("id"), QString());
    option(QStringLiteral("virtual-name"), QStringLiteral("Local virtual IQ channel name"), QStringLiteral("name"), QStringLiteral("obrii-virtual-iq"));
    option(QStringLiteral("virtual-rx-transport"), QStringLiteral("Virtual RX listener: local or tcp"), QStringLiteral("mode"), QStringLiteral("local"));
    option(QStringLiteral("virtual-rx-bind"), QStringLiteral("Virtual RX TCP bind address; trusted LAN only"), QStringLiteral("IP"), QStringLiteral("127.0.0.1"));
    option(QStringLiteral("virtual-rx-port"), QStringLiteral("Virtual RX TCP listen port"), QStringLiteral("port"), QStringLiteral("17661"));
    option(QStringLiteral("virtual-rx-queue-mib"), QStringLiteral("Virtual RX bounded IQ queue, 1..64 MiB"), QStringLiteral("MiB"), QStringLiteral("16"));
    option(QStringLiteral("tx-sink"), QStringLiteral("On-demand TX output: file, virtual, hackrf or uhd"), QStringLiteral("sink"), QString());
    parser.addOption(QCommandLineOption(QStringLiteral("rf-confirm"),
                                        QStringLiteral("Explicitly authorize RF transmission from CLI")));
    option(QStringLiteral("tx-device"), QStringLiteral("HackRF TX device index"), QStringLiteral("index"), QStringLiteral("0"));
    option(QStringLiteral("device-rate"), QStringLiteral("HackRF hardware sample rate"), QStringLiteral("Hz"), QStringLiteral("2M"));
    option(QStringLiteral("tx-vga-db"), QStringLiteral("HackRF TX VGA gain, 0..47 dB"), QStringLiteral("dB"), QStringLiteral("0"));
    option(QStringLiteral("tx-rf-bandwidth"), QStringLiteral("HackRF RF filter bandwidth; 0=auto"), QStringLiteral("Hz"), QStringLiteral("0"));
    option(QStringLiteral("tx-channel"), QStringLiteral("UHD TX channel index"), QStringLiteral("index"), QStringLiteral("0"));
    option(QStringLiteral("tx-antenna"), QStringLiteral("UHD TX antenna"), QStringLiteral("name"), QString());
    option(QStringLiteral("tx-fpga-image"), QStringLiteral("UHD FPGA image"), QStringLiteral("path"), QString());
    option(QStringLiteral("tx-gain-db"), QStringLiteral("UHD TX gain in dB"), QStringLiteral("dB"), QStringLiteral("0"));
    option(QStringLiteral("tx-tone"), QStringLiteral("TX audio/baseband tone offset"), QStringLiteral("Hz"), QString());
    option(QStringLiteral("tx-level"), QStringLiteral("TX IQ amplitude, 0..0.95"), QStringLiteral("level"), QStringLiteral("0.5"));
    option(QStringLiteral("pcm-file"), QStringLiteral("DMR TX raw mono PCM16LE input at --pcm-rate"), QStringLiteral("path"), QString());
    option(QStringLiteral("pcm-rate"), QStringLiteral("DMR TX PCM input sample rate"), QStringLiteral("Hz"), QStringLiteral("48000"));
    option(QStringLiteral("lab-preset"), QStringLiteral("Signal-lab JSON preset for on-demand TX"), QStringLiteral("path"), QString());
    option(QStringLiteral("wav-file"), QStringLiteral("Audio TX RIFF/WAVE input"), QStringLiteral("path"), QString());
    option(QStringLiteral("image-file"), QStringLiteral("SSTV TX image input"), QStringLiteral("path"), QString());
    option(QStringLiteral("sstv-mode"), QStringLiteral("SSTV robot36|robot72|martin-m1|martin-m2"), QStringLiteral("mode"), QStringLiteral("robot36"));
    option(QStringLiteral("voice-mode"), QStringLiteral("Audio/SSTV AM, FM or SSB modulation"), QStringLiteral("mode"), QStringLiteral("nfm"));
    option(QStringLiteral("deviation"), QStringLiteral("FM peak deviation in Hz"), QStringLiteral("Hz"), QStringLiteral("2500"));
    option(QStringLiteral("signal-bandwidth"), QStringLiteral("Full TX signal bandwidth in Hz; 0=unfiltered"), QStringLiteral("Hz"), QStringLiteral("0"));
    option(QStringLiteral("ax25-source"), QStringLiteral("AX.25 TX source callsign-SSID"), QStringLiteral("call"), QString());
    option(QStringLiteral("ax25-destination"), QStringLiteral("AX.25 TX destination callsign-SSID"), QStringLiteral("call"), QStringLiteral("APRS"));
    option(QStringLiteral("ax25-path"), QStringLiteral("AX.25 TX digipeater path"), QStringLiteral("calls"), QString());
    option(QStringLiteral("dmr-cc"), QStringLiteral("DMR TX color code"), QStringLiteral("code"), QStringLiteral("1"));
    option(QStringLiteral("dmr-slot"), QStringLiteral("DMR TX time slot"), QStringLiteral("slot"), QStringLiteral("1"));
    option(QStringLiteral("dmr-source"), QStringLiteral("DMR TX source ID"), QStringLiteral("id"), QStringLiteral("1"));
    option(QStringLiteral("dmr-target"), QStringLiteral("DMR TX destination ID"), QStringLiteral("id"), QStringLiteral("9"));
    parser.addOption(QCommandLineOption(QStringLiteral("dmr-private"), QStringLiteral("DMR TX private call")));
    parser.addOption(QCommandLineOption(QStringLiteral("no-ft8-align"),
                                        QStringLiteral("Do not wait for the next UTC FT8 slot on virtual TX")));
    parser.addOption(QCommandLineOption(QStringLiteral("no-slot-align"),
                                        QStringLiteral("Do not wait for the next FT8/FT4 UTC slot on virtual TX")));
    option(QStringLiteral("control-port"), QStringLiteral("Local-only JSON control port"), QStringLiteral("port"), QStringLiteral("17675"));
    option(QStringLiteral("preset"), QStringLiteral("Restore a CLI experiment preset when starting RX"), QStringLiteral("path"), QString());
    option(QStringLiteral("profile"), QStringLiteral("Explicit decoder profile for module start dmr|rtty|ft8|ft4|m17"), QStringLiteral("path"), QString());
    option(QStringLiteral("host"), QStringLiteral("Host for control command"), QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    option(QStringLiteral("view-id"), QStringLiteral("Independent visual window ID"), QStringLiteral("id"), QStringLiteral("main"));
    option(QStringLiteral("min-hz"), QStringLiteral("View frequency start"), QStringLiteral("frequency"), QString());
    option(QStringLiteral("max-hz"), QStringLiteral("View frequency end"), QStringLiteral("frequency"), QString());
    option(QStringLiteral("min-db"), QStringLiteral("View lower dBFS bound"), QStringLiteral("dBFS"), QString());
    option(QStringLiteral("max-db"), QStringLiteral("View upper dBFS bound"), QStringLiteral("dBFS"), QString());
    parser.addOption(QCommandLineOption(QStringLiteral("paused"), QStringLiteral("Pause a visual window")));
    parser.addOption(QCommandLineOption(QStringLiteral("resume"), QStringLiteral("Resume a visual window")));
    if (!parser.parse(app.arguments())) return fail(parser.errorText());
    QJsonObject selectedPreset;
    if (parser.isSet(QStringLiteral("preset"))) {
        if (parser.positionalArguments().size() != 1 ||
            parser.positionalArguments().front() != QStringLiteral("run"))
            return fail(QStringLiteral("--preset applies only to run"));
        QString profileError;
        if (!readProfile(parser.value(QStringLiteral("preset")), &selectedPreset, &profileError))
            return fail(profileError);
        const QJsonObject source = selectedPreset.value(QStringLiteral("source")).toObject();
        if (source.value(QStringLiteral("iqReplayable")).toBool()) {
            const QFileInfo iq(source.value(QStringLiteral("iqFile")).toString());
            if (!iq.isFile() || static_cast<double>(iq.size()) != source.value(QStringLiteral("iqBytes")).toDouble(-1.0) ||
                iq.lastModified().toUTC().toString(Qt::ISODateWithMs) !=
                    source.value(QStringLiteral("iqModifiedUtc")).toString())
                return fail(QStringLiteral("IQ recording has moved or changed since the experiment was saved"));
        }
        QStringList merged{app.arguments().front()};
        for (const QJsonValue &value : selectedPreset.value(QStringLiteral("launch")).toArray())
            merged.append(value.toString());
        const QStringList original = app.arguments();
        for (int i = 1; i < original.size(); ++i) {
            if (original[i] == QStringLiteral("--preset")) { ++i; continue; }
            if (original[i].startsWith(QStringLiteral("--preset="))) continue;
            merged.append(original[i]);
        }
        if (!parser.parse(merged)) return fail(parser.errorText());
    }
    if (parser.isSet(QStringLiteral("help")) || parser.isSet(QStringLiteral("help-all"))) {
        QTextStream(stdout) << parser.helpText() << Qt::endl;
        return 0;
    }
    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) return fail(QStringLiteral("Missing command. Use --help."));
    if (parser.isSet(QStringLiteral("profile"))) {
        const bool moduleStart = args.size() >= 3 && args[0] == QStringLiteral("module") && args[1] == QStringLiteral("start");
        const bool controlModuleStart = args.size() >= 4 && args[0] == QStringLiteral("control") && args[1] == QStringLiteral("module") && args[2] == QStringLiteral("start");
        if (parser.values(QStringLiteral("profile")).size() != 1 || (!moduleStart && !controlModuleStart))
            return fail(QStringLiteral("Use exactly one --profile with module start dmr|rtty|ft8|ft4|m17"));
    }
    if (args.front() == QStringLiteral("capabilities") && args.size() == 1) {
        printJson(cliCapabilities()); return 0;
    }
    if (args.front() == QStringLiteral("doctor")) {
        if (args.size() > 2) return fail(QStringLiteral("doctor [REQUEST.json] [--doctor-dir DIR --doctor-write-test --doctor-port PORT]"));
        QJsonObject request;
        if (args.size() == 2) {
            if (parser.isSet("doctor-dir") || parser.isSet("doctor-write-test") || parser.isSet("doctor-port"))
                return fail(QStringLiteral("Use either a doctor request file or doctor options, not both"));
            QFile file(args[1]);
            QJsonParseError error;
            const auto document = file.open(QIODevice::ReadOnly) && file.size() <= 65536
                ? QJsonDocument::fromJson(file.readAll(), &error) : QJsonDocument{};
            if (!document.isObject() || error.error != QJsonParseError::NoError)
                return fail(QStringLiteral("Doctor requires a JSON object of at most 64 KiB"));
            request = document.object();
        } else {
            if (parser.isSet("doctor-dir")) request.insert("directory", QFileInfo(parser.value("doctor-dir")).absoluteFilePath());
            if (parser.isSet("doctor-write-test")) request.insert("checkWritableDirectory", true);
            if (parser.isSet("doctor-port")) {
                QJsonArray ports;
                for (const QString &text : parser.values("doctor-port")) {
                    bool okay = false; const int port = text.toInt(&okay);
                    if (!okay || port < 1 || port > 65535) return fail(QStringLiteral("Doctor port must be 1..65535"));
                    ports.append(port);
                }
                request.insert("tcpPorts", ports); request.insert("checkPorts", true);
            }
        }
        const auto report = runCliDoctor(request); printJson(report);
        return report.value("ok").toBool() ? 0 : 1;
    }
    if (args.front() == QStringLiteral("compute") && args.size() == 2) {
        bool valid = false;
        const double seconds = parser.value("seconds").toDouble(&valid);
        if (!valid || !std::isfinite(seconds) || seconds < 0) return fail(QStringLiteral("Invalid compute duration"));
        return runComputeService(app, args[1], seconds);
    }
    if (args.front() == QStringLiteral("calibration") && args.size() == 2) {
        QFile file(args[1]); QJsonParseError error;
        const auto document = file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024
            ? QJsonDocument::fromJson(file.readAll(), &error) : QJsonDocument{};
        if (error.error != QJsonParseError::NoError || (!document.isArray() && !document.isObject()))
            return fail(QStringLiteral("Expected calibration request object or array, maximum 4 MiB"));
        const QJsonArray requests = document.isArray() ? document.array() : QJsonArray{document.object()};
        if (requests.isEmpty() || requests.size() > 64) return fail(QStringLiteral("Calibration batch needs 1..64 requests"));
        CliCalibration calibration;
        for (const auto &value : requests) {
            if (!value.isObject()) return fail(QStringLiteral("Calibration requests must be objects"));
            const auto reply = calibration.command(value.toObject()); printJson(reply);
            if (!reply.value("ok").toBool()) return 1;
            if (calibration.capturing()) return fail(QStringLiteral("Live capture requires a running receiver: use control calibration"));
        }
        return 0;
    }
    if (args[0] == QStringLiteral("research") && args.size() == 2) {
        QFile file(args[1]);
        QJsonParseError error;
        const auto document = file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024
            ? QJsonDocument::fromJson(file.readAll(), &error) : QJsonDocument{};
        if ((!document.isObject() && !document.isArray()) || error.error != QJsonParseError::NoError)
            return fail(QStringLiteral("Expected research request object or array, maximum 4 MiB"));
        const QJsonArray requests = document.isArray() ? document.array() : QJsonArray{document.object()};
        if (requests.isEmpty() || requests.size() > 64) return fail(QStringLiteral("Research batch needs 1..64 requests"));
        CliResearchTools research;
        for (const auto &value : requests) {
            if (!value.isObject()) return fail(QStringLiteral("Research requests must be objects"));
            const auto reply = research.command(value.toObject()); printJson(reply);
            if (!reply.value("ok").toBool()) return 1;
        }
        return 0;
    }
    if (args[0] == QStringLiteral("preset") && args.size() == 3 &&
        args[1] == QStringLiteral("list")) {
        const QDir directory(args[2]);
        if (!directory.exists()) return fail(QStringLiteral("Preset directory does not exist"));
        QJsonArray presets;
        for (const QFileInfo &file : directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            QJsonObject profile;
            QString ignored;
            if (!readProfile(file.absoluteFilePath(), &profile, &ignored)) continue;
            presets.append(QJsonObject{{QStringLiteral("name"), file.fileName()},
                {QStringLiteral("path"), file.absoluteFilePath()},
                {QStringLiteral("kind"), profile.value(QStringLiteral("kind"))},
                {QStringLiteral("savedUtc"), profile.value(QStringLiteral("savedUtc"))},
                {QStringLiteral("source"), profile.value(QStringLiteral("source"))}});
        }
        printJson({{QStringLiteral("ok"), true}, {QStringLiteral("presets"), presets}});
        return 0;
    }
    if ((args[0] == QStringLiteral("preset") || args[0] == QStringLiteral("experiment")) &&
        args.size() >= 3 && args.size() <= 6) {
        QJsonObject profile;
        QString profileError;
        if (!readProfile(args[2], &profile, &profileError)) return fail(profileError);
        if (args[1] == QStringLiteral("inspect") && args.size() == 3) {
            printJson(profile);
        } else if (args[1] == QStringLiteral("results") && args.size() == 3) {
            if (!profile.value(QStringLiteral("results")).isObject())
                return fail(QStringLiteral("Preset has no saved measurement results"));
            printJson({{QStringLiteral("ok"), true},
                {QStringLiteral("savedUtc"), profile.value(QStringLiteral("savedUtc"))},
                {QStringLiteral("results"), profile.value(QStringLiteral("results"))}});
        } else if (args[1] == QStringLiteral("summary") && args.size() == 3) {
            const QJsonObject results = profile.value(QStringLiteral("results")).toObject();
            const QJsonObject traces = results.value(QStringLiteral("traces")).toObject();
            QJsonArray names;
            for (auto it = traces.begin(); it != traces.end(); ++it) names.append(it.key());
            printJson({{QStringLiteral("ok"), true},
                {QStringLiteral("kind"), profile.value(QStringLiteral("kind"))},
                {QStringLiteral("savedUtc"), profile.value(QStringLiteral("savedUtc"))},
                {QStringLiteral("source"), profile.value(QStringLiteral("source"))},
                {QStringLiteral("spectrum"), results.value(QStringLiteral("spectrum"))},
                {QStringLiteral("traces"), names},
                {QStringLiteral("samplesReceived"), results.value(QStringLiteral("samplesReceived"))}});
        } else if (args[1] == QStringLiteral("zero") && args.size() >= 3 && args.size() <= 5) {
            const QJsonObject history = profile.value(QStringLiteral("results")).toObject()
                .value(QStringLiteral("zeroSpanHistory")).toObject();
            if (history.isEmpty()) return fail(QStringLiteral("Preset has no stored zero-span history"));
            bool valid = true;
            const int offset = args.size() >= 4 ? args[3].toInt(&valid) : 0;
            if (!valid || offset < 0) return fail(QStringLiteral("Zero-span offset must be >= 0"));
            const int limit = args.size() == 5 ? args[4].toInt(&valid) : 100;
            if (!valid || limit < 1 || limit > 1000)
                return fail(QStringLiteral("Zero-span limit must be 1..1000"));
            const QJsonArray stored = history.value(QStringLiteral("points")).toArray();
            QJsonArray page;
            for (int i = offset; i < stored.size() && i < offset + limit; ++i) page.append(stored[i]);
            printJson({{QStringLiteral("ok"), true},
                {QStringLiteral("offset"), offset}, {QStringLiteral("totalPoints"), history.value(QStringLiteral("totalPoints"))},
                {QStringLiteral("savedPoints"), stored.size()},
                {QStringLiteral("truncated"), history.value(QStringLiteral("truncated"))},
                {QStringLiteral("points"), page}});
        } else if ((args[1] == QStringLiteral("trace") && args.size() == 4) ||
                   (args[1] == QStringLiteral("measure") &&
                    (args.size() == 4 || args.size() == 6))) {
            const QString source = args[3].toLower();
            const QJsonObject trace = profile.value(QStringLiteral("results")).toObject()
                .value(QStringLiteral("traces")).toObject().value(source).toObject();
            if (trace.isEmpty()) return fail(QStringLiteral("Requested stored trace is unavailable"));
            if (args[1] == QStringLiteral("trace"))
                printJson({{QStringLiteral("ok"), true}, {QStringLiteral("source"), source},
                    {QStringLiteral("trace"), trace}});
            else {
                bool valid = true;
                const double start = args.size() == 6 ? frequency(args[4], &valid) : 0.0;
                if (!valid) return fail(QStringLiteral("Invalid measurement start frequency"));
                const double end = args.size() == 6 ? frequency(args[5], &valid) : 0.0;
                if (!valid) return fail(QStringLiteral("Invalid measurement end frequency"));
                const QJsonObject measured = measureStoredTrace(trace, source, start, end, args.size() == 6);
                printJson(measured);
                return measured.value(QStringLiteral("ok")).toBool() ? 0 : 1;
            }
        } else return fail(QStringLiteral("Use preset list DIR|inspect|summary FILE.json or experiment inspect|summary|results FILE.json|zero FILE.json [OFFSET [LIMIT]]|trace FILE.json SOURCE|measure FILE.json SOURCE [START_HZ END_HZ]"));
        return 0;
    }
    if (args[0] == QStringLiteral("view")) return viewCommand(args, parser);
    if (args[0] == QStringLiteral("module") && args.size() >= 2 &&
        args[1] == QStringLiteral("tx"))
        return runLocalModule(app, QStringLiteral("ObriiCliTxModule"), 2);
    if (args[0] == QStringLiteral("module") && args.size() >= 2 &&
        args[1] == QStringLiteral("m17"))
        return runLocalModule(app, QStringLiteral("ObriiCliM17Module"), 3);
    bool okay = false;
    CliTextDecoder::Settings decoderSettings;
    const QString decodeMode = parser.value(QStringLiteral("decode")).trimmed().toLower();
    if (decodeMode == QStringLiteral("cw")) decoderSettings.mode = CliTextDecoder::Mode::Cw;
    else if (decodeMode == QStringLiteral("psk31")) decoderSettings.mode = CliTextDecoder::Mode::Psk31;
    else if (decodeMode != QStringLiteral("off")) return fail(QStringLiteral("--decode must be off, cw or psk31"));
    const QString tone = parser.isSet(QStringLiteral("decode-tone"))
        ? parser.value(QStringLiteral("decode-tone"))
        : decoderSettings.mode == CliTextDecoder::Mode::Psk31 ? QStringLiteral("1000") : QStringLiteral("700");
    decoderSettings.toneHz = frequency(tone, &okay);
    if (!okay || decoderSettings.toneHz < 100.0 || decoderSettings.toneHz > 10000.0)
        return fail(QStringLiteral("--decode-tone must be 100..10000 Hz"));
    decoderSettings.cwWpm = parser.value(QStringLiteral("cw-wpm")).toInt(&okay);
    if (!okay || decoderSettings.cwWpm < 5 || decoderSettings.cwWpm > 60)
        return fail(QStringLiteral("--cw-wpm must be 5..60"));
    decoderSettings.cwSelectivity = parser.value(QStringLiteral("cw-selectivity")).toInt(&okay);
    if (!okay || decoderSettings.cwSelectivity < 1 || decoderSettings.cwSelectivity > 10)
        return fail(QStringLiteral("--cw-selectivity must be 1..10"));
    decoderSettings.cwSquelchDb = parser.value(QStringLiteral("cw-squelch-db")).toDouble(&okay);
    if (!okay || !std::isfinite(decoderSettings.cwSquelchDb) ||
        decoderSettings.cwSquelchDb < 0.0 || decoderSettings.cwSquelchDb > 30.0)
        return fail(QStringLiteral("--cw-squelch-db must be 0..30"));
    const QString alphabet = parser.value(QStringLiteral("cw-alphabet")).trimmed().toLower();
    decoderSettings.cwAlphabet = alphabet == QStringLiteral("latin") ? 0 :
                                 alphabet == QStringLiteral("ukrainian") ? 1 :
                                 alphabet == QStringLiteral("both") ? 2 : -1;
    if (decoderSettings.cwAlphabet < 0) return fail(QStringLiteral("--cw-alphabet must be latin, ukrainian or both"));
    decoderSettings.pskSquelch = parser.value(QStringLiteral("psk-squelch")).toInt(&okay);
    if (!okay || decoderSettings.pskSquelch < 1 || decoderSettings.pskSquelch > 100)
        return fail(QStringLiteral("--psk-squelch must be 1..100"));
    const QString afc = parser.value(QStringLiteral("psk-afc")).trimmed().toLower();
    if (afc != QStringLiteral("on") && afc != QStringLiteral("off"))
        return fail(QStringLiteral("--psk-afc must be on or off"));
    decoderSettings.pskAfc = afc == QStringLiteral("on");
    const int index = parser.value(QStringLiteral("device")).toInt(&okay);
    if (!okay || index < 0) return fail(QStringLiteral("Invalid device index"));
    const QString apiOption = parser.value(QStringLiteral("api")).trimmed().toLower();
    if (apiOption != QStringLiteral("auto") && apiOption != QStringLiteral("standard") &&
        apiOption != QStringLiteral("agile")) return fail(QStringLiteral("API must be auto, standard or agile"));
    const QString backendOption = parser.value(QStringLiteral("backend")).trimmed().toLower();
    const BackendKind backend = backendOption == QStringLiteral("hackrf") ? BackendKind::HackRf :
                                backendOption == QStringLiteral("soapy") ? BackendKind::Soapy :
                                backendOption == QStringLiteral("uhd") ? BackendKind::Uhd :
                                backendOption == QStringLiteral("rtl") ? BackendKind::RtlNative :
                                backendOption == QStringLiteral("rtl-tcp") ? BackendKind::RtlTcp :
                                backendOption == QStringLiteral("file") ? BackendKind::File :
                                backendOption == QStringLiteral("virtual") ? BackendKind::Virtual :
                                BackendKind::Fobos;
    if (backendOption != QStringLiteral("fobos") && backendOption != QStringLiteral("hackrf") && backendOption != QStringLiteral("soapy") &&
        backendOption != QStringLiteral("uhd") && backendOption != QStringLiteral("rtl") &&
        backendOption != QStringLiteral("rtl-tcp") && backendOption != QStringLiteral("file") && backendOption != QStringLiteral("virtual"))
        return fail(QStringLiteral("Backend must be fobos, hackrf, soapy, uhd, rtl, rtl-tcp, file or virtual"));
    if (backend != BackendKind::Fobos && parser.isSet(QStringLiteral("api")))
        return fail(QStringLiteral("--api selects Fobos Standard/Agile only"));
    if (args[0] == QStringLiteral("devices")) return backend == BackendKind::Fobos
        ? listDevices() : listExternalDevices(backend);
    if (args[0] == QStringLiteral("rates")) return backend == BackendKind::Fobos
        ? listRates(index, chooseApi(apiOption)) : listExternalRates(backend, index);
    const int controlPort = parser.value(QStringLiteral("control-port")).toInt(&okay);
    if (!okay || controlPort < 1 || controlPort > 65535) return fail(QStringLiteral("Invalid control port"));
    if (args[0] == QStringLiteral("control"))
        return remoteCommand(parser.value(QStringLiteral("host")), static_cast<quint16>(controlPort), args.mid(1), parser.value(QStringLiteral("profile")));
    if (args[0] == QStringLiteral("module"))
        return remoteCommand(parser.value(QStringLiteral("host")), static_cast<quint16>(controlPort), args, parser.value(QStringLiteral("profile")));
    if (args[0] == QStringLiteral("client")) {
        if (parser.isSet(QStringLiteral("audio-stdout")) && parser.isSet(QStringLiteral("module-voice-stdout")))
            return fail(QStringLiteral("Choose either --audio-stdout or --module-voice-stdout"));
#if !defined(_WIN32) && !defined(FOBOSAPP_HAS_QT_AUDIO)
        if (parser.isSet(QStringLiteral("audio-play")))
            return fail(QStringLiteral("This CLI build has no Qt Multimedia audio output; use --audio-stdout"));
#endif
        const int remotePort = parser.value(QStringLiteral("network-port")).toInt(&okay);
        if (!okay || remotePort < 1 || remotePort > 65535)
            return fail(QStringLiteral("Invalid remote network port"));
        const double duration = parser.value(QStringLiteral("seconds")).toDouble(&okay);
        if (!okay || !std::isfinite(duration) || duration < 0.0)
            return fail(QStringLiteral("Invalid duration"));
        return runClient(app, parser.value(QStringLiteral("host")),
                         static_cast<quint16>(remotePort), static_cast<quint16>(controlPort),
                         duration, parser.isSet(QStringLiteral("audio-stdout")),
                         parser.isSet(QStringLiteral("module-voice-stdout")),
                         parser.isSet(QStringLiteral("audio-play")),
                         parser.value(QStringLiteral("audio-file")),
                         parser.isSet(QStringLiteral("spectrum")),
                         parser.isSet(QStringLiteral("progress")), parser.isSet(QStringLiteral("start")),
                         decoderSettings, parser.value(QStringLiteral("audio-filters")));
    }
    if (args[0] != QStringLiteral("run")) return fail(QStringLiteral("Unknown command. Use --help."));
    RunOptions run;
    run.preset = selectedPreset;
    run.decoder = decoderSettings;
    run.backend = backend;
    run.api = backend == BackendKind::Fobos ? chooseApi(apiOption) : ApiKind::Standard;
    run.index = index;
    run.iqFile = parser.value(QStringLiteral("iq-file"));
    const QString virtualRxTransport = parser.value(QStringLiteral("virtual-rx-transport"));
    if (virtualRxTransport != QStringLiteral("local") && virtualRxTransport != QStringLiteral("tcp"))
        return fail(QStringLiteral("--virtual-rx-transport must be local or tcp"));
    run.virtualRxEndpoint.mode = virtualRxTransport == QStringLiteral("tcp") ? VirtualSignalTransportMode::Tcp : VirtualSignalTransportMode::LocalIpc;
    run.virtualRxEndpoint.host = parser.value(QStringLiteral("virtual-rx-bind"));
    run.virtualRxEndpoint.localName = parser.value(QStringLiteral("virtual-name"));
    const int virtualRxPort = parser.value(QStringLiteral("virtual-rx-port")).toInt(&okay);
    if (!okay || virtualRxPort < 1 || virtualRxPort > 65535) return fail(QStringLiteral("Invalid virtual RX port"));
    run.virtualRxEndpoint.port = static_cast<quint16>(virtualRxPort);
    const int virtualQueueMiB = parser.value(QStringLiteral("virtual-rx-queue-mib")).toInt(&okay);
    if (!okay || virtualQueueMiB < 1 || virtualQueueMiB > 64) return fail(QStringLiteral("Virtual RX queue must be 1..64 MiB"));
    run.virtualRxQueueBytes = static_cast<qint64>(virtualQueueMiB) * 1024 * 1024;
    if ((backend == BackendKind::File) != !run.iqFile.isEmpty())
        return fail(QStringLiteral("--iq-file is required for --backend file and invalid for other backends"));
    QJsonObject fileMetadata;
    if (backend == BackendKind::File) {
        QFile metadata(run.iqFile + QStringLiteral(".json"));
        if (metadata.exists()) {
            if (!metadata.open(QIODevice::ReadOnly)) return fail(metadata.errorString());
            const QJsonDocument parsed = QJsonDocument::fromJson(metadata.readAll());
            if (!parsed.isObject()) return fail(QStringLiteral("Invalid CF32LE sidecar JSON"));
            fileMetadata = parsed.object();
            if (fileMetadata.value(QStringLiteral("format")).toString() != QStringLiteral("cf32le") ||
                fileMetadata.value(QStringLiteral("input")).toString() != QStringLiteral("rf"))
                return fail(QStringLiteral("Only RF CF32LE recordings are supported by file replay"));
        }
        if (fileMetadata.isEmpty() &&
            (!parser.isSet(QStringLiteral("freq")) || !parser.isSet(QStringLiteral("rate"))))
            return fail(QStringLiteral("File replay needs --freq and --rate when no sidecar JSON is present"));
    }
    run.rtlHost = parser.value(QStringLiteral("rtl-host")).trimmed();
    const int rtlPort = parser.value(QStringLiteral("rtl-port")).toInt(&okay);
    if (run.rtlHost.isEmpty() || !okay || rtlPort < 1 || rtlPort > 65535)
        return fail(QStringLiteral("Invalid rtl_tcp host or port"));
    run.rtlPort = static_cast<quint16>(rtlPort);
    if (backend != BackendKind::RtlTcp &&
        (parser.isSet(QStringLiteral("rtl-host")) || parser.isSet(QStringLiteral("rtl-port"))))
        return fail(QStringLiteral("--rtl-host and --rtl-port require --backend rtl-tcp"));
    run.controlPort = static_cast<quint16>(controlPort);
    run.centerHz = backend == BackendKind::File && !parser.isSet(QStringLiteral("freq"))
        ? fileMetadata.value(QStringLiteral("centerFrequency")).toDouble()
        : frequency(parser.value(QStringLiteral("freq")), &okay);
    if (backend == BackendKind::File && !parser.isSet(QStringLiteral("freq")))
        okay = std::isfinite(run.centerHz) && run.centerHz > 0.0;
    if (!okay) return fail(QStringLiteral("Invalid center frequency"));
    if (backend == BackendKind::File && !fileMetadata.isEmpty() &&
        std::abs(fileMetadata.value(QStringLiteral("centerFrequency")).toDouble() - run.centerHz) > 0.5)
        return fail(QStringLiteral("--freq does not match the CF32LE recording metadata"));
    const QString inputMode = parser.value(QStringLiteral("input")).trimmed().toLower();
    if (inputMode == QStringLiteral("auto")) run.inputMode =
        backend == BackendKind::Fobos && run.centerHz < 50e6 ? INPUT_HF1 : INPUT_RF;
    else if (inputMode == QStringLiteral("rf")) run.inputMode = INPUT_RF;
    else if (inputMode == QStringLiteral("hf1")) run.inputMode = INPUT_HF1;
    else if (inputMode == QStringLiteral("hf2")) run.inputMode = INPUT_HF2;
    else if (inputMode == QStringLiteral("hf-combined")) run.inputMode = INPUT_HF_COMBINED;
    else return fail(QStringLiteral("Input must be auto, rf, hf1, hf2 or hf-combined"));
    if (backend == BackendKind::Fobos && run.inputMode == INPUT_RF && run.centerHz < 50e6)
        return fail(QStringLiteral("Fobos RF input starts at 50 MHz; use an HF input below that"));
    if (run.inputMode != INPUT_RF && run.centerHz >= 50e6)
        return fail(QStringLiteral("HF direct-sampling frequency must be below 50 MHz"));
    const QString requestedRateText = !parser.isSet(QStringLiteral("rate")) &&
        (backend == BackendKind::RtlNative || backend == BackendKind::RtlTcp)
        ? QStringLiteral("2.048M") : parser.value(QStringLiteral("rate"));
    run.sampleRateHz = backend == BackendKind::File && !parser.isSet(QStringLiteral("rate"))
        ? fileMetadata.value(QStringLiteral("sampleRate")).toDouble()
        : frequency(requestedRateText, &okay);
    if (backend == BackendKind::File && !parser.isSet(QStringLiteral("rate")))
        okay = std::isfinite(run.sampleRateHz) && run.sampleRateHz > 0.0;
    if (!okay) return fail(QStringLiteral("Invalid sample rate"));
    if (backend == BackendKind::Virtual && (run.sampleRateHz < 8000 || run.sampleRateHz > 100000000 ||
        std::floor(run.sampleRateHz) != run.sampleRateHz || std::floor(run.centerHz) != run.centerHz))
        return fail(QStringLiteral("Virtual RX requires whole-Hz --freq and --rate 8000..100000000 matching TX"));
    if (backend == BackendKind::HackRf && (run.sampleRateHz < 2000000.0 ||
                                           run.sampleRateHz > 20000000.0 ||
                                           std::floor(run.sampleRateHz) != run.sampleRateHz))
        return fail(QStringLiteral("HackRF --rate must be a whole number from 2 to 20 MS/s"));
    if (backend == BackendKind::File && !fileMetadata.isEmpty() &&
        std::abs(fileMetadata.value(QStringLiteral("sampleRate")).toDouble() - run.sampleRateHz) > 0.5)
        return fail(QStringLiteral("--rate does not match the CF32LE recording metadata"));
    if ((backend == BackendKind::RtlTcp || backend == BackendKind::RtlNative) &&
        (run.centerHz > 4294967295.0 || run.sampleRateHz > 4294967295.0 ||
         std::floor(run.centerHz) != run.centerHz || std::floor(run.sampleRateHz) != run.sampleRateHz))
        return fail(QStringLiteral("RTL frequency and rate must be whole Hz within uint32 range"));
    run.lna = parser.value(QStringLiteral("lna")).toInt(&okay);
    if (!okay || run.lna < 0 || run.lna > 2) return fail(QStringLiteral("LNA must be 0..2"));
    run.vga = parser.value(QStringLiteral("vga")).toInt(&okay);
    if (!okay || run.vga < 0 || run.vga > 15) return fail(QStringLiteral("VGA must be 0..15"));
    run.gainDb = parser.value(QStringLiteral("gain-db")).toDouble(&okay);
    if (!okay || !std::isfinite(run.gainDb) || run.gainDb < 0.0 || run.gainDb > 120.0)
        return fail(QStringLiteral("RX gain must be 0..120 dB"));
    if (backend == BackendKind::HackRf && run.gainDb > 102.0)
        return fail(QStringLiteral("HackRF RX gain must be 0..102 dB (LNA plus VGA)"));
    run.rxChannel = parser.value(QStringLiteral("rx-channel")).toInt(&okay);
    if (!okay || run.rxChannel < 0 || run.rxChannel > 16)
        return fail(QStringLiteral("RX channel must be 0..16"));
    run.antenna = parser.value(QStringLiteral("antenna"));
    run.fpgaImage = parser.value(QStringLiteral("fpga-image"));
    if (backend == BackendKind::File &&
        (parser.isSet(QStringLiteral("gain-db")) || parser.isSet(QStringLiteral("rx-channel")) ||
         parser.isSet(QStringLiteral("antenna")) || parser.isSet(QStringLiteral("fpga-image"))))
        return fail(QStringLiteral("File replay has no hardware gain, RX channel, antenna or FPGA image"));
    if (backend == BackendKind::Virtual && (parser.isSet(QStringLiteral("gain-db")) ||
        parser.isSet(QStringLiteral("rx-channel")) || parser.isSet(QStringLiteral("antenna")) || parser.isSet(QStringLiteral("fpga-image"))))
        return fail(QStringLiteral("Virtual RX has no hardware gain, antenna, channel or FPGA image"));
    if ((backend == BackendKind::RtlTcp || backend == BackendKind::RtlNative) &&
        (parser.isSet(QStringLiteral("rx-channel")) || parser.isSet(QStringLiteral("antenna")) ||
         parser.isSet(QStringLiteral("fpga-image"))))
        return fail(QStringLiteral("RTL does not support RX channel, antenna or FPGA image selection"));
    if (backend == BackendKind::HackRf &&
        (parser.isSet(QStringLiteral("rx-channel")) || parser.isSet(QStringLiteral("antenna")) ||
         parser.isSet(QStringLiteral("fpga-image"))))
        return fail(QStringLiteral("HackRF exposes one RX channel; antenna and FPGA image are not selectable"));
    if (backend != BackendKind::Fobos && (run.lna != 0 || run.vga != 0))
        return fail(QStringLiteral("--lna and --vga are Fobos-only; use --gain-db"));
    if (backend == BackendKind::Fobos && (parser.isSet(QStringLiteral("gain-db")) ||
        parser.isSet(QStringLiteral("rx-channel")) || parser.isSet(QStringLiteral("antenna")) ||
        parser.isSet(QStringLiteral("fpga-image"))))
        return fail(QStringLiteral("--gain-db, --rx-channel, --antenna and --fpga-image are external-backend options"));
    run.seconds = parser.value(QStringLiteral("seconds")).toDouble(&okay);
    if (!okay || !std::isfinite(run.seconds) || run.seconds < 0) return fail(QStringLiteral("Invalid duration"));
    run.blockSamples = parser.value(QStringLiteral("block-samples")).toInt(&okay);
    if (!okay || run.blockSamples < 16384 || run.blockSamples > 65536 || run.blockSamples % 1024)
        return fail(QStringLiteral("Block samples must be 16384..65536 in multiples of 1024"));
    run.output = parser.value(QStringLiteral("output"));
    run.audioFile = parser.value(QStringLiteral("audio-file"));
    run.audioFilterPath = parser.value(QStringLiteral("audio-filters"));
    run.spectrumFramesFile = parser.value(QStringLiteral("spectrum-frames"));
    run.channelIqBase = parser.value(QStringLiteral("channel-iq"));
    run.fullIq8Base = parser.value(QStringLiteral("full-iq8"));
    run.spectrumIntervalMs = parser.value(QStringLiteral("spectrum-ms")).toInt(&okay);
    if (!okay || run.spectrumIntervalMs < 20 || run.spectrumIntervalMs > 10000)
        return fail(QStringLiteral("--spectrum-ms must be 20..10000"));
    QStringList outputPaths;
    for (const QString &path : {run.output, run.audioFile, run.spectrumFramesFile})
        if (!path.isEmpty()) outputPaths << QFileInfo(path).absoluteFilePath();
    if (!run.channelIqBase.isEmpty()) {
        outputPaths << QFileInfo(run.channelIqBase + QStringLiteral("_channel_iq.wav")).absoluteFilePath();
        outputPaths << QFileInfo(run.channelIqBase + QStringLiteral("_channel_iq.json")).absoluteFilePath();
    }
    if (!run.fullIq8Base.isEmpty()) {
        outputPaths << QFileInfo(run.fullIq8Base + QStringLiteral("_full_iq_s8.iq8")).absoluteFilePath();
        outputPaths << QFileInfo(run.fullIq8Base + QStringLiteral("_full_iq_s8.json")).absoluteFilePath();
        outputPaths << QFileInfo(run.fullIq8Base + QStringLiteral("_full_iq_s8.sigmf-meta")).absoluteFilePath();
    }
    if (run.output.size()) outputPaths << QFileInfo(run.output + QStringLiteral(".json")).absoluteFilePath();
    outputPaths.removeDuplicates();
    const int requestedOutputs = (!run.output.isEmpty() ? 2 : 0) + (!run.audioFile.isEmpty() ? 1 : 0) +
        (!run.spectrumFramesFile.isEmpty() ? 1 : 0) + (!run.channelIqBase.isEmpty() ? 2 : 0) +
        (!run.fullIq8Base.isEmpty() ? 3 : 0);
    if (outputPaths.size() != requestedOutputs)
        return fail(QStringLiteral("Recording output paths must be distinct"));
    for (const QString &path : outputPaths)
        if (QFile::exists(path)) return fail(QStringLiteral("Recording output already exists: %1").arg(path));
    run.audioStdout = parser.isSet(QStringLiteral("audio-stdout"));
    run.moduleVoiceStdout = parser.isSet(QStringLiteral("module-voice-stdout"));
    if (run.audioStdout && run.moduleVoiceStdout)
        return fail(QStringLiteral("Choose either --audio-stdout or --module-voice-stdout"));
    run.audioPlay = parser.isSet(QStringLiteral("audio-play"));
#if !defined(_WIN32) && !defined(FOBOSAPP_HAS_QT_AUDIO)
    if (run.audioPlay)
        return fail(QStringLiteral("This CLI build has no Qt Multimedia audio output; use --audio-stdout"));
#endif
    run.spectrum = parser.isSet(QStringLiteral("spectrum"));
    run.progress = parser.isSet(QStringLiteral("progress"));
    run.serve = parser.isSet(QStringLiteral("serve"));
    run.networkAudioEnabled = !parser.isSet(QStringLiteral("no-audio"));
    run.bindAddress = parser.value(QStringLiteral("bind"));
    const int networkPort = parser.value(QStringLiteral("network-port")).toInt(&okay);
    if (!okay || networkPort < 1 || networkPort > 65535 ||
        (run.serve && networkPort == run.controlPort))
        return fail(QStringLiteral("Invalid GUI network port"));
    run.networkPort = static_cast<quint16>(networkPort);
    const int webPort = parser.value(QStringLiteral("web-port")).toInt(&okay);
    if (!okay || webPort < 0 || webPort > 65535 || (webPort && (webPort == run.controlPort || (run.serve && webPort == run.networkPort))))
        return fail(QStringLiteral("Invalid or conflicting browser port"));
    run.webPort = static_cast<quint16>(webPort);
    run.webBind = parser.value(QStringLiteral("web-bind"));
    if (run.webPort && run.inputMode != INPUT_RF) return fail(QStringLiteral("Browser service requires complex RF IQ"));
    run.fftSize = parser.value(QStringLiteral("fft")).toInt(&okay);
    if (!okay || !CliSpectrumLimits::supportsSize(run.fftSize))
        return fail(QStringLiteral("FFT length must be 512..50000000"));
    if (parser.isSet(QStringLiteral("hz-per-bin"))) {
        run.hzPerBin = parser.value(QStringLiteral("hz-per-bin")).toDouble(&okay);
        const double wanted = std::ceil(run.sampleRateHz / run.hzPerBin);
        if (parser.isSet(QStringLiteral("fft")) || !okay || !std::isfinite(run.hzPerBin) ||
            run.hzPerBin <= 0.0 || !std::isfinite(wanted) || wanted < 512 || wanted > 50000000)
            return fail(QStringLiteral("--hz-per-bin excludes --fft and must yield 512..50000000 bins"));
        run.fftSize = static_cast<int>(wanted);
    }
    if ((parser.isSet(QStringLiteral("fft")) || parser.isSet(QStringLiteral("hz-per-bin"))) && !run.spectrum && !run.serve && run.spectrumFramesFile.isEmpty())
        return fail(QStringLiteral("--fft/--hz-per-bin requires --spectrum, --serve or --spectrum-frames"));
    const QString mode = parser.isSet(QStringLiteral("mode"))
        ? parser.value(QStringLiteral("mode")).trimmed().toLower()
        : decoderSettings.mode == CliTextDecoder::Mode::Cw ? QStringLiteral("cw")
        : decoderSettings.mode == CliTextDecoder::Mode::Psk31 ? QStringLiteral("usb")
        : parser.value(QStringLiteral("mode")).trimmed().toLower();
    if (mode == QStringLiteral("am")) { run.audioMode = MOD_AM; run.audioBandwidthHz = 10000.0; }
    else if (mode == QStringLiteral("nfm")) { run.audioMode = MOD_NFM; run.audioBandwidthHz = 12500.0; }
    else if (mode == QStringLiteral("wfm")) { run.audioMode = MOD_WFM; run.audioBandwidthHz = 200000.0; }
    else if (mode == QStringLiteral("usb")) { run.audioMode = MOD_USB; run.audioBandwidthHz = 3000.0; }
    else if (mode == QStringLiteral("lsb")) { run.audioMode = MOD_LSB; run.audioBandwidthHz = 3000.0; }
    else if (mode == QStringLiteral("cw")) { run.audioMode = MOD_CW; run.audioBandwidthHz = 1200.0; }
    else return fail(QStringLiteral("Unsupported audio mode"));
    if (parser.isSet(QStringLiteral("bandwidth"))) {
        run.audioBandwidthHz = frequency(parser.value(QStringLiteral("bandwidth")), &okay);
        if (!okay) return fail(QStringLiteral("Invalid audio bandwidth"));
    }
    if (parser.isSet(QStringLiteral("listen"))) {
        run.listeningHz = frequency(parser.value(QStringLiteral("listen")), &okay);
        if (!okay) return fail(QStringLiteral("Invalid listening frequency"));
    }
    binaryStdout = run.audioStdout || run.moduleVoiceStdout;
#ifdef _WIN32
    if (run.moduleVoiceStdout && _setmode(_fileno(stdout), _O_BINARY) < 0)
        return fail(QStringLiteral("Cannot set binary stdout mode"));
#endif
    run.virtualHost = parser.value(QStringLiteral("virtual-host"));
    run.virtualNodeId = parser.value(QStringLiteral("virtual-node-id")).trimmed();
    if (!run.virtualNodeId.isEmpty() && !virtualSignalValidNodeId(run.virtualNodeId))
        return fail(QStringLiteral("Invalid virtual node ID (1-64 ASCII letters/digits/._-, excluding txN)"));
    if (!run.virtualNodeId.isEmpty() && run.virtualHost.isEmpty())
        return fail(QStringLiteral("--virtual-node-id requires --virtual-host"));
    const int virtualPort = parser.value(QStringLiteral("virtual-port")).toInt(&okay);
    if (!okay || virtualPort < 1 || virtualPort > 65535) return fail(QStringLiteral("Invalid virtual RX port"));
    run.virtualPort = static_cast<quint16>(virtualPort);
    return runReceiver(app, run);
}
