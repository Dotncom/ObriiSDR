#include "meshpacketengine.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
void check(bool ok,const char *name){std::printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)std::exit(1);}
QJsonObject topology(bool diamond=false) {
    QJsonArray nodes{QJsonObject{{"id","A"},{"name","A"}},QJsonObject{{"id","B"},{"name","B"}},QJsonObject{{"id","C"},{"name","C"}}};
    auto link=[](const char *tx,const char *rx,int delay){return QJsonObject{{"tx",tx},{"rx",rx},{"delayMs",delay},{"enabled",true}};};
    QJsonArray links{link("A","B",10),link("B","C",20)};
    if(diamond){nodes.append(QJsonObject{{"id","D"},{"name","D"}});links=QJsonArray{link("A","B",10),link("A","C",10),link("B","D",10),link("B","C",0),link("C","D",10)};}
    return {{"nodes",nodes},{"links",links}};
}
quint64 count(const Mesh::PacketEngine &e,const char *key){return e.snapshot(false).value(key).toString().toULongLong();}
QJsonObject last(const Mesh::PacketEngine &e){return e.results().last().toObject();}
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);QString error;Mesh::PacketEngine e;
    check(e.configure(topology(),&error),"configure directed A-B-C");
    check(e.route("A","C")==QStringList{"A","B","C"}&&e.route("C","A").isEmpty(),"directed min-hop route, no invented reverse link");
    const QByteArray binary("A\0B\xff",4);
    check(!e.send("A","C",binary,2,&error).isEmpty(),"binary datagram accepted");
    check(e.advanceTo(10)&&count(e,"delivered")==0&&e.pending()==1,"packet waits for first link delay");
    check(e.step()&&e.now()==11&&count(e,"forwarded")==1&&count(e,"delivered")==0,"B actually receives and queues onward forwarding");
    check(e.advanceTo(31)&&count(e,"delivered")==0&&e.advanceTo(32),"second hop delay respected");
    check(count(e,"delivered")==1&&last(e).value("path").toArray()==QJsonArray{"A","B","C"}&&last(e).value("latencyMs")==32&&
        QByteArray::fromBase64(last(e).value("payloadBase64").toString().toLatin1())==binary,"C receives exact bytes over two hops");
    check(!e.send("A","C","ttl",1,&error).isEmpty()&&e.advanceTo(43)&&last(e).value("status")=="ttl_expired","TTL prevents second hop");
    check(e.setLink("B","C",false,20,&error)&&e.route("A","C").isEmpty(),"partition invalidates cached routes");
    check(!e.send("A","C","cut",16,&error).isEmpty()&&e.advanceTo(43)&&last(e).value("status")=="no_route","partition reports no route, not fake delivery");
    check(e.setLink("B","C",true,20,&error)&&!e.send("A","C","restored",16,&error).isEmpty()&&e.advanceTo(75)&&last(e).value("status")=="delivered","route restored after link repair");
    check(!e.send("A","C","failure",16,&error).isEmpty()&&e.advanceTo(80)&&e.setNodeEnabled("B",false,&error)&&e.advanceTo(86)&&last(e).value("status")=="node_down","in-flight packet fails on disabled relay");
    check(e.setNodeEnabled("B",true,&error)&&e.route("A","C").size()==3,"relay recovery repairs route");
    check(!e.send("A","A","local",0,&error).isEmpty()&&e.advanceTo(e.now())&&last(e).value("hops")==0&&last(e).value("status")=="delivered","zero-hop local delivery with TTL zero");
    const auto normalized=e.scenario(),snapshot=e.snapshot();Mesh::PacketEngine replay;
    check(Mesh::PacketEngine::replay(normalized,&replay,&error)&&replay.snapshot()==snapshot,"GUI action history replays byte-identical results and trace");
    check(e.send("A","C","next",16,&error)==replay.send("A","C","next",16,&error),"automatic IDs remain valid after scenario import");
    check(!e.advanceTo(-1)&&!e.advanceTo(Mesh::PacketEngine::MaximumTimeMs+1),"simulation time bounds enforced");
    auto invalid=topology();auto ns=invalid.value("nodes").toArray();ns.append(ns.first());invalid["nodes"]=ns;
    const auto before=e.snapshot();check(!e.configure(invalid,&error)&&e.snapshot()==before,"invalid topology cannot partly reset experiment");
    check(e.configure(topology(true),&error)&&e.route("A","D")==QStringList{"A","B","D"},"deterministic lexical tie-break between equal-hop paths");
    check(!e.send("A","D","reroute",4,&error).isEmpty()&&e.advanceTo(5)&&e.setLink("B","D",false,10,&error)&&e.advanceTo(23),"change graph while datagram is in flight");
    check(last(e).value("path").toArray()==QJsonArray{"A","B","C","D"}&&last(e).value("status")=="delivered","B chooses a new next hop instead of replaying a precomputed path");
    check(e.configure(topology(),&error)&&!e.send("A","C","flight",16,&error).isEmpty()&&e.advanceTo(5)&&e.setLink("A","B",false,10,&error)&&e.advanceTo(11)&&last(e).value("status")=="link_down","link failure during transit is explicit");
    check(e.configure(topology(), &error) && e.setRadioRange(100, &error) &&
          e.setNodeMotion("B", {{99, 0}, {100, 0}}, &error) &&
          e.route("A", "C") == QStringList{"A", "B", "C"},
          "moving relay starts within the configured range");
    check(!e.send("A", "C", "crossing", 16, &error).isEmpty() && e.advanceTo(11) &&
          last(e).value("status") == "out_of_range" && e.route("A", "C").isEmpty(),
          "range crossing drops an in-flight packet and invalidates cached routes");
    check(e.setNodeMotion("B", {e.nodeMotion("B").position, {-100, 0}}, &error) &&
          e.advanceTo(30) && e.route("A", "C") == QStringList{"A", "B", "C"} &&
          !e.send("A", "C", "return", 16, &error).isEmpty() && e.advanceTo(62) &&
          last(e).value("status") == "delivered",
          "reversing velocity restores the route and delivery");
    check(Mesh::PacketEngine::replay(e.scenario(), &replay, &error) && replay.snapshot() == e.snapshot(),
          "scenario replay reproduces motion, range crossings and deliveries");
    const auto beforeInvalidMotion = e.snapshot();
    check(!e.setNodeMotion("B", {{0,0}, {std::numeric_limits<double>::infinity(),0}}, &error) &&
          !e.setRadioRange(-1, &error) && e.snapshot() == beforeInvalidMotion,
          "invalid motion and range leave the simulation unchanged");
    check(e.setNodeMotion("B", {{500, 0}, {0, 0}}, &error) && e.route("A", "C").isEmpty() &&
          e.setRadioRange(0, &error) && e.route("A", "C").size() == 3,
          "disabling the range limit restores the directed graph");
    check(e.configure(topology(true), &error) && e.setAirBitrate(8000, &error) &&
          !e.send("B", "D", "x", 1, &error).isEmpty() &&
          !e.send("C", "D", "x", 1, &error).isEmpty() && e.advanceTo(4) &&
          !e.setAirBitrate(0, &error),
          "two overlapping transmissions share a receiver, bitrate stays fixed in flight");
    check(Mesh::PacketEngine::replay(e.scenario(), &replay, &error) && replay.pending() == 2 &&
          replay.advanceTo(20) && e.advanceTo(20) && replay.snapshot() == e.snapshot() &&
          e.snapshot().value("dropReasons").toObject().value("collision") == "2",
          "overlapping packets collide and in-flight replay preserves the outcomes");
    auto halfDuplex = topology();
    halfDuplex["airBitrate"] = 8000;
    check(e.configure(halfDuplex, &error) && e.airBitrate() == 8000 &&
          !e.send("A", "B", "x", 1, &error).isEmpty() &&
          !e.send("B", "C", "x", 1, &error).isEmpty() && e.advanceTo(30) &&
          count(e, "delivered") == 1 && last(e).value("destination") == "C" &&
          e.snapshot().value("dropReasons").toObject().value("half_duplex") == "1",
          "a transmitting node loses its incoming frame without harming an isolated receiver");
    check(e.configure(halfDuplex, &error) &&
          !e.send("A", "B", "x", 1, &error).isEmpty() &&
          !e.send("A", "B", "y", 1, &error).isEmpty() && e.advanceTo(9) &&
          last(e).value("status") == "tx_busy" &&
          !e.send("A", "B", "z", 1, &error).isEmpty() && e.advanceTo(29) &&
          count(e, "delivered") == 2 && last(e).value("latencyMs").toInt() == 20,
          "sender occupancy ends with airtime, ahead of the link delay");
    check(e.configure(topology(),&error),"reset for duplicate ingress");
    check(!e.send("A","C","pending",16,&error).isEmpty()&&e.advanceTo(5),"prepare partial experiment");
    const auto partial=e.scenario();
    check(Mesh::PacketEngine::replay(partial,&replay,&error)&&replay.snapshot()==e.snapshot()&&replay.pending()==1,"in-progress export preserves virtual time and pending delivery");
    check(replay.advanceTo(32)&&e.advanceTo(32)&&replay.snapshot()==e.snapshot(),"replayed partial experiment can continue identically");
    check(e.configure(topology(),&error),"reset duplicate experiment");
    Mesh::PacketEngine::Datagram duplicate{"same","A","C","bytes",16};
    check(e.submit(duplicate,&error)&&e.submit(duplicate,&error)&&e.advanceTo(32)&&count(e,"submitted")==2&&count(e,"delivered")==1&&
        e.snapshot().value("dropReasons").toObject().value("duplicate")=="1","duplicate packet ID suppressed at node ingress");
    duplicate.source="B";
    check(e.submit(duplicate,&error)&&e.advanceTo(53)&&count(e,"delivered")==2,"packet ID is namespaced by origin");
    check(e.configure(topology(),&error),"reset for queue load");
    bool acceptedAll=true;for(int i=0;i<2000;++i)acceptedAll&=!e.send("A","C","x",16,&error).isEmpty();
    check(acceptedAll,"bounded load injection");
    check(e.pending()<=Mesh::PacketEngine::MaximumEvents,"global event queue bounded");
    check(!e.advanceTo(100,1)&&e.now()<100,"event budget yields without skipping pending simulation time");
    while(!e.advanceTo(100,512)){}
    check(count(e,"submitted")==2000&&count(e,"delivered")==256&&count(e,"dropped")==1744&&e.pending()==0,"per-node overflow is counted and other accepted packets finish");
    check(e.results().size()==512&&e.snapshot().value("trace").toArray().size()<=4096&&e.snapshot().value("historyEvicted").toString().toULongLong()>0,"bounded evidence history with explicit eviction count");
    check(Mesh::PacketEngine::replay(e.scenario(),&replay,&error)&&replay.snapshot()==e.snapshot(),"overload is reproducible with same counters and retained history");
    check(e.configure(topology(),&error),"reset for input bounds");
    check(e.send("missing","C","x",16,&error).isEmpty()&&e.send("A","C",QByteArray(4097,'x'),16,&error).isEmpty()&&e.send("A","C","x",65,&error).isEmpty()&&count(e,"submitted")==0,"invalid addresses, payload length and TTL rejected before mutation");
    auto bad=normalized;bad["version"]=99;const auto old=replay.snapshot();
    check(!Mesh::PacketEngine::replay(bad,&replay,&error)&&replay.snapshot()==old,"future scenario schema does not replace an experiment");
    bad=normalized;auto actions=bad.value("actions").toArray();auto first=actions.first().toObject();first["payloadBase64"]="!";actions[0]=first;bad["actions"]=actions;
    check(!Mesh::PacketEngine::replay(bad,&replay,&error),"malformed base64 rejected");
    bad=normalized;bad["untilMs"]=0;check(!Mesh::PacketEngine::replay(bad,&replay,&error),"scenario cannot finish before its last action");
    check(e.advanceTo(Mesh::PacketEngine::MaximumTimeMs)&&!e.send("A","C","last",16,&error).isEmpty()&&e.advanceTo(e.now())&&last(e).value("status")=="time_limit","end-of-time packets terminate rather than scheduling forever");
    check(e.configure(topology(),&error)&&e.submit({"p18446744073709551615","A","A","x",0},&error)&&e.send("A","A","x",0,&error).isEmpty(),"automatic ID exhaustion cannot wrap into duplicates");
    check(e.configure(topology(),&error),"reset for recording size bound");
    int accepted=0;while(!e.send("A","A",QByteArray(4096,'x'),0,&error).isEmpty())++accepted;
    check(accepted>100&&accepted<2000&&e.snapshot(false).value("actionBytes").toInt()<=7*1024*1024,"experiment action memory bounded independently of packet queues");
    std::printf("Packet routing self-test passed\n");return 0;
}
