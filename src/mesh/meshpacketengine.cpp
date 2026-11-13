#include "meshpacketengine.h"
#include "meshproject.h"
#include <QRegularExpression>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Mesh {
namespace {
constexpr qint64 IqDecodeTimeoutMs = 15000;
constexpr qint64 IqLinkRecoveryMs = 30000;
constexpr int PacketOverheadBytes = 8;
bool fail(QString *error,const QString &text){if(error)*error=text;return false;}
bool address(const QString &id){return QRegularExpression("^[A-Za-z][A-Za-z0-9_.-]{0,63}$").match(id).hasMatch();}
bool integer(const QJsonValue &v,int low,int high){return v.isDouble()&&v.toDouble()>=low&&v.toDouble()<=high&&v.toDouble()==v.toInt(-1);}
QJsonArray array(const QStringList &list){QJsonArray a;for(const auto &s:list)a.append(s);return a;}
bool validMotion(const PacketEngine::NodeMotion &motion) {
    return std::abs(motion.position.x()) <= PacketEngine::MaximumPositionM &&
           std::abs(motion.position.y()) <= PacketEngine::MaximumPositionM &&
           std::abs(motion.velocity.x()) <= PacketEngine::MaximumVelocityMps &&
           std::abs(motion.velocity.y()) <= PacketEngine::MaximumVelocityMps;
}

QJsonObject motionJson(const PacketEngine::NodeMotion &motion) {
    return {{"xM", motion.position.x()}, {"yM", motion.position.y()},
            {"vxMps", motion.velocity.x()}, {"vyMps", motion.velocity.y()}};
}

bool readMotion(const QJsonValue &value, PacketEngine::NodeMotion &motion) {
    const auto object = value.toObject();
    if (object.size() != 4 || !object.value("xM").isDouble() || !object.value("yM").isDouble() ||
        !object.value("vxMps").isDouble() || !object.value("vyMps").isDouble())
        return false;
    motion = {{object.value("xM").toDouble(), object.value("yM").toDouble()},
              {object.value("vxMps").toDouble(), object.value("vyMps").toDouble()}};
    return validMotion(motion);
}
}
bool PacketEngine::configure(const Project &p,QString *error) {
    QJsonArray ns,ls;
    for(const auto &n:p.nodes)ns.append(QJsonObject{{"id",n.id},{"name",n.name},{"enabled",n.enabled}});
    for(const auto &l:p.links)ls.append(QJsonObject{{"tx",l.tx},{"rx",l.rx},{"enabled",l.enabled},{"delayMs",l.delayMs}});
    return configure(QJsonObject{{"nodes",ns},{"links",ls}},error);
}
bool PacketEngine::configure(const QJsonObject &value,QString *error) {
    if(!value.value("nodes").isArray()||!value.value("links").isArray()||value.value("nodes").toArray().isEmpty()||
       value.value("nodes").toArray().size()>MaximumNodes||value.value("links").toArray().size()>MaximumNodes*(MaximumNodes-1))
        return fail(error,"Topology needs 1..64 nodes and at most 4032 directed links");
    PacketEngine candidate;
    if (value.contains("radioRangeM")) {
        candidate.radioRangeM = value.value("radioRangeM").toDouble(-1);
        if (!(candidate.radioRangeM >= 0 && candidate.radioRangeM <= MaximumCoordinateM))
            return fail(error, "Invalid packet radio range");
    }
    if (value.contains("airBitrate")) {
        if (!integer(value.value("airBitrate"), 0, MaximumAirBitrate))
            return fail(error, "Invalid packet air bitrate");
        candidate.airBitsPerSecond = value.value("airBitrate").toInt();
    }
    for(const auto &v:value.value("nodes").toArray()){
        const auto n=v.toObject();const auto id=n.value("id").toString(),name=n.value("name").toString();
        if(!v.isObject()||!address(id)||candidate.nodes.contains(id)||name.trimmed().isEmpty()||name.size()>200||
           (n.contains("enabled")&&!n.value("enabled").isBool()))
            return fail(error,"Invalid or duplicate packet node");
        Node node;node.name=name;node.enabled=n.value("enabled").toBool(true);
        if (n.contains("motion")) {
            if (!readMotion(n.value("motion"), node.motion)) return fail(error, "Invalid packet node motion");
            node.hasMotion = true;
        }
        candidate.nodes.insert(id,node);
    }
    for(const auto &v:value.value("links").toArray()){
        const auto l=v.toObject();const auto tx=l.value("tx").toString(),rx=l.value("rx").toString(),key=tx+"/"+rx;
        if(!v.isObject()||!candidate.nodes.contains(tx)||!candidate.nodes.contains(rx)||tx==rx||candidate.links.contains(key)||
           !l.value("enabled").isBool()||!integer(l.value("delayMs"),0,60000))return fail(error,"Invalid or duplicate packet link");
        candidate.links.insert(key,{tx,rx,l.value("enabled").toBool(),l.value("delayMs").toInt()});
    }
    candidate.initial=candidate.topology();*this=std::move(candidate);return true;
}
QJsonObject PacketEngine::topology() const {
    QJsonArray ns,ls;
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it) {
        QJsonObject node{{"id", it.key()}, {"name", it->name}, {"enabled", it->enabled}};
        if (it->hasMotion) node.insert("motion", motionJson(it->motion));
        ns.append(node);
    }
    for(const auto &l:links)ls.append(QJsonObject{{"tx",l.tx},{"rx",l.rx},{"enabled",l.enabled},{"delayMs",l.delayMs}});
    QJsonObject result{{"nodes", ns}, {"links", ls}};
    if (radioRangeM != 0.0) result.insert("radioRangeM", radioRangeM);
    if (airBitsPerSecond != 0) result.insert("airBitrate", airBitsPerSecond);
    return result;
}
bool PacketEngine::record(const QJsonObject &action,QString *error) {
    if(actions.size()>=MaximumActions)return fail(error,"Experiment reached its 10000-action limit; reset or open a new scenario");
    const int size=QJsonDocument(action).toJson(QJsonDocument::Compact).size()+1;
    if(size>7*1024*1024-actionBytes)return fail(error,"Experiment action data exceeds 7 MiB; reset or open a new scenario");
    if(!advanceTo(timeMs,MaximumEvents))return fail(error,"Current-time event budget exceeded");
    actions.append(action);actionBytes+=size;
    return true;
}
void PacketEngine::changed(){++revision;parents.clear();}
bool PacketEngine::setLink(const QString &tx,const QString &rx,bool enabled,int delay,QString *error) {
    if(!nodes.contains(tx)||!nodes.contains(rx)||tx==rx||delay<0||delay>60000)return fail(error,"Invalid packet link");
    if(!record({{"atMs",double(timeMs)},{"op","link"},{"tx",tx},{"rx",rx},{"enabled",enabled},{"delayMs",delay}},error))return false;
    const QString key=tx+"/"+rx;
    const auto existing=links.constFind(key);
    if(existing==links.cend()||existing->enabled!=enabled||existing->delayMs!=delay){links.insert(key,{tx,rx,enabled,delay});changed();}
    return true;
}
bool PacketEngine::setNodeEnabled(const QString &id,bool enabled,QString *error) {
    if(!nodes.contains(id))return fail(error,"Unknown packet node");
    if(!record({{"atMs",double(timeMs)},{"op","node"},{"id",id},{"enabled",enabled}},error))return false;
    if(nodes[id].enabled!=enabled){nodes[id].enabled=enabled;changed();}
    return true;
}
bool PacketEngine::setNodeMotion(const QString &id, const NodeMotion &motion, QString *error) {
    if (iqTransmit || !nodes.contains(id) || !validMotion(motion))
        return fail(error, "Invalid node motion for the packet model");
    if (!record({{"atMs", double(timeMs)}, {"op", "motion"}, {"id", id}, {"motion", motionJson(motion)}}, error))
        return false;
    auto &node = nodes[id];
    node.motion = motion;
    node.motionStartMs = timeMs;
    node.hasMotion = true;
    changed();
    return true;
}

