#include "meshnodecli.h"
#include "meshnode.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QSaveFile>
#include <QFile>
#include <QTimer>
#include <QTextStream>
#include <cmath>

namespace Mesh {
int runNodeService(QCoreApplication &app,const QStringList &args) {
    QCommandLineParser parser;parser.setApplicationDescription("Obrii distributed mesh node: data / IQ / locally authorized radio bridges");parser.addHelpOption();
    parser.addOption({"bind","Explicit listener bind address; trusted LAN only","IP","127.0.0.1"});
    parser.addOption({"mesh-port","Mesh node TCP port (0 = OS-assigned)","port","17690"});
    parser.addOption({"credentials-file","Private local startup JSON (includes token); otherwise stdout","path"});
    parser.addOption({"seconds","Stop after this many seconds; 0 = until terminated","seconds","0"});
    parser.addOption({"node-options","Private local JSON: radio RX allowlist, protocol profiles and Reticulum runtime","path"});
    parser.addOption({"directory","Enable the metadata-only node directory on this mesh service (trusted LAN/VPN only)"});
    parser.addOption({"directory-client-file","Private JSON: directory host, port, dedicated token, optional advertiseHost","path"});
    parser.addOption({"rf-confirm","Explicitly authorize bounded RF TX profiles from --node-options"});
    parser.addOption({"rf-arm-seconds","RF authorization lifetime, 1..3600 seconds; requires --rf-confirm","seconds","60"});
    auto fail=[](const QString &s){QTextStream(stderr)<<s<<Qt::endl;return 1;};
    if(!parser.parse(args))return fail(parser.errorText());
    if(parser.isSet("help")){QTextStream(stdout)<<parser.helpText()<<Qt::endl;return 0;}
    if(!parser.positionalArguments().isEmpty())return fail("Unexpected mesh-node arguments");
    bool valid=false;int port=parser.value("mesh-port").toInt(&valid);if(!valid||port<0||port>65535)return fail("Invalid mesh port");
    double seconds=parser.value("seconds").toDouble(&valid);if(!valid||!std::isfinite(seconds)||seconds<0||seconds>86400)return fail("Invalid duration 0..86400 seconds");
    QJsonObject options;
    if(parser.isSet("node-options"))options["optionsFile"]=parser.value("node-options");
    if(parser.isSet("directory"))options["directoryEnabled"]=true;
    if(parser.isSet("directory-client-file")){
        QFile file(parser.value("directory-client-file"));QJsonParseError error;
        if(!file.open(QIODevice::ReadOnly)||file.size()>16384)return fail("Cannot read private directory client configuration (limit 16 KiB)");
        const auto document=QJsonDocument::fromJson(file.readAll(),&error);if(error.error!=QJsonParseError::NoError||!document.isObject())return fail("Directory client configuration must be a JSON object");
        options["directoryClient"]=document.object();
    }
    options["operation"]="start";options["bind"]=parser.value("bind");options["port"]=port;
    NodeService service;auto reply=service.command(options);
    if(!reply.value("ok").toBool())return fail(reply.value("error").toString());
    if(parser.isSet("rf-confirm")){
        const int armSeconds=parser.value("rf-arm-seconds").toInt(&valid);if(!valid||armSeconds<1||armSeconds>3600)return fail("RF arm lifetime must be 1..3600 seconds");
        const auto armed=service.command({{"operation","arm-tx"},{"seconds",armSeconds}});if(!armed.value("ok").toBool())return fail(armed.value("error").toString());
        const auto token=reply.value("token"),pairing=reply.value("pairingSecret");reply=armed;reply["token"]=token;reply["pairingSecret"]=pairing;
    }else if(parser.isSet("rf-arm-seconds"))return fail("--rf-arm-seconds requires --rf-confirm");
    const auto bytes=QJsonDocument(reply).toJson(QJsonDocument::Compact)+'\n';
    if(parser.isSet("credentials-file")){
        QSaveFile file(parser.value("credentials-file"));if(!file.open(QIODevice::WriteOnly))return fail(file.errorString());
        file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
        if(file.write(bytes)!=bytes.size()||!file.commit())return fail(file.errorString());
    }else QTextStream(stdout)<<QString::fromUtf8(bytes)<<Qt::flush;
    if(seconds>0)QTimer::singleShot(int(seconds*1000),&app,&QCoreApplication::quit);
    return app.exec();
}
}
