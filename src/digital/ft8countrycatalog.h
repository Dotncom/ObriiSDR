#ifndef FT8COUNTRYCATALOG_H
#define FT8COUNTRYCATALOG_H
#include "ft8stationutils.h"
#include "qthlocator.h"
QVector<Ft8CallsignRule> parseFt8CountryCatalog(const QByteArray &cty);
QVector<Ft8CallsignRule> defaultFt8CountryCatalog();
QVector<Ft8CallsignRule> mergeFt8CountryCatalog(const QVector<Ft8CallsignRule> &saved,
                                             const QVector<Ft8CallsignRule> &catalog);
QVector<qth::UserMarker> ft8LogMapMarkers(const QVector<Ft8QsoRecord> &records);
#endif
