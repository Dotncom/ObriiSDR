#pragma once

#include <QString>
#include <QStringList>

struct OpenXc7ProjectReport {
    QString projectFile;
    QString root;
    QString part;
    QString top;
    QString constraintFile;
    QStringList verilogFiles;
    QStringList headerFiles;
    QStringList vhdlFiles;
    QStringList edifFiles;
    QStringList ipFiles;
    QStringList memoryInitFiles;
    QStringList activeMissingFiles;
    QStringList externalFiles;
    QStringList unsupportedXdcCommands;
    int inactiveMissingFiles = 0;
    QString error;

    bool canBuildBitstream() const;
    QString summary() const;
};

class OpenXc7Project {
public:
    static OpenXc7ProjectReport analyze(const QString &xprPath);
    static bool writePreflight(const QString &outputDir, QString *scriptPath, QString *error);
    static bool writeModuleCheck(const QString &sourceFile, const QString &top,
                                 const QString &outputDir, QString *scriptPath, QString *error);
    static bool writeBitstreamBuild(const OpenXc7ProjectReport &report,
                                    const QString &outputDir, QString *scriptPath, QString *error);
};
