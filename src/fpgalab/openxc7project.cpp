#include "openxc7project.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QXmlStreamReader>

namespace {
QString resolveProjectPath(const QString &value, const QString &root, const QString &srcDir) {
    QString path = value;
    if (path.startsWith(QStringLiteral("$PPRDIR/")))
        path = QDir(root).filePath(path.mid(8));
    else if (path.startsWith(QStringLiteral("$PSRCDIR/")))
        path = QDir(srcDir).filePath(path.mid(9));
    else if (path.startsWith(QLatin1Char('$')))
        return QString();
    else if (QDir::isRelativePath(path))
        path = QDir(root).filePath(path);
    return QDir::cleanPath(path);
}

bool insideRoot(const QString &root, const QString &file) {
    const QString base = QDir::fromNativeSeparators(QDir(root).canonicalPath());
    const QString candidate = QDir::fromNativeSeparators(QFileInfo(file).canonicalFilePath());
    if (base.isEmpty() || candidate.isEmpty()) return false;
    const auto mode =
#ifdef Q_OS_WIN
        Qt::CaseInsensitive;
#else
        Qt::CaseSensitive;
#endif
    return candidate.compare(base, mode) == 0 || candidate.startsWith(base + QLatin1Char('/'), mode);
}

bool writeText(const QString &path, const QByteArray &data, QString *error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = QStringLiteral("Не вдалося записати %1").arg(path);
        return false;
    }
    return true;
}

bool prepareOutput(const QString &outputDir, QString *error) {
    if (outputDir.isEmpty() || !QDir().mkpath(outputDir)) {
        if (error) *error = QStringLiteral("Не вдалося створити теку збірки.");
        return false;
    }
    return true;
}

QString yosysPath(const QString &outputDir, const QString &file) {
    QString relative = QDir::fromNativeSeparators(
        QDir(outputDir).relativeFilePath(QFileInfo(file).absoluteFilePath()));
    relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
    relative.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + relative + QLatin1Char('"');
}

bool safeTop(const QString &top) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_$]*$"));
    return pattern.match(top).hasMatch();
}

QByteArray scriptHeader() {
    return QByteArrayLiteral(
        "#!/bin/sh\nset -eu\ncd \"$(dirname \"$0\")\"\n"
        "if [ -f /opt/openxc7/export.sh ]; then . /opt/openxc7/export.sh; fi\n");
}

QString relativeFiles(const QString &root, const QStringList &files) {
    QStringList relative;
    for (const QString &file : files) relative << QDir(root).relativeFilePath(file);
    return relative.join(QStringLiteral(", "));
}
}

bool OpenXc7ProjectReport::canBuildBitstream() const {
    static const QRegularExpression supportedPart(
        QStringLiteral("^xc7k(?:70|160|325|420|480)t[a-z0-9]+-[123]$"),
        QRegularExpression::CaseInsensitiveOption);
    return error.isEmpty() && supportedPart.match(part).hasMatch() && safeTop(top) &&
        !verilogFiles.isEmpty() && !constraintFile.isEmpty() &&
        vhdlFiles.isEmpty() && edifFiles.isEmpty() && ipFiles.isEmpty() && memoryInitFiles.isEmpty() &&
        activeMissingFiles.isEmpty() && externalFiles.isEmpty() && unsupportedXdcCommands.isEmpty();
}

