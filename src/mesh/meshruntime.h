#pragma once
#include "meshproject.h"
#include <QObject>
#include <QTimer>
#include <QTemporaryDir>
#include <QProcess>
#include <memory>
#include <vector>

namespace Mesh {
// Owns only processes started for this session. All sockets and processes are async.
class Runtime : public QObject {
    Q_OBJECT
public:
    enum State { Idle, Starting, Running, Stopping };
    explicit Runtime(QObject *parent=nullptr);
    ~Runtime() override;
    bool start(const Project &,const QVector<HostPreset> &,QString *error);
    void stop();
    State state() const { return state_; }
    bool apply(const Node &,QString *error);
    bool testSignal(const QString &nodeId,QString *error);
    QJsonObject snapshot(const QString &nodeId) const;
    int ownedProcessCount() const;
signals:
    void changed();
    void nodeChanged(const QString &id);
    void log(const QString &text);
private:
    struct Entry;
    Entry *entry(const QString &id) const;
    void poll();
    void startHub();
    void pump(Entry *);
    void enqueue(Entry *,const QString &,const QJsonObject &);
    void checkStopped();
    void failure(const QString &);
    void readLog(QProcess *,const QString &);
    State state_=Idle;
    Project project;
    std::vector<std::unique_ptr<Entry>> entries;
    QProcess *hub=nullptr;
    quint16 hubPort=0;
    QString cliPath;
    QTimer polling,killDeadline;
    std::unique_ptr<QTemporaryDir> session;
    qint64 startedMs=0;
    quint64 generation=0;
};
}
