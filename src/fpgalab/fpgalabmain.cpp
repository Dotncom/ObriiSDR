#include "fpgalabwindow.h"

#include <QApplication>
#include <QFileInfo>
#include <QIcon>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Obrii FPGA Lab"));
    app.setOrganizationName(QStringLiteral("ObriiSDR"));
    app.setStyle(QStringLiteral("Fusion"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/obriisdr.png")));
    FpgaLabWindow window;
    if (argc > 1)
        window.openPath(QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath());
    window.show();
    return app.exec();
}
