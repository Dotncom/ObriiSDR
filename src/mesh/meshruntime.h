#pragma once
#include "meshproject.h"
#include <QObject>
#include <QTimer>
#include <QTemporaryDir>
#include <QProcess>
#include <memory>
#include <vector>
class QTcpSocket;

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
    bool applyLink(const Link &,QString *error);
    QJsonObject hubSnapshot() const;
    bool testSignal(const QString &nodeId,QString *error);
    bool stopSignal(const QString &nodeId,QString *error);
    bool setNodeRunning(const QString &nodeId,bool running,QString *error);
    void inspectNode(const QString &nodeId);
    QJsonObject snapshot(const QString &nodeId) const;
    int ownedProcessCount() const;
signals:
    void changed();
    void nodeChanged(const QString &id);
    void spectrumChanged(const QString &id);
    void topologyChanged();
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
    void hubRequest(const QString &,const QJsonObject &args={});
    void sendQueuedLink();
    void cancelHubRequest();
    State state_=Idle;
    Project project;
    std::vector<std::unique_ptr<Entry>> entries;
    QProcess *hub=nullptr;
    quint16 hubPort=0;
    quint16 hubControlPort=0;
    QTcpSocket *hubSocket=nullptr;
    QString hubKey,hubInstance;
    QJsonObject hubState;
    Link queuedLink;
    bool hasQueuedLink=false,linkChangePending=false;
    QString cliPath;
    QString inspectedNode;
    QTimer polling,killDeadline;
    std::unique_ptr<QTemporaryDir> session;
    qint64 startedMs=0;
    quint64 generation=0;
};
}