QString OpenXc7ProjectReport::summary() const {
    if (!error.isEmpty()) return error;
    QStringList lines = {
        QStringLiteral("FPGA: %1 | top: %2").arg(part, top),
        QStringLiteral("Verilog/SystemVerilog: %1 | headers: %2 | VHDL: %3 | EDIF: %4 | IP: %5 | MIF: %6")
            .arg(verilogFiles.size()).arg(headerFiles.size()).arg(vhdlFiles.size())
            .arg(edifFiles.size()).arg(ipFiles.size()).arg(memoryInitFiles.size()),
        QStringLiteral("XDC: %1").arg(constraintFile.isEmpty() ? QStringLiteral("немає")
                                                          : QDir(root).relativeFilePath(constraintFile))};
    if (inactiveMissingFiles)
        lines << QStringLiteral("Вимкнені посилання на відсутні файли: %1 (не блокують)")
                     .arg(inactiveMissingFiles);
    if (!activeMissingFiles.isEmpty())
        lines << QStringLiteral("Відсутні активні файли: %1").arg(activeMissingFiles.join(QStringLiteral(", ")));
    if (!externalFiles.isEmpty())
        lines << QStringLiteral("Зовнішні файли: %1").arg(externalFiles.join(QStringLiteral(", ")));
    if (!vhdlFiles.isEmpty()) lines << QStringLiteral("VHDL потребує окремого GHDL-маршруту; автоматичний експорт поки заблоковано.");
    if (!edifFiles.isEmpty())
        lines << QStringLiteral("EDIF: %1. Потрібна заміна або перевірка для Yosys/nextpnr.")
                     .arg(relativeFiles(root, edifFiles));
    if (!ipFiles.isEmpty())
        lines << QStringLiteral("Vivado IP: %1. Потрібна HDL-заміна.")
                     .arg(relativeFiles(root, ipFiles));
    if (!memoryInitFiles.isEmpty())
        lines << QStringLiteral("MIF ініціалізації пам'яті: %1. Перенесення не перевірено.")
                     .arg(relativeFiles(root, memoryInitFiles));
    if (!unsupportedXdcCommands.isEmpty())
        lines << QStringLiteral("XDC містить команди, не перевірені для openXC7: %1")
                     .arg(unsupportedXdcCommands.join(QStringLiteral(", ")));
    lines << (canBuildBitstream()
        ? QStringLiteral("Можна пробувати збірку .bit. Результат усе одно потребує перевірки на обладнанні.")
        : QStringLiteral("Повну збірку образу заблоковано до усунення перелічених залежностей. Синтез окремого Verilog-модуля доступний."));
    return lines.join(QLatin1Char('\n'));
}

OpenXc7ProjectReport OpenXc7Project::analyze(const QString &xprPath) {
    OpenXc7ProjectReport report;
    report.projectFile = QFileInfo(xprPath).absoluteFilePath();
    report.root = QFileInfo(xprPath).absolutePath();
    QFile file(xprPath);
    if (!file.open(QIODevice::ReadOnly)) {
        report.error = QStringLiteral("Не вдалося відкрити Vivado .xpr.");
        return report;
    }
    QXmlStreamReader xml(&file);
    QString fileSet;
    QString currentPath;
    QString currentSet;
    bool disabled = false;
    bool usedInSynthesis = false;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = xml.name().toString();
            const auto attrs = xml.attributes();
            if (name == QStringLiteral("FileSet")) fileSet = attrs.value(QStringLiteral("Name")).toString();
            else if (name == QStringLiteral("File")) {
                currentPath = attrs.value(QStringLiteral("Path")).toString();
                currentSet = fileSet;
                disabled = false;
                usedInSynthesis = false;
            } else if (name == QStringLiteral("Option")) {
                const QString option = attrs.value(QStringLiteral("Name")).toString();
                if (option == QStringLiteral("Part") && report.part.isEmpty())
                    report.part = attrs.value(QStringLiteral("Val")).toString();
                if (option == QStringLiteral("TopModule") && fileSet == QStringLiteral("sources_1"))
                    report.top = attrs.value(QStringLiteral("Val")).toString();
            } else if (name == QStringLiteral("Attr") && !currentPath.isEmpty()) {
                const QString attr = attrs.value(QStringLiteral("Name")).toString();
                if (attr == QStringLiteral("AutoDisabled") && attrs.value(QStringLiteral("Val")) == QStringLiteral("1"))
                    disabled = true;
                if (attr == QStringLiteral("UsedIn") && attrs.value(QStringLiteral("Val")) == QStringLiteral("synthesis"))
                    usedInSynthesis = true;
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QString name = xml.name().toString();
            if (name == QStringLiteral("FileSet")) fileSet.clear();
            else if (name == QStringLiteral("File") && !currentPath.isEmpty()) {
                const QString projectName = QFileInfo(xprPath).completeBaseName();
                const QString resolved = resolveProjectPath(currentPath, report.root,
                    QDir(report.root).filePath(projectName + QStringLiteral(".srcs")));
                const QString suffix = QFileInfo(currentPath).suffix().toLower();
                const bool relevant = usedInSynthesis || currentSet == QStringLiteral("constrs_1") ||
                    suffix == QStringLiteral("xci") || suffix == QStringLiteral("bd") || suffix == QStringLiteral("edif");
                if (relevant) {
                    if (disabled) {
                        if (resolved.isEmpty() || !QFileInfo::exists(resolved)) ++report.inactiveMissingFiles;
                    } else if (resolved.isEmpty() || !QFileInfo::exists(resolved)) {
                        report.activeMissingFiles << currentPath;
                    } else if (!insideRoot(report.root, resolved)) {
                        report.externalFiles << currentPath;
                    } else if (suffix == QStringLiteral("v") || suffix == QStringLiteral("sv")) {
                        report.verilogFiles << resolved;
                    } else if (suffix == QStringLiteral("vh") || suffix == QStringLiteral("svh")) {
                        report.headerFiles << resolved;
                    } else if (suffix == QStringLiteral("vhd") || suffix == QStringLiteral("vhdl")) {
                        report.vhdlFiles << resolved;
                    } else if (suffix == QStringLiteral("edif") || suffix == QStringLiteral("edf")) {
                        report.edifFiles << resolved;
                    } else if (suffix == QStringLiteral("xci") || suffix == QStringLiteral("bd") ||
                               (suffix == QStringLiteral("dcp") && currentSet == QStringLiteral("sources_1"))) {
                        report.ipFiles << resolved;
                    } else if (suffix == QStringLiteral("mif")) {
                        report.memoryInitFiles << resolved;
                    } else if (suffix == QStringLiteral("xdc") && currentSet == QStringLiteral("constrs_1")) {
                        if (report.constraintFile.isEmpty()) report.constraintFile = resolved;
                    }
                }
                currentPath.clear();
            }
        }
    }
    if (xml.hasError()) {
        report.error = QStringLiteral("Помилка XML у .xpr: %1").arg(xml.errorString());
        return report;
    }
    report.verilogFiles.removeDuplicates();
    report.headerFiles.removeDuplicates();
    report.vhdlFiles.removeDuplicates();
    report.edifFiles.removeDuplicates();
    report.ipFiles.removeDuplicates();
    report.memoryInitFiles.removeDuplicates();
    if (!report.constraintFile.isEmpty()) {
        QFile xdc(report.constraintFile);
        if (xdc.open(QIODevice::ReadOnly) && xdc.size() < 2 * 1024 * 1024) {
            while (!xdc.atEnd()) {
                const QString line = QString::fromUtf8(xdc.readLine()).trimmed();
                if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
                const QString command = line.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
                if (!QStringList{QStringLiteral("set_property"), QStringLiteral("create_clock")}.contains(command) &&
                    !report.unsupportedXdcCommands.contains(command))
                    report.unsupportedXdcCommands << command;
            }
        }
    }
    return report;
}

