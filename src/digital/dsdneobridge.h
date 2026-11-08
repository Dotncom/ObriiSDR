#ifndef DSDNEOBRIDGE_H
#define DSDNEOBRIDGE_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <atomic>
#include <deque>
#include <mutex>

class QTimer;

class DsdNeoBridge : public QObject {
    Q_OBJECT

public:
    explicit DsdNeoBridge(QObject *parent = nullptr);
    ~DsdNeoBridge() override;

    static QStringList dmrProcessArguments(int sampleRate, quint16 outputPort);
    struct PrivacyArguments {
        QStringList actual;
        QStringList display;
        QString privacyError;
        QString privacyMode;
    };
    static PrivacyArguments privacyProcessArguments(const QStringList &baseArguments,
                                                    const QString &mode,
                                                    const QString &keyHex,
                                                    bool forwardToBackend);

    void configureInputServer(bool enabled, quint16 port);
    void configureUdpOutput(bool enabled, quint16 port, int channels);
    void configureProcess(bool autoStart,
                          const QString &program,
                          const QStringList &arguments,
                          const QString &workingDirectory = QString());
    void configurePrivacy(const QString &mode,
                          int keyId,
                          const QString &keyHex,
                          bool forwardToBackend);
    void setEnabled(bool enabled);
    bool isEnabled() const { return inputSubmissionEnabled.load(); }

    // Thread-safe, bounded handoff from the SDR worker.
    void submitInputPcm(const QByteArray &pcmData, int sampleRate);
    void sendInputPcm(const QByteArray &pcmData, int sampleRate);

signals:
    void statusChanged(const QString &status);
    void decodedPcmReady(const QByteArray &pcmData);

private:
    void startInputServer();
    void stopInputServer();
    void startUdpOutput();
    void stopUdpOutput();
    void startProcessIfNeeded();
    void stopProcess();
    bool usesProcessInput() const;
    void emitStatus(const QString &status);
    bool processRestartBackoffActive() const;
    void armProcessRestartBackoff(const QString &reason);
    void acceptInputClient();
    void removeInputClient(QTcpSocket *client);
    void readUdpOutput();
    void readProcessOutput();
    void drainInputPcm();

    QByteArray prepareInputPcmForDsd(const QByteArray &pcmData);
    QByteArray pcm16ToMono(const QByteArray &pcmData, int channels) const;
    QByteArray upsample8kMonoTo48k(const QByteArray &pcmData);

    bool bridgeEnabled = false;
    bool inputServerEnabled = true;
    bool udpOutputEnabled = true;
    bool processAutoStart = false;
    quint16 inputPort = 7355;
    quint16 udpOutputPort = 23456;
    int udpOutputChannels = 1;
    int inputSampleRate = 0;

    QString processProgram;
    QStringList processArguments;
    QString processWorkingDirectory;
    QString privacyMode;
    int privacyKeyId = 0;
    QString privacyKeyHex;
    QString processPrivacyKeyHex;
    bool privacyForwardToBackend = true;

    QTcpServer *inputServer = nullptr;
    QUdpSocket *udpInputSocket = nullptr;
    QUdpSocket *udpOutputSocket = nullptr;
    QProcess *managedProcess = nullptr;
    QTimer *processStopTimer = nullptr;
    QTimer *processStartTimer = nullptr;
    QList<QTcpSocket *> inputClients;
    QElapsedTimer noClientLogTimer;
    QElapsedTimer processRestartBackoffTimer;
    QElapsedTimer processRunTimer;
    qint64 processRestartBackoffMs = 0;
    bool processStoppingIntentionally = false;
    int inputWarmupFramesRemaining = 0;
    quint64 pcmFramesForwarded = 0;
    quint64 udpInputPacketsForwarded = 0;
    quint64 udpOutputPacketsReceived = 0;
    quint64 processLogLines = 0;
    std::atomic<bool> inputSubmissionEnabled{false};
    std::mutex inputQueueMutex;
    std::deque<std::pair<QByteArray, int>> inputQueue;
    bool inputDrainScheduled = false;
    quint64 inputQueueDrops = 0;
    QByteArray processStdoutPending;
    QByteArray processStderrPending;
    qint16 previousOutputSample = 0;
    bool previousOutputSampleValid = false;
};

#endif // DSDNEOBRIDGE_H
