#ifndef DMRDECODER_H
#define DMRDECODER_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

#include "dmrvoicepayload.h"
#include "radiosettings.h"

class DmrDecoder {
public:
    struct DibitBurst {
        QString dibits;
        QString burstKind = QStringLiteral("voice");
        int dataType = -1;
        QString dataTypeName;
        int slotTypeErrors = -1;
        quint64 sample = 0;
        quint64 baseDibit = 0;
        int burstIndex = 0;
        int cadenceSymbols = 0;
        int colorCode = -1;
        int timeslot = 0;
        QString timeslotSource;
        bool cachDecoded = false;
        int cachChannel = 0;
        int cachLcss = 0;
        QString cachMapName;
        QString cachPayloadBits;
        QStringList ambe49Payloads;
    };

    struct Result {
        QString decodedText;
        QString statusText;
        std::vector<QString> ambeFrames;
        std::vector<DmrAmbeSoftFrame> ambeSoftFrames;
        std::vector<DmrAmbePayload> ambePayloads;
        std::vector<DibitBurst> dibitBursts;
        int ambeFecCorrections = 0;
        bool voiceAudioTrusted = false;
        int voiceAudioConfidence = 0;
        bool metadataValid = false;
        int metadataColorCode = -1;
        int metadataTimeslot = 0;
        quint32 metadataTargetId = 0;
        quint32 metadataSourceId = 0;
        int metadataFlco = -1;
        QString serviceStatusText;
        bool statusChanged = false;
        bool lockAcquired = false;
        bool lockLost = false;
    };

    void reset();
    ~DmrDecoder();
    void configure(int sampleRate);
    void setLabHints(bool enabled,
                     int expectedColorCode,
                     int expectedTimeslot,
                     int expectedSourceId,
                     int expectedTargetId,
                     bool manualTimingEnabled = false,
                     int manualTimingOffset = 0,
                     float slicerRatio = 0.625f,
                     bool adaptiveSlicer = true,
                     int ambeLayout = DMR_DEFAULT_AMBE_LAYOUT);
    void setLiveTimingHint(bool enabled, double phaseFraction, double confidence);
    void setVoicePayloadEnabled(bool enabled) { voicePayloadEnabled = enabled; }
    Result processPcmFrame(const QByteArray &pcmData, int sampleRate, double rfFrequencyHz = 0.0);

    struct SyncPattern {
        const char *name = "";
        const char *kind = "";
        const char *source = "";
        const char *symbols = "";
    };

private:
    bool voicePayloadEnabled = true;
    struct SyncHit {
        const SyncPattern *pattern = nullptr;
        int score = 0;
        int errors = 24;
        int outerScore = 0;
        int phase = 0;
        int symbolIndex = 0;
        bool inverted = false;
        double qualityDb = 0.0;
        quint64 absoluteSample = 0;
    };

    struct CachInfo {
        bool decoded = false;
        bool accessType = false;
        int channel = 0;
        int lcss = 0;
        int timingOffset = 0;
        double slicerRatio = 0.0;
        QString mapName;
        QString payloadBits;
    };

    struct SlotTypeInfo {
        bool decoded = false;
        int colorCode = -1;
        int dataType = -1;
        int correctedErrors = 0;
        QString dataTypeName;
        int timingOffset = 0;
        double slicerRatio = 0.0;
        QString mapName;
    };

    struct EmbInfo {
        bool decoded = false;
        int colorCode = -1;
        bool privacyIndicator = false;
        int lcss = 0;
        int correctedErrors = 0;
        int variantIndex = -1;
        int timingOffset = 0;
        float slicerRatio = 0.625f;
        bool inverted = false;
    };

    struct PendingEmb {
        quint64 anchorSample = 0;
        quint64 absoluteSample = 0;
        int burstIndex = 0;
        int cadenceSymbols = 0;
        bool inverted = false;
        float minLevel = 0.0f;
        float maxLevel = 0.0f;
        bool cachDecoded = false;
        int cachChannel = 0;
        int cachLcss = 0;
        QString cachMapName;
        QString cachPayloadBits;
    };

