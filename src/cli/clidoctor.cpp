#include "clidoctor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <QStringList>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

#include <cmath>

namespace {
constexpr int kMaxPaths = 16;
constexpr int kMaxPorts = 32;
constexpr int kMaxPathLength = 4096;

struct Report {
    QJsonArray checks;
    int passed = 0;
    int warnings = 0;
    int failures = 0;
    bool loopbackPortProbe = false;

    void add(const QString &id, const char *status, const char *code,
             const char *message, const QJsonObject &data = {}) {
        const QString state = QLatin1String(status);
        const char *severity = "info";
        if (state == QLatin1String("failure")) { ++failures; severity = "error"; }
        else if (state == QLatin1String("warning")) { ++warnings; severity = "warning"; }
        else ++passed;
        checks.append(QJsonObject{{"id", id}, {"status", state}, {"severity", severity},
            {"code", QLatin1String(code)}, {"message", QLatin1String(message)}, {"data", data}});
    }

    QJsonObject json() const {
        return {{"schema", "obrii.cli.doctor"}, {"schemaVersion", 1}, {"ok", failures == 0},
            {"platform", QJsonObject{{"os", QSysInfo::productType()},
                {"cpuArchitecture", QSysInfo::currentCpuArchitecture()},
                {"pointerBits", int(sizeof(void *) * 8)},
                {"byteOrder", QSysInfo::ByteOrder == QSysInfo::LittleEndian ? "little" : "big"}}},
            {"qt", QJsonObject{{"buildVersion", QT_VERSION_STR}, {"runtimeVersion", qVersion()}}},
            {"policy", QJsonObject{{"hardwareAccess", false}, {"networkAccess", loopbackPortProbe},
                {"loopbackPortProbe", loopbackPortProbe},
                {"processLaunch", false}, {"libraryLoading", false}, {"profileAccess", false},
                {"pathsRedacted", true}}},
            {"checks", checks}, {"summary", QJsonObject{{"total", checks.size()},
                {"passed", passed}, {"warnings", warnings}, {"failures", failures},
                {"status", failures ? "failure" : warnings ? "warning" : "passed"}}}};
    }
};

// Reject explicit network/device/resource paths. Filesystem mounts and reparse
// points still depend on the host; never infer locality by contacting a server.
bool validPath(const QJsonValue &value) {
    if (!value.isString()) return false;
    const QString path = QDir::fromNativeSeparators(value.toString());
    if (path.isEmpty() || path.size() > kMaxPathLength || !QDir::isAbsolutePath(path) ||
        path.startsWith(QLatin1String("//")) || path.startsWith(QLatin1Char(':')))
        return false;
    for (const QChar c : path)
        if (c.unicode() < 32 || c.unicode() == 127) return false;
    for (const QString &part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        if (part == QLatin1String(".") || part == QLatin1String("..")) return false;
#ifdef Q_OS_WIN
    if (path.size() < 3 || !((path[0] >= QLatin1Char('A') && path[0] <= QLatin1Char('Z')) ||
        (path[0] >= QLatin1Char('a') && path[0] <= QLatin1Char('z'))) ||
        path[1] != QLatin1Char(':') || path[2] != QLatin1Char('/') || path.mid(2).contains(QLatin1Char(':')))
        return false;
    for (const QString &part : path.mid(3).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))) return false;
        const QString base = part.section(QLatin1Char('.'), 0, 0).toUpper();
        if (base == QLatin1String("CON") || base == QLatin1String("PRN") ||
            base == QLatin1String("AUX") || base == QLatin1String("NUL") ||
            base == QLatin1String("CONIN$") || base == QLatin1String("CONOUT$") ||
            (base.size() == 4 && (base.startsWith(QLatin1String("COM")) || base.startsWith(QLatin1String("LPT"))) &&
             (base[3].isDigit() || base[3] == QChar(0xb9) || base[3] == QChar(0xb2) || base[3] == QChar(0xb3))))
            return false;
        for (const QChar c : part)
            if (QStringLiteral("<>\"|?*").contains(c)) return false;
    }
#else
    if (path.contains(QLatin1String("://"))) return false;
#endif
    return true;
}

