#include "openradiobackend.h"

#include <QMutexLocker>

OpenRadioBackendRegistry &OpenRadioBackendRegistry::instance() {
    static OpenRadioBackendRegistry registry;
    return registry;
}

bool OpenRadioBackendRegistry::registerFactory(const QString &id, Factory factory) {
    const QString normalized = id.trimmed().toLower();
    if (normalized.isEmpty() || !factory) return false;
    QMutexLocker locker(&mutex);
    if (factories.contains(normalized)) return false;
    factories.insert(normalized, std::move(factory));
    return true;
}

QStringList OpenRadioBackendRegistry::backendIds() const {
    QMutexLocker locker(&mutex);
    return factories.keys();
}

std::unique_ptr<OpenRadioBackend> OpenRadioBackendRegistry::create(const QString &id) const {
    Factory factory;
    {
        QMutexLocker locker(&mutex);
        const auto it = factories.constFind(id.trimmed().toLower());
        if (it == factories.constEnd()) return nullptr;
        factory = it.value();
    }
    return factory();
}