    struct VoiceEmbeddedBits {
        bool decoded = false;
        QString hex;
    };

    struct VoicePayloadBits {
        bool decoded = false;
        bool adaptiveSlicer = false;
        bool hasSoftBits = false;
        float slicerRange = 0.0f;
        float slicerMinSeparation = 0.0f;
        std::array<std::uint8_t, 216> softReliability = {};
        QString leftDibits;
        QString syncDibits;
        QString rightDibits;
        QString leftSamples;
        QString syncSamples;
        QString rightSamples;
        QString leftHex;
        QString rightHex;
        QString combinedHex;
    };

    struct VoiceEmbeddedFrame {
        quint64 anchorSample = 0;
        int cadenceSymbols = 0;
        bool reportedLc = false;
        std::array<bool, 6> present = {};
        std::array<int, 6> colorCode = {};
        std::array<int, 6> correctedErrors = {};
        std::array<int, 6> lcss = {};
        std::array<int, 6> timing = {};
        std::array<QString, 6> emb32 = {};
    };

    struct VoiceLcSequence {
        int colorCode = -1;
        int nextStage = 0;
        int cadenceSymbols = 0;
        quint64 startSample = 0;
        quint64 lastSample = 0;
        std::array<QString, 4> emb32 = {};
        std::array<int, 4> burstIndex = {};
        std::array<int, 4> correctedErrors = {};
        std::array<int, 4> lcss = {};
        std::array<int, 4> timing = {};
    };

    struct VoiceLcFragmentCandidate {
        quint64 absoluteSample = 0;
        int colorCode = -1;
        int lcss = 0;
        int burstIndex = 0;
        int correctedErrors = 0;
        int timing = 0;
        int cadenceSymbols = 0;
        int variantIndex = -1;
        int fragmentVariantIndex = 0;
        float slicerRatio = 0.625f;
        QString emb32;
    };

    struct VoiceServiceInfo {
        bool valid = false;
        int colorCode = -1;
        int timeslot = 0;
        quint32 target = 0;
        quint32 source = 0;
        int flco = 0;
        int fid = 0;
        int serviceOptions = 0;
        int bptcCorrections = 0;
        QString method;
        quint64 absoluteSample = 0;
    };

