#include "meshports.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);Mesh::PortPlan plan;QString error;
    const QJsonObject input{{"nodes",QJsonArray{QJsonObject{{"id","A"},{"protocol",QJsonObject{{"type","meshtastic"},{"profileId","unit"}}}}}},{"links",QJsonArray()}};
    if(!plan.configure(input,"data",&error))return 1;
    auto config=plan.protocol("A");const auto type=config.value("type").toString();config.remove("type");config.remove("profileId");
    Mesh::PortPlan assigned;assigned=std::move(plan);
    const auto result=assigned.protocol("A");
    std::printf("type=%s plan=%s\n",type.toUtf8().constData(),QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    if(type!="meshtastic"||result.value("type")!="meshtastic"||result.value("profileId")!="unit")return 1;
    if(!assigned.configure({{"nodes",QJsonArray{QJsonObject{{"id","A"}}}},{"links",QJsonArray()}},"data",&error)||assigned.protocol("A").value("type")!="lab")return 1;
    std::puts("PASS port plan preserves public protocol metadata across configuration extraction and move");return 0;
}
