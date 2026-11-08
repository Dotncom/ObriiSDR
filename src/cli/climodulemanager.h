#ifndef OBRII_CLI_MODULE_MANAGER_H
#define OBRII_CLI_MODULE_MANAGER_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>

#include "dmrprivacyutils.h"
#include "radiosettings.h"

#include <functional>
#include <memory>
#include <vector>

namespace CliDigitalProfile {
constexpr int MaximumBytes = 64 * 1024;
constexpr int MaximumKeys = 256;

struct Profile {
    RadioSettings radio;
    QVector<DmrPrivacyKeyEntry> keys;
    QString voiceBackend;
    bool voiceOutput = true;
    bool voicePayload = true;
    QJsonObject json;
};

bool supports(const QString &module);
bool parse(const QJsonObject &json, const QString &module, Profile &profile, QString &error);
bool readFile(const QString &path, const QString &module, Profile &profile, QString &error);
}

class CliModuleManager : public QObject {
public:
    explicit CliModuleManager(QObject *parent = nullptr);
    ~CliModuleManager() override;

    QJsonObject command(const QJsonObject &request);
    void submitPcm(const QByteArray &pcm);
    void submitChannelIq(const float *interleavedIq, int complexSamples, double sampleRate);
    void submitSpectrum(const std::vector<float> &levelsDbfs, double centerHz, double sampleRateHz);
    void reset();
    void setListeningHz(double frequencyHz);
    bool needsAudio() const;
    bool needsChannelIq() const;
    bool needsSpectrum() const;

    std::function<void(const QJsonObject &)> onEvent;
    std::function<void(const QByteArray &)> onVoicePcm;

private:
    struct Slot {
        QString name;
        std::unique_ptr<QProcess> process;
        QByteArray outputBuffer;
        QString lastStatus;
        QString lastError;
        quint64 frames = 0;
        quint64 droppedFrames = 0;
        quint64 textEvents = 0;
        quint64 voiceFrames = 0;
        bool pendingReset = false;
        QJsonObject digitalProfile;
        bool profileSelected = false;
    };

    Slot ft8;
    Slot ft4;
    Slot m17;
    Slot rtty;
    Slot dmr;
    Slot ax25;
    Slot sstv;
    Slot apt;
    Slot wefax;
    Slot monitor;
    double listeningHz = 100000000.0;
    Slot *slot(const QString &name);
    const Slot *slot(const QString &name) const;
    bool running(const Slot &module) const;
    QJsonObject status(const Slot &module) const;
    QJsonObject start(Slot &module, const QJsonObject &request);
    QJsonObject stop(Slot &module);
    QByteArray configuration(const Slot &module) const;
    bool submit(Slot &module, quint32 kind, quint32 sampleRate, const char *data, quint32 size);
    void consumeOutput(Slot &module);
};

#endif
