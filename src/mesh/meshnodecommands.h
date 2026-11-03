#pragma once
#include "meshnode.h"
#include "../core/commanddispatcher.h"

inline void registerMeshNodeCommands(CommandDispatcher &dispatcher,QObject *owner) {
    auto service=new Mesh::NodeService(owner);
    CommandDispatcher::Spec spec;
    spec.name="mesh.service";spec.description="Start/status/stop optional mesh node service; RF TX needs a separate local authorization";
    spec.access=CommandDispatcher::Access::Files;
    spec.inputSchema={{"type","object"},{"properties",QJsonObject{
        {"operation",QJsonObject{{"type","string"},{"enum",QJsonArray{"start","status","stop","revoke","registrations","directory-on","directory-off","directory-client"}}}},
        {"clientId",QJsonObject{{"type","string"},{"maxLength",64}}},
        {"allowRadioRx",QJsonObject{{"type","boolean"}}},
        {"radioSources",QJsonObject{{"type","array"},{"maxItems",8}}},
        {"reticulum",QJsonObject{{"type","object"}}},
        {"protocolProfiles",QJsonObject{{"type","object"}}},
        {"radioTxProfiles",QJsonObject{{"type","object"}}},
        {"directoryEnabled",QJsonObject{{"type","boolean"}}},
        {"directoryClient",QJsonObject{{"type","object"}}},
        {"optionsFile",QJsonObject{{"type","string"},{"maxLength",4096}}},
        {"protocolStateDirectory",QJsonObject{{"type","string"},{"maxLength",4096}}},
        {"bind",QJsonObject{{"type","string"},{"maxLength",64}}},
        {"port",QJsonObject{{"type","integer"},{"minimum",0},{"maximum",65535}}}}},
        {"required",QJsonArray{"operation"}},{"additionalProperties",false}};
    spec.positional=QStringList{"operation","bind","port"};
    dispatcher.add(spec,[service](QJsonObject args){
        if(!args.contains("port"))args["port"]=17690;
        auto reply=service->command(args);if(reply.contains("instanceId"))reply["nodeInstanceId"]=reply.value("instanceId");return reply;
    });
    CommandDispatcher::Spec rf;rf.name="mesh.rf";rf.description="Locally arm bounded mesh RF profiles for a limited time, or disarm immediately";rf.access=CommandDispatcher::Access::Transmit;
    rf.inputSchema={{"type","object"},{"properties",QJsonObject{
        {"operation",QJsonObject{{"type","string"},{"enum",QJsonArray{"arm","disarm"}}}},
        {"seconds",QJsonObject{{"type","integer"},{"minimum",1},{"maximum",3600}}}}},
        {"required",QJsonArray{"operation"}},{"additionalProperties",false}};
    rf.positional=QStringList{"operation","seconds"};
    dispatcher.add(rf,[service](QJsonObject args){args["operation"]=args.value("operation")=="arm"?"arm-tx":"disarm-tx";if(!args.contains("seconds"))args["seconds"]=60;return service->command(args);});
}
