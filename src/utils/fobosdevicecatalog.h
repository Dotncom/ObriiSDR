#pragma once

#include "radiosettings.h"
#include <QString>
#include <QVector>

struct FobosDeviceInfo {
    FobosApiKind apiKind = FobosApiKind::Standard;
    int nativeIndex = 0;
    QString label;
    QString serial;
    QString hardwareRevision;
    QString firmwareVersion;
    QString product;
    QString manufacturer;
    bool present = true;
};

// Keep the selected physical receiver even when native USB indices change.
void preserveFobosDeviceSelection(const QVector<FobosDeviceInfo> &previous,
                                 QVector<FobosDeviceInfo> &discovered,
                                 int &selectedIndex);