bool PacketEngine::setRadioRange(double meters, QString *error) {
    if (iqTransmit || !(meters >= 0 && meters <= MaximumCoordinateM))
        return fail(error, "Invalid radio range for the packet model");
    if (meters == radioRangeM) return true;
    if (!record({{"atMs", double(timeMs)}, {"op", "range"}, {"meters", meters}}, error)) return false;
    radioRangeM = meters;
    changed();
    return true;
}

bool PacketEngine::setAirBitrate(int bitsPerSecond, QString *error) {
    if (iqTransmit || bitsPerSecond < 0 || bitsPerSecond > MaximumAirBitrate)
        return fail(error, "Invalid air bitrate for the packet model");
    if (bitsPerSecond == airBitsPerSecond) return true;
    if (pending()) return fail(error, "Finish pending packets before changing air bitrate");
    if (!record({{"atMs", double(timeMs)}, {"op", "airtime"}, {"bitrate", bitsPerSecond}}, error))
        return false;
    airBitsPerSecond = bitsPerSecond;
    lastBurstBySender.clear();
    changed();
    return true;
}

PacketEngine::NodeMotion PacketEngine::nodeMotion(const QString &id) const {
    const auto it = nodes.constFind(id);
    if (it == nodes.cend()) return {};
    auto motion = it->motion;
    motion.position += motion.velocity * (double(timeMs - it->motionStartMs) / 1000);
    return motion;
}

