#include "hfprediction.h"
#include "hfimagevalidation_p.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSet>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace hf {
namespace {
bool inRange(double v, double low, double high) { return std::isfinite(v) && v >= low && v <= high; }
bool ascii(const QString &s) {
    for (const QChar c : s) if (c.unicode() < 32 || c.unicode() > 126 || c == '"') return false;
    return true;
}
QString dataPath(const QString &path) {
    QString absolute = QDir(path).absolutePath();
#ifdef Q_OS_WIN
    if (!ascii(absolute)) {
        const QString native = QDir::toNativeSeparators(absolute);
        wchar_t buffer[32768];
        const DWORD n = GetShortPathNameW(reinterpret_cast<LPCWSTR>(native.utf16()), buffer, 32768);
        if (!n || n >= 32768) return {};
        absolute = QString::fromWCharArray(buffer);
    }
#endif
    absolute = QDir::fromNativeSeparators(absolute) + '/';
    return ascii(absolute) && absolute.size() <= 180 ? absolute : QString();
}
bool nativeImage(const QString &fileName) {
    QFile f(fileName);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray dos = f.read(64);
#ifdef Q_OS_LINUX
    return detail::elfHeaderMatches(dos, detail::hostElfTarget());
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64) && QT_POINTER_SIZE == 8
    if (dos.size() != 64 || !dos.startsWith("MZ")) return false;
    const quint32 offset = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(dos.constData() + 60));
    if (offset > 1024 * 1024 || !f.seek(offset)) return false;
    return detail::peX64HeaderMatches(f.read(26));
#else
    return false;
#endif
}
QVector<int> hours(const Request &r) {
    if (!r.hoursUtc.isEmpty()) return r.hoursUtc;
    QVector<int> h;
    for (int i = 0; i < 24; ++i) h.append(i);
    return h;
}
const QStringList &columns() {
    static const QStringList c = QStringLiteral("month,hour,frequency,distance,BMUF,MUF50,MUF90,MUF10,OPMUF,OPMUF90,OPMUF10,PR,FamT,SNR,DuSN,DlSN,SNRXXp,BCR").split(',');
    return c;
}
}

