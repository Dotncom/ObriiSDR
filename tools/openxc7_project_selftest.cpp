#include "openxc7project.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

static bool writeFile(const QString &path, const QByteArray &content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    if (!temp.isValid()) return 1;
    const QString root = temp.path();
    const QString xpr = QDir(root).filePath(QStringLiteral("demo.xpr"));
    if (!writeFile(QDir(root).filePath(QStringLiteral("top.v")), "module top(input clk, output reg led); always @(posedge clk) led <= ~led; endmodule\n") ||
        !writeFile(QDir(root).filePath(QStringLiteral("pins.xdc")), "set_property PACKAGE_PIN A1 [get_ports clk]\n")) return 2;
    const QByteArray project = QByteArrayLiteral(
        "<Project><Configuration><Option Name=\"Part\" Val=\"xc7k325tffg676-2\"/></Configuration>"
        "<FileSets><FileSet Name=\"sources_1\"><File Path=\"$PPRDIR/top.v\"><FileInfo>"
        "<Attr Name=\"UsedIn\" Val=\"synthesis\"/></FileInfo></File>"
        "<Option Name=\"TopModule\" Val=\"top\"/></FileSet>"
        "<FileSet Name=\"constrs_1\"><File Path=\"$PPRDIR/pins.xdc\"/></FileSet>"
        "</FileSets></Project>");
    if (!writeFile(xpr, project)) return 3;
    const auto report = OpenXc7Project::analyze(xpr);
    if (!report.canBuildBitstream() || report.verilogFiles.size() != 1 || report.top != QStringLiteral("top")) return 4;
    QString script;
    QString error;
    const QString output = QDir(root).filePath(QStringLiteral(".obrii-openxc7"));
    if (!OpenXc7Project::writeBitstreamBuild(report, output, &script, &error)) return 5;
    QFile build(script);
    if (!build.open(QIODevice::ReadOnly)) return 6;
    const QByteArray buildScript = build.readAll();
    if (!buildScript.contains("fpga-as") || !buildScript.contains("xilinx_gen.py") ||
        !buildScript.contains("bbasm")) return 6;
    if (!OpenXc7Project::writeModuleCheck(QDir(root).filePath(QStringLiteral("top.v")),
                                          QStringLiteral("top"), output, &script, &error)) return 7;
    if (!OpenXc7Project::writePreflight(output, &script, &error)) return 8;
    if (argc > 2) {
        const QString smokeDir = QString::fromLocal8Bit(argv[2]);
        if (!OpenXc7Project::writePreflight(smokeDir, &script, &error)) return 11;
        const QString smokeRtl = QDir(smokeDir).filePath(QStringLiteral("obrii_iq_pipeline.v"));
        if (QFileInfo::exists(smokeRtl) && !OpenXc7Project::writeModuleCheck(
            smokeRtl, QStringLiteral("obrii_iq_pipeline"), smokeDir, &script, &error)) return 12;
    }
    if (argc > 1) {
        const auto actual = OpenXc7Project::analyze(QString::fromLocal8Bit(argv[1]));
        QTextStream stream(stdout);
        stream << actual.summary() << Qt::endl;
        if (!actual.error.isEmpty()) return 9;
        if (actual.part == QStringLiteral("xc7k325tffg676-2") &&
            (actual.canBuildBitstream() || actual.edifFiles.size() != 2 ||
             actual.ipFiles.size() != 1 || actual.memoryInitFiles.size() != 2 ||
             actual.inactiveMissingFiles != 2)) return 10;
    }
    return 0;
}
