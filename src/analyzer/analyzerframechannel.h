#ifndef ANALYZERFRAMECHANNEL_H
#define ANALYZERFRAMECHANNEL_H

#include <QObject>
#include <QElapsedTimer>
#include <QVector>
#include <memory>
#include <vector>

class QLocalServer;
class QLocalSocket;

struct AnalyzerSpectrumFrame {
    quint64 sequence = 0;
    qint64 utcMs = 0;
    double centerHz = 0.0;
    double minimumHz = 0.0;
    double maximumHz = 0.0;
    int sourceFftLength = 0;
    std::vector<float> levelsDb;
};

class AnalyzerFramePublisher final : public QObject {
    Q_OBJECT
public:
    explicit AnalyzerFramePublisher(QObject *parent = nullptr);
    ~AnalyzerFramePublisher() override;

    bool start(const QString &channelName = defaultChannelName());
    bool hasSubscribers() const;
    QString errorString() const;
    void publish(const std::vector<float> &frequencies,
                 const std::vector<float> &levels,
                 int sourceFftLength);

    static QString defaultChannelName();

private:
    void acceptConnections();
    std::unique_ptr<QLocalServer> server;
    QVector<QLocalSocket *> clients;
    quint64 sequence = 0;
    QElapsedTimer publishClock;
    QString lastError;
};

class AnalyzerFrameClient final : public QObject {
    Q_OBJECT
public:
    explicit AnalyzerFrameClient(QObject *parent = nullptr);
    ~AnalyzerFrameClient() override;
    void connectToPublisher(const QString &channelName = AnalyzerFramePublisher::defaultChannelName());
    void disconnectFromPublisher();
    bool isConnected() const;
    QString errorString() const;

signals:
    void connected();
    void disconnected();
    void frameReady(const AnalyzerSpectrumFrame &frame);
    void connectionError(const QString &message);

private:
    void readAvailable();
    std::unique_ptr<QLocalSocket> socket;
    QByteArray inputBuffer;
    QString lastError;
};

Q_DECLARE_METATYPE(AnalyzerSpectrumFrame)

#endif