QString privateRoot() {
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(base).filePath(QStringLiteral("ObriiSDR/itu_hf"));
}
Runtime Runtime::defaults() {
    Runtime r;
#ifdef Q_OS_WIN
    r.helperExecutable = QDir(QCoreApplication::applicationDirPath()).filePath("itu_hf/itu_hf_helper.exe");
#else
    r.helperExecutable = QDir(QCoreApplication::applicationDirPath()).filePath("itu_hf/itu_hf_helper");
#endif
    const QString bundled = QDir(QCoreApplication::applicationDirPath()).filePath("itu_hf/data");
    r.dataDirectory = QDir(bundled).exists() ? bundled : QDir(privateRoot()).filePath("data");
    if (qEnvironmentVariableIsSet("ITU_HF_DATA")) r.dataDirectory = qEnvironmentVariable("ITU_HF_DATA");
    return r;
}
QString validate(const Request &r) {
    for (const auto p : {r.tx, r.rx})
        if (!inRange(p.latitude, -90, 90) || !inRange(p.longitude, -180, 180)) return "Invalid TX/RX coordinates";
    const double radians = std::acos(-1.0) / 180;
    const double a = r.tx.latitude * radians, b = r.rx.latitude * radians;
    const double h = std::pow(std::sin((b - a) / 2), 2)
        + std::cos(a) * std::cos(b) * std::pow(std::sin((r.rx.longitude - r.tx.longitude) * radians / 2), 2);
    if (2 * 6371.009 * std::asin(std::sqrt((std::min)(1.0, (std::max)(0.0, h)))) < 1)
        return "HF sky-wave endpoints must be at least 1 km apart";
    if (r.year < 1900 || r.year > 2100 || r.month < 1 || r.month > 12) return "Invalid model year/month";
    if (r.smoothedSsn < 1 || r.smoothedSsn > 311) return "Smoothed SSN must be 1..311 (not Kp or F10.7)";
    if (!inRange(r.powerW, 1, 1e9)) return "TX power must be 1..1e9 W (-30..60 dB(kW))";
    if (!inRange(r.txGainDbi, -50, 60) || !inRange(r.rxGainDbi, -50, 60)) return "Isotropic gain offsets must be -50..60 dBi";
    if (!inRange(r.bandwidthHz, 0.005, 3e6) || !inRange(r.requiredSnrDb, -30, 200)) return "Invalid bandwidth or required SNR";
    if (r.snrPercent < 1 || r.snrPercent > 99) return "SNR exceedance percentage must be 1..99";
    if (int(r.noise) < 0 || int(r.noise) > 5) return "Invalid noise environment";
    QSet<int> seen;
    for (int hour : hours(r)) {
        if (hour < 0 || hour > 23 || seen.contains(hour)) return "UTC hours must be unique in 0..23";
        seen.insert(hour);
    }
    if (r.frequenciesMHz.isEmpty() || r.frequenciesMHz.size() > 32) return "Choose 1..32 frequencies";
    QSet<int> frequencies;
    for (double f : r.frequenciesMHz) {
        if (!inRange(f, 1.6, 30) || std::abs(f * 100 - std::round(f * 100)) > 1e-7)
            return "HF frequencies must be 1.6..30 MHz in 0.01 MHz steps";
        int key = qRound(f * 100);
        if (frequencies.contains(key)) return "HF frequencies must be unique";
        frequencies.insert(key);
    }
    return {};
}
QString checkRuntime(const Runtime &r, int month) {
    if (month < 1 || month > 12) return "Invalid data month";
    if (!QFileInfo(r.helperExecutable).isAbsolute() || !nativeImage(r.helperExecutable))
        return "An ITU HF helper matching this application's architecture and word size is required";
    const QDir binaries = QFileInfo(r.helperExecutable).absoluteDir();
#ifdef Q_OS_WIN
    const QStringList libraries{"P533.dll", "P372.dll"};
#else
    const QStringList libraries{"libp533.so", "libp372.so"};
#endif
    for (const QString &name : libraries)
        if (!nativeImage(binaries.filePath(name)))
            return "ITU libraries matching this application's architecture and word size are required beside the helper";
    if (r.timeoutMs < 1 || r.timeoutMs > 600000) return "Helper timeout must be 1..600000 ms";
    if (dataPath(r.dataDirectory).isEmpty()) return "ITU data requires an ASCII/8.3 path of at most 180 characters";
    const QDir data(r.dataDirectory);
    const QStringList names{QString("ionos%1.bin").arg(month, 2, 10, QChar('0')),
        QString("COEFF%1W.txt").arg(month, 2, 10, QChar('0')), "P1239-3 Decile Factors.txt"};
    const qint64 sizes[]{11197844, month == 1 ? 235276 : 235275, 73427};
    for (int i = 0; i < names.size(); ++i) {
        const QFileInfo file(data.filePath(names[i]));
        if (!file.isFile() || !file.isReadable() || file.size() != sizes[i])
            return QString("Missing/incomplete pinned ITU data: %1; use itu_hf_fetch.ps1 -Months %2").arg(names[i]).arg(month);
    }
    return {};
}
QByteArray inputConfiguration(const Request &r, const QString &data) {
    if (!validate(r).isEmpty() || !ascii(data) || data.size() > 180 || !data.endsWith('/')) return {};
    QStringList lines;
    auto number = [&](const char *key, double v) { lines << QString::fromLatin1(key) + ' ' + QString::number(v, 'g', 15); };
    lines << "PathName \"HF monthly climatology\"" << "PathTXName \"TX\"" << "PathRXName \"RX\""
        << "TXAntFilePath \"ISOTROPIC\"" << "RXAntFilePath \"ISOTROPIC\"" << "AntennaOrientation \"TX2RX\"";
    number("Path.L_tx.lat", r.tx.latitude); number("Path.L_tx.lng", r.tx.longitude);
    number("TXGOS", r.txGainDbi); number("RXGOS", r.rxGainDbi);
    number("Path.year", r.year); number("Path.month", r.month); number("Path.SSN", r.smoothedSsn);
    number("Path.txpower", 10 * std::log10(r.powerW / 1000));
    number("Path.BW", r.bandwidthHz); number("Path.SNRr", r.requiredSnrDb); number("Path.SNRXXp", r.snrPercent);
    QStringList h, f;
    for (int hour : hours(r)) h << QString::number(hour + 1);
    for (double frequency : r.frequenciesMHz) f << QString::number(frequency, 'f', 2);
    lines << "Path.hour " + h.join(',') << "Path.frequency " + f.join(',');
    const char *noise[]{"CITY", "RESIDENTIAL", "RURAL", "QUIETRURAL", "QUIET", "NOISY"};
    lines << QString("Path.ManMadeNoise \"%1\"").arg(noise[int(r.noise)]) << "Path.Modulation \"ANALOG\""
        << QString("Path.SorL \"%1\"").arg(r.longPath ? "LONGPATH" : "SHORTPATH")
        << "RptFileFormat \"RPT_D | RPT_BMUF | RPT_BMUFD | RPT_OPMUF | RPT_OPMUFD | RPT_PR | RPT_NOISETOTAL | RPT_SNR | RPT_SNRD | RPT_SNRXX | RPT_BCR\"";
    for (const char *corner : {"LL", "LR", "UL", "UR"}) {
        number((QByteArray(corner) + ".lat").constData(), r.rx.latitude);
        number((QByteArray(corner) + ".lng").constData(), r.rx.longitude);
    }
    lines << "latinc 1.0" << "lnginc 1.0" << "DataFilePath \"" + data + "\"";
    for (const auto &line : lines) if (line.size() >= 250) return {};
    return (lines.join('\n') + '\n').toLatin1();
}
bool parseReport(const QByteArray &csv, const Request &r, Result *result, QString *error) {
    auto reject = [&](const QString &why) { if (error) *error = why; return false; };
    if (!result || !validate(r).isEmpty() || csv.size() > 2 * 1024 * 1024) return reject("Invalid HF report request/size");
    const QList<QByteArray> lines = csv.trimmed().split('\n');
    if (lines.isEmpty() || QString::fromLatin1(lines.front()).trimmed().split(',') != columns())
        return reject("Unexpected ITU CSV schema");
    if (lines.size() != hours(r).size() * r.frequenciesMHz.size() + 1) return reject("Incomplete ITU result grid");
    Result parsed; parsed.request = r;
    QSet<QString> keys;
    for (int row = 1; row < lines.size(); ++row) {
        const auto fields = QString::fromLatin1(lines[row]).trimmed().split(',');
        if (fields.size() != columns().size()) return reject("Malformed ITU CSV row");
        QVector<double> v;
        for (const auto &field : fields) {
            bool ok = false;
            const double n = QLocale::c().toDouble(field, &ok);
            if (!ok || !std::isfinite(n)) return reject("Non-finite/non-numeric ITU result");
            v.append(n);
        }
        if (v[0] != r.month || !inRange(v[1], 1, 24) || v[1] != std::floor(v[1]) || !hours(r).contains(int(v[1]) - 1)) return reject("Wrong ITU month/hour");
        if (!inRange(v[2], 1.6, 30)) return reject("Invalid ITU frequency");
        bool frequencyFound = false;
        for (double frequency : r.frequenciesMHz) if (std::abs(v[2] - frequency) < 1e-6) frequencyFound = true;
        const QString key = QString::number(int(v[1])) + '/' + QString::number(qRound(v[2] * 100));
        if (!frequencyFound || keys.contains(key)) return reject("Wrong/duplicate ITU frequency/hour");
        keys.insert(key);
        if (!inRange(v[3], 1, 40031) || !inRange(v[4], 0, 1000)) return reject("Invalid ITU distance/MUF");
        for (int index = 5; index <= 10; ++index)
            if (!inRange(v[index], 0, 1000)) return reject("Invalid ITU MUF decile");
        if (!inRange(v[11], -1000, 300) || !inRange(v[12], -400, 400)) return reject("Invalid ITU power/noise");
        for (int index = 13; index <= 16; ++index)
            if (!inRange(v[index], -1000, 1000)) return reject("Invalid ITU SNR");
        if (!inRange(v[17], 0, 100)) return reject("Invalid ITU reliability percentage");
        Sample s;
        s.month = int(v[0]); s.hourUtc = int(v[1]) - 1; s.frequencyMHz = v[2]; s.distanceKm = v[3];
        s.basicMufMHz = v[4]; s.muf50MHz = v[5]; s.muf90MHz = v[6]; s.muf10MHz = v[7];
        s.operationalMufMHz = v[8]; s.operationalMuf90MHz = v[9]; s.operationalMuf10MHz = v[10];
        s.receivedDbm = v[11] + 30; s.hasSignal = std::abs(v[11] + 307) > 0.005; s.externalNoiseDb = v[12];
        s.medianSnrDb = v[13]; s.upperSnrDecileDb = v[14]; s.lowerSnrDecileDb = v[15]; s.percentileSnrDb = v[16];
        s.basicReliabilityPercent = v[17];
        parsed.samples.append(s);
    }
    *result = parsed;
    if (error) error->clear();
    return true;
}
bool isSuccessExit(int code) { return code == 0 || code == 1000; }

