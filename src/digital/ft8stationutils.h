#ifndef FT8STATIONUTILS_H
#define FT8STATIONUTILS_H

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

struct Ft8MessageInfo {
    QString raw;
    QString senderCall;
    QString recipientCall;
    QString grid;
    QString report;
    bool cq = false;
    bool roger = false;
    bool signoff = false;
    bool valid = false;
};

struct Ft8CallsignRule {
    QString country;
    QStringList prefixes;
    bool blocked = false;
};

struct Ft8CallsignFilterResult {
    QString country;
    QString matchedPrefix;
    QString matchedPattern;
    bool blocked = false;
    bool callsignBlocked = false;
    bool countryBlocked = false;
};

struct Ft8QsoRecord {
    QDateTime timeOnUtc;
    QDateTime timeOffUtc;
    QString stationCall;
    QString stationGrid;
    QString call;
    QString grid;
    QString reportSent;
    QString reportReceived;
    QString comment;
    double frequencyHz = 0.0;
};

struct Ft8ReceptionReport {
    QString senderCall;
    QString senderGrid;
    quint32 frequencyHz = 0;
    quint32 epochSeconds = 0;
};

Ft8MessageInfo parseFt8Message(const QString &message);
QString normalizeFt8Callsign(const QString &callsign);
QStringList parseFt8CallsignPatterns(const QString &text);
bool ft8CallsignPatternMatches(const QString &callsign, const QString &pattern);
Ft8CallsignFilterResult evaluateFt8Callsign(
    const QString &callsign,
    const QStringList &blockedCallsignPatterns,
    const QVector<Ft8CallsignRule> &countryRules);
QString ft8BandForFrequency(double frequencyHz);
QString ft8AdifRecord(const Ft8QsoRecord &record);
QString ft8AdifDocument(const QVector<Ft8QsoRecord> &records);
QVector<Ft8QsoRecord> parseFt8Adif(const QByteArray &data);

QByteArray buildPskReporterPacket(const QString &receiverCall,
                                  const QString &receiverGrid,
                                  const QString &software,
                                  const QString &antenna,
                                  const QVector<Ft8ReceptionReport> &reports,
                                  quint32 sequence,
                                  quint32 sessionId,
                                  bool includeTemplates);

QByteArray buildWsjtHeartbeat(const QString &id, const QString &version);
QByteArray buildWsjtStatus(const QString &id,
                           quint64 dialFrequencyHz,
                           const QString &dxCall,
                           const QString &report,
                           quint32 rxAudioHz,
                           quint32 txAudioHz,
                           const QString &stationCall,
                           const QString &stationGrid,
                           const QString &dxGrid,
                           const QString &txMessage);
QByteArray buildWsjtDecode(const QString &id,
                           const QDateTime &utc,
                           int snrDb,
                           double dtSeconds,
                           quint32 audioHz,
                           const QString &message,
                           bool lowConfidence = false);
QByteArray buildWsjtQsoLogged(const QString &id, const Ft8QsoRecord &record);
QByteArray buildWsjtLoggedAdif(const QString &id, const QString &adifRecord);

#endif // FT8STATIONUTILS_H