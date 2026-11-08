#include "ft8countrycatalog.h"
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

QVector<Ft8CallsignRule> parseFt8CountryCatalog(const QByteArray &cty) {
    QVector<Ft8CallsignRule> rules;
    const auto lines=QString::fromUtf8(cty).split('\n');
    Ft8CallsignRule current;
    QSet<QString> seen;
    bool active=false;
    const QRegularExpression modifier(QStringLiteral("[\\(\\[\\{<~]"));
    for (const auto &line:lines) {
        if (!line.isEmpty() && !line[0].isSpace()) {
            const auto fields=line.split(':');
            active=fields.size()>=9 && !fields[7].trimmed().startsWith('*');
            current={};seen.clear();
            if(active)current.country=fields[0].trimmed();
            continue;
        }
        if(!active)continue;
        for(QString value:line.trimmed().split(QRegularExpression("[,;]"),Qt::SkipEmptyParts)) {
            value=value.trimmed().toUpper();
            const auto match=modifier.match(value);if(match.hasMatch())value.truncate(match.capturedStart());
            if(!value.isEmpty()&&!seen.contains(value)){current.prefixes.push_back(value);seen.insert(value);}
        }
        if(line.contains(';')){if(!current.country.isEmpty())rules.push_back(current);active=false;}
    }
    std::sort(rules.begin(),rules.end(),[](const Ft8CallsignRule &a,const Ft8CallsignRule &b){return a.country<b.country;});
    return rules;
}
QVector<Ft8CallsignRule> defaultFt8CountryCatalog(){
    QFile file(QStringLiteral(":/ft8/cty.dat"));if(!file.open(QIODevice::ReadOnly))return {};
    return parseFt8CountryCatalog(file.readAll());
}
QVector<Ft8CallsignRule> mergeFt8CountryCatalog(const QVector<Ft8CallsignRule> &saved,const QVector<Ft8CallsignRule> &catalog){
    QVector<Ft8CallsignRule> result;QSet<QString> names;
    bool legacyRussia=false,blockRussia=false;
    const QSet<QString> oldPrefixes={"R","UA","UB","UC","UD","UE","UF","UG","UH","UI"};
    for(const auto &rule:saved){
        QSet<QString> prefixes;for(const auto &p:rule.prefixes)prefixes.insert(p.toUpper());
        if(rule.country=="Russia"&&prefixes==oldPrefixes){legacyRussia=true;blockRussia=rule.blocked;continue;}
        result.push_back(rule);names.insert(rule.country.toCaseFolded());
    }
    for(auto rule:catalog){
        if(names.contains(rule.country.toCaseFolded()))continue;
        if(legacyRussia && (rule.country.contains("Russia")||rule.country=="Kaliningrad"))rule.blocked=blockRussia;
        result.push_back(rule);
    }
    return result;
}
QVector<qth::UserMarker> ft8LogMapMarkers(const QVector<Ft8QsoRecord> &records){
    QVector<qth::UserMarker> markers;QSet<QString> seen;
    for(auto it=records.crbegin();it!=records.crend();++it){
        const auto call=normalizeFt8Callsign(it->call);const auto grid=it->grid.trimmed().toUpper();
        if(call.isEmpty()||call==normalizeFt8Callsign(it->stationCall)||seen.contains(call))continue;
        const auto bounds=qth::maidenheadBounds(grid);if(!bounds.valid)continue;
        markers.push_back({markers.size()+1,call,QStringLiteral("FT8 Maidenhead: %1 (cell center)").arg(grid),
            (bounds.minLatitude+bounds.maxLatitude)/2,(bounds.minLongitude+bounds.maxLongitude)/2});
        seen.insert(call);
    }
    return markers;
}