PredictionJob::PredictionJob(QObject *parent) : QObject(parent) {
    qRegisterMetaType<hf::Result>();
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [this] { timedOut = true; process.kill(); });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] { process.readAllStandardOutput(); });
    connect(&process, &QProcess::readyReadStandardError, this, [this] { process.readAllStandardError(); });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &PredictionJob::finish);
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) fail("Unable to start the optional ITU HF helper");
    });
}
PredictionJob::~PredictionJob() {
    disconnect(&process, nullptr, this, nullptr);
    if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(3000); }
}
bool PredictionJob::start(const Request &r, const Runtime &runtime, QString *error) {
    auto reject = [&](const QString &why) { if (error) *error = why; return false; };
    if (active) return reject("An HF prediction is already running");
    QString problem = validate(r);
    if (problem.isEmpty()) problem = checkRuntime(runtime, r.month);
    if (!problem.isEmpty()) return reject(problem);
    const QString jobs = QDir(privateRoot()).filePath("jobs");
    if (!QDir().mkpath(jobs)) return reject("Cannot create private HF job directory");
    directory = std::make_unique<QTemporaryDir>(QDir(jobs).filePath("job-XXXXXX"));
    if (!directory->isValid()) { directory.reset(); return reject("Cannot create private HF request"); }
    QFile input(directory->filePath("request.in"));
    const QByteArray config = inputConfiguration(r, dataPath(runtime.dataDirectory));
    if (config.isEmpty() || !input.open(QIODevice::WriteOnly) || input.write(config) != config.size()) {
        input.close(); directory.reset(); return reject("Cannot write private HF request");
    }
    input.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    input.close();
    snapshot = r; active = true; stopping = false; timedOut = false;
    process.setWorkingDirectory(directory->path());
    process.setProgram(runtime.helperExecutable);
    process.setArguments({"-s", "-c", "request.in", "result.csv"});
    process.start(); deadline.start(runtime.timeoutMs);
    if (error) error->clear();
    return true;
}
void PredictionJob::cancel() {
    if (!active) return;
    stopping = true;
    process.kill();
}
void PredictionJob::fail(const QString &message) {
    if (!active) return;
    deadline.stop(); active = false; directory.reset();
    emit failed(message);
}
void PredictionJob::finish(int code, QProcess::ExitStatus status) {
    if (!active) return;
    deadline.stop();
    if (stopping) { active = false; directory.reset(); emit cancelled(); return; }
    if (timedOut) { fail("ITU HF calculation timed out"); return; }
    if (status != QProcess::NormalExit || !isSuccessExit(code)) { fail(QString("ITU HF helper failed (code %1)").arg(code)); return; }
    QFile report(directory->filePath("result.csv"));
    if (!report.open(QIODevice::ReadOnly) || report.size() > 2 * 1024 * 1024) { fail("ITU HF report is missing/oversized"); return; }
    const QByteArray csv = report.readAll(); report.close();
    Result result; QString error;
    if (!parseReport(csv, snapshot, &result, &error)) { fail(error); return; }
    active = false; directory.reset();
    emit completed(result);
}
}
