#include "analyzermainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ObriiAnalyzer"));
    QCoreApplication::setOrganizationName(QStringLiteral("Obrii"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Obrii Analyzer"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/obriisdr.png")));
    if (qEnvironmentVariableIsEmpty("UHD_IMAGES_DIR")) {
        const QString images = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("uhd-images"));
        if (QFileInfo(QDir(images).filePath(QStringLiteral("usrp_b200_fw.hex"))).isFile())
            qputenv("UHD_IMAGES_DIR", QFile::encodeName(images));
    }
    const bool connectOnStart = app.arguments().contains(QStringLiteral("--connect"));
    AnalyzerMainWindow window(connectOnStart);
    window.show();
    return app.exec();
}