bool OpenXc7Project::writePreflight(const QString &outputDir, QString *scriptPath, QString *error) {
    if (!prepareOutput(outputDir, error)) return false;
    const QString path = QDir(outputDir).filePath(QStringLiteral("preflight.sh"));
    QByteArray script = scriptHeader();
    script += QByteArrayLiteral(
        "printf 'openXC7 toolchain preflight\\n'\n"
        "for tool in yosys nextpnr-xilinx bbasm fpga-as python3; do command -v \"$tool\" || exit 1; done\n"
        "test -n \"${PRJXRAY_DB_DIR:-}\" || { echo 'PRJXRAY_DB_DIR is missing'; exit 1; }\n"
        "test -d \"$PRJXRAY_DB_DIR/kintex7\" || { echo 'Kintex-7 database is missing'; exit 1; }\n"
        "nextpnr_dir=${NEXTPNR_XILINX_DIR:-$(dirname \"$(dirname \"$(command -v nextpnr-xilinx)\")\")}\n"
        "test -f \"$nextpnr_dir/share/nextpnr/himbaechel/uarch/xilinx/gen/xilinx_gen.py\" || { echo 'Kintex chipdb generator is missing'; exit 1; }\n"
        "printf 'PRJXRAY_DB_DIR=%s\\nKintex chipdb will be generated on first full build if absent.\\n' \"$PRJXRAY_DB_DIR\"\n");
    if (!writeText(path, script, error)) return false;
    if (scriptPath) *scriptPath = path;
    return true;
}

bool OpenXc7Project::writeModuleCheck(const QString &sourceFile, const QString &top,
                                      const QString &outputDir, QString *scriptPath, QString *error) {
    const QString suffix = QFileInfo(sourceFile).suffix().toLower();
    if (!safeTop(top) || (suffix != QStringLiteral("v") && suffix != QStringLiteral("sv")) ||
        !QFileInfo::exists(sourceFile)) {
        if (error) *error = QStringLiteral("Оберіть Verilog/SystemVerilog-файл і коректну назву top-модуля.");
        return false;
    }
    if (!prepareOutput(outputDir, error)) return false;
    const QString script = QStringLiteral("read_verilog -sv -I%1 %2\n"
        "synth_xilinx -flatten -abc9 -arch xc7 -top %3\n"
        "stat\nwrite_json module-check.json\n")
        .arg(yosysPath(outputDir, QFileInfo(sourceFile).absolutePath()),
             yosysPath(outputDir, sourceFile), top);
    if (!writeText(QDir(outputDir).filePath(QStringLiteral("module-check.ys")), script.toUtf8(), error)) return false;
    const QString path = QDir(outputDir).filePath(QStringLiteral("module-check.sh"));
    const QByteArray shell = scriptHeader() + QByteArrayLiteral(
        "if yosys -Q -T -l module-check.log -s module-check.ys >/dev/null 2>&1; then\n"
        "  tail -n 16 module-check.log\n"
        "else\n"
        "  tail -n 80 module-check.log\n"
        "  exit 1\n"
        "fi\n");
    if (!writeText(path, shell, error)) return false;
    if (scriptPath) *scriptPath = path;
    return true;
}

