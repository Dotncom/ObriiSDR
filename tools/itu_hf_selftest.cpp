#include "hfprediction.h"
#include "hfpredictiondialog.h"
#include "hfimagevalidation_p.h"
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSet>
#include <QTableWidget>
#include <QtTest>
#include <limits>

static QString helper;
static QString dataDirectory;
static hf::Runtime runtime() {
    auto r = hf::Runtime::defaults(); r.helperExecutable = helper;
    if (!dataDirectory.isEmpty()) r.dataDirectory = dataDirectory;
    return r;
}
static hf::Request request() {
    hf::Request r; r.tx = {1.42, 103.73}; r.rx = {40, 116.4};
    r.year = 1981; r.month = 4; r.smoothedSsn = 140; r.powerW = 1000;
    r.bandwidthHz = 1000; r.hoursUtc = {0, 12, 23}; r.frequenciesMHz = {7.1, 14.1}; return r;
}
static QByteArray report() {
    return "month,hour,frequency,distance,BMUF,MUF50,MUF90,MUF10,OPMUF,OPMUF90,OPMUF10,PR,FamT,SNR,DuSN,DlSN,SNRXXp,BCR\n"
        "4,1,7.10,4477,27,27,21,35,36,28,47,-110,45,19,9,14,5,75\n";
}
class HfSelfTest : public QObject {
    Q_OBJECT
private slots:
    void architectureHeaders() {
        auto elf = [](hf::detail::ElfTarget target) {
            QByteArray header(target.pointerBytes == 8 ? 64 : 52, '\0');
            header.replace(0, 4, QByteArray("\x7f" "ELF", 4));
            header[4] = target.pointerBytes == 8 ? 2 : 1;
            header[5] = target.byteOrder == Q_LITTLE_ENDIAN ? 1 : 2;
            header[6] = 1;
            auto word = [&](int offset, quint16 value) {
                auto *bytes = reinterpret_cast<uchar *>(header.data() + offset);
                if (target.byteOrder == Q_LITTLE_ENDIAN) qToLittleEndian<quint16>(value, bytes);
                else qToBigEndian<quint16>(value, bytes);
            };
            word(16, 3); word(18, target.machine);
            auto *version = reinterpret_cast<uchar *>(header.data() + 20);
            if (target.byteOrder == Q_LITTLE_ENDIAN) qToLittleEndian<quint32>(1, version);
            else qToBigEndian<quint32>(1, version);
            word(target.pointerBytes == 8 ? 52 : 40, quint16(header.size()));
            return header;
        };
        const hf::detail::ElfTarget targets[]{
            {183, 8, Q_LITTLE_ENDIAN}, {40, 4, Q_LITTLE_ENDIAN},
            {62, 8, Q_LITTLE_ENDIAN}, {3, 4, Q_LITTLE_ENDIAN},
            {183, 8, Q_BIG_ENDIAN}
        };
        for (const auto expected : targets) {
            for (const auto actual : targets) {
                const bool same = actual.machine == expected.machine && actual.pointerBytes == expected.pointerBytes
                    && actual.byteOrder == expected.byteOrder;
                QCOMPARE(hf::detail::elfHeaderMatches(elf(actual), expected), same);
            }
            QVERIFY(!hf::detail::elfHeaderMatches(elf(expected).chopped(1), expected));
            auto bad = elf(expected); bad[0] = 'X';
            QVERIFY(!hf::detail::elfHeaderMatches(bad, expected));
            bad = elf(expected); bad[6] = 2;
            QVERIFY(!hf::detail::elfHeaderMatches(bad, expected));
            bad = elf(expected);
            const int sizeOffset = expected.pointerBytes == 8 ? 52 : 40;
            bad[sizeOffset] = 0; bad[sizeOffset + 1] = 0;
            QVERIFY(!hf::detail::elfHeaderMatches(bad, expected));
        }
        const auto host = hf::detail::hostElfTarget();
        QCOMPARE(host.pointerBytes, int(sizeof(void *))); QCOMPARE(host.byteOrder, Q_BYTE_ORDER);
#if defined(Q_PROCESSOR_ARM_64)
        QCOMPARE(host.machine, quint16(183));
#elif defined(Q_PROCESSOR_ARM_32)
        QCOMPARE(host.machine, quint16(40));
#elif defined(Q_PROCESSOR_X86_64)
        QCOMPARE(host.machine, quint16(62));
#elif defined(Q_PROCESSOR_X86_32)
        QCOMPARE(host.machine, quint16(3));
#endif
        QVERIFY(hf::detail::elfHeaderMatches(elf(host), host));
        QVERIFY(!hf::detail::elfHeaderMatches(elf(targets[0]), {0, 8, Q_LITTLE_ENDIAN}));
        QVERIFY(!hf::detail::elfHeaderMatches(elf(targets[0]), {183, 4, Q_LITTLE_ENDIAN}));
        QVERIFY(!hf::detail::elfHeaderMatches(elf(targets[1]), {40, 8, Q_LITTLE_ENDIAN}));
        QByteArray pe(26, '\0'); pe.replace(0, 4, QByteArray("PE\0\0", 4));
        auto *bytes = reinterpret_cast<uchar *>(pe.data());
        qToLittleEndian<quint16>(0x8664, bytes + 4);
        qToLittleEndian<quint16>(240, bytes + 20); qToLittleEndian<quint16>(0x20b, bytes + 24);
        QVERIFY(hf::detail::peX64HeaderMatches(pe));
        QVERIFY(!hf::detail::peX64HeaderMatches(pe.left(25)));
        qToLittleEndian<quint16>(0x14c, bytes + 4); QVERIFY(!hf::detail::peX64HeaderMatches(pe));
        qToLittleEndian<quint16>(0xaa64, bytes + 4); QVERIFY(!hf::detail::peX64HeaderMatches(pe));
        qToLittleEndian<quint16>(0x8664, bytes + 4); qToLittleEndian<quint16>(0x10b, bytes + 24);
        QVERIFY(!hf::detail::peX64HeaderMatches(pe));
    }
    void validationAndUnits() {
        auto r = request(); QVERIFY(hf::validate(r).isEmpty());
        const auto config = hf::inputConfiguration(r, "C:/model/data/");
        QVERIFY(config.contains("Path.hour 1,13,24")); QVERIFY(config.contains("Path.txpower 0\n"));
        QVERIFY(config.contains("TXAntFilePath \"ISOTROPIC\""));
        QVERIFY(!config.contains("DIGITAL")); QVERIFY(!config.contains("RPT_OCR"));
        r.smoothedSsn = 0; QVERIFY(!hf::validate(r).isEmpty()); r = request();
        r.tx.latitude = std::numeric_limits<double>::quiet_NaN(); QVERIFY(!hf::validate(r).isEmpty()); r = request();
        r.frequenciesMHz = {14.123}; QVERIFY(!hf::validate(r).isEmpty()); r = request();
        r.hoursUtc = {0, 0}; QVERIFY(!hf::validate(r).isEmpty()); r = request();
        r.powerW = 0.1; QVERIFY(!hf::validate(r).isEmpty());
        QVERIFY(hf::isSuccessExit(0)); QVERIFY(hf::isSuccessExit(1000)); QVERIFY(!hf::isSuccessExit(1));
    }
    void strictParser() {
        auto r = request(); r.hoursUtc = {0}; r.frequenciesMHz = {7.1};
        hf::Result result; QString error;
        QVERIFY2(hf::parseReport(report(), r, &result, &error), qPrintable(error));
        QCOMPARE(result.samples.size(), 1); QCOMPARE(result.samples[0].hourUtc, 0);
        QCOMPARE(result.samples[0].receivedDbm, -80.0);
        QVERIFY(hf::parseReport(report().replace(",-110,", ",-307,"), r, &result, &error));
        QVERIFY(!result.samples[0].hasSignal);
        QVERIFY(!hf::parseReport(report().replace("7.10", "7.11"), r, &result, &error));
        QVERIFY(!hf::parseReport(report().replace(",-110,", ",nan,"), r, &result, &error));
        QVERIFY(!hf::parseReport(report().replace(",75\n", ",101\n"), r, &result, &error));
        QVERIFY(!hf::parseReport(report().replace("BMUF", "muf"), r, &result, &error));
        QVERIFY(!hf::parseReport(report().replace("4,1,", "4,1e300,"), r, &result, &error));
        QVERIFY(!hf::parseReport(report().replace("7.10", "1e300"), r, &result, &error));
        r.hoursUtc = {0, 1}; QVERIFY(!hf::parseReport(report(), r, &result, &error));
    }
    void asynchronousGridAndCleanup() {
        hf::PredictionJob job; QSignalSpy done(&job, &hf::PredictionJob::completed), failed(&job, &hf::PredictionJob::failed);
        QString error;
        QVERIFY2(job.start(request(), runtime(), &error), qPrintable(error));
        QVERIFY(!job.start(request(), runtime(), &error));
        QTRY_COMPARE_WITH_TIMEOUT(done.count() + failed.count(), 1, 15000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
        const auto result = qvariant_cast<hf::Result>(done[0][0]); QCOMPARE(result.samples.size(), 6);
        QVERIFY(!job.isRunning());
        QVERIFY(!QDir(QDir(hf::privateRoot()).filePath("jobs")).entryList({"job-*"}, QDir::Dirs | QDir::NoDotAndDotDot).size());
    }
    void cancelAndTimeout() {
        hf::PredictionJob job; QString error; QSignalSpy cancelled(&job, &hf::PredictionJob::cancelled);
        QVERIFY2(job.start(request(), runtime(), &error), qPrintable(error)); job.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(cancelled.count(), 1, 10000); QVERIFY(!job.isRunning());
        auto shortRuntime = runtime(); shortRuntime.timeoutMs = 1;
        QSignalSpy failed(&job, &hf::PredictionJob::failed);
        QVERIFY(job.start(request(), shortRuntime, &error));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY(failed[0][0].toString().contains("timed out"));
    }
    void longPathGrid() {
        auto r = request(); r.longPath = true;
        hf::PredictionJob job; QString error;
        QSignalSpy done(&job, &hf::PredictionJob::completed), failed(&job, &hf::PredictionJob::failed);
        QVERIFY2(job.start(r, runtime(), &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(done.count() + failed.count(), 1, 15000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
        const auto result = qvariant_cast<hf::Result>(done[0][0]);
        QCOMPARE(result.samples.size(), 6); QVERIFY(result.samples.front().distanceKm > 30000);
    }
    void missingRuntime() {
        auto missing = runtime(); missing.dataDirectory = QDir(hf::privateRoot()).filePath("no-such-data");
        QVERIFY(!hf::checkRuntime(missing, 4).isEmpty());
        hf::PredictionJob job; QString error;
        QVERIFY(!job.start(request(), missing, &error)); QVERIFY(!job.isRunning());
    }
    void dialogSeedingAndInvalidation() {
        HfPredictionDialog dialog; QVERIFY(dialog.setRequest(request())); dialog.setRuntime(runtime());
        QVERIFY(!dialog.setLink({1.42, 103.73}, {40, 116.4}, 0.1, 2, 3));
        QCOMPARE(dialog.request().powerW, 1000.0);
        QVERIFY(dialog.setLink({1.42, 103.73}, {40, 116.4}, 1000, 2, 3));
        QCOMPARE(dialog.request().powerW, 1000.0); QCOMPARE(dialog.request().txGainDbi, 2.0);
        QCOMPARE(dialog.request().rxGainDbi, 3.0);
        QSignalSpy done(&dialog, &HfPredictionDialog::resultReady);
        dialog.findChild<QPushButton *>("hfCalculate")->click();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 15000);
        auto *grid = dialog.findChild<QTableWidget *>("hfGrid");
        QCOMPARE(grid->rowCount(), 2); QCOMPARE(grid->columnCount(), 3);
        dialog.resize(1120, 720); dialog.show(); QTest::qWait(50);
        QVERIFY(dialog.grab().save(QDir(hf::privateRoot()).filePath("selftest-desktop.png")));
        dialog.resize(720, 480); QTest::qWait(50);
        QVERIFY(dialog.grab().save(QDir(hf::privateRoot()).filePath("selftest-compact.png")));
        dialog.findChild<QDoubleSpinBox *>("hfPowerW")->setValue(200);
        QCOMPARE(grid->rowCount(), 0);
        dialog.close();
    }
    void translationCallbackAndFallback() {
        QFile catalogFile(QFINDTESTDATA("../src/propagation/ituhf/hfprediction_ui_en.json"));
        QVERIFY(catalogFile.open(QIODevice::ReadOnly));
        QJsonParseError parseError;
        const auto catalogDocument = QJsonDocument::fromJson(catalogFile.readAll(), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(catalogDocument.isObject());
        const auto catalog = catalogDocument.object();
        QStringList mismatches; QSet<QString> seen;
        HfPredictionDialog::Translation translation = [&](const QString &key, const QString &fallback) {
            seen.insert(key);
            if (!catalog.value(key).isString() || catalog.value(key).toString() != fallback) mismatches.append(key);
            if (key == "hf_prediction_title") return QString("Callback HF");
            if (key == "hf_prediction_calculate") return QString("Run callback");
            if (key == "hf_prediction_isotropic_tooltip") return QString("Callback isotropic");
            if (key == "hf_prediction_calculating") return QString("Callback calculating");
            if (key == "hf_prediction_completed") return QString("Completed %1 cells; %2-%3; R12 %4");
            if (key == "hf_prediction_cancelled") return QString("Callback cancelled");
            if (key == "hf_prediction_hour_header") return QString("UTC hour %1");
            if (key == "hf_prediction_sample_tooltip") return "Callback sample:\n" + fallback;
            return fallback;
        };
        HfPredictionDialog dialog(nullptr, translation);
        QCOMPARE(dialog.windowTitle(), QString("Callback HF"));
        QCOMPARE(dialog.findChild<QPushButton *>("hfCalculate")->text(), QString("Run callback"));
        QCOMPARE(dialog.findChild<QDoubleSpinBox *>("hfTxGain")->toolTip(), QString("Callback isotropic"));
        QCOMPARE(dialog.findChild<QDoubleSpinBox *>("hfRxGain")->toolTip(), QString("Callback isotropic"));
        QVERIFY(dialog.setRequest(request())); dialog.setRuntime(runtime());
        QSignalSpy done(&dialog, &HfPredictionDialog::resultReady);
        dialog.findChild<QPushButton *>("hfCalculate")->click();
        auto *status = dialog.findChild<QLabel *>("hfStatus");
        QCOMPARE(status->text(), QString("Callback calculating"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 15000);
        QCOMPARE(status->text(), QString("Completed 6 cells; 1981-04; R12 140"));
        auto *grid = dialog.findChild<QTableWidget *>("hfGrid");
        QCOMPARE(grid->horizontalHeaderItem(0)->text(), QString("UTC hour 00"));
        QVERIFY(grid->item(0, 0)->toolTip().startsWith("Callback sample:\n"));
        QVERIFY(!grid->item(0, 0)->toolTip().contains("%14"));
        QVERIFY(!grid->item(0, 0)->toolTip().contains("%%"));
        dialog.findChild<QPushButton *>("hfCalculate")->click();
        dialog.findChild<QPushButton *>("hfCancel")->click();
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QString("Callback cancelled"), 10000);
        QVERIFY(seen.contains("hf_prediction_ready"));
        QVERIFY(seen.contains("hf_prediction_metric_bcr"));
        QVERIFY(seen.contains("hf_prediction_frequencies_tooltip"));
        QVERIFY2(mismatches.isEmpty(), qPrintable(mismatches.join(", ")));
        HfPredictionDialog empty(nullptr, [](const QString &, const QString &) { return QString(); });
        QCOMPARE(empty.windowTitle(), QString("HF prediction / ITU-R P.533"));
        QCOMPARE(empty.findChild<QPushButton *>("hfCalculate")->text(), QString("Calculate"));
        HfPredictionDialog standalone;
        QCOMPARE(standalone.windowTitle(), QString("HF prediction / ITU-R P.533"));
        QCOMPARE(standalone.findChild<QPushButton *>("hfCalculate")->text(), QString("Calculate"));
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    // Windows' offscreen Qt platform has no system font discovery.
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
    app.setFont(QFont("Segoe UI", 10));
#endif
    if (argc < 2) return 2;
    helper = QString::fromLocal8Bit(argv[1]);
    if (argc > 2) dataDirectory = QString::fromLocal8Bit(argv[2]);
    int testArgc = 1; HfSelfTest test; return QTest::qExec(&test, testArgc, argv);
}
#include "itu_hf_selftest.moc"
