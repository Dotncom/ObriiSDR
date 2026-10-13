#include "experimentbundle.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void fixture(const QString &path, int count = 4) {
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture open failed");
    file.write("FBSPEC1\n", 8);
    const auto metadata = QJsonDocument(QJsonObject{{"format", "fobos_spectrum_frames"}, {"version", 1},
        {"sourceSampleRate", 192000.0}, {"listeningFrequency", 100000000.0}, {"privateToken", "SECRET"}}).toJson(QJsonDocument::Compact);
    QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian); stream.setVersion(QDataStream::Qt_5_15);
    stream << quint32(metadata.size()); file.write(metadata);
    stream << quint32(0x314d5246U) << qint64(0) << qint64(1700000000000LL)
           << double(100000000) << double(99900000) << double(100100000) << qint32(512) << qint32(count);
    const std::vector<float> bins(std::size_t(count), -80.0f);
    require(file.write(reinterpret_cast<const char *>(bins.data()), qint64(bins.size() * sizeof(float))) ==
        qint64(bins.size() * sizeof(float)), "fixture payload write failed");
}
void writeBytes(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(), "fixture write failed");
}
void writeBundle(const QString &destination, const QString &source) {
    QString error;
    if (!ExperimentBundle::write(destination, source, {}, &error))
        throw std::runtime_error((QStringLiteral("write fixture bundle: ") + error).toStdString());
}
void rejectedRead(const QString &manifest) {
    ExperimentBundle::Contents contents;
    contents.recordingPath = "stale";
    contents.context = QJsonObject{{"stale", true}}; contents.manifest = contents.context;
    QString error;
    const bool accepted = ExperimentBundle::read(manifest, contents, &error);
    require(!accepted && !error.isEmpty() && contents.recordingPath.isEmpty() && contents.context.isEmpty() && contents.manifest.isEmpty(),
        "Rejected bundle exposed stale or unverified contents");
}
void testMissingIqPairs(const QDir &dir) {
    const QStringList suffixes{"_channel_iq.wav", "_channel_iq.json", "_full_iq_s8.iq8", "_full_iq_s8.json"};
    for (int i = 0; i < suffixes.size(); ++i) {
        const auto base = dir.filePath(QStringLiteral("missing-%1").arg(i));
        const auto source = base + "_spectrum.fbspec"; fixture(source);
        writeBytes(base + suffixes[i], QByteArray("{}"));
        const auto destination = base + "-bundle"; QString error;
        const bool accepted = ExperimentBundle::write(destination, source, {}, &error);
        require(!accepted && error.contains("must both be present") && !QFileInfo::exists(destination),
            "Missing IQ data/timing companion was accepted or created a destination");
    }
}
void testTraversal(const QString &bundle) {
    const auto path = QDir(bundle).filePath("manifest.json");
    QFile file(path); require(file.open(QIODevice::ReadOnly), "manifest fixture read failed");
    const auto original = file.readAll(); file.close();
    const auto originalManifest = QJsonDocument::fromJson(original).object();
    for (const auto &name : {"../synthetic_spectrum.fbspec", "..\\synthetic_spectrum.fbspec", "C:/outside.fbspec", "CON.fbspec"}) {
        auto manifest = originalManifest;
        auto entries = manifest.value("files").toArray(); auto entry = entries[0].toObject();
        entry["path"] = name; entries[0] = entry; manifest["files"] = entries;
        writeBytes(path, QJsonDocument(manifest).toJson(QJsonDocument::Compact));
        rejectedRead(path);
    }
    writeBytes(path, original);
}
void testCancellation(const QDir &dir, const QString &validBundle) {
    const auto source = dir.filePath("cancel_spectrum.fbspec"); fixture(source, 131072);
    const auto destination = dir.filePath("cancel-bundle");
    std::atomic_bool cancel{false}; QString error;
    const bool accepted = ExperimentBundle::write(destination, source, {}, &error, &cancel,
        [&](qint64 done, qint64 total) {
            require(done >= 0 && done <= total, "Invalid bundle copy progress");
            if (done >= 384 * 1024) cancel.store(true);
        });
    const QDir partial(destination);
    require(!accepted && cancel.load() && error.contains("cancelled") &&
        QFileInfo::exists(partial.filePath(".experiment-bundle.partial")) &&
        !QFileInfo::exists(partial.filePath("manifest.json")), "Cancelled copy left a usable bundle");
    const QFileInfo copied(partial.filePath("recording_spectrum.fbspec"));
    require(copied.size() > 0 && copied.size() < QFileInfo(source).size(), "Cancellation did not interrupt a partially copied file");
    rejectedRead(partial.filePath("manifest.json"));
    cancel.store(false);
    require(!ExperimentBundle::write(destination, source, {}, &error, &cancel), "Cancelled partial bundle was overwritten");

    ExperimentBundle::Contents contents; contents.recordingPath = "stale";
    const bool verified = ExperimentBundle::read(QDir(validBundle).filePath("manifest.json"), contents, &error, &cancel,
        [&](qint64 done, qint64) { if (done > 0) cancel.store(true); });
    require(!verified && cancel.load() && error.contains("cancelled") && contents.recordingPath.isEmpty() &&
        contents.context.isEmpty() && contents.manifest.isEmpty(), "Cancelled verification returned unverified contents");
}
void testActiveWriters(const QDir &dir) {
#ifdef Q_OS_WIN
    const auto source = dir.filePath("active_spectrum.fbspec"); fixture(source);
    const auto iqPath = dir.filePath("active_full_iq_s8.iq8");
    writeBytes(iqPath, QByteArray(16, char(0)));
    writeBytes(dir.filePath("active_full_iq_s8.json"), QJsonDocument(QJsonObject{
        {"format", "full_iq_raw_s8_interleaved"}, {"sampleRate", 192000.0}, {"bytesWritten", 16}}).toJson());
    for (const auto &path : {source, iqPath}) {
        QFile activeWriter(path); require(activeWriter.open(QIODevice::ReadWrite), "Cannot open synthetic active writer");
        const auto destination = dir.filePath(path == source ? "active-spectrum-bundle" : "active-iq-bundle");
        QString error;
        const bool accepted = ExperimentBundle::write(destination, source, {}, &error);
        require(!accepted && error.contains("stop recording") && !QFileInfo::exists(destination),
            "Windows export did not reject an open spectrum/IQ writer");
        activeWriter.close();
        writeBundle(destination, source);
        ExperimentBundle::Contents contents;
        const bool verified = ExperimentBundle::read(QDir(destination).filePath("manifest.json"), contents, &error);
        if (!verified) throw std::runtime_error((QStringLiteral("read completed IQ fixture: ") + error).toStdString());
        require(contents.manifest.value("files").toArray().size() == 3, "Completed IQ pair was not verified after writer close");
    }
#else
    Q_UNUSED(dir);
    std::cout << "Active-writer exclusion test is Windows-only\n";
#endif
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary; require(temporary.isValid(), "temporary directory failed");
        const QDir dir(temporary.path()); const auto source = dir.filePath("synthetic_spectrum.fbspec");
        fixture(source);
        QJsonObject context{{"appVersion", "4.0.0"}, {"settings", QJsonObject{{"sampleRate", 192000.0},
            {"dmrPrivacyKeyHex", "SECRET"}, {"qthLatitude", 50.0}}}, {"password", "SECRET"}};
        const auto clean = ExperimentBundle::sanitizeContext(context);
        require(!QJsonDocument(clean).toJson().contains("SECRET") && !clean.value("settings").toObject().contains("qthLatitude"), "privacy sanitizer failed");
        const auto session = ExperimentBundle::sanitizeContext(QJsonObject{{"session", QJsonObject{
            {"name", QString(1024, 'n')}, {"notes", QString(65536, 'x')},
            {"totalSnapshotCount", 2}, {"omittedSnapshotCount", 1},
            {"snapshots", QJsonArray{QJsonObject{{"utcMs", 1700000000000.0}}}}}}}).value("session").toObject();
        require(session.value("name").toString().size() == 1024 && session.value("notes").toString().size() == 65536 &&
                session.value("exportedSnapshotCount").toInt() == 1 && session.value("omittedSnapshotCount").toInt() == 1,
                "Session export limits/counts differ from the UI");
        QString error; const auto destination = dir.filePath("bundle");
        const bool written = ExperimentBundle::write(destination, source, context, &error);
        if (!written) {
            const QString diagnostic = error.isEmpty() ? QStringLiteral("backend returned false without an error") : error;
            throw std::runtime_error((QStringLiteral("write: ") + diagnostic).toStdString());
        }
        ExperimentBundle::Contents contents;
        const bool verified = ExperimentBundle::read(QDir(destination).filePath("manifest.json"), contents, &error);
        if (!verified) {
            const QString diagnostic = error.isEmpty() ? QStringLiteral("backend returned false without an error") : error;
            throw std::runtime_error((QStringLiteral("read: ") + diagnostic).toStdString());
        }
        require(QFile::exists(contents.recordingPath) && contents.manifest.value("files").toArray().size() == 1, "bundle round trip failed");
        require(!ExperimentBundle::write(destination, source, context, &error), "existing destination overwritten");
        testMissingIqPairs(dir);
        testTraversal(destination);
        testCancellation(dir, destination);
        testActiveWriters(dir);
        QFile corrupt(contents.recordingPath); require(corrupt.open(QIODevice::ReadWrite), "corruption fixture open failed");
        require(corrupt.seek(corrupt.size() - 1) && corrupt.write("x", 1) == 1, "corruption fixture failed"); corrupt.close();
        require(!ExperimentBundle::read(QDir(destination).filePath("manifest.json"), contents, &error) && contents.recordingPath.isEmpty(), "corrupt bundle accepted");
        std::cout << "Experiment bundle round-trip/privacy/integrity, missing IQ pairs, traversal, cancellation and active-writer tests passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "Experiment bundle selftest: " << e.what() << '\n'; return 1; }
}
