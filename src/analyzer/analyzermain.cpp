#include "analyzermainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ObriiAnalyzer"));
    QCoreApplication::setOrganizationName(QStringLiteral("Obrii"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Obrii Analyzer"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/obriisdr.png")));
    const bool connectOnStart = app.arguments().contains(QStringLiteral("--connect"));
    AnalyzerMainWindow window(connectOnStart);
    window.show();
    return app.exec();
}