bool validRequest(const QJsonObject &request) {
    const QSet<QString> keys{QStringLiteral("directory"), QStringLiteral("checkWritableDirectory"),
        QStringLiteral("externalPaths"), QStringLiteral("tcpPorts"), QStringLiteral("checkPorts")};
    if (request.size() > keys.size()) return false;
    for (auto it = request.begin(); it != request.end(); ++it)
        if (!keys.contains(it.key())) return false;
    for (const QString &key : {QStringLiteral("checkWritableDirectory"), QStringLiteral("checkPorts")})
        if (request.contains(key) && !request.value(key).isBool()) return false;
    if (request.contains("directory") && !validPath(request.value("directory"))) return false;
    if (request.value("checkWritableDirectory").toBool() && !request.contains("directory")) return false;
    if (request.contains("externalPaths")) {
        if (!request.value("externalPaths").isArray()) return false;
        const auto paths = request.value("externalPaths").toArray();
        if (paths.size() > kMaxPaths) return false;
        for (const auto &value : paths) {
            if (!value.isObject()) return false;
            const auto path = value.toObject();
            if (path.size() > 3 || !validPath(path.value("path"))) return false;
            for (auto it = path.begin(); it != path.end(); ++it)
                if (it.key() != QLatin1String("path") && it.key() != QLatin1String("kind") &&
                    it.key() != QLatin1String("required")) return false;
            if (path.contains("required") && !path.value("required").isBool()) return false;
            if (path.contains("kind") && (!path.value("kind").isString() ||
                (path.value("kind") != QJsonValue("file") && path.value("kind") != QJsonValue("directory") &&
                 path.value("kind") != QJsonValue("executable")))) return false;
        }
    }
    if (request.contains("tcpPorts")) {
        if (!request.value("tcpPorts").isArray()) return false;
        const auto ports = request.value("tcpPorts").toArray();
        if (ports.size() > kMaxPorts) return false;
        QSet<int> seen;
        for (const auto &value : ports) {
            const double port = value.toDouble();
            if (!value.isDouble() || !std::isfinite(port) || port < 1 || port > 65535 ||
                std::floor(port) != port || seen.contains(int(port))) return false;
            seen.insert(int(port));
        }
    }
    return true;
}

QString executableName(const QString &base) {
#ifdef Q_OS_WIN
    return base + QStringLiteral(".exe");
#else
    return base;
#endif
}

QStringList qtLibraryNames(const QString &component) {
    const QString major = QString::number(QT_VERSION_MAJOR);
#ifdef Q_OS_WIN
#ifdef QT_DEBUG
    const QString suffix = QStringLiteral("d.dll");
#else
    const QString suffix = QStringLiteral(".dll");
#endif
    return {QStringLiteral("Qt") + major + component + suffix};
#elif defined(Q_OS_MACOS)
    return {QStringLiteral("libQt") + major + component + QStringLiteral(".dylib"),
        QStringLiteral("Qt") + component + QStringLiteral(".framework/Versions/") + major +
            QStringLiteral("/Qt") + component};
#else
    return {QStringLiteral("libQt") + major + component + QStringLiteral(".so.") + major,
        QStringLiteral("libQt") + major + component + QStringLiteral(".so")};
#endif
}

QStringList libraryNames(const QString &base) {
#ifdef Q_OS_WIN
    if (base == QLatin1String("fftw3f")) return {QStringLiteral("libfftw3f-3.dll"), QStringLiteral("fftw3f.dll")};
    return {base + QStringLiteral(".dll")};
#elif defined(Q_OS_MACOS)
    return {QStringLiteral("lib") + base + QStringLiteral(".dylib"),
        QStringLiteral("lib") + base + (base == QLatin1String("fftw3f") ? QStringLiteral(".3.dylib") : QStringLiteral(".1.dylib"))};
#else
    return {QStringLiteral("lib") + base + (base == QLatin1String("fftw3f") ? QStringLiteral(".so.3") : QStringLiteral(".so.1")),
        QStringLiteral("lib") + base + QStringLiteral(".so")};
#endif
}

void libraryCheck(Report &report, const QDir &directory, const QString &id,
                  const QStringList &names, bool required) {
    bool found = false;
    for (const auto &name : names) {
        const QFileInfo file(directory.filePath(name));
        if (file.isFile() && file.isReadable()) { found = true; break; }
    }
    const QJsonObject data{{"method", "file_metadata"}, {"required", required},
        {"foundAdjacent", found}, {"loadabilityChecked", false}};
    if (found) report.add(id, "passed", "file_present", "Adjacent library file is present; loading was not tested.", data);
    else {
#ifdef Q_OS_WIN
        const char *status = required ? "failure" : "warning";
#else
        // System loader locations are deliberately not searched on Unix.
        const char *status = "warning";
#endif
        report.add(id, status, "file_not_found", "No adjacent library candidate found; system loader paths were not searched.", data);
    }
}

