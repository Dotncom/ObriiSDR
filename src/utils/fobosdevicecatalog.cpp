#include "fobosdevicecatalog.h"

void preserveFobosDeviceSelection(const QVector<FobosDeviceInfo> &previous,
                                 QVector<FobosDeviceInfo> &discovered,
                                 int &selectedIndex) {
    if (selectedIndex < 0 || selectedIndex >= previous.size()) {
        return;
    }
    const FobosDeviceInfo selected = previous.at(selectedIndex);
    for (int i = 0; i < discovered.size(); ++i) {
        const FobosDeviceInfo &candidate = discovered.at(i);
        const bool sameDevice = !selected.serial.isEmpty()
            ? candidate.serial == selected.serial
            : candidate.apiKind == selected.apiKind && candidate.nativeIndex == selected.nativeIndex;
        if (candidate.present && sameDevice) {
            selectedIndex = i;
            return;
        }
    }

    // A missing receiver remains selectable, but must not open a different one.
    FobosDeviceInfo missing = selected;
    missing.present = false;
    selectedIndex = discovered.size();
    discovered.append(missing);
}
