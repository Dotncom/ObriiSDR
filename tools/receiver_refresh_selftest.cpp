#include "fobosdevicecatalog.h"
#include "receiverdeviceutils.h"
#include <QCoreApplication>
#include <QDebug>
#include <cstdlib>
#include <cstdio>

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        qCritical() << message;
        std::exit(1);
    }
}
FobosDeviceInfo device(const char *serial, int index, FobosApiKind kind = FobosApiKind::Agile) {
    FobosDeviceInfo info;
    info.serial = QString::fromLatin1(serial);
    info.nativeIndex = index;
    info.apiKind = kind;
    return info;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QVector<FobosDeviceInfo> previous{device("A", 0), device("B", 1)};
    QVector<FobosDeviceInfo> next{device("B", 0), device("A", 1)};
    int selected = 1;
    preserveFobosDeviceSelection(previous, next, selected);
    require(selected == 0 && next[selected].serial == "B", "USB reordering changed selected receiver");

    previous = next;
    next = {device("A", 0)};
    preserveFobosDeviceSelection(previous, next, selected);
    require(selected == 1 && !next[selected].present && next[selected].serial == "B",
            "Missing receiver silently selected another radio");
    for (int attempt = 0; attempt < 10; ++attempt) {
        previous = next;
        next.clear();
        preserveFobosDeviceSelection(previous, next, selected);
        require(next.size() == 1 && selected == 0 && !next[0].present && next[0].serial == "B",
                "Empty enumeration lost or duplicated missing selection");
    }
    previous = next;
    next = {device("A", 0), device("B", 1)};
    preserveFobosDeviceSelection(previous, next, selected);
    require(next.size() == 2 && selected == 1 && next[selected].present,
            "Delayed USB appearance did not restore selected receiver");

    previous = {device("B", 0, FobosApiKind::Standard)};
    next = {device("B", 2, FobosApiKind::Agile)};
    selected = 0;
    preserveFobosDeviceSelection(previous, next, selected);
    require(next.size() == 1 && selected == 0 && next[0].apiKind == FobosApiKind::Agile,
            "Serial identity must survive API and native-index changes");

    previous = {device("", 0, FobosApiKind::Standard)};
    next = {device("", 0, FobosApiKind::Agile)};
    preserveFobosDeviceSelection(previous, next, selected);
    require(selected == 1 && !next[selected].present,
            "Serial-less devices from different APIs must not match");
    previous = {device("", 0)};
    next = previous;
    selected = 0;
    preserveFobosDeviceSelection(previous, next, selected);
    require(next.size() == 1 && selected == 0 && next[0].present, "Serial-less index fallback failed");

    for (const int external : {rtlSdrNativeComboValue(0), rtlSdrNativeComboValue(2),
                              hackRfNativeComboValue(0), remoteReceiverComboValue(0)}) {
        next.clear();
        selected = external;
        preserveFobosDeviceSelection(previous, next, selected);
        require(selected == external && next.isEmpty(), "USB enumeration changed external selection");
    }
    selected = 3;
    previous.clear();
    next.clear();
    preserveFobosDeviceSelection(previous, next, selected);
    require(selected == 3 && next.isEmpty(), "First empty enumeration changed saved selection");
    std::puts("Receiver refresh: reordering, missing/delayed devices, API identity and external selections passed");
    return 0;
}