    void appendSamples(const QByteArray &pcmData);
    bool findBestSync(SyncHit &hit, quint64 minimumAbsoluteSample) const;
    CachInfo decodeCachBeforeSync(const SyncHit &hit, bool allowTimingSearch = false) const;
    SlotTypeInfo decodeSlotTypeAroundSync(const SyncHit &hit) const;
    void scanAndForwardDataBursts();
    bool queueDataBurstForBackend(const SyncHit &hit,
                                  const SlotTypeInfo &slotType,
                                  const CachInfo &cach);
    void scheduleVoiceEmbBursts(const SyncHit &hit, const CachInfo &anchorCach);
    void processPendingVoiceEmb();
    bool measureSyncLevels(const SyncHit &hit, float &minLevel, float &maxLevel) const;
    EmbInfo decodeVoiceEmbAt(const PendingEmb &pending) const;
    VoiceEmbeddedBits decodeVoiceEmbeddedFragmentAt(const PendingEmb &pending,
                                                    bool inverted,
                                                    int timingOffset,
                                                    float slicerRatio) const;
    VoicePayloadBits decodeVoicePayloadAt(const PendingEmb &pending,
                                          bool inverted,
                                          int timingOffset,
                                          float slicerRatio,
                                          bool useAdaptiveSlicer,
                                          int ambeLayout,
                                          int bitMapVariant,
                                          bool includeDebug = false) const;
    void recordVoiceEmbeddedFragment(const PendingEmb &pending,
                                     const EmbInfo &emb,
                                     const VoiceEmbeddedBits &fragment);
    void recordVoiceLcSequenceFragment(const PendingEmb &pending,
                                       const EmbInfo &emb,
                                       const VoiceEmbeddedBits &fragment);
    void recordVoiceLcFragmentCandidate(const PendingEmb &pending,
                                        const EmbInfo &emb,
                                        const VoiceEmbeddedBits &fragment);
    void tryReportVoiceLcCandidateMatches(const VoiceLcFragmentCandidate &latest);
    void rememberVoiceLcRaw(const QString &emb128);
    void queueVoiceLcDecode(const QString &emb128,
                            int colorCode,
                            quint64 absoluteSample,
                            const QString &method);
    void reportVoiceLcSequence(const VoiceLcSequence &sequence);
    QString takePendingDecodedMessages();
    void flushDmrDibitDump();
    void voteVoicePayloadCadence(int cadenceSymbols,
                                 int colorCode,
                                 int weight,
                                 quint64 absoluteSample,
                                 const QString &source);
    bool shouldRejectVoicePayloadCadence(int cadenceSymbols) const;
    void rememberVoicePayloadCadence(int cadenceSymbols,
                                     int colorCode,
                                     quint64 absoluteSample,
                                     const QString &source);
    bool acceptsVoicePayloadCadence(const PendingEmb &pending, const EmbInfo &emb) const;
    bool acceptsKnownVoicePayloadCadence(const PendingEmb &pending) const;
    bool hasTrustedVoiceAudio(int *confidence = nullptr) const;
    void queueVoicePayloadFrames(const PendingEmb &pending,
                                 bool inverted,
                                 int timingOffset,
                                 float slicerRatio);
    std::vector<QString> takePendingAmbeFrames();
    std::vector<DmrAmbeSoftFrame> takePendingAmbeSoftFrames();
    std::vector<DmrAmbePayload> takePendingAmbePayloads(int *correctedErrors);
    QString voiceLcRawSummaryText() const;
    QString signalQualitySummaryText() const;
    QString voicePayloadColorScoreSummaryText() const;
    QString serviceStatusText() const;
    QString voiceServiceSummaryText() const;
    int currentServiceTimeslot() const;
    void rememberVoiceServiceInfo(int colorCode,
                                  quint32 target,
                                  quint32 source,
                                  int flco,
                                  int fid,
                                  int serviceOptions,
                                  int bptcCorrections,
                                  quint64 absoluteSample,
                                  const QString &method);
    bool labLoggingEnabled() const;
    void resetSignalQualityCounters();
    QString formatHit(const SyncHit &hit) const;
    QString idleStatus() const;

