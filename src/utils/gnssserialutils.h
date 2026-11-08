#ifndef GNSSSERIALUTILS_H
#define GNSSSERIALUTILS_H

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <limits>

struct NmeaSerialStatus {
    bool recognized = false;
    QString talker;
    QString sentence;
    QString utc;
    int fixQuality = -1;
    int fixMode = -1;
    int satellitesUsed = -1;
    int satellitesInView = -1;
    double hdop = std::numeric_limits<double>::quiet_NaN();
    double vdop = std::numeric_limits<double>::quiet_NaN();
    double pdop = std::numeric_limits<double>::quiet_NaN();
    double altitudeM = std::numeric_limits<double>::quiet_NaN();
    double geoidSeparationM = std::numeric_limits<double>::quiet_NaN();
    double speedKmh = std::numeric_limits<double>::quiet_NaN();
    double courseDeg = std::numeric_limits<double>::quiet_NaN();
};

int parseNmeaIntField(const QStringList &fields, int index, int fallback = -1);
double parseNmeaDoubleField(const QStringList &fields,
                            int index,
                            double fallback = std::numeric_limits<double>::quiet_NaN());
QStringList nmeaBodyFields(const QString &line);
QString gnssSystemForTalker(const QString &talker, int prn = -1);
QString gnssSatelliteKey(const QString &talker, int prn);
quint16 ubxU2(const QByteArray &bytes, int offset);
quint32 ubxU4(const QByteArray &bytes, int offset);
void ubxPutU4(QByteArray &bytes, int offset, quint32 value);
void ubxAppendU4(QByteArray &bytes, quint32 value);
qint16 ubxI2(const QByteArray &bytes, int offset);
qint32 ubxI4(const QByteArray &bytes, int offset);
QByteArray makeUbxFrame(quint8 messageClass, quint8 messageId, const QByteArray &payload);
bool hasValidUbxChecksum(const QByteArray &frame);
QString ubxGnssSystemName(quint8 gnssId);
QString ubxGnssTalker(quint8 gnssId);
QString ubxSatelliteKey(quint8 gnssId, int svId);
QString normalizedGnssPositionPolicy(QString policy);
QString formatGnssUtcForDisplay(const QString &rawUtc, int offsetMinutes);
NmeaSerialStatus parseNmeaSerialStatusSentence(const QString &line);

#endif // GNSSSERIALUTILS_H
