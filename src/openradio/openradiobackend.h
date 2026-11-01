#ifndef OPENRADIOBACKEND_H
#define OPENRADIOBACKEND_H

#include "openradioprotocol.h"
#include "virtualsignalframe.h"

#include <QList>
#include <QMap>
#include <QMutex>
#include <QString>
#include <QVariantMap>

#include <functional>
#include <memory>

class OpenRadioBackend {
public:
    virtual ~OpenRadioBackend() = default;
    virtual QString id() const = 0;
    virtual QList<OpenRadioProtocol> protocols() const = 0;
    virtual bool start(const QVariantMap &settings, QString *error) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual bool submit(const VirtualSignalFrame &frame, QString *error) = 0;
    virtual QList<VirtualSignalFrame> takeReceived(int maximumFrames,
                                                   QString *error) = 0;
};

class OpenRadioBackendRegistry {
public:
    using Factory = std::function<std::unique_ptr<OpenRadioBackend>()>;

    static OpenRadioBackendRegistry &instance();
    bool registerFactory(const QString &id, Factory factory);
    QStringList backendIds() const;
    std::unique_ptr<OpenRadioBackend> create(const QString &id) const;

private:
    OpenRadioBackendRegistry() = default;
    mutable QMutex mutex;
    QMap<QString, Factory> factories;
};

#endif // OPENRADIOBACKEND_H
