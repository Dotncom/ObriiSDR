#include "meshpacketengine.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <cstdio>

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);app.setApplicationName("ObriiMeshSim");app.setApplicationVersion("0.1.0");
    QCommandLineParser parser;parser.setApplicationDescription("Deterministic packet-only mesh simulation; no RF, IQ or network sockets");
    parser.addHelpOption();parser.addVersionOption();
    parser.addOption({"scenario","Packet scenario JSON","file"});parser.addOption({"output","Result JSON file (otherwise stdout)","file"});parser.process(app);
    auto failure=[](const QString &error){const auto wire=QJsonDocument(QJsonObject{{"ok",false},{"error",error}}).toJson(QJsonDocument::Compact);std::fprintf(stderr,"%s\n",wire.constData());return 1;};
    if(!parser.isSet("scenario"))return failure("Specify --scenario FILE.json");
    QFile file(parser.value("scenario"));
    if(!file.open(QIODevice::ReadOnly)||file.size()>16*1024*1024)return failure("Cannot read scenario or file exceeds 16 MiB");
    QJsonParseError parse;const auto document=QJsonDocument::fromJson(file.readAll(),&parse);
    if(parse.error!=QJsonParseError::NoError||!document.isObject())return failure("Scenario must be a JSON object");
    Mesh::PacketEngine engine;QString error;
    if(!Mesh::PacketEngine::replay(document.object(),&engine,&error))return failure(error);
    const auto result=engine.snapshot();const auto wire=QJsonDocument(result).toJson(QJsonDocument::Indented);
    if(parser.isSet("output")){
        if(QFileInfo(parser.value("output")).absoluteFilePath().compare(QFileInfo(file).absoluteFilePath(),
#ifdef Q_OS_WIN
            Qt::CaseInsensitive
#else
            Qt::CaseSensitive
#endif
        )==0)return failure("Output must not overwrite the scenario");
        QSaveFile output(parser.value("output"));
        if(!output.open(QIODevice::WriteOnly)||output.write(wire)!=wire.size()||!output.commit())return failure("Cannot save simulation results");
        const auto summary=QJsonDocument(QJsonObject{{"ok",true},{"delivered",result.value("delivered")},{"dropped",result.value("dropped")},{"pending",engine.pending()}}).toJson(QJsonDocument::Compact);
        std::printf("%s\n",summary.constData());
    } else if(std::fwrite(wire.constData(),1,size_t(wire.size()),stdout)!=size_t(wire.size()))return 1;
    return 0;
}
