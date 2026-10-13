#include "main.h"
#include "propagationlabdialog.h"
#include "gnssqthhelpers.h"

void YourClassName::openPropagationLab() {
    if (!propagationLabDialog) propagationLabDialog = new PropagationLabDialog(
        normalizedUiLanguage(uiLanguage) == QStringLiteral("uk"), this);
    propagationLabDialog->setUiTranslator([this](const QString &key, const QString &fallback) { return uiText(key, fallback); });
    propagationLabDialog->setReferenceMarkers(qthUserMarkers, qthTileDirectory);
    const auto provider=qthOnlineProviderPreset(qthOnlineProviderId);
    propagationLabDialog->setOnlineMapSource(uiText(provider.textKey,provider.fallbackName),
        resolvedQthOnlineTileUrlTemplate(),qthOnlineAttribution,qthMapLayer,qthOnlineApiKey);
    propagationLabDialog->show();
    propagationLabDialog->raise();
}
