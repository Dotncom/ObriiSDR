#include "dspsourceidentity.h"

#include <QRegularExpression>

namespace {
QString normalizedPart(const QString &value, const QString &fallback) {
    const QString candidate = value.trimmed();
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_.:-]{1,64}$"));
    return valid.match(candidate).hasMatch() ? candidate : fallback;
}
}

QString DspSourceIdentity::key() const {
    return receiverId + QLatin1Char('/') + channelId;
}

bool DspSourceIdentity::isPrimary() const {
    return receiverId == QStringLiteral("main") && channelId == QStringLiteral("rx0");
}

bool DspSourceIdentity::isValid() const {
    const DspSourceIdentity canonical = normalized(receiverId, channelId);
    return canonical.receiverId == receiverId && canonical.channelId == channelId;
}

QJsonObject DspSourceIdentity::toJson() const {
    return {{QStringLiteral("receiverId"), receiverId},
            {QStringLiteral("channelId"), channelId}};
}

DspSourceIdentity DspSourceIdentity::normalized(const QString &receiver,
                                                const QString &channel) {
    DspSourceIdentity source;
    source.receiverId = normalizedPart(receiver, QStringLiteral("main"));
    source.channelId = normalizedPart(channel, QStringLiteral("rx0"));
    return source;
}

DspSourceIdentity DspSourceIdentity::fromJson(const QJsonObject &object) {
    return normalized(object.value(QStringLiteral("receiverId")).toString(QStringLiteral("main")),
                      object.value(QStringLiteral("channelId")).toString(QStringLiteral("rx0")));
}
