#include "../src/cli/clidoctor.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>

#include <iostream>
#include <stdexcept>

namespace {
int assertions = 0;
void require(bool condition, const char *message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

QJsonObject check(const QJsonObject &report, const QString &id) {
    for (const auto &value : report.value("checks").toArray()) {
        const auto item = value.toObject();
        if (item.value("id").toString() == id) return item;
    }
    throw std::runtime_error((QStringLiteral("Missing check: ") + id).toStdString());
}

void validateReport(const QJsonObject &report, bool loopbackPortProbe = false) {
    require(report.value("schema") == QJsonValue("obrii.cli.doctor"), "schema name");
    require(report.value("schemaVersion").toInt() == 1, "schema version");
    require(report.value("platform").toObject().value("pointerBits").toInt() == int(sizeof(void *) * 8), "pointer bits");
    require(!report.value("qt").toObject().value("runtimeVersion").toString().isEmpty(), "Qt runtime version");
    const auto policy = report.value("policy").toObject();
    for (const QString &key : {QStringLiteral("hardwareAccess"),
         QStringLiteral("processLaunch"), QStringLiteral("libraryLoading"), QStringLiteral("profileAccess")})
        require(policy.value(key).isBool() && !policy.value(key).toBool(), "passive policy");
    for (const QString &key : {QStringLiteral("networkAccess"), QStringLiteral("loopbackPortProbe")})
        require(policy.value(key).isBool() && policy.value(key).toBool() == loopbackPortProbe, "network opt-in policy");
    require(policy.value("pathsRedacted").toBool(), "redaction policy");
    int passed = 0, warnings = 0, failures = 0;
    QSet<QString> ids;
    const auto items = report.value("checks").toArray();
    require(items.size() < 100, "bounded report");
    for (const auto &value : items) {
        const auto item = value.toObject();
        const QString id = item.value("id").toString();
        require(!id.isEmpty() && !ids.contains(id), "unique nonempty check id");
        ids.insert(id);
        require(item.value("data").isObject(), "structured check data");
        require(!item.value("code").toString().isEmpty() && !item.value("message").toString().isEmpty(), "check code/message");
        const QString status = item.value("status").toString();
        if (status == QLatin1String("passed")) {
            ++passed;
            require(item.value("severity") == QJsonValue("info"), "passed severity");
        } else if (status == QLatin1String("warning")) {
            ++warnings;
            require(item.value("severity") == QJsonValue("warning"), "warning severity");
        } else {
            require(status == QLatin1String("failure"), "valid status");
            ++failures;
            require(item.value("severity") == QJsonValue("error"), "failure severity");
        }
    }
    const auto summary = report.value("summary").toObject();
    require(summary.value("total").toInt() == items.size(), "summary total");
    require(summary.value("passed").toInt() == passed, "summary passed");
    require(summary.value("warnings").toInt() == warnings, "summary warnings");
    require(summary.value("failures").toInt() == failures, "summary failures");
    require(report.value("ok").toBool() == (failures == 0), "ok means no failures");
    require(summary.value("status").toString() == QLatin1String(failures ? "failure" : warnings ? "warning" : "passed"), "summary status");
}

void writeFixture(const QString &path, const QByteArray &bytes = "inert fixture, not a library or executable\n") {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "create fixture without overwriting");
    require(file.write(bytes) == bytes.size(), "write fixture");
    file.close();
    require(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "set fixture permissions");
}

QMap<QString, QByteArray> snapshot(const QString &root) {
    QMap<QString, QByteArray> result;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo info(path);
        QByteArray signature = QByteArray::number(int(info.permissions())) + ':';
        if (info.isSymLink()) signature += "link";
        else if (info.isDir()) signature += "directory";
        else {
            QFile file(path);
            require(file.open(QIODevice::ReadOnly), "read temporary fixture for snapshot");
            signature += QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
        }
        result.insert(QDir(root).relativeFilePath(path), signature);
    }
    return result;
}

QString workerName(const QString &base) {
#ifdef Q_OS_WIN
    return base + QStringLiteral(".exe");
#else
    return base;
#endif
}