    int activeSampleRate = 48000;
    int samplesPerSymbol = 10;
    double dcEstimate = 0.0;
    double averageMagnitude = 0.0;
    quint64 totalSamples = 0;
    quint64 lastReportSample = 0;
    quint64 lastAcceptedSyncSample = 0;
    bool haveAcceptedSync = false;
    bool haveLock = false;
    bool lockedPolarityInverted = false;
    int lockedPhase = 0;
    double lockedRfFrequencyHz = 0.0;
    int confirmedSyncsSinceReport = 0;
    int candidateSyncsSinceReport = 0;
    int polarityFlipsSinceReport = 0;
    int bestScoreSinceReport = 0;
    int bestPhaseSinceReport = 0;
    double bestQualitySinceReport = 0.0;
    bool bestPolarityInvertedSinceReport = false;
    quint64 lastConfirmedSyncSample = 0;
    int spacingSamplesCount = 0;
    double spacingMsSum = 0.0;
    double spacingMsMin = 0.0;
    double spacingMsMax = 0.0;
    int phaseHistogram[16] = {};
    int intervalHistogram[12] = {};
    int cachOkSinceReport = 0;
    int cachStrictOkSinceReport = 0;
    int cachFailSinceReport = 0;
    int cachChannelHistogram[2] = {};
    int cachLcssHistogram[4] = {};
    int cachTimingOffsetHistogram[5] = {};
    int cachMapEtsiCount = 0;
    int cachMapLegacyCount = 0;
    int slotTypeOkSinceReport = 0;
    int slotTypeFailSinceReport = 0;
    int colorCodeHistogram[16] = {};
    int dataTypeHistogram[16] = {};
    int slotTypeCorrectedErrors = 0;
    int embOkSinceReport = 0;
    int embAnchorsSinceReport = 0;
    int embWeakSinceReport = 0;
    int embFailSinceReport = 0;
    int embColorCodeHistogram[16] = {};
    int embLcssHistogram[4] = {};
    int embPrivacyCount = 0;
    int embCorrectedErrors = 0;
    int embVariantHistogram[8] = {};
    int embWeakVariantHistogram[8] = {};
    int embErrorHistogram[3] = {};
    int embWeakErrorHistogram[3] = {};
    int embWeakColorCodeHistogram[16] = {};
    int embTimingOffsetHistogram[5] = {};
    int embWeakTimingOffsetHistogram[5] = {};
    quint64 pcmSamplesSinceReport = 0;
    double pcmSumSinceReport = 0.0;
    double pcmSquareSumSinceReport = 0.0;
    double pcmAbsPeakSinceReport = 0.0;
    int pcmClippedSinceReport = 0;
    int ambePayloadsSinceReport = 0;
    int ambeFecCorrectionsSinceReport = 0;
    int ambeFecDecodeFailSinceReport = 0;
    int ambeFecCorrectionHistogram[7] = {};
    int voicePayloadFallbackSinceReport = 0;
    int voiceLcAssemblySinceReport = 0;
    int voiceLcBptcSinceReport = 0;
    int voiceLcStrictSinceReport = 0;
    int voiceLcLabTargetMatchSinceReport = 0;
    int voiceLcLabSourceMatchSinceReport = 0;
    int voiceLcLabFullMatchSinceReport = 0;
    int voiceLcFragmentHistogram[4] = {};
    int voiceLcUsableFragmentHistogram[4] = {};
    QString lastCachText;
    QString lastStatus;
    QString lastPatternName;
    QString lockedPatternName;
    quint64 lastVoiceEmbAnchorSample = 0;
    quint64 lastForwardedDataBurstSample = 0;
    std::deque<float> sampleBuffer;
    std::deque<PendingEmb> pendingVoiceEmb;
    std::deque<VoiceEmbeddedFrame> voiceEmbeddedFrames;
    std::deque<VoiceLcSequence> voiceLcSequences;
    std::deque<VoiceLcFragmentCandidate> voiceLcFragmentCandidates;
    std::deque<QString> reportedVoiceLcCandidateRaw;
    std::deque<QString> pendingDecodedMessages;
    std::deque<QString> pendingAmbeFrames;
    std::deque<DmrAmbeSoftFrame> pendingAmbeSoftFrames;
    std::deque<DmrAmbePayload> pendingAmbePayloads;
    std::deque<quint64> queuedVoicePayloadBurstSamples;
    std::vector<DibitBurst> pendingDibitBursts;
    std::vector<QString> voiceLcRawSinceReport;
    int voicePayloadCadenceScore30 = 0;
    int voicePayloadCadenceScore60 = 0;
    int voicePayloadColorCodeScore[16] = {};
    int lastVoicePayloadAmbeLayout = DMR_DEFAULT_AMBE_LAYOUT;
    int lastVoicePayloadBitMapVariant = 0;
    int dmrDibitDumpRemaining = 0;
    int dmrDibitDumpSequence = 0;
    quint64 dmrDibitDumpSession = 0;
    QString dmrDibitDumpBuffer;
    int selectedVoicePayloadCadenceSymbols = 0;
    int selectedVoicePayloadColorCode = -1;
    quint64 selectedVoicePayloadSample = 0;
    QString lastVoiceLcUserKey;
    quint64 lastVoiceLcUserSample = 0;
    VoiceServiceInfo lastVoiceService;
    int lastObservedTimeslot = 0;
    bool labHintsEnabled = false;
    int labExpectedColorCode = -1;
    int labExpectedTimeslot = 0;
    int labExpectedSourceId = 0;
    int labExpectedTargetId = 0;
    bool labManualTimingEnabled = false;
    int labManualTimingOffset = 0;
    float labSlicerRatio = 0.625f;
    bool labAdaptiveSlicer = true;
    int labAmbeLayout = DMR_DEFAULT_AMBE_LAYOUT;
    bool liveTimingHintEnabled = false;
    double liveTimingPhaseFraction = 0.5;
    double liveTimingConfidence = 0.0;
};

#endif // DMRDECODER_H