void writableCheck(Report &report, const QDir &directory) {
    const QString canonical = QFileInfo(directory.absolutePath()).canonicalFilePath();
    // Avoid writing through directory symlinks/junctions, including ancestors.
    QString ancestor = directory.absolutePath();
    for (int depth = 0; depth <= kMaxPathLength; ++depth) {
        const QFileInfo info(ancestor);
        if (info.isSymLink() || info.isJunction()) {
            report.add("directory.writable", "failure", "probe_unsafe_directory", "Writable probe refuses a directory symlink or junction.",
                {{"attempted", false}, {"cleanedUp", true}});
            return;
        }
        const QString parent = info.absoluteDir().absolutePath();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    if (canonical.isEmpty()) {
        report.add("directory.writable", "failure", "probe_directory_unavailable", "Writable probe directory is unavailable.",
            {{"attempted", false}, {"cleanedUp", true}});
        return;
    }
    QTemporaryFile file(QDir(canonical).filePath(QStringLiteral(".obrii-cli-doctor-XXXXXX")));
    file.setAutoRemove(true);
    if (!file.open()) {
        report.add("directory.writable", "failure", "probe_open_failed", "Could not create the unique temporary probe file.",
            {{"attempted", true}, {"cleanedUp", true}});
        return;
    }
    const QByteArray marker("obrii-cli-doctor-probe\n");
    const bool verified = file.write(marker) == marker.size() && file.flush() &&
        file.seek(0) && file.read(marker.size() + 1) == marker;
    const bool removed = file.remove();
    report.add("directory.writable", verified && removed ? "passed" : "failure",
        !removed ? "probe_cleanup_failed" : verified ? "probe_passed" : "probe_io_failed",
        !removed ? "Temporary probe cleanup failed; automatic cleanup will retry on scope exit." :
            verified ? "Unique temporary file was written, verified and removed." : "Temporary probe I/O failed; the file was removed.",
        {{"attempted", true}, {"cleanedUp", removed}, {"bytes", marker.size()}});
}

void portCheck(Report &report, int port) {
    report.loopbackPortProbe = true;
    QTcpSocket socket;
    const bool available = socket.bind(QHostAddress::LocalHost, static_cast<quint16>(port),
        QAbstractSocket::DontShareAddress);
    const auto error = socket.error();
    socket.close();

    QJsonObject data{{"port", port}, {"requested", true}, {"attempted", true},
        {"address", "127.0.0.1"}, {"method", "exclusive_bind"}, {"reservationHeld", false}};
    const QString id = QStringLiteral("tcp.") + QString::number(port);
    if (available) {
        data.insert("availability", "available");
        report.add(id, "passed", "port_available", "IPv4 loopback bind succeeded and was immediately closed; the port may be claimed at any time.", data);
        return;
    }
    data.insert("socketError", int(error));
    if (error == QAbstractSocket::AddressInUseError) {
        data.insert("availability", "in_use");
        report.add(id, "failure", "port_in_use", "IPv4 loopback bind failed: the address/port is already in use.", data);
    } else if (error == QAbstractSocket::SocketAccessError) {
        data.insert("availability", "permission_denied");
        report.add(id, "failure", "port_permission_denied", "IPv4 loopback bind failed: permission was denied (possibly an exclusive reservation).", data);
    } else {
        data.insert("availability", "unknown");
        report.add(id, "warning", "port_probe_failed", "IPv4 loopback bind failed for another reason; port occupancy is unknown.", data);
    }
}
} // namespace

