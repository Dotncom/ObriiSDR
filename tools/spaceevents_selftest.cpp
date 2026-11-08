#include "spaceeventsfeed.h"
#include "publicdatafetch.h"
#include "solarconditionswidget.h"
#include <QApplication>
#include <QFile>
#include <iostream>
#include <stdexcept>
namespace {int checks=0;void require(bool ok,const char *message){++checks;if(!ok)throw std::runtime_error(message);}}
int main(int argc,char **argv){
    QApplication app(argc,argv);
    try{
        spaceevents::Source rss{"test","test","rss","https://example.org/feed/",{},true};
        QVector<spaceevents::Entry> entries;QString error;
        require(spaceevents::parse("<rss><channel><item><title>Comet &amp; meteor</title><pubDate>Mon, 12 Oct 2026 10:00:00 GMT</pubDate><link>/article</link></item></channel></rss>",rss,&entries,&error),"RSS parse");
        require(entries.size()==1 && entries[0].title=="Comet & meteor" && entries[0].link==QUrl("https://example.org/article"),"RSS text and relative link");
        require(entries[0].utc.isValid() && entries[0].dateText.endsWith("UTC"),"RSS time");
        require(spaceevents::parse("<feed xmlns=\"http://www.w3.org/2005/Atom\"><entry><title>Event</title><updated>2026-10-12T10:00:00Z</updated><link rel=\"alternate\" href=\"https://example.org/event\"/></entry></feed>",rss,&entries,&error),"Atom parse");
        require(entries.size()==1 && entries[0].link==QUrl("https://example.org/event"),"Atom alternate link");
        require(!spaceevents::parse("<!DOCTYPE rss [<!ENTITY secret SYSTEM 'file:///C:/secret.txt'>]><rss><channel/></rss>",rss,&entries,&error),"DTD/entity rejection");
        require(!spaceevents::parse("<html><body>Not a feed</body></html>",rss,&entries,&error),"HTML is not RSS");
        require(spaceevents::parse("<rss><channel><item><title>Event</title><link>file:///C:/secret.txt</link></item></channel></rss>",rss,&entries,&error)&&entries[0].link.isEmpty(),"Unsafe article link rejection");
        rss.filter="comet";
        require(spaceevents::parse("<rss><channel><item><title>Meteor</title></item><item><title>A comet</title></item></channel></rss>",rss,&entries,&error)&&entries.size()==1,"Topic filtering");
        spaceevents::Source fire{"test","NASA/JPL","fireballs","https://ssd-api.jpl.nasa.gov/fireball.api",{},true};
        const QByteArray fireJson=R"({"signature":{"version":"1.2"},"count":1,"fields":["impact-e","date","energy"],"data":[["0.08","2026-10-11 12:00:00","2.3"]]})";
        require(spaceevents::parse(fireJson,fire,&entries,&error)&&entries.size()==1,"Fireball JSON reordered fields");
        require(entries[0].title.contains("2.3 x 10^10 J")&&entries[0].title.contains("0.08 kt"),"Fireball physical units");
        require(entries[0].utc.timeSpec()==Qt::UTC,"Fireball UTC");
        require(!spaceevents::parse(R"({"signature":{"version":"9.0"},"count":0})",fire,&entries,&error),"API version safety");
        require(spaceevents::parse(R"({"signature":{"version":"1.2"},"count":0})",fire,&entries,&error)&&entries.isEmpty(),"Empty API result is valid");
        spaceevents::Source comet{"test","NASA/JPL","comets","https://ssd-api.jpl.nasa.gov/sbdb_query.api",{},true};
        require(spaceevents::parse(R"({"signature":{"version":"1.0"},"count":1,"fields":["full_name","pdes","soln_date","tp_cal"],"data":[["C/2026 A1","C/2026 A1","2026-10-10 13:00:00","20270101.12"]]})",comet,&entries,&error),"Comet JSON");
        require(entries.size()==1 && entries[0].title.contains("orbit solution") && entries[0].title.contains("PT") && !entries[0].utc.isValid(),"Do not relabel Pacific solution time as UTC or a discovery");
        solar::Observation observation;
        require(solar::parseKp(R"([["time_tag","Kp","a_running"],["2026-10-12 00:00:00.000","3.00","15"]])",&observation)&&observation.kp==3&&observation.ap==15,"NOAA matrix with string numeric values");
        std::atomic_bool cancel{false};
        require(fetchPublicData(QUrl("file:///C:/secret.txt"),cancel,&error).isEmpty()&&!error.isEmpty(),"No local-file public fetch");
        require(fetchPublicData(QUrl("https://user:password@example.org"),cancel,&error).isEmpty()&&!error.isEmpty(),"No embedded credentials");
        for(const auto &pair:{qMakePair(QStringLiteral("build/comet-response.json"),comet),qMakePair(QStringLiteral("build/fireball-response.json"),fire),qMakePair(QStringLiteral("build/meteor-feed.xml"),rss)}){
            QFile file(pair.first);if(file.open(QIODevice::ReadOnly)){auto source=pair.second;source.filter.clear();require(spaceevents::parse(file.readAll(),source,&entries,&error),"Real upstream response fixture");require(!entries.isEmpty(),"Upstream fixture must return events");}
        }
        std::cout<<checks<<" checks, 0 failures\n";return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
