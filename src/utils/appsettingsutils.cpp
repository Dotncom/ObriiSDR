#include "appsettingsutils.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QStandardPaths>

QString persistentSettingsFilePath() {
    static const QString settingsPath = []() {
        QString configDirectory =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        if (configDirectory.isEmpty()) {
            configDirectory = QDir(
                QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
                                  .filePath(QStringLiteral("ObriiSDR"));
        }
        if (configDirectory.isEmpty()) {
            configDirectory = QCoreApplication::applicationDirPath();
        }

        QDir directory(configDirectory);
        if (!directory.exists() && !QDir().mkpath(configDirectory)) {
            qWarning() << "[Settings] could not create per-user settings directory"
                       << configDirectory;
        }

        const QString userPath = directory.filePath(QStringLiteral("ObriiSDR.ini"));
        if (!QFileInfo::exists(userPath)) {
            const QString genericConfig =
                QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
            const QStringList legacyCandidates = {
                QDir(genericConfig).filePath(QStringLiteral("FobosAPP/FobosAPP.ini")),
                QDir(QCoreApplication::applicationDirPath())
                    .filePath(QStringLiteral("ObriiSDR.ini")),
                QDir(QCoreApplication::applicationDirPath())
                    .filePath(QStringLiteral("FobosAPP.ini"))
            };
            for (const QString &legacyPath : legacyCandidates) {
                if (!QFileInfo::exists(legacyPath) ||
                    QFileInfo(userPath).absoluteFilePath() ==
                        QFileInfo(legacyPath).absoluteFilePath()) {
                    continue;
                }
                if (QFile::copy(legacyPath, userPath)) {
                    qInfo() << "[Settings] migrated settings from"
                            << legacyPath << "to" << userPath;
                    break;
                }
                qWarning() << "[Settings] could not migrate legacy settings"
                           << legacyPath << "to" << userPath;
            }
        }
        return userPath;
    }();
    return settingsPath;
}
