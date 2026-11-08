#ifndef RECEIVERBACKENDREGISTRY_H
#define RECEIVERBACKENDREGISTRY_H

#include "receiverbackend.h"

#include <QList>
#include <QString>

struct ReceiverBackendRegistration {
    QString id;
    QString displayName;
    ReceiverBackendStreamKind streamKind = ReceiverBackendStreamKind::FobosStandard;
    ReceiverBackendFeatures features;
    bool builtIn = false;
    bool implemented = false;
    QString status;
};

QList<ReceiverBackendRegistration> receiverBackendRegistrySnapshot();
ReceiverBackendRegistration receiverBackendRegistrationForId(const QString &id);
void logReceiverBackendRegistry();

#endif // RECEIVERBACKENDREGISTRY_H
