#ifndef DIGITALDECODER_H
#define DIGITALDECODER_H

#include <QObject>
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QtGlobal>

#include <vector>

#include "dmrdecoder.h"
#include "dmrvocoder.h"
#include "dmrprivacyprocessor.h"
#include "cwaudiodecoder.h"
#include "psk31audiodecoder.h"
#include <QTextCodec>
#include "radiosettings.h"

class DigitalDecoder : public QObject {
    Q_OBJECT

public:
    explicit DigitalDecoder(QObject *parent = nullptr);

    void setEnabled(bool enabled);
    bool isEnabled() const;
    void reset();
    void configure(const RadioSettings &settings, int sampleRate = 48000);
    void setDmrVoiceBackendId(const QString &backendId);
    void setDmrVoiceOutputEnabled(bool enabled);
    void setDmrVoicePayloadEnabled(bool enabled);
    void processPcmFrame(const QByteArray &pcmData, const RadioSettings &settings, int sampleRate = 48000);

signals:
    void textDecoded(const QString &text);
    void ft8Decoded(const QDateTime &utc,
                    double rfFrequencyHz,
                    double audioFrequencyHz,
                    double dtSeconds,
                    int snrDbEstimate,
                    double qualityDb,
                    int costasHits,
                    const QString &message);
    void statusChanged(const QString &status);
    void voicePcmReady(const QByteArray &pcmData);
    void dmrMetadataDetected(int colorCode,
                             int timeslot,
                             quint32 targetId,
                             quint32 sourceId,
                             int flco);
    void dmrDibitBurstReady(const QString &dibits,
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

private:
    enum class RttyState {
        Idle,
        ValidateStart,
        Data,
        Stop
    };

    void configureForMode(int modulationType, int sampleRate);
    void resetRttyState();
    void resetFt8State();
    void processFt8Samples(const char *raw, int sampleCount, int sampleRate, QString &decodedText);
    void analyzeFt8Buffer(QString &decodedText, qint64 decodeSlotStartMs, qint64 bufferStartMs);
    bool detectAfskBit(float sample, bool &signalPresent);
    bool detectFskBit(float sample, bool &signalPresent);
    void advanceRttyState(bool markBit, QString &decodedText);
    QString decodeBaudotCode(unsigned int code);
    void updateStatus(const QString &status);
    void clearDmrVoicePcmBuffer();
    void queueDmrVoicePcm(const QByteArray &pcmData);
    void configureDmrPrivacy(const RadioSettings &settings);

    bool decoderEnabled = true;
    int activeMode = -1;
    int activeSampleRate = 48000;

    double markPhase = 0.0;
    double spacePhase = 0.0;
    double markI = 0.0;
    double markQ = 0.0;
    double spaceI = 0.0;
    double spaceQ = 0.0;
    double detectorAlpha = 0.0;

    RttyState rttyState = RttyState::Idle;
    double samplesPerBit = 48000.0 / 45.45;
    double bitCountdown = 0.0;
    unsigned int rttyCode = 0;
    int rttyBitIndex = 0;
    bool previousRttyBit = true;
    bool rttyLettersShift = true;
    bool rttyLastWasNewline = false;
    bool rttyInvertPolarity = false;
    int rttyNoSignalSamples = 0;
    int rttyBadStopCount = 0;
    int rttyDecodedCount = 0;
    double fskLevel = 0.0;
    double signalQuality = 0.0;
    RadioSettings currentSettings;
    QString preferredDmrVoiceBackendId;
    bool dmrVoiceOutputEnabled = true;
    std::vector<float> ft8AudioBuffer;
    int ft8SamplesSinceAnalysis = 0;
    int ft8AnalysisCounter = 0;
    qint64 ft8LastAnalyzedSlot = -1;
    std::vector<double> recentFt8CandidateHz;
    std::vector<int> recentFt8CandidateAnalysis;
    QString lastStatus;
    CwAudioDecoder cwDecoder;
    Psk31AudioDecoder pskDecoder;
    std::unique_ptr<QTextDecoder> pskTextDecoder;
    int pskEncoding = -1;
    qint64 pskStatusSamples = 0;
    DmrDecoder dmrDecoder;
    DmrPrivacyProcessor dmrPrivacy;
    DmrVocoder dmrVocoder;
    DmrVocoder dmrPayloadProbeVocoder;
    DmrVocoder dmrRawProbeVocoder;
    DmrVocoder dmrCanonicalProbeVocoder;
    qint64 dmrAmbeFrameCount = 0;
    qint64 dmrAmbePayloadCount = 0;
    qint64 dmrAmbeFecCorrectionCount = 0;
    qint64 dmrVocoderFrameCount = 0;
    qint64 dmrVocoderErrorCount = 0;
    int dmrAudioLogCounter = 0;
    int dmrCollapsedVoiceCount = 0;
    int dmrVocoderResetRecoveryCount = 0;
    int dmrArtifactConcealCount = 0;
    QByteArray pendingDmrVoicePcm;
};

#endif // DIGITALDECODER_H
