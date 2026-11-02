#include "meshwindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QStandardPaths>
int main(int argc,char **argv) {
    QApplication app(argc,argv);app.setApplicationName("ObriiMeshDesigner");app.setOrganizationName("Obrii");app.setApplicationVersion("0.1.0");
    QCommandLineParser parser;parser.addHelpOption();parser.addVersionOption();
    parser.addOption({"library","Device preset library JSON (defaults to application config)","path"});parser.process(app);
    const auto path=parser.isSet("library")?parser.value("library"):QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("devices.json");
    Mesh::Window window(path);window.show();return app.exec();
}
