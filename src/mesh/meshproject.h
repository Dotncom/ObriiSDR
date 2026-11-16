#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QPointF>
#include <QVector>

namespace Mesh {
struct HostPreset {
    QString id, name, kind = "local", host, user, directory;
    int sshPort = 22;
    QJsonObject json() const;
};
struct Node {
    QString id, name, presetId;
    QPointF position;
    double listeningHz = 145500000, bandwidthHz = 12500, txLevel = 0.3;
    QString mode = "nfm";
    QJsonObject json() const;
    QJsonArray ports;
    QJsonObject protocol;
    bool enabled = true;
};
struct Link {
    QString tx, rx;
    double gainDb = -6;
    int delayMs = 0;
    bool enabled = true;
    QJsonObject json() const;
    QString bearer="inherit",txPort="radio",rxPort="radio";
    QJsonObject modem;
    double frequencyOffsetHz = 0;
    double clockPpm = 0;
};
struct Project {
    double centerHz = 145500000;
    int sampleRate = 48000;
    bool mixIq = true;
    QVector<Node> nodes;
    QVector<Link> links;
    QJsonObject json() const;
    QString validate(const QVector<HostPreset> &presets) const;
    static bool parse(const QJsonObject &, Project *, QString *error);
};
bool validId(const QString &id);
QJsonArray defaultNodePorts();
QString validateNodeOptions(const Node &);
QString validateLinkOptions(const Link &);
bool validLinkFrequencyOffset(double offsetHz, int sampleRate, bool mixIq);
bool validLinkClockPpm(double ppm, bool mixIq);
bool saveJson(const QString &path, const QJsonObject &, QString *error);
bool loadJson(const QString &path, QJsonObject *, QString *error);
bool saveLibrary(const QString &path, const QVector<HostPreset> &, QString *error);
bool loadLibrary(const QString &path, QVector<HostPreset> *, QString *error);
QString validatePresets(const QVector<HostPreset> &);
QString executable(const HostPreset &, const QString &name);
Project example(const QString &presetId);
}