void installMinimalFixtures(const QString &root) {
    const QDir directory(root);
    for (const QString &name : {QStringLiteral("ObriiCliModule"), QStringLiteral("ObriiCliPacketModule"),
         QStringLiteral("ObriiCliImageModule"), QStringLiteral("ObriiCliSpectrumModule"),
         QStringLiteral("ObriiCliM17Module"), QStringLiteral("ObriiCliTxModule"),
         QStringLiteral("ObriiCliGnssModule"), QStringLiteral("ObriiCliView"), QStringLiteral("ObriiIqHub")})
        writeFixture(directory.filePath(workerName(name)));
    for (const QString &component : {QStringLiteral("Core"), QStringLiteral("Network"), QStringLiteral("Gui"),
         QStringLiteral("Concurrent"), QStringLiteral("Multimedia")}) {
#ifdef Q_OS_WIN
        const QString name = QStringLiteral("Qt") + QString::number(QT_VERSION_MAJOR) + component +
#ifdef QT_DEBUG
            QStringLiteral("d.dll");
#else
            QStringLiteral(".dll");
#endif
#elif defined(Q_OS_MACOS)
        const QString name = QStringLiteral("libQt") + QString::number(QT_VERSION_MAJOR) + component + QStringLiteral(".dylib");
#else
        const QString name = QStringLiteral("libQt") + QString::number(QT_VERSION_MAJOR) + component + QStringLiteral(".so");
#endif
        writeFixture(directory.filePath(name));
    }
    for (const QString &base : {QStringLiteral("fobos"), QStringLiteral("fobos_sdr"), QStringLiteral("fftw3f")}) {
#ifdef Q_OS_WIN
        const QString name = base == QLatin1String("fftw3f") ? QStringLiteral("libfftw3f-3.dll") : base + QStringLiteral(".dll");
#elif defined(Q_OS_MACOS)
        const QString name = QStringLiteral("lib") + base + QStringLiteral(".dylib");
#else
        const QString name = QStringLiteral("lib") + base + QStringLiteral(".so");
#endif
        writeFixture(directory.filePath(name));
    }
#ifdef Q_OS_WIN
    writeFixture(directory.filePath(QStringLiteral("libusb-1.0.dll")));
#endif
}

void invalidRequest(const QJsonObject &request, const QString &root) {
    const auto before = snapshot(root);
    const auto report = runCliDoctor(request);
    validateReport(report);
    require(!report.value("ok").toBool(), "invalid request fails");
    require(report.value("checks").toArray().size() == 1, "invalid request aborts all checks");
    require(check(report, "request").value("code") == QJsonValue("invalid_request"), "invalid request code");
    require(snapshot(root) == before, "invalid request has no side effects");
    require(!QJsonDocument(report).toJson().contains(root.toUtf8()), "invalid report redacts directory");
}
} // namespace

