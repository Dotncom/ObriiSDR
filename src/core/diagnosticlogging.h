#ifndef DIAGNOSTICLOGGING_H
#define DIAGNOSTICLOGGING_H

#include <QByteArray>
#include <QtGlobal>

#include <atomic>

inline bool fobosVerboseLoggingDefaultEnabled() {
    const QByteArray value = qgetenv("FOBOS_VERBOSE_LOG").trimmed().toLower();
    return value == "1" || value == "true" || value == "yes" ||
           value == "on" || value == "debug" || value == "verbose";
}

inline std::atomic_bool &fobosVerboseLoggingFlag() {
    static std::atomic_bool enabled{fobosVerboseLoggingDefaultEnabled()};
    return enabled;
}

inline void setFobosVerboseLoggingEnabled(bool enabled) {
    fobosVerboseLoggingFlag().store(enabled, std::memory_order_relaxed);
}

inline bool fobosVerboseLoggingEnabled() {
    return fobosVerboseLoggingFlag().load(std::memory_order_relaxed);
}

#endif // DIAGNOSTICLOGGING_H