bool OpenXc7Project::writeBitstreamBuild(const OpenXc7ProjectReport &report,
                                         const QString &outputDir, QString *scriptPath, QString *error) {
    if (!report.canBuildBitstream() || !safeTop(report.top)) {
        if (error) *error = QStringLiteral("Проєкт має непереносимі залежності; спершу усуньте блокери зі звіту.");
        return false;
    }
    if (!prepareOutput(outputDir, error)) return false;
    QString yosys;
    QSet<QString> includeDirs;
    for (const QString &source : report.verilogFiles)
        includeDirs.insert(QFileInfo(source).absolutePath());
    for (const QString &header : report.headerFiles)
        includeDirs.insert(QFileInfo(header).absolutePath());
    QStringList options;
    for (const QString &dir : includeDirs) options << QStringLiteral("-I%1").arg(yosysPath(outputDir, dir));
    options.sort();
    for (const QString &source : report.verilogFiles)
        yosys += QStringLiteral("read_verilog -sv %1 %2\n")
            .arg(options.join(QLatin1Char(' ')), yosysPath(outputDir, source));
    yosys += QStringLiteral("synth_xilinx -flatten -abc9 -arch xc7 -top %1\nwrite_json full.json\n")
                 .arg(report.top);
    if (!writeText(QDir(outputDir).filePath(QStringLiteral("full.ys")), yosys.toUtf8(), error)) return false;
    const QString dbPart = report.part.section(QLatin1Char('-'), 0, 0);
    const auto dieMatch = QRegularExpression(QStringLiteral("^(xc7k(?:70|160|325|420|480)t)"),
                                             QRegularExpression::CaseInsensitiveOption).match(dbPart);
    if (!dieMatch.hasMatch()) {
        if (error) *error = QStringLiteral("Невідома Kintex-7 мікросхема для генерації chipdb.");
        return false;
    }
    const QString xdc = QDir::fromNativeSeparators(QDir(outputDir).relativeFilePath(report.constraintFile));
    if (xdc.contains(QLatin1Char('\'')) || xdc.contains(QLatin1Char('\n'))) {
        if (error) *error = QStringLiteral("Шлях XDC містить недопустимі символи для shell.");
        return false;
    }
    QByteArray shell = scriptHeader();
    shell += QByteArrayLiteral(
        ": \"${PRJXRAY_DB_DIR:?Set PRJXRAY_DB_DIR}\"\n"
        "nextpnr_dir=${NEXTPNR_XILINX_DIR:-$(dirname \"$(dirname \"$(command -v nextpnr-xilinx)\")\")}\n"
        "chipdb_dir=${KINTEX7_CHIPDB:-$PWD/chipdb}\n"
        "mkdir -p \"$chipdb_dir\"\n"
        "yosys -s full.ys\n");
    shell += QStringLiteral("if [ ! -f \"$chipdb_dir/%1.bin\" ]; then\n"
        "  python3 \"$nextpnr_dir/share/nextpnr/himbaechel/uarch/xilinx/gen/xilinx_gen.py\" "
        "--xray \"$PRJXRAY_DB_DIR/kintex7\" --device %4 --bba \"%1.bba\"\n"
        "  bbasm -l \"%1.bba\" \"$chipdb_dir/%1.bin\"\n"
        "  rm -f \"%1.bba\"\n"
        "fi\n"
        "nextpnr-xilinx --chipdb \"$chipdb_dir/%1.bin\" "
        "--xdc '%2' --json full.json --fasm full.fasm\n"
        "trap 'rm -f full.bit.tmp' EXIT\n"
        "fpga-as --prjxray_db_path=\"${PRJXRAY_DB_DIR}/kintex7\" --part %3 full.fasm > full.bit.tmp\n"
        "mv full.bit.tmp full.bit\ntrap - EXIT\n")
        .arg(dbPart, xdc, report.part, dieMatch.captured(1)).toUtf8();
    const QString path = QDir(outputDir).filePath(QStringLiteral("full.sh"));
    if (!writeText(path, shell, error)) return false;
    if (scriptPath) *scriptPath = path;
    return true;
}
