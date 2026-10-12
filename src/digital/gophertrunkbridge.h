#ifndef GOPHERTRUNKBRIDGE_H
#define GOPHERTRUNKBRIDGE_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTcpSocket>
#include <QUdpSocket>

class QJsonObject;

class GopherTrunkBridge : public QObject {
    Q_OBJECT

public:
    explicit GopherTrunkBridge(QObject *parent = nullptr);
    ~GopherTrunkBridge() override;

    void configure(bool autoStart,
                   const QString &program,
                   quint16 tcpPort,
                   quint16 audioUdpPort,
                   const QString &workingDirectory = QString());
    void configurePrivacy(const QString &mode,
                          int keyId,
                          const QString &keyHex,
                          bool forwardToBackend,
                          const QString &variant = QStringLiteral("dmra"),
                          const QString &layout = QStringLiteral("normal"),
                          int frameOffset = 0);
    void setEnabled(bool enabled);
    bool isEnabled() const { return bridgeEnabled; }

    void resetStream();
    static QString normalizedBurstDibits(const QString &dibits, bool dataBurst,
                                         bool voicePolarityFlip);
    void sendDibitBurst(const QString &dibits,
                        const QString &burstKind,
                        int dataType,
                        const QString &dataTypeName,
                        int slotTypeErrors,
                        quint64 sample,
                        quint64 baseDibit,
                        int burstIndex,
                        int cadenceSymbols,
                        int colorCode,
                        int timeslot,
                        const QString &timeslotSource,
                        bool cachDecoded,
                        int cachChannel,
                        int cachLcss,
                        const QString &cachMapName,
                        const QString &cachPayloadBits,
                        const QStringList &ambe49Payloads);

signals:
    void statusChanged(const QString &status);
    void decodedPcmReady(const QByteArray &pcmData);

private:
    void startProcessIfNeeded();
    void stopProcess();
    void connectTcpIfNeeded();
    void disconnectTcp();
    void startUdpOutput();
    void stopUdpOutput();
    void readTcpOutput();
    void readProcessOutput();
    void readUdpOutput();
    void emitStatus(const QString &status);
    bool processRestartBackoffActive() const;
    void armProcessRestartBackoff(const QString &reason);
    QJsonObject currentPrivacyObject() const;
    QByteArray pcm16ToMono(const QByteArray &pcmData, int channels) const;
    QByteArray normalizeOutputPcmForPlayback(const QByteArray &pcmData, quint64 packetIndex) const;
    QByteArray upsample8kMonoTo48k(const QByteArray &pcmData) const;

    bool bridgeEnabled = false;
    bool processAutoStart = false;
    bool processStoppingIntentionally = false;
    bool tcpConnectPending = false;
    quint16 tcpPort = 7460;
    quint16 audioUdpPort = 23456;
    QString processProgram;
    QStringList processArguments;
    QString processWorkingDirectory;
    QString privacyMode;
    int privacyKeyId = 0;
    QString privacyKeyHex;
    bool privacyForwardToBackend = true;
    QString privacyVariant = QStringLiteral("dmra");
    QString privacyLayout = QStringLiteral("normal");
    int privacyFrameOffset = 0;
    QString privacyOverridePath;

    QProcess *managedProcess = nullptr;
    QTcpSocket *tcpSocket = nullptr;
    QUdpSocket *udpOutputSocket = nullptr;
    QElapsedTimer processRestartBackoffTimer;
    qint64 processRestartBackoffMs = 0;
    quint64 dibitPacketsSent = 0;
    quint64 virtualBaseDibit = 0;
    quint64 udpOutputPacketsReceived = 0;
    quint64 processLogLines = 0;
    quint64 tcpOutputLines = 0;
    bool streamPolarityKnown = false;
    bool streamPolarityFlip = false;
    int lastColorCode = -1;
    QString lastPrivacySummary;
    QString lastPrivacyResetSummary;
};

#endif // GOPHERTRUNKBRIDGE_H