bool PacketEngine::hasMovingNodes() const {
    return !iqTransmit && std::any_of(nodes.cbegin(), nodes.cend(),
        [](const Node &node) { return !node.motion.velocity.isNull(); });
}

bool PacketEngine::withinRadioRange(const QString &tx, const QString &rx) const {
    if (iqTransmit || radioRangeM == 0.0) return true;
    const auto offset = nodeMotion(tx).position - nodeMotion(rx).position;
    return offset.x() * offset.x() + offset.y() * offset.y() <= radioRangeM * radioRangeM;
}

bool PacketEngine::canHear(const QString &tx, const QString &rx) const {
    const auto link = links.constFind(tx + '/' + rx);
    return link != links.cend() && link->enabled && nodes[tx].enabled && nodes[rx].enabled &&
           withinRadioRange(tx, rx);
}

QStringList PacketEngine::route(const QString &source,const QString &destination) const {
    if(!nodes.contains(source)||!nodes.contains(destination)||!nodes[source].enabled||!nodes[destination].enabled)return {};
    if (radioRangeM > 0 && routeTimeMs != timeMs && hasMovingNodes()) {
        parents.clear();
        routeTimeMs = timeMs;
    }
    if(!parents.contains(source)){
        QMap<QString,QString> previous;QQueue<QString> frontier;frontier.enqueue(source);previous.insert(source,source);
        while(!frontier.isEmpty()){
            const QString at=frontier.dequeue();
            // QMap order gives stable lexical tie-breaking between equal-hop routes.
            const QString prefix = at + QLatin1Char('/');
            for(auto it=std::as_const(links).lowerBound(prefix);it!=links.cend()&&it.key().startsWith(prefix);++it){
                const auto &l=it.value();
                if(l.tx!=at||!l.enabled||!nodes[l.rx].enabled||previous.contains(l.rx)||
                   iqLinkFailedUntil.contains(it.key()) || !withinRadioRange(l.tx, l.rx))continue;
                previous.insert(l.rx,at);frontier.enqueue(l.rx);
            }
        }
        parents.insert(source,previous);
    }
    const auto &previous=parents[source];if(!previous.contains(destination))return {};
    QStringList path;QString at=destination;
    for(int i=0;i<=MaximumNodes;++i){path.prepend(at);if(at==source)return path;at=previous.value(at);}
    return {};
}
QString PacketEngine::send(const QString &source,const QString &destination,const QByteArray &payload,int ttl,QString *error) {
    if(nextPacket==std::numeric_limits<quint64>::max()){fail(error,"Automatic packet IDs exhausted");return {};}
    const QString id="p"+QString::number(nextPacket+1);
    if(!submit({id,source,destination,payload,ttl},error))return {};return id;
}
bool PacketEngine::submit(const Datagram &p,QString *error) {
    if(iqTransmit&&(pending()>0||p.payload.size()>256))return fail(error,"IQ mode accepts one datagram at a time, payload at most 256 bytes");
    if(!nodes.contains(p.source)||!nodes.contains(p.destination)||p.payload.isEmpty()||p.payload.size()>MaximumPayload||p.ttl<0||p.ttl>64||
       !QRegularExpression("^[A-Za-z0-9_.:-]{1,64}$").match(p.id).hasMatch())return fail(error,"Packet needs known addresses, 1..4096 bytes, TTL 0..64 and an ID of at most 64 characters");
    if(!record({{"atMs",double(timeMs)},{"op","send"},{"id",p.id},{"source",p.source},{"destination",p.destination},
        {"ttl",p.ttl},{"payloadBase64",QString::fromLatin1(p.payload.toBase64())}},error))return false;
    const auto autoId=QRegularExpression("^p([0-9]{1,20})$").match(p.id);bool ok=false;
    if(autoId.hasMatch()){const auto n=autoId.captured(1).toULongLong(&ok);if(ok)nextPacket=std::max(nextPacket,n);}
    ++submitted;Event e;e.due=e.created=timeMs;e.at=p.source;e.packet=p;
    enqueue(std::move(e));return true;
}
bool PacketEngine::enqueue(Event e) {
    if(pending()>=MaximumEvents||nodes[e.at].queued>=QueuePerNode){finish(e,"queue_full");return false;}
    e.order=++order;++nodes[e.at].queued;events.push(std::move(e));return true;
}
bool PacketEngine::transmitPacketHop(Event arrival) {
    const auto sender = arrival.from;
    const auto burst = arrival.air;
    if (!enqueue(std::move(arrival))) return false;
    if (!burst) return true;
    for (auto it = lastBurstBySender.cbegin(); it != lastBurstBySender.cend(); ++it) {
        auto &other = *it.value();
        if (other.endMs <= timeMs || !std::as_const(nodes)[it.key()].enabled) continue;
        if (other.receiver == sender) other.halfDuplex = true;
        else if (canHear(sender, other.receiver)) other.collided = true;
        if (burst->receiver == it.key()) burst->halfDuplex = true;
        else if (canHear(it.key(), burst->receiver)) burst->collided = true;
    }
    lastBurstBySender.insert(sender, burst);
    return true;
}