int main(int argc, char **argv) {
    try {
        const auto noApplication = runCliDoctor();
        validateReport(noApplication);
        require(check(noApplication, "directory").value("code") == QJsonValue("directory_unavailable"), "safe without QCoreApplication");
        QCoreApplication app(argc, argv);
        QTemporaryDir temporary;
        require(temporary.isValid(), "isolated temporary directory");
        const QString root = temporary.path();
        const QDir directory(root);
        require(directory.mkdir("package"), "create package fixture directory");
        const QString package = directory.filePath("package");
        const QJsonObject request{{"directory", package}};
        const auto emptyBefore = snapshot(root);
        const auto empty = runCliDoctor(request);
        validateReport(empty);
        require(check(empty, "worker.ObriiCliModule").value("status") == QJsonValue("warning"), "missing worker is optional");
#ifdef Q_OS_WIN
        require(check(empty, "dependency.fftw3f").value("status") == QJsonValue("failure"), "missing Windows runtime dependency fails");
#else
        require(check(empty, "dependency.fftw3f").value("status") == QJsonValue("warning"), "Unix system loader remains unchecked");
#endif
        require(snapshot(root) == emptyBefore, "passive empty scan unchanged");

        installMinimalFixtures(package);
        writeFixture(directory.filePath("private-profile-secret.txt"), "private-profile-sentinel-53a31\n");
        const auto before = snapshot(root);
        const auto report = runCliDoctor(request);
        validateReport(report);
        require(report.value("ok").toBool(), "complete inert package has no failures");
        require(check(report, "worker.ObriiCliGnssModule").value("status") == QJsonValue("passed"), "GNSS worker found without launching");
        require(check(report, "dependency.fftw3f").value("status") == QJsonValue("passed"), "inert FFTW discovered without loading");
        require(check(report, "fftw.runtime").value("code") == QJsonValue("unchecked"), "FFTW runtime not claimed usable");
        require(check(report, "audio.runtime").value("data").toObject().value("devicesEnumerated") == QJsonValue(false), "audio never enumerated");
        const auto bytes = QJsonDocument(report).toJson();
        require(!bytes.contains(root.toUtf8()) && !bytes.contains("private-profile-sentinel-53a31"), "no path or profile content leak");
        require(runCliDoctor(request) == report, "deterministic report for unchanged fixtures");
        require(snapshot(root) == before, "passive fixture scan unchanged");
        validateReport(runCliDoctor());
        require(snapshot(root) == before, "no-argument doctor does not touch fixtures");

        QJsonObject writable = request;
        writable.insert("checkWritableDirectory", true);
        for (int iteration = 0; iteration < 3; ++iteration) {
            const auto result = runCliDoctor(writable);
            validateReport(result);
            const auto probe = check(result, "directory.writable");
            require(probe.value("status") == QJsonValue("passed"), "opt-in writable probe succeeds");
            require(probe.value("data").toObject().value("cleanedUp").toBool(), "probe cleaned up");
            require(snapshot(root) == before, "unique writable probe leaves no mutation");
        }
        for (const QString &badDirectory : {directory.filePath("does-not-exist"), directory.filePath("private-profile-secret.txt")}) {
            const auto result = runCliDoctor({{"directory", badDirectory}, {"checkWritableDirectory", true}});
            validateReport(result);
            require(check(result, "directory.writable").value("status") == QJsonValue("failure"), "bad writable directory fails");
            require(check(result, "directory.writable").value("data").toObject().value("cleanedUp").toBool(), "failed probe has no leftover");
            require(snapshot(root) == before, "failed probe neither mutates nor creates directories");
        }
#ifndef Q_OS_WIN
        const QString link = directory.filePath("linked-package");
        require(QFile::link(package, link), "temporary directory symlink");
        const auto linkedBefore = snapshot(root);
        const auto linked = runCliDoctor({{"directory", link}, {"checkWritableDirectory", true}});
        require(check(linked, "directory.writable").value("code") == QJsonValue("probe_unsafe_directory"), "writable symlink refused");
        require(snapshot(root) == linkedBefore, "rejected symlink probe unchanged");
        require(QFile::remove(link), "remove test symlink");
#endif
        const QJsonArray external{
            QJsonObject{{"path", package}, {"kind", "directory"}},
            QJsonObject{{"path", directory.filePath("private-profile-secret.txt")}},
            QJsonObject{{"path", directory.filePath("absent-secret")}, {"required", true}},
            QJsonObject{{"path", package}, {"kind", "file"}},
            QJsonObject{{"path", QDir(package).filePath(workerName("ObriiCliModule"))}, {"kind", "executable"}}};
        QJsonObject extended = request;
        extended.insert("externalPaths", external);
        QTcpServer reservation;
        require(reservation.listen(QHostAddress::LocalHost, 0), "explicit loopback listener fixture");
        const int fixturePort = reservation.serverPort();
        require(fixturePort > 0, "fixture reserves an ephemeral port");
        const QString tcpId = QStringLiteral("tcp.") + QString::number(fixturePort);
        extended.insert("tcpPorts", QJsonArray{fixturePort});
        for (bool requested : {false, true}) {
            extended.insert("checkPorts", requested);
            const auto result = runCliDoctor(extended);
            validateReport(result, requested);
            require(check(result, "external.0").value("status") == QJsonValue("passed"), "explicit directory metadata");
            require(check(result, "external.1").value("status") == QJsonValue("passed"), "explicit file metadata");
            require(check(result, "external.2").value("status") == QJsonValue("failure"), "missing required external fails");
            require(check(result, "external.3").value("status") == QJsonValue("warning"), "wrong optional external kind warns");
            require(check(result, "external.4").value("status") == QJsonValue("passed"), "explicit executable metadata");
            const auto tcp = check(result, tcpId);
            const auto data = tcp.value("data").toObject();
            require(data.value("requested").toBool() == requested, "port opt-in acknowledged");
            require(data.value("attempted").toBool() == requested, "only opted-in port probe attempted");
            if (requested) {
                require(tcp.value("status") == QJsonValue("failure"), "reserved port blocks bind");
                const int error = data.value("socketError").toInt();
                require(error == QAbstractSocket::AddressInUseError || error == QAbstractSocket::SocketAccessError, "blocked port has explicit occupancy/access error");
                require(tcp.value("code") == QJsonValue(error == QAbstractSocket::AddressInUseError ? "port_in_use" : "port_permission_denied"), "bind error classified accurately");
                require(data.value("address") == QJsonValue("127.0.0.1"), "probe address is IPv4 loopback only");
            } else {
                require(tcp.value("code") == QJsonValue("unchecked"), "no default port-ownership test");
                require(data.value("availability") == QJsonValue("unchecked"), "default availability unchecked");
            }
            require(!QJsonDocument(result).toJson().contains("absent-secret"), "external paths are redacted");
        }
        require(!reservation.hasPendingConnections(), "bind-only doctor never connects to listener fixture");
        reservation.close();
        const QJsonObject portRequest{{"directory", package}, {"checkPorts", true}, {"tcpPorts", QJsonArray{fixturePort}}};
        for (int i = 0; i < 2; ++i) {
            const auto released = runCliDoctor(portRequest);
            validateReport(released, true);
            require(released.value("ok").toBool(), "released fixture port is available");
            const auto tcp = check(released, tcpId);
            require(tcp.value("status") == QJsonValue("passed") && tcp.value("code") == QJsonValue("port_available"), "released port bind passes");
            require(tcp.value("data").toObject().value("availability") == QJsonValue("available"), "available report is explicit");
            require(tcp.value("data").toObject().value("reservationHeld") == QJsonValue(false), "doctor does not hold reservation");
            require(reservation.listen(QHostAddress::LocalHost, static_cast<quint16>(fixturePort)), "doctor immediately releases its successful bind");
            reservation.close();
        }
        const auto noPorts = runCliDoctor({{"directory", package}, {"checkPorts", true}});
        validateReport(noPorts);
        require(check(noPorts, "tcp").value("code") == QJsonValue("unchecked"), "empty explicit port check unchecked");
        require(snapshot(root) == before, "external and TCP reports never mutate fixtures");

        QJsonArray pathsAtLimit, portsAtLimit;
        for (int i = 0; i < 16; ++i) pathsAtLimit.append(QJsonObject{{"path", package}, {"kind", "directory"}});
        for (int i = 1; i <= 32; ++i) portsAtLimit.append(i);
        QJsonObject atLimit{{"directory", package}, {"externalPaths", pathsAtLimit}, {"tcpPorts", portsAtLimit}};
        const auto bounded = runCliDoctor(atLimit);
        validateReport(bounded);
        require(bounded.value("ok").toBool(), "maximum permitted arrays accepted");
        pathsAtLimit.append(QJsonObject{{"path", package}});
        portsAtLimit.append(33);

        QList<QJsonObject> invalid{
            {{"checkWritableDirectory", true}}, {{"directory", "relative/path"}}, {{"directory", ""}},
            {{"directory", 1}}, {{"directory", QString(4097, QLatin1Char('a'))}},
            {{"directory", "//uncontacted.invalid/share"}}, {{"directory", ":/resource"}},
            {{"directory", "https://uncontacted.invalid/path"}}, {{"directory", package + QChar(0)}},
            {{"directory", package + "/../package"}}, {{"directory", package + "/./"}},
            {{"directory", package}, {"checkWritableDirectory", "true"}},
            {{"directory", package}, {"checkPorts", 1}}, {{"directory", package}, {"unknown-secret-key", true}},
            {{"directory", package}, {"tcpPorts", QJsonArray{0}}},
            {{"directory", package}, {"tcpPorts", QJsonArray{65536}}},
            {{"directory", package}, {"tcpPorts", QJsonArray{1.5}}},
            {{"directory", package}, {"tcpPorts", QJsonArray{"80"}}},
            {{"directory", package}, {"tcpPorts", QJsonArray{true}}},
            {{"directory", package}, {"tcpPorts", QJsonArray{80, 80}}},
            {{"directory", package}, {"tcpPorts", portsAtLimit}},
            {{"directory", package}, {"tcpPorts", QJsonObject{}}},
            {{"directory", package}, {"externalPaths", pathsAtLimit}},
            {{"directory", package}, {"externalPaths", "bad"}},
            {{"directory", package}, {"externalPaths", QJsonArray{1}}},
            {{"directory", package}, {"externalPaths", QJsonArray{QJsonObject{{"path", package}, {"kind", "device"}}}}},
            {{"directory", package}, {"externalPaths", QJsonArray{QJsonObject{{"path", package}, {"required", "true"}}}}},
            {{"directory", package}, {"externalPaths", QJsonArray{QJsonObject{{"path", package}, {"secret", true}}}}},
            {{"directory", package}, {"externalPaths", QJsonArray{QJsonObject{{"path", "relative"}}}}}
        };
#ifdef Q_OS_WIN
        for (const QString &path : {QStringLiteral("C:relative"), QStringLiteral("C:/NUL"), QStringLiteral("C:/folder/CON.txt"),
             QStringLiteral("C:/file:stream"), QStringLiteral("\\\\.\\PhysicalDrive0"), QStringLiteral("C:/trailing.")})
            invalid.append(QJsonObject{{"directory", path}});
#endif
        for (auto bad : invalid) {
            // Late-invalid fields must stop otherwise authorized writes/binds.
            if (!bad.contains("checkWritableDirectory") && bad.contains("directory")) bad.insert("checkWritableDirectory", true);
            if (!bad.contains("checkPorts")) bad.insert("checkPorts", true);
            if (!bad.contains("tcpPorts")) bad.insert("tcpPorts", QJsonArray{fixturePort});
            invalidRequest(bad, root);
        }
        require(snapshot(root) == before, "all tests leave fixtures unchanged");
        std::cout << "cli_doctor_selftest: " << assertions << " assertions passed" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "cli_doctor_selftest: " << error.what() << '\n';
        return 1;
    }
}
