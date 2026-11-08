#pragma once

#include "dspgraphmodel.h"
#include "dspstreammanager.h"
#include "radiosettings.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <memory>

class DspBranchEngine;
struct DspBranchPcmFrame;

// Transport-neutral multi-user VFO broker. Each active VFO owns one bounded
// DDC branch, while the physical receiver and the established main path remain
// authoritative and untouched.
class RemoteVfoService final : public QObject {
    Q_OBJECT
public:
    static constexpr int MaxVfos = DspMaxBranches;
    static constexpr int MaxVfosPerSession = 2;

    explicit RemoteVfoService(QObject *parent = nullptr);
    ~RemoteVfoService() override;

    void setSourceState(const RadioSettings &settings, bool receiverRunning);
    void setExternalEndpoint(const DspStreamEndpoint &endpoint);
    void openEndpoint(const QString &ownerId, const QString &label);
    void closeEndpoint(const QString &ownerId);
    void handleCommand(const QString &ownerId, const QJsonObject &command);

    int sessionCount() const;
    int activeVfoCount() const;
    QJsonObject statusJson() const;

signals:
    void jsonFrameReady(const QString &ownerId, const QJsonObject &frame);
    void binaryFrameReady(const QString &ownerId,
                          const QJsonObject &frame,
                          const QByteArray &payload);
    void statusChanged();

private slots:
    void drainFrames();

private:
    struct Vfo;
    struct Session;

    Session &ensureSession(const QString &ownerId, const QString &label = QString());
    void sendSessionState(const Session &session, const QString &action = QStringLiteral("session"));
    void sendError(const QString &ownerId,
                   const QString &action,
                   const QString &message,
                   const QString &requestId = QString());
    bool applyVfoSettings(Vfo &vfo, const QJsonObject &settings, QString *error);
    void rebuildPlan();
    void deliverAudio(const DspBranchPcmFrame &frame);
    QJsonObject vfoJson(const Vfo &vfo) const;
    QJsonObject capabilitiesJson() const;
    QString ownerForBranch(const QString &branchId) const;
    QString ownerForAudioSink(const QString &sinkId) const;
    const Vfo *vfoForFft(const QString &fftId) const;

    std::unique_ptr<DspBranchEngine> engine_;
    QTimer drainTimer_;
    QHash<QString, Session> sessions_;
    QHash<QString, QString> branchOwners_;
    QHash<QString, QString> audioOwners_;
    QHash<QString, QString> fftOwners_;
    QHash<QString, QString> frameErrors_;
    RadioSettings sourceSettings_;
    DspStreamEndpoint externalEndpoint_;
    bool receiverRunning_ = false;
    quint64 nextSessionNumber_ = 1;
    quint64 spectrumFrames_ = 0;
    quint64 audioFrames_ = 0;
};
