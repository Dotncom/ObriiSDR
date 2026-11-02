#ifndef OBRII_CLI_GNSS_SESSION_H
#define OBRII_CLI_GNSS_SESSION_H

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <functional>

// All methods/callbacks run on the owning Qt event-loop thread. Feed full-source
// IQ, never channelized/audio IQ. Construction/status do not launch anything.
class CliGnssSession : public QObject {
public:
    explicit CliGnssSession(QObject *parent = nullptr);
    ~CliGnssSession() override;
    QJsonObject command(const QJsonObject &request, double actualSampleRateHz, double centerHz);
    bool submitIq(const float *interleavedIq, int complexSamples, double actualSampleRateHz,
                  double centerHz, bool discontinuity = false);
    void reset();
    void stop();
    bool ready() const;
    bool running() const;
    // Only after RX has ended: pumps Qt events for EOF/cancellation cleanup.
    bool waitForStopped(int timeoutMs = 11000);
    QJsonObject status() const;
    std::function<void(const QJsonObject &)> onEvent;
    static constexpr qint64 MaximumQueuedBytes = 4 * 1024 * 1024;
private:
    void consumeOutput();
    void fail(const QString &message);
    bool sendControl(const QJsonObject &object);
    void sendReset();
    QProcess process;
    QTimer watchdog;
    QElapsedTimer phaseTimer;
    QByteArray output;
    QString state = QStringLiteral("stopped"), lastError;
    double rate = 0, center = 0;
    quint64 sourceSamples = 0, rejectedBlocks = 0;
    int decimation = 1;
    bool accepting = false, eofRequested = false, eofAcknowledged = false;
    bool terminalReceived = false, failed = false, stopping = false;
    bool stdinClosed = false, terminateSent = false, killSent = false;
};

#endif
