#pragma once
#include <QObject>
#include <QThread>
#include <QJsonObject>

namespace Mesh {
// Optional mesh endpoint. RX taps an existing stream; RF TX is separately authorized locally.
class NodeService : public QObject {
    Q_OBJECT
public:
    explicit NodeService(QObject *parent=nullptr);
    ~NodeService() override;
    QJsonObject command(const QJsonObject &);
    QJsonObject status() const { return snapshot; }
    static bool radioCaptureActive();
private:
    void stop();
    QThread thread;
    QObject *worker=nullptr;
    QJsonObject snapshot;
    QString secret;
    quint64 generation=0;
};
}
