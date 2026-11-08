#pragma once

#include <QJsonObject>
#include <QString>

struct DspSourceIdentity {
    QString receiverId = QStringLiteral("main");
    QString channelId = QStringLiteral("rx0");

    QString key() const;
    bool isPrimary() const;
    bool isValid() const;
    QJsonObject toJson() const;

    static DspSourceIdentity normalized(const QString &receiverId,
                                        const QString &channelId);
    static DspSourceIdentity fromJson(const QJsonObject &object);

    bool operator==(const DspSourceIdentity &other) const {
        return receiverId == other.receiverId && channelId == other.channelId;
    }
    bool operator!=(const DspSourceIdentity &other) const { return !(*this == other); }
};

inline uint qHash(const DspSourceIdentity &source, uint seed = 0) {
    return qHash(source.key(), seed);
}