QJsonObject runCliDoctor(const QJsonObject &request) {
    Report report;
    if (!validRequest(request)) {
        report.add("request", "failure", "invalid_request", "Invalid doctor request; no filesystem checks, writes or network probes were attempted.");
        return report.json();
    }
    report.add("request", "passed", "valid_request", "Doctor request is valid.");
    report.add("platform", "passed", "platform_reported", "Platform and Qt version information is available.");
    report.add("qt.core", "passed", "qt_core_running", "QtCore is running in this process; no additional library was loaded.");

    const QString path = request.contains("directory") ? request.value("directory").toString() :
        (QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QString());
    const bool directoryReady = !path.isEmpty() && validPath(path) && QFileInfo(path).isDir();
    if (!directoryReady) {
        report.add("directory", "failure", "directory_unavailable", "Inspection directory does not exist or is not a supported local absolute directory.");
        if (request.value("checkWritableDirectory").toBool())
            report.add("directory.writable", "failure", "probe_directory_unavailable", "Writable probe requires an existing directory; nothing was created.",
                {{"attempted", false}, {"cleanedUp", true}});
    } else {
        const QDir directory(QDir::cleanPath(path));
        report.add("directory", "passed", "directory_present", "Inspection directory exists; its contents are not enumerated.");
        for (const QString &worker : {QStringLiteral("ObriiCliModule"), QStringLiteral("ObriiCliPacketModule"),
             QStringLiteral("ObriiCliImageModule"), QStringLiteral("ObriiCliSpectrumModule"),
             QStringLiteral("ObriiCliM17Module"), QStringLiteral("ObriiCliTxModule"),
             QStringLiteral("ObriiCliGnssModule"), QStringLiteral("ObriiCliView"), QStringLiteral("ObriiIqHub")}) {
            const QFileInfo file(directory.filePath(executableName(worker)));
            const bool present = file.isFile() && file.isReadable() && file.isExecutable();
            report.add(QStringLiteral("worker.") + worker, present ? "passed" : "warning",
                present ? "worker_present" : "worker_unavailable",
                present ? "Optional executable file is present; it was not launched." : "Optional executable file is missing or not readable/executable.",
                {{"optional", true}, {"launched", false}});
        }
        for (const QString &component : {QStringLiteral("Core"), QStringLiteral("Network"), QStringLiteral("Gui"), QStringLiteral("Concurrent")})
            libraryCheck(report, directory, QStringLiteral("dependency.qt.") + component, qtLibraryNames(component), true);
        libraryCheck(report, directory, "dependency.fobos", libraryNames("fobos"), true);
        libraryCheck(report, directory, "dependency.fobos_sdr", libraryNames("fobos_sdr"), true);
        libraryCheck(report, directory, "dependency.fftw3f", libraryNames("fftw3f"), true);
#ifdef Q_OS_WIN
        libraryCheck(report, directory, "dependency.libusb", {QStringLiteral("libusb-1.0.dll")}, true);
#endif
        libraryCheck(report, directory, "dependency.qt.Multimedia", qtLibraryNames("Multimedia"), false);
        if (request.value("checkWritableDirectory").toBool()) writableCheck(report, directory);
    }
    report.add("fftw.runtime", "warning", "unchecked", "FFTW symbol resolution is unchecked: loading a library can execute initialization code.",
        {{"availability", "unchecked"}, {"symbolsResolved", false}});

#ifdef Q_OS_WIN
    const char *audioBackend = "winmm_platform_support";
#elif defined(FOBOSAPP_HAS_QT_AUDIO) || defined(OBRII_CLI_QT_MICROPHONE)
    const char *audioBackend = "qt_multimedia_compile_flag";
#else
    const char *audioBackend = "not_enabled_in_this_translation_unit";
#endif
    report.add("audio.runtime", "warning", "unchecked", "Audio runtime availability is unchecked; no plugins or devices were enumerated or opened.",
        {{"backendHint", QLatin1String(audioBackend)}, {"availability", "unchecked"}, {"devicesEnumerated", false}});
    const auto paths = request.value("externalPaths").toArray();
    for (int i = 0; i < paths.size(); ++i) {
        const auto item = paths[i].toObject();
        const QString kind = item.value("kind").toString(QStringLiteral("file"));
        const bool required = item.value("required").toBool(false);
        const QFileInfo file(item.value("path").toString());
        const bool found = kind == QLatin1String("directory") ? file.isDir() :
            file.isFile() && file.isReadable() && (kind != QLatin1String("executable") || file.isExecutable());
        report.add(QStringLiteral("external.") + QString::number(i), found ? "passed" : required ? "failure" : "warning",
            found ? "path_present" : "path_unavailable",
            found ? "Explicit external path satisfies the requested metadata check; no content was read." : "Explicit external path is unavailable or has the wrong type/permissions.",
            {{"index", i}, {"kind", kind}, {"required", required}});
    }
    const bool portsRequested = request.value("checkPorts").toBool();
    const auto ports = request.value("tcpPorts").toArray();
    for (const auto &port : ports) {
        if (portsRequested) portCheck(report, port.toInt());
        else report.add(QStringLiteral("tcp.") + QString::number(port.toInt()), "warning", "unchecked",
            "TCP availability is unchecked; an explicit checkPorts opt-in is required for loopback bind/close.",
            {{"port", port}, {"requested", false}, {"attempted", false}, {"availability", "unchecked"}});
    }
    if (portsRequested && ports.isEmpty())
        report.add("tcp", "warning", "unchecked", "No ports were provided; no network probe was attempted.",
            {{"requested", true}, {"attempted", false}, {"availability", "unchecked"}});
    return report.json();
}