void PacketEngine::trace(const Event &e,const QString &kind,const QString &next) {
    if(steps.size()>=4096){steps.pop_front();++traceEvicted;}
    steps.push_back({{"atMs",double(timeMs)},{"event",kind},{"id",e.packet.id},{"source",e.packet.source},
        {"node",e.at},{"previous",e.from},{"next",next},{"ttlRemaining",e.packet.ttl},{"revision",QString::number(revision)}});
}
void PacketEngine::finish(const Event &e,const QString &reason) {
    const bool success=reason=="delivered";
    if(success){++delivered;++nodes[e.at].delivered;bytes+=quint64(e.packet.payload.size());delayTotal+=quint64(timeMs-e.created);}
    else{++dropped;++nodes[e.at].dropped;++dropReasons[reason];}
    trace(e,reason);
    if(history.size()>=512){history.pop_front();++historyEvicted;}
    history.push_back({{"serial",QString::number(++serial)},{"id",e.packet.id},{"source",e.packet.source},{"destination",e.packet.destination},
        {"status",reason},{"atNode",e.at},{"atMs",double(timeMs)},{"latencyMs",double(timeMs-e.created)},{"path",array(e.path)},
        {"hops",std::max(0,e.path.size()-1)},{"payloadBase64",QString::fromLatin1(e.packet.payload.toBase64())}});
}
void PacketEngine::process(Event e) {
    --nodes[e.at].queued;
    if(!nodes[e.at].enabled){finish(e,"node_down");return;}
    if(!e.from.isEmpty()){
        const auto link=links.constFind(e.from+"/"+e.at);
        if(!nodes[e.from].enabled||link==links.cend()||!link->enabled){finish(e,"link_down");return;}
        if(!withinRadioRange(e.from,e.at)){finish(e,"out_of_range");return;}
    }
    if (e.air) {
        if (e.air->halfDuplex) { finish(e, "half_duplex"); return; }
        if (e.air->collided) { finish(e, "collision"); return; }
    }
    auto &node=nodes[e.at];const auto key=e.packet.source+"/"+e.packet.id;
    if(node.seen.contains(key)){finish(e,"duplicate");return;}
    if(node.seenOrder.size()>=4096)node.seen.remove(node.seenOrder.dequeue());
    node.seen.insert(key);node.seenOrder.enqueue(key);++node.received;e.path.append(e.at);
    if(e.at==e.packet.destination){finish(e,"delivered");return;}
    if(e.packet.ttl==0){finish(e,"ttl_expired");return;}
    const auto path=route(e.at,e.packet.destination);
    if(path.size()<2){finish(e,"no_route");return;}
    const auto link=links.value(e.at+"/"+path[1]);
    qint64 airtimeMs = 0;
    if (airBitsPerSecond && !iqTransmit) {
        const auto previous = lastBurstBySender.constFind(e.at);
        if (previous != lastBurstBySender.cend() && (*previous)->endMs > timeMs) {
            finish(e, "tx_busy");
            return;
        }
        const qint64 bitMilliseconds = (qint64(e.packet.payload.size()) + PacketOverheadBytes) * 8000;
        airtimeMs = (bitMilliseconds + airBitsPerSecond - 1) / airBitsPerSecond;
    }
    if(timeMs>MaximumTimeMs-1-link.delayMs-airtimeMs){finish(e,"time_limit");return;}
    trace(e,"forward",path[1]);
    Event next=e;next.from=e.at;next.at=path[1];next.due=timeMs+1+link.delayMs+airtimeMs;--next.packet.ttl;
    next.air = airtimeMs ? std::make_shared<AirBurst>(AirBurst{next.at, timeMs + airtimeMs}) : nullptr;
    if(iqTransmit){
        if(timeMs>MaximumTimeMs-IqDecodeTimeoutMs){finish(e,"time_limit");return;}
        if(!transmitIqHop(next, iqRetries))finish(e,"modem_tx_rejected");
        else if(e.at!=e.packet.source){++forwarded;++node.forwarded;}
        return;
    }
    if(transmitPacketHop(std::move(next))&&e.at!=e.packet.source){++forwarded;++node.forwarded;}
}
bool PacketEngine::attachIqBearer(IqTransmit transmit,QString *error) {
    if(pending()||!actions.isEmpty()||!transmit)return fail(error,"Attach IQ bearer only to a fresh experiment");
    iqTransmit=std::move(transmit);parents.clear();return true;
}
bool PacketEngine::setIqRecovery(int retries, bool avoidFailedLinks) {
    if(!iqTransmit||pending()||retries<0||retries>2)return false;
    iqRetries=retries;
    iqAvoidFailedLinks=avoidFailedLinks;
    if(!avoidFailedLinks&&!iqLinkFailedUntil.isEmpty()){
        iqLinkFailedUntil.clear();
        parents.clear();
    }
    return true;
}
bool PacketEngine::transmitIqHop(const Event &arrival, int retriesLeft) {
    const quint64 id=++nextFlight;
    flights.insert(id,{arrival,timeMs+IqDecodeTimeoutMs,retriesLeft});
    ++nodes[arrival.at].queued;
    QString error;
    if(iqTransmit(id,arrival.from,arrival.at,arrival.packet,&error))return true;
    flights.remove(id);
    --nodes[arrival.at].queued;
    return false;
}
void PacketEngine::expireIqLinkFailures() {
    for(auto it=iqLinkFailedUntil.begin();it!=iqLinkFailedUntil.end();){
        if(it.value()>timeMs){++it;continue;}
        it=iqLinkFailedUntil.erase(it);
        parents.clear();
    }
}
void PacketEngine::abortIq() {
    if(!iqTransmit)return;
    while(!events.empty()){auto e=events.top();events.pop();--nodes[e.at].queued;finish(e,"modem_stopped");}
    for(const auto &f:flights){--nodes[f.arrival.at].queued;finish(f.arrival,"modem_stopped");}
    flights.clear();
}
bool PacketEngine::receiveIq(quint64 id,const QString &from,const QString &at,const Datagram &packet,QString *error) {
    const auto flight=flights.constFind(id);
    if(!iqTransmit||flight==flights.cend()){++iqRejected;return fail(error,"Unknown or expired IQ hop");}
    auto event=flight->arrival;const auto &expected=event.packet;
    if(from!=event.from||at!=event.at||packet.id!=expected.id||packet.source!=expected.source||packet.destination!=expected.destination||
       packet.ttl!=expected.ttl||packet.payload!=expected.payload){++iqRejected;return fail(error,"Decoded IQ packet does not match pending hop");}
    flights.remove(id);--nodes[at].queued;event.packet=packet;event.due=timeMs;
    trace(event,"iq_decoded");return enqueue(std::move(event));
}
qint64 PacketEngine::nextDue() const {
    qint64 next=events.empty()?MaximumTimeMs+1:events.top().due;
    for(const auto &f:flights)next=std::min(next,f.deadline);return next;
}
bool PacketEngine::step() {
    if(!flights.isEmpty()&&(events.empty()||nextDue()<events.top().due)){
        auto earliest=flights.begin();for(auto it=flights.begin();it!=flights.end();++it)if(it->deadline<earliest->deadline)earliest=it;
        const Flight expired=earliest.value();
        const Event &e=expired.arrival;
        timeMs=expired.deadline;
        expireIqLinkFailures();
        flights.erase(earliest);
        --nodes[e.at].queued;
        const QString linkKey=e.from+"/"+e.at;
        const auto link=links.constFind(linkKey);
        if(expired.retriesLeft>0&&timeMs<=MaximumTimeMs-IqDecodeTimeoutMs&&
           nodes[e.at].enabled&&nodes[e.from].enabled&&link!=links.cend()&&link->enabled){
            trace(e,"iq_retry");
            ++iqRetried;
            if(!transmitIqHop(e,expired.retriesLeft-1))finish(e,"modem_tx_rejected");
            return true;
        }
        if(iqAvoidFailedLinks){
            iqLinkFailedUntil.insert(linkKey,timeMs+IqLinkRecoveryMs);
            parents.clear();
        }
        finish(e,"modem_timeout");
        return true;
    }
    if(events.empty())return false;
    auto event=events.top();events.pop();timeMs=event.due;expireIqLinkFailures();process(std::move(event));return true;
}
bool PacketEngine::advanceTo(qint64 target,int budget) {
    if(target<timeMs||target>MaximumTimeMs||budget<1)return false;
    while(pending()&&nextDue()<=target&&budget-->0)step();
    if(pending()&&nextDue()<=target)return false;
    timeMs=target;expireIqLinkFailures();return true;
}
QJsonArray PacketEngine::results(quint64 after) const {
    QJsonArray values;for(const auto &r:history)if(r.value("serial").toString().toULongLong()>after)values.append(r);return values;
}
QJsonObject PacketEngine::snapshot(bool includeHistory) const {
    QJsonArray ns,traceValues;QJsonObject reasons;
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it) {
        QJsonObject node{{"id", it.key()}, {"enabled", it->enabled}, {"queued", it->queued},
            {"received", QString::number(it->received)}, {"forwarded", QString::number(it->forwarded)},
            {"delivered", QString::number(it->delivered)}, {"dropped", QString::number(it->dropped)}};
        if (it->hasMotion) node.insert("motion", motionJson(nodeMotion(it.key())));
        ns.append(node);
    }
    if(includeHistory)for(const auto &v:steps)traceValues.append(v);
    for(auto it=dropReasons.cbegin();it!=dropReasons.cend();++it)reasons.insert(it.key(),QString::number(it.value()));
    return {{"format","obrii-packet-results"},{"version",1},{"model",iqTransmit?"ax25-afsk1200-fm-virtual-iq":"centralized-min-hop-packet-simulation"},
        {"timeMs",double(timeMs)},{"revision",QString::number(revision)},{"pending",pending()},{"submitted",QString::number(submitted)},
        {"delivered",QString::number(delivered)},{"forwarded",QString::number(forwarded)},{"dropped",QString::number(dropped)},
        {"deliveredBytes",QString::number(bytes)},{"meanDeliveryMs",delivered?double(delayTotal)/delivered:0.0},{"dropReasons",reasons},
        {"recordedActions",actions.size()},{"actionBytes",actionBytes},{"iqPending",flights.size()},{"rejectedIqFrames",QString::number(iqRejected)},
        {"iqRetries",QString::number(iqRetried)},{"iqLinksOnHold",iqLinkFailedUntil.size()},
        {"airBitrate",iqTransmit ? 0 : airBitsPerSecond},
        {"nodes",ns},{"results",includeHistory?results():QJsonArray{}},{"trace",traceValues},{"historyEvicted",QString::number(historyEvicted)},{"traceEvicted",QString::number(traceEvicted)}};
}
QJsonObject PacketEngine::scenario() {
    advanceTo(timeMs,MaximumEvents);
    return {{"format",iqTransmit?"obrii-iq-packet-plan":"obrii-packet-scenario"},{"version",1},{"topology",initial},{"actions",actions},{"untilMs",double(timeMs)}};
}
bool PacketEngine::replay(const QJsonObject &input,PacketEngine *output,QString *error) {
    if(!output)return fail(error,"Missing packet simulation output");
    if(input.value("format")!="obrii-packet-scenario"||input.value("version")!=1||!input.value("actions").isArray()||
       input.value("actions").toArray().size()>MaximumActions)return fail(error,"Invalid packet scenario format/version/actions");
    PacketEngine e;if(!e.configure(input.value("topology").toObject(),error))return false;
    int processed=0;
    auto advance=[&](qint64 target){
        while(!e.advanceTo(target,512))if(++processed>2000)return false;
        return true;
    };
    for(const auto &v:input.value("actions").toArray()){
        const auto a=v.toObject();
        if(!v.isObject()||!integer(a.value("atMs"),0,int(MaximumTimeMs))||a.value("atMs").toDouble()<e.now())return fail(error,"Actions must be chronological whole milliseconds");
        if(!advance(qint64(a.value("atMs").toDouble())))return fail(error,"Scenario event budget exceeded");
        const auto op=a.value("op").toString();
        if(op=="send"){
            if(!integer(a.value("ttl"),0,64)||!a.value("payloadBase64").isString())return fail(error,"Invalid packet TTL/payload");
            const auto encoded=a.value("payloadBase64").toString().toLatin1();
            if(encoded.size()>5464)return fail(error,"Packet exceeds 4096 bytes");
            const auto payload=QByteArray::fromBase64(encoded);
            if(payload.toBase64()!=encoded)return fail(error,"Payload must be canonical base64");
            if(!e.submit({a.value("id").toString(),a.value("source").toString(),a.value("destination").toString(),payload,a.value("ttl").toInt()},error))return false;
        } else if(op=="link"){
            if(!a.value("enabled").isBool()||!integer(a.value("delayMs"),0,60000))return fail(error,"Invalid link action");
            if(!e.setLink(a.value("tx").toString(),a.value("rx").toString(),a.value("enabled").toBool(),a.value("delayMs").toInt(),error))return false;
        } else if(op=="node"){
            if(!a.value("enabled").isBool()||!e.setNodeEnabled(a.value("id").toString(),a.value("enabled").toBool(),error))return fail(error,"Invalid node action");
        } else if (op == "motion") {
            NodeMotion motion;
            if (!readMotion(a.value("motion"), motion)) return fail(error, "Invalid node motion action");
            if (!e.setNodeMotion(a.value("id").toString(), motion, error)) return false;
        } else if (op == "range") {
            if (!e.setRadioRange(a.value("meters").toDouble(-1), error)) return false;
        } else if (op == "airtime") {
            if (!integer(a.value("bitrate"), 0, MaximumAirBitrate))
                return fail(error, "Invalid air bitrate action");
            if (!e.setAirBitrate(a.value("bitrate").toInt(), error)) return false;
        } else return fail(error,"Unknown packet action");
    }
    if(input.contains("untilMs")){
        if(!integer(input.value("untilMs"),0,int(MaximumTimeMs))||input.value("untilMs").toDouble()<e.now())return fail(error,"Invalid scenario untilMs");
        if(!advance(qint64(input.value("untilMs").toDouble())))return fail(error,"Scenario event budget exceeded");
    } else while(e.pending())if(!e.step()||++processed>1000000)return fail(error,"Scenario event budget exceeded");
    *output=std::move(e);return true;
}
}
