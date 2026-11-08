#include "main.h"

#include "appconstants.h"
#include "diagnosticlogging.h"
#include "gnssqthhelpers.h"
#include "qthmapwidget.h"
#include "samplefileutils.h"
#include "tuningutils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace {

struct GpsLnavHistorySearchResult {
    bool found = false;
    int offset = -1;
    int polarityMask = 0;
    QVector<int> segmentIds;
    int polarityTransitions = 0;
    int polarityInvertedSegments = 0;
    int parityAvailableWords = 0;
    int parityValidWords = 0;
    int parityInitialPrevD29 = 0;
    int parityInitialPrevD30 = 0;
    bool parityUsesD30DeWhitening = true;
    bool tlmHowValid = false;
};

struct GpsLnavDecodedSummary {
    bool valid = false;
    int tlmPreamble = 0;
    int tlmMessage = 0;
    bool tlmIntegrityStatus = false;
    int howTowCount = 0;
    int howTowSeconds = 0;
    bool howAlert = false;
    bool howAntiSpoof = false;
    int howSubframeId = 0;
};

QString bitsToString(const QVector<int> &bits, int maxBits = 96) {
    QString out;
    const int count = std::min(bits.size(), maxBits);
    out.reserve(count + (bits.size() > count ? 3 : 0));
    for (int i = 0; i < count; ++i) {
        out.append(bits.at(i) ? QLatin1Char('1') : QLatin1Char('0'));
    }
    if (bits.size() > count) {
        out.append(QStringLiteral("..."));
    }
    return out;
}

int gpsXorTerms(const QVector<int> &dataBits, std::initializer_list<int> oneBasedIndexes) {
    int value = 0;
    for (int index : oneBasedIndexes) {
        value ^= dataBits.at(index - 1) & 1;
    }
    return value;
}

bool gpsLnavWordParityMatches(const QVector<int> &wordBits,
                              int previousD29,
                              int previousD30,
                              bool applyD30DeWhitening) {
    if (wordBits.size() < 30) {
        return false;
    }

    QVector<int> dataBits;
    dataBits.reserve(24);
    for (int i = 0; i < 24; ++i) {
        int bit = wordBits.at(i) & 1;
        if (applyD30DeWhitening) {
            bit ^= (previousD30 & 1);
        }
        dataBits.push_back(bit);
    }

    const int expectedD25 = (previousD29 & 1) ^
                            gpsXorTerms(dataBits, {1, 2, 3, 5, 6, 10, 11, 12, 13, 14, 17, 18, 20, 23});
    const int expectedD26 = (previousD30 & 1) ^
                            gpsXorTerms(dataBits, {2, 3, 4, 6, 7, 11, 12, 13, 14, 15, 18, 19, 21, 24});
    const int expectedD27 = (previousD29 & 1) ^
                            gpsXorTerms(dataBits, {1, 3, 4, 5, 7, 8, 12, 13, 14, 15, 16, 19, 20, 22});
    const int expectedD28 = (previousD30 & 1) ^
                            gpsXorTerms(dataBits, {2, 4, 5, 6, 8, 9, 13, 14, 15, 16, 17, 20, 21, 23});
    const int expectedD29 = (previousD30 & 1) ^
                            gpsXorTerms(dataBits, {1, 3, 5, 6, 7, 9, 10, 14, 15, 16, 17, 18, 21, 22, 24});
    const int expectedD30 = (previousD29 & 1) ^
                            gpsXorTerms(dataBits, {3, 5, 6, 8, 9, 10, 11, 13, 15, 19, 22, 23, 24});

    return ((wordBits.at(24) & 1) == (expectedD25 & 1)) &&
           ((wordBits.at(25) & 1) == (expectedD26 & 1)) &&
           ((wordBits.at(26) & 1) == (expectedD27 & 1)) &&
           ((wordBits.at(27) & 1) == (expectedD28 & 1)) &&
           ((wordBits.at(28) & 1) == (expectedD29 & 1)) &&
           ((wordBits.at(29) & 1) == (expectedD30 & 1));
}

GpsLnavHistorySearchResult validateGpsLnavParityPrefix(const QVector<int> &candidateBits) {
    GpsLnavHistorySearchResult result;
    result.parityAvailableWords = candidateBits.size() / 30;
    if (result.parityAvailableWords <= 0) {
        return result;
    }

    for (bool dewhiten : {true, false}) {
        for (int initialD29 = 0; initialD29 <= 1; ++initialD29) {
            for (int initialD30 = 0; initialD30 <= 1; ++initialD30) {
                int previousD29 = initialD29;
                int previousD30 = initialD30;
                int validWords = 0;
                for (int wordIndex = 0; wordIndex < result.parityAvailableWords; ++wordIndex) {
                    QVector<int> wordBits;
                    wordBits.reserve(30);
                    for (int bit = 0; bit < 30; ++bit) {
                        wordBits.push_back(candidateBits.at(wordIndex * 30 + bit) & 1);
                    }
                    if (!gpsLnavWordParityMatches(wordBits, previousD29, previousD30, dewhiten)) {
                        break;
                    }
                    ++validWords;
                    previousD29 = wordBits.at(28) & 1;
                    previousD30 = wordBits.at(29) & 1;
                }

                if (validWords > result.parityValidWords) {
                    result.parityValidWords = validWords;
                    result.parityInitialPrevD29 = initialD29;
                    result.parityInitialPrevD30 = initialD30;
                    result.parityUsesD30DeWhitening = dewhiten;
                }
            }
        }
    }

    return result;
}

QVector<int> uniqueSegmentsInRange(const QVector<int> &segmentIds, int offset, int count) {
    QVector<int> segments;
    segments.reserve((std::min)(count, 32));
    const int end = (std::min)(segmentIds.size(), offset + count);
    for (int i = offset; i < end; ++i) {
        const int segmentId = segmentIds.at(i);
        if (!segments.contains(segmentId)) {
            segments.push_back(segmentId);
        }
    }
    return segments;
}

int maskForSegmentId(const QVector<int> &segments, int mask, int segmentId) {
    const int index = segments.indexOf(segmentId);
    return index >= 0 ? ((mask >> index) & 1) : 0;
}

int countMaskBits(int mask) {
    int count = 0;
    while (mask != 0) {
        count += mask & 1;
        mask >>= 1;
    }
    return count;
}

int countPolarityTransitions(const QVector<int> &segments, int mask) {
    if (segments.size() < 2) {
        return 0;
    }
    int transitions = 0;
    int previous = maskForSegmentId(segments, mask, segments.first());
    for (int i = 1; i < segments.size(); ++i) {
        const int current = maskForSegmentId(segments, mask, segments.at(i));
        if (current != previous) {
            ++transitions;
        }
        previous = current;
    }
    return transitions;
}

int bitsToUnsigned(const QVector<int> &bits, int offset, int count) {
    int value = 0;
    for (int i = 0; i < count && offset + i < bits.size(); ++i) {
        value = (value << 1) | (bits.at(offset + i) & 1);
    }
    return value;
}

QVector<int> normalizeGpsLnavBits(const QVector<int> &bits,
                                  const QVector<int> &segmentIds,
                                  const GpsLnavHistorySearchResult &search,
                                  int maxBits) {
    QVector<int> normalizedBits;
    if (!search.found || search.offset < 0 || search.offset >= bits.size() || maxBits <= 0) {
        return normalizedBits;
    }
    const int count = (std::min)(maxBits, bits.size() - search.offset);
    normalizedBits.reserve(count);
    for (int i = 0; i < count; ++i) {
        const bool invertSegment =
            maskForSegmentId(search.segmentIds, search.polarityMask, segmentIds.at(search.offset + i)) != 0;
        normalizedBits.push_back((bits.at(search.offset + i) ^ (invertSegment ? 1 : 0)) & 1);
    }
    return normalizedBits;
}

QVector<int> gpsLnavDataBitsFromWords(const QVector<int> &normalizedWords,
                                      int initialPreviousD30,
                                      bool applyD30DeWhitening) {
    QVector<int> dataBits;
    const int words = normalizedWords.size() / 30;
    dataBits.reserve(words * 24);
    int previousD30 = initialPreviousD30 & 1;
    for (int wordIndex = 0; wordIndex < words; ++wordIndex) {
        for (int bit = 0; bit < 24; ++bit) {
            int value = normalizedWords.at(wordIndex * 30 + bit) & 1;
            if (applyD30DeWhitening) {
                value ^= previousD30;
            }
            dataBits.push_back(value);
        }
        previousD30 = normalizedWords.at(wordIndex * 30 + 29) & 1;
    }
    return dataBits;
}

GpsLnavDecodedSummary decodeGpsLnavTlmHow(const QVector<int> &normalizedWords,
                                          const GpsLnavHistorySearchResult &search) {
    GpsLnavDecodedSummary summary;
    if (!search.found || search.parityValidWords < 2 || normalizedWords.size() < 60) {
        return summary;
    }

    const QVector<int> dataBits =
        gpsLnavDataBitsFromWords(normalizedWords,
                                 search.parityInitialPrevD30,
                                 search.parityUsesD30DeWhitening);
    if (dataBits.size() < 48) {
        return summary;
    }

    summary.tlmPreamble = bitsToUnsigned(dataBits, 0, 8);
    summary.tlmMessage = bitsToUnsigned(dataBits, 8, 14);
    summary.tlmIntegrityStatus = (dataBits.at(22) & 1) != 0;
    summary.howTowCount = bitsToUnsigned(dataBits, 24, 17);
    summary.howTowSeconds = summary.howTowCount * 6;
    summary.howAlert = (dataBits.at(41) & 1) != 0;
    summary.howAntiSpoof = (dataBits.at(42) & 1) != 0;
    summary.howSubframeId = bitsToUnsigned(dataBits, 43, 3);
    summary.valid = summary.tlmPreamble == 0x8b &&
                    summary.howSubframeId >= 1 &&
                    summary.howSubframeId <= 5;
    return summary;
}

bool lnavSearchResultIsBetter(const GpsLnavHistorySearchResult &candidate,
                              const GpsLnavHistorySearchResult &current) {
    if (!candidate.found) {
        return false;
    }
    if (!current.found) {
        return true;
    }
    if (candidate.tlmHowValid != current.tlmHowValid) {
        return candidate.tlmHowValid;
    }
    if (candidate.parityValidWords != current.parityValidWords) {
        return candidate.parityValidWords > current.parityValidWords;
    }
    if (candidate.parityAvailableWords != current.parityAvailableWords) {
        return candidate.parityAvailableWords > current.parityAvailableWords;
    }
    if (candidate.polarityTransitions != current.polarityTransitions) {
        return candidate.polarityTransitions < current.polarityTransitions;
    }
    if (candidate.polarityInvertedSegments != current.polarityInvertedSegments) {
        return candidate.polarityInvertedSegments < current.polarityInvertedSegments;
    }
    return candidate.offset < current.offset;
}

GpsLnavHistorySearchResult searchGpsLnavPreambleWithSegmentPolarity(const QVector<int> &bits,
                                                                     const QVector<int> &segmentIds) {
    GpsLnavHistorySearchResult bestResult;
    if (bits.size() < 8 || bits.size() != segmentIds.size()) {
        return bestResult;
    }

    constexpr std::array<int, 8> kGpsLnavPreamble = {{1, 0, 0, 0, 1, 0, 1, 1}};
    for (int offset = 0; offset + static_cast<int>(kGpsLnavPreamble.size()) <= bits.size(); ++offset) {
        const QVector<int> preambleSegments =
            uniqueSegmentsInRange(segmentIds, offset, static_cast<int>(kGpsLnavPreamble.size()));
        if (preambleSegments.size() > 8) {
            continue;
        }

        const int preamblePolarityCombinations = 1 << preambleSegments.size();
        for (int preambleMask = 0; preambleMask < preamblePolarityCombinations; ++preambleMask) {
            bool match = true;
            for (int i = 0; i < static_cast<int>(kGpsLnavPreamble.size()); ++i) {
                const bool invertSegment =
                    maskForSegmentId(preambleSegments, preambleMask, segmentIds.at(offset + i)) != 0;
                const int bit = bits.at(offset + i) ^ (invertSegment ? 1 : 0);
                if (bit != kGpsLnavPreamble[static_cast<std::size_t>(i)]) {
                    match = false;
                    break;
                }
            }

            if (!match) {
                continue;
            }

            GpsLnavHistorySearchResult candidate;
            candidate.found = true;
            candidate.offset = offset;
            candidate.polarityMask = preambleMask;
            candidate.segmentIds = preambleSegments;
            candidate.polarityTransitions =
                countPolarityTransitions(candidate.segmentIds, candidate.polarityMask);
            candidate.polarityInvertedSegments = countMaskBits(candidate.polarityMask);

            const int availableWords = (bits.size() - offset) / 30;
            const int paritySpanBits = (std::min)(60, availableWords * 30);
            if (paritySpanBits >= 30) {
                const QVector<int> paritySegments = uniqueSegmentsInRange(segmentIds, offset, paritySpanBits);
                if (paritySegments.size() <= 18) {
                    int fixedParityMask = 0;
                    QVector<int> freeSegmentIndexes;
                    freeSegmentIndexes.reserve(paritySegments.size());
                    for (int segmentIndex = 0; segmentIndex < paritySegments.size(); ++segmentIndex) {
                        const int preambleIndex = preambleSegments.indexOf(paritySegments.at(segmentIndex));
                        if (preambleIndex >= 0) {
                            if ((preambleMask & (1 << preambleIndex)) != 0) {
                                fixedParityMask |= (1 << segmentIndex);
                            }
                        } else {
                            freeSegmentIndexes.push_back(segmentIndex);
                        }
                    }

                    const int extraCombinations =
                        freeSegmentIndexes.size() <= 14 ? (1 << freeSegmentIndexes.size()) : 0;
                    for (int extraMask = 0; extraMask < extraCombinations; ++extraMask) {
                        int parityMask = fixedParityMask;
                        for (int bitIndex = 0; bitIndex < freeSegmentIndexes.size(); ++bitIndex) {
                            if ((extraMask & (1 << bitIndex)) != 0) {
                                parityMask |= (1 << freeSegmentIndexes.at(bitIndex));
                            }
                        }

                        QVector<int> normalizedBits;
                        normalizedBits.reserve(paritySpanBits);
                        for (int i = 0; i < paritySpanBits; ++i) {
                            const bool invertSegment =
                                maskForSegmentId(paritySegments, parityMask, segmentIds.at(offset + i)) != 0;
                            normalizedBits.push_back((bits.at(offset + i) ^ (invertSegment ? 1 : 0)) & 1);
                        }

                        const GpsLnavHistorySearchResult parity =
                            validateGpsLnavParityPrefix(normalizedBits);
                        GpsLnavHistorySearchResult parityCandidate = candidate;
                        parityCandidate.polarityMask = parityMask;
                        parityCandidate.segmentIds = paritySegments;
                        parityCandidate.polarityTransitions =
                            countPolarityTransitions(parityCandidate.segmentIds,
                                                     parityCandidate.polarityMask);
                        parityCandidate.polarityInvertedSegments =
                            countMaskBits(parityCandidate.polarityMask);
                        parityCandidate.parityAvailableWords = parity.parityAvailableWords;
                        parityCandidate.parityValidWords = parity.parityValidWords;
                        parityCandidate.parityInitialPrevD29 = parity.parityInitialPrevD29;
                        parityCandidate.parityInitialPrevD30 = parity.parityInitialPrevD30;
                        parityCandidate.parityUsesD30DeWhitening = parity.parityUsesD30DeWhitening;
                        parityCandidate.tlmHowValid =
                            decodeGpsLnavTlmHow(normalizedBits, parityCandidate).valid;
                        if (lnavSearchResultIsBetter(parityCandidate, candidate)) {
                            candidate = parityCandidate;
                        }
                        if (candidate.parityValidWords >= 2 && candidate.tlmHowValid) {
                            return candidate;
                        }
                    }
                } else {
                    candidate.parityAvailableWords = paritySpanBits / 30;
                }
            }

            if (lnavSearchResultIsBetter(candidate, bestResult)) {
                bestResult = candidate;
            }
        }
    }

    return bestResult;
}

} // namespace

void YourClassName::logGnssRawContext() {
    const QString locator = qth::maidenheadLocator(qthLatitude, qthLongitude, 6);
    std::vector<float> snapshot;
    std::uint64_t sequence = 0;
    const bool haveSnapshot =
        IqBuffer::snapshotRecent(snapshot, GNSS_QUICK_SNAPSHOT_MAX_FLOATS, &sequence) &&
        snapshot.size() >= 2;
    const double measuredSampleRate = IqBuffer::sampleRateEstimate();
    const double configuredSampleRate = pendingSettings.sampleRate;
    const bool haveConfiguredSampleRate =
        std::isfinite(configuredSampleRate) && configuredSampleRate > 0.0;
    const bool haveMeasuredSampleRate =
        std::isfinite(measuredSampleRate) && measuredSampleRate > 0.0;
    double snapshotSampleRate = haveConfiguredSampleRate ? configuredSampleRate : measuredSampleRate;
    const QString sampleRateSource =
        haveConfiguredSampleRate ? QStringLiteral("configured") : QStringLiteral("measured");
    qDebug() << "[GNSS raw]"
             << "system" << gnssSystemId
             << "centerHz" << pendingSettings.centerFrequency
             << "actualHz" << pendingSettings.actualFrequency
             << "listeningHz" << pendingSettings.listeningFrequency
             << "sampleRate" << snapshotSampleRate
             << "sampleRateSource" << sampleRateSource
             << "configuredSampleRate" << configuredSampleRate
             << "measuredSampleRate" << measuredSampleRate
             << "bandwidthHz" << pendingSettings.bandwidth
             << "gnssIntegrationMs" << gnssAcquisitionIntegrationMs
             << "gnssChannelFilterCutoffHz" << gnssChannelFilterCutoffHz
             << "inputMode" << pendingSettings.inputMode
             << "standardScanCentersMhz" << standardScanCentersMhz
             << "agileScanRangesMhz" << agileScanRangesMhz
             << "listeningScanTargetsMhz" << listeningScanTargetsMhz
             << "listeningScanEnabled" << listeningScanEnabled
             << "qth" << locator
             << "snapshotSequence" << sequence
             << "snapshotFloats" << static_cast<int>(snapshot.size())
             << "recording" << "GNSS IQ snapshot WAV";
    if (!haveSnapshot || !std::isfinite(snapshotSampleRate) || snapshotSampleRate <= 0.0) {
        if (qthStatusLabel) {
            qthStatusLabel->setText(uiText(QStringLiteral("gnss_raw_no_iq"),
                                           QStringLiteral("GNSS IQ snapshot: no IQ data yet.")));
        }
        return;
    }

    const int iqSamples = static_cast<int>(snapshot.size() / 2);
    const quint64 dataBytes64 = static_cast<quint64>(iqSamples) * 2U * sizeof(qint16);
    if (dataBytes64 == 0 || dataBytes64 > 0xffffffffULL) {
        if (qthStatusLabel) {
            qthStatusLabel->setText(uiText(QStringLiteral("gnss_raw_save_failed"),
                                           QStringLiteral("GNSS IQ snapshot save failed: %1"))
                                        .arg(QStringLiteral("snapshot is too large")));
        }
        return;
    }

    QDir recordingsDir(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("recordings")));
    if (!recordingsDir.exists() && !recordingsDir.mkpath(QStringLiteral("."))) {
        if (qthStatusLabel) {
            qthStatusLabel->setText(uiText(QStringLiteral("gnss_raw_save_failed"),
                                           QStringLiteral("GNSS IQ snapshot save failed: %1"))
                                        .arg(QStringLiteral("cannot create recordings folder")));
        }
        return;
    }

    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString fileName = QStringLiteral("ObriiSDR_%1_gnss_iq.wav").arg(timestamp);
    const QString wavPath = recordingsDir.filePath(fileName);
    QFile wavFile(wavPath);
    if (!wavFile.open(QIODevice::WriteOnly)) {
        if (qthStatusLabel) {
            qthStatusLabel->setText(uiText(QStringLiteral("gnss_raw_save_failed"),
                                           QStringLiteral("GNSS IQ snapshot save failed: %1"))
                                        .arg(wavFile.errorString()));
        }
        return;
    }

    const int wavRate = static_cast<int>((std::max)(1.0, std::round(snapshotSampleRate)));
    wavFile.write(fixedPcm16StereoWavHeader(wavRate, static_cast<quint32>(dataBytes64)));

    QByteArray chunk;
    chunk.reserve(64 * 1024);
    for (int n = 0; n < iqSamples; ++n) {
        appendLe16(chunk, static_cast<quint16>(pcm16FromFloat(snapshot[static_cast<std::size_t>(2 * n)])));
        appendLe16(chunk, static_cast<quint16>(pcm16FromFloat(snapshot[static_cast<std::size_t>(2 * n + 1)])));
        if (chunk.size() >= 64 * 1024) {
            wavFile.write(chunk);
            chunk.clear();
        }
    }
    if (!chunk.isEmpty()) {
        wavFile.write(chunk);
    }
    wavFile.close();

    QJsonObject metadata;
    metadata["app"] = QStringLiteral("ObriiSDR");
    metadata["mode"] = QStringLiteral("gnss_iq_snapshot");
    metadata["gnssSystemId"] = gnssSystemId;
    metadata["fileName"] = fileName;
    metadata["filePath"] = QFileInfo(wavPath).absoluteFilePath();
    metadata["recordedAtLocal"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    metadata["sampleRate"] = snapshotSampleRate;
    metadata["sampleRateSource"] = sampleRateSource;
    metadata["configuredSampleRate"] = haveConfiguredSampleRate ? configuredSampleRate : 0.0;
    metadata["measuredSampleRate"] = haveMeasuredSampleRate ? measuredSampleRate : 0.0;
    metadata["waveSampleRate"] = wavRate;
    metadata["iqSamples"] = iqSamples;
    metadata["dataBytes"] = static_cast<double>(dataBytes64);
    metadata["snapshotSequence"] = static_cast<double>(sequence);
    metadata["centerFrequency"] = pendingSettings.centerFrequency;
    metadata["actualFrequency"] = pendingSettings.actualFrequency;
    metadata["listeningFrequency"] = pendingSettings.listeningFrequency;
    metadata["bandwidth"] = pendingSettings.bandwidth;
    metadata["gnssAcquisitionIntegrationMs"] = gnssAcquisitionIntegrationMs;
    metadata["gnssChannelFilterCutoffHz"] = gnssChannelFilterCutoffHz;
    metadata["inputMode"] = pendingSettings.inputMode;
    metadata["lnaGain"] = pendingSettings.lnaGain;
    metadata["vgaGain"] = pendingSettings.vgaGain;
    metadata["rtlAgc"] = pendingSettings.rtlAgc;
    metadata["rtlTunerGainTenthsDb"] = pendingSettings.rtlTunerGainTenthsDb;
    metadata["qth"] = locator;
    metadata["positionPrivacy"] = QStringLiteral("Exact latitude/longitude omitted from shared GNSS sidecar metadata.");

    const QString jsonPath =
        recordingsDir.filePath(QFileInfo(fileName).completeBaseName() + QStringLiteral(".json"));
    QFile jsonFile(jsonPath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        jsonFile.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
        jsonFile.close();
    } else {
        qDebug() << "[GNSS raw] failed to write sidecar"
                 << jsonPath
                 << jsonFile.errorString();
    }

    qDebug() << "[GNSS raw saved]"
             << "path" << wavPath
             << "json" << jsonPath
             << "sampleRate" << snapshotSampleRate
             << "sampleRateSource" << sampleRateSource
             << "measuredSampleRate" << measuredSampleRate
             << "iqSamples" << iqSamples
             << "dataBytes" << dataBytes64;
    if (qthStatusLabel) {
        qthStatusLabel->setText(uiText(QStringLiteral("gnss_raw_saved"),
                                       QStringLiteral("GNSS IQ snapshot saved: %1"))
                                    .arg(fileName));
    }
}

bool YourClassName::isGnssMonitorActive() const {
    return gnssMonitorEnabled &&
           (!gnssMonitorCheckbox || gnssMonitorCheckbox->isChecked());
}

void YourClassName::resetGnssMonitor() {
    gnssSignalMonitor.reset();
    gnssMonitorSnapshotTimer.invalidate();
    gnssMonitorLogTimer.invalidate();
    gnssMonitorUiTimer.invalidate();
    if (gnssMonitorStatusLabel) {
        gnssMonitorStatusLabel->setText(isGnssMonitorActive()
                                            ? uiText(QStringLiteral("gnss_iq_monitor_waiting"),
                                                     QStringLiteral("GNSS IQ monitor: waiting for IQ samples"))
                                            : uiText(QStringLiteral("gnss_iq_monitor_idle"),
                                                     QStringLiteral("GNSS IQ monitor: off")));
    }
    qDebug() << "[GNSS IQ] monitor reset"
             << "enabled" << isGnssMonitorActive()
             << "centerHz" << pendingSettings.centerFrequency
             << "sampleRate" << pendingSettings.sampleRate;
}

void YourClassName::processGnssIqSnapshot(const RadioSettings &settings) {
    if (!isGnssMonitorActive()) {
        return;
    }
    if (gnssMonitorSnapshotTimer.isValid() &&
        gnssMonitorSnapshotTimer.elapsed() < 500) {
        return;
    }
    gnssMonitorSnapshotTimer.restart();

    std::vector<float> snapshot;
    std::uint64_t sequence = 0;
    if (!IqBuffer::snapshotRecent(snapshot, GNSS_QUICK_SNAPSHOT_MAX_FLOATS, &sequence) ||
        snapshot.size() < 4) {
        return;
    }
    if (sequence != 0 && sequence == gnssSignalMonitor.lastSnapshotSequence()) {
        return;
    }

    double sampleRate = settings.sampleRate;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        sampleRate = IqBuffer::sampleRateEstimate();
    }
    const GnssSignalReport report =
        gnssSignalMonitor.analyzeFloatSnapshot(snapshot, sampleRate, settings, sequence);
    updateGnssMonitorStatus(report);
}

void YourClassName::processGnssPackedIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount) {
    if (!isGnssMonitorActive() || iqData.isEmpty()) {
        return;
    }
    const bool channelized = sampleCount > 0 && iqData.size() >= sampleCount * 4;
    RadioSettings settings = pendingSettings;
    settings.sampleRate = sampleRate > 0.0 ? sampleRate : settings.sampleRate;
    if (channelized) {
        settings.centerFrequency = pendingSettings.listeningFrequency;
        settings.actualFrequency = pendingSettings.listeningFrequency;
    }
    const GnssSignalReport report =
        gnssSignalMonitor.analyzePackedIqFrame(iqData,
                                               settings.sampleRate,
                                               sampleCount,
                                               channelized,
                                               settings);
    updateGnssMonitorStatus(report);
}

void YourClassName::updateGnssMonitorStatus(const GnssSignalReport &report, bool forceLog) {
    if (!report.valid) {
        return;
    }

    const bool updateUi =
        forceLog ||
        !gnssMonitorUiTimer.isValid() ||
        gnssMonitorUiTimer.elapsed() >= 500;
    if (updateUi) {
        gnssMonitorUiTimer.restart();
        if (gnssMonitorStatusLabel) {
            const double centerMhz = report.centerFrequency / 1000000.0;
            const double rateMhz = report.sampleRate / 1000000.0;
            const QString source = report.fromSnapshot
                                       ? uiText(QStringLiteral("live_snapshot"),
                                                QStringLiteral("live snapshot"))
                                       : uiText(QStringLiteral("iq_frame"),
                                                QStringLiteral("IQ frame"));
            QString statusText = uiText(
                QStringLiteral("gnss_iq_monitor_status"),
                QStringLiteral("%1: %2, center %3 MHz, rate %4 MHz, RMS %5 dBFS, peak %6 dBFS, DC/RMS %7 dB, clip %8%, I/Q %9 dB, frames %10"));
            statusText = statusText.arg(source)
                             .arg(report.sampleFormat)
                             .arg(QString::number(centerMhz, 'f', 6))
                             .arg(QString::number(rateMhz, 'f', 3))
                             .arg(QString::number(report.rmsDbfs, 'f', 1))
                             .arg(QString::number(report.peakDbfs, 'f', 1))
                             .arg(QString::number(report.dcToRmsDb, 'f', 1))
                             .arg(QString::number(report.clippingPercent, 'f', 3))
                             .arg(QString::number(report.iqBalanceDb, 'f', 1))
                             .arg(QString::number(report.frames));
            gnssMonitorStatusLabel->setText(statusText);
        }
    }

    if (forceLog ||
        (fobosVerboseLoggingEnabled() &&
         (!gnssMonitorLogTimer.isValid() ||
          gnssMonitorLogTimer.elapsed() >= 3000))) {
        gnssMonitorLogTimer.restart();
        qDebug() << "[GNSS IQ]"
                 << "source" << (report.fromSnapshot ? "snapshot" : "frame")
                 << "format" << report.sampleFormat
                 << "frames" << report.frames
                 << "samples" << report.samples
                 << "sampleRate" << report.sampleRate
                 << "centerHz" << report.centerFrequency
                 << "listeningHz" << report.listeningFrequency
                 << "rmsDbfs" << report.rmsDbfs
                 << "peakDbfs" << report.peakDbfs
                 << "dcDbfs" << report.dcDbfs
                 << "dcToRmsDb" << report.dcToRmsDb
                 << "clipPercent" << report.clippingPercent
                 << "crestDb" << report.crestDb
                 << "iqBalanceDb" << report.iqBalanceDb
                 << "meanI" << report.meanI
                 << "meanQ" << report.meanQ;
    }
}

void YourClassName::requestGnssNetworkTime() {
    if (!gnssNtpSocket) {
        return;
    }

    QByteArray packet(48, '\0');
    packet[0] = static_cast<char>(0x1b);
    gnssNtpSocket->abort();
    gnssNtpSocket->setProperty("ntpPending", true);
    if (gnssAcquireStatusLabel) {
        gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gnss_ntp_querying"),
                                              QStringLiteral("NTP UTC: querying pool.ntp.org")));
    }

    const QString server = QStringLiteral("pool.ntp.org");
    const QHostInfo hostInfo = QHostInfo::fromName(server);
    QHostAddress serverAddress;
    if (hostInfo.error() == QHostInfo::NoError) {
        const QList<QHostAddress> addresses = hostInfo.addresses();
        for (const QHostAddress &address : addresses) {
            if (address.protocol() == QAbstractSocket::IPv4Protocol) {
                serverAddress = address;
                break;
            }
        }
        if (serverAddress.isNull() && !addresses.isEmpty()) {
            serverAddress = addresses.first();
        }
    }

    if (serverAddress.isNull()) {
        gnssNtpSocket->setProperty("ntpPending", false);
        const QString errorText =
            uiText(QStringLiteral("gnss_ntp_lookup_failed"),
                   QStringLiteral("NTP UTC: DNS lookup failed for %1"))
                .arg(server);
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(errorText);
        }
        qDebug() << "[GNSS NTP] lookup failed"
                 << "server" << server
                 << "error" << hostInfo.errorString();
        return;
    }

    const qint64 written = gnssNtpSocket->writeDatagram(packet, serverAddress, 123);
    if (written < 0) {
        gnssNtpSocket->setProperty("ntpPending", false);
        const QString errorText =
            uiText(QStringLiteral("gnss_ntp_send_failed"),
                   QStringLiteral("NTP UTC: request send failed: %1"))
                .arg(gnssNtpSocket->errorString());
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(errorText);
        }
        qDebug() << "[GNSS NTP] send failed"
                 << "server" << server
                 << "address" << serverAddress.toString()
                 << "error" << gnssNtpSocket->errorString();
        return;
    }

    QTimer::singleShot(3500, this, [this]() {
        if (!gnssNtpSocket || !gnssNtpSocket->property("ntpPending").toBool()) {
            return;
        }
        gnssNtpSocket->setProperty("ntpPending", false);
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(
                uiText(QStringLiteral("gnss_ntp_timeout"),
                       QStringLiteral("NTP UTC: no response; network or UDP/123 may be blocked")));
        }
        qDebug() << "[GNSS NTP] timeout";
    });

    qDebug() << "[GNSS NTP] query"
             << "bytes" << written
             << "server" << server
             << "address" << serverAddress.toString();
}

void YourClassName::handleGnssNetworkTimeResponse() {
    if (!gnssNtpSocket) {
        return;
    }
    while (gnssNtpSocket->hasPendingDatagrams()) {
        gnssNtpSocket->setProperty("ntpPending", false);
        QByteArray datagram;
        datagram.resize(static_cast<int>(gnssNtpSocket->pendingDatagramSize()));
        gnssNtpSocket->readDatagram(datagram.data(), datagram.size());
        if (datagram.size() < 48) {
            continue;
        }
        auto byteAt = [&datagram](int index) -> quint32 {
            return static_cast<quint8>(datagram.at(index));
        };
        const quint32 seconds =
            (byteAt(40) << 24) | (byteAt(41) << 16) | (byteAt(42) << 8) | byteAt(43);
        const quint32 fraction =
            (byteAt(44) << 24) | (byteAt(45) << 16) | (byteAt(46) << 8) | byteAt(47);
        constexpr quint64 kNtpUnixEpochDeltaSeconds = 2208988800ULL;
        if (seconds < kNtpUnixEpochDeltaSeconds) {
            continue;
        }
        const qint64 unixSeconds = static_cast<qint64>(seconds - kNtpUnixEpochDeltaSeconds);
        const qint64 unixMillis =
            unixSeconds * 1000LL +
            static_cast<qint64>((static_cast<double>(fraction) * 1000.0) / 4294967296.0);
        const QDateTime networkUtc = QDateTime::fromMSecsSinceEpoch(unixMillis, Qt::UTC);
        const qint64 localOffsetMs =
            networkUtc.msecsTo(QDateTime::currentDateTimeUtc());
        const QString text =
            uiText(QStringLiteral("gnss_ntp_result"),
                   QStringLiteral("NTP UTC: %1, local offset %2 ms"))
                .arg(networkUtc.toString(Qt::ISODateWithMs),
                     QString::number(localOffsetMs));
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(text);
        }
        qDebug() << "[GNSS NTP] result"
                 << "utc" << networkUtc.toString(Qt::ISODateWithMs)
                 << "localOffsetMs" << localOffsetMs;
    }
}

void YourClassName::updateGnssAcquisitionPlot(const GnssAcquisitionResult &result) {
    if (!gnssAcquisitionPlotLabel) {
        return;
    }
    gnssAcquisitionPlotShowsAcquisition = true;

    const int width = (std::max)(520, gnssAcquisitionPlotLabel->width());
    const int height = 220;
    QPixmap pixmap(width, height);
    pixmap.fill(QColor(18, 22, 27));
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QColor(210, 218, 224));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8));

    const QRect heatRect(42, 22, width - 74, 88);
    const QRect corrRect(42, 128, width - 220, 68);
    const QRect histRect(width - 150, 128, 118, 68);

    const QString systemName = result.systemName.trimmed().isEmpty()
                                   ? QStringLiteral("GPS acquisition")
                                   : result.systemName.trimmed();
    const QString rowName = result.heatmapRowLabel.trimmed().isEmpty()
                                ? QStringLiteral("PRN")
                                : result.heatmapRowLabel.trimmed();
    painter.drawText(heatRect.left(), 14, QStringLiteral("%1 %2 x Doppler").arg(systemName, rowName));
    painter.drawText(corrRect.left(), 122, QStringLiteral("Best code-phase correlation"));
    painter.drawText(histRect.left(), 122, QStringLiteral("Peak history"));

    const int dopplerCount = result.dopplerBinsHz.size();
    const int metricCount = result.prnDopplerMetricDb.size();
    const int heatmapRows = (std::max)(1, result.heatmapRows);
    const bool haveHeatmap = dopplerCount > 0 && metricCount >= heatmapRows * dopplerCount;
    if (haveHeatmap) {
        double minMetric = std::numeric_limits<double>::max();
        double maxMetric = -std::numeric_limits<double>::max();
        for (const double value : result.prnDopplerMetricDb) {
            if (!std::isfinite(value)) {
                continue;
            }
            minMetric = (std::min)(minMetric, value);
            maxMetric = (std::max)(maxMetric, value);
        }
        if (!std::isfinite(minMetric) || !std::isfinite(maxMetric) || maxMetric <= minMetric) {
            minMetric = 0.0;
            maxMetric = 20.0;
        }
        for (int row = 0; row < heatmapRows; ++row) {
            const int y0 = heatRect.top() + (row * heatRect.height()) / heatmapRows;
            const int y1 = heatRect.top() + ((row + 1) * heatRect.height()) / heatmapRows;
            for (int doppler = 0; doppler < dopplerCount; ++doppler) {
                const int x0 = heatRect.left() + (doppler * heatRect.width()) / dopplerCount;
                const int x1 = heatRect.left() + ((doppler + 1) * heatRect.width()) / dopplerCount;
                const double value = result.prnDopplerMetricDb.at(row * dopplerCount + doppler);
                painter.fillRect(QRect(QPoint(x0, y0), QPoint((std::max)(x0, x1 - 1), (std::max)(y0, y1 - 1))),
                                 gnssHeatColor(value, minMetric, maxMetric));
            }
        }
        painter.setPen(QColor(130, 142, 154));
        painter.drawRect(heatRect);
        painter.drawText(4, heatRect.top() + 12,
                         result.heatmapFirstRowLabel.isEmpty()
                             ? QStringLiteral("%1 1").arg(rowName)
                             : result.heatmapFirstRowLabel);
        painter.drawText(4, heatRect.bottom(),
                         result.heatmapLastRowLabel.isEmpty()
                             ? QStringLiteral("%1 %2").arg(rowName).arg(heatmapRows)
                             : result.heatmapLastRowLabel);
        painter.drawText(heatRect.left(), heatRect.bottom() + 14,
                         QStringLiteral("%1 Hz").arg(result.dopplerBinsHz.first(), 0, 'f', 0));
        painter.drawText(heatRect.right() - 60, heatRect.bottom() + 14,
                         QStringLiteral("%1 Hz").arg(result.dopplerBinsHz.last(), 0, 'f', 0));
        painter.drawText(heatRect.center().x() - 28, heatRect.bottom() + 14, QStringLiteral("Doppler"));
        painter.drawText(heatRect.right() - 92, 14,
                         QStringLiteral("%1..%2 dB").arg(minMetric, 0, 'f', 1).arg(maxMetric, 0, 'f', 1));
    } else {
        painter.setPen(QColor(120, 130, 140));
        painter.drawRect(heatRect);
        const QString plotMessage = result.status.trimmed().isEmpty()
                                        ? uiText(QStringLiteral("gnss_acq_plot_waiting"),
                                                 QStringLiteral("Run GPS C/A accumulation to draw correlation diagnostics."))
                                        : result.status.trimmed();
        painter.drawText(heatRect.adjusted(8, 8, -8, -8),
                         Qt::AlignCenter,
                         plotMessage);
    }

    painter.setPen(QColor(130, 142, 154));
    painter.drawRect(corrRect);
    if (!result.bestCorrelationProfileDb.isEmpty()) {
        QPolygonF polyline;
        polyline.reserve(result.bestCorrelationProfileDb.size());
        constexpr double minDb = -36.0;
        constexpr double maxDb = 0.0;
        for (int i = 0; i < result.bestCorrelationProfileDb.size(); ++i) {
            const double x = corrRect.left() +
                             (static_cast<double>(i) / (std::max)(1, result.bestCorrelationProfileDb.size() - 1)) *
                                 corrRect.width();
            const double clamped = (std::clamp)(static_cast<double>(result.bestCorrelationProfileDb.at(i)), minDb, maxDb);
            const double y = corrRect.bottom() -
                             ((clamped - minDb) / (maxDb - minDb)) * corrRect.height();
            polyline << QPointF(x, y);
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(250, 205, 80), 1.5));
        painter.drawPolyline(polyline);
        painter.setRenderHint(QPainter::Antialiasing, false);
        if (!result.topCandidates.isEmpty()) {
            const GnssAcquisitionCandidate best = result.topCandidates.first();
            const int x = corrRect.left() +
                          (best.codePhaseSamples * corrRect.width()) /
                              (std::max)(1, result.bestCorrelationProfileDb.size());
            painter.setPen(QPen(QColor(255, 255, 255, 180), 1));
            painter.drawLine(x, corrRect.top(), x, corrRect.bottom());
        }
    }

    painter.setPen(QColor(130, 142, 154));
    painter.drawRect(histRect);
    if (!gnssPeakToSecondHistoryDb.isEmpty()) {
        QPolygonF polyline;
        const int n = gnssPeakToSecondHistoryDb.size();
        for (int i = 0; i < n; ++i) {
            const double value = (std::clamp)(gnssPeakToSecondHistoryDb.at(i), 0.0, 24.0);
            const double x = histRect.left() + (static_cast<double>(i) / (std::max)(1, n - 1)) * histRect.width();
            const double y = histRect.bottom() - (value / 24.0) * histRect.height();
            polyline << QPointF(x, y);
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(125, 220, 135), 1.6));
        painter.drawPolyline(polyline);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QColor(170, 180, 190));
        painter.drawText(histRect.left(), histRect.top() + 12, QStringLiteral("24 dB"));
        painter.drawText(histRect.left(), histRect.bottom() - 2, QStringLiteral("0"));
    }

    if (!result.topCandidates.isEmpty()) {
        const GnssAcquisitionCandidate best = result.topCandidates.first();
        const double metricDb = 10.0 * std::log10((std::max)(best.metric, std::numeric_limits<double>::min()));
        const double peakToSecondDb = 10.0 * std::log10((std::max)(best.peakToSecond, std::numeric_limits<double>::min()));
        const QString bestLabel = best.label.trimmed().isEmpty()
                                      ? QStringLiteral("%1 %2").arg(rowName).arg(best.prn)
                                      : best.label.trimmed();
        painter.setPen(QColor(225, 230, 235));
        painter.drawText(heatRect.left(), height - 8,
                         QStringLiteral("Best %1  Doppler %2 Hz  Code %3  Metric %4 dB  Peak2 %5 dB")
                             .arg(bestLabel)
                             .arg(best.dopplerHz, 0, 'f', 0)
                             .arg(best.codePhaseSamples)
                             .arg(metricDb, 0, 'f', 1)
                             .arg(peakToSecondDb, 0, 'f', 1));
    }

    gnssAcquisitionPlotLabel->setPixmap(pixmap);
}

void YourClassName::saveGnssAcquisitionArtifacts(const GnssAcquisitionResult &result,
                                                 const QString &sourceLabel) {
    if (result.cancelled) {
        return;
    }
    constexpr qint64 kContinuousArtifactSaveIntervalMs = 30000;
    const bool continuousSource = sourceLabel.startsWith(QStringLiteral("continuous"));
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (continuousSource &&
        gnssLastContinuousArtifactSaveMs > 0 &&
        nowMs - gnssLastContinuousArtifactSaveMs < kContinuousArtifactSaveIntervalMs) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[GNSS acquisition artifacts] skipped"
                     << "source" << sourceLabel
                     << "reason" << "continuous-rate-limit"
                     << "elapsedMs" << (nowMs - gnssLastContinuousArtifactSaveMs)
                     << "intervalMs" << kContinuousArtifactSaveIntervalMs;
        }
        return;
    }

    QDir baseDir(QCoreApplication::applicationDirPath());
    if (!baseDir.mkpath(QStringLiteral("recordings/gnss_reports"))) {
        qDebug() << "[GNSS acquisition artifacts] cannot create report directory"
                 << baseDir.filePath(QStringLiteral("recordings/gnss_reports"));
        return;
    }

    QDir reportDir(baseDir.filePath(QStringLiteral("recordings/gnss_reports")));
    QString safeSource = sourceLabel.trimmed();
    if (safeSource.isEmpty()) {
        safeSource = QStringLiteral("unknown");
    }
    safeSource.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")),
                       QStringLiteral("_"));

    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString baseName = QStringLiteral("gnss_acq_%1_%2").arg(stamp, safeSource);
    const QString reportName = baseName + QStringLiteral(".json");
    const QString heatmapName = baseName + QStringLiteral("_heatmap.csv");
    const QString profileName = baseName + QStringLiteral("_profile.csv");
    const QString reportPath = reportDir.filePath(reportName);
    const QString heatmapPath = reportDir.filePath(heatmapName);
    const QString profilePath = reportDir.filePath(profileName);

    auto finiteJson = [](double value) -> QJsonValue {
        return std::isfinite(value) ? QJsonValue(value) : QJsonValue();
    };
    auto metricDb = [](double value) -> double {
        return 10.0 * std::log10((std::max)(value, std::numeric_limits<double>::min()));
    };

    bool heatmapSaved = false;
    if (!result.dopplerBinsHz.isEmpty() && !result.prnDopplerMetricDb.isEmpty()) {
        QFile heatmapFile(heatmapPath);
        if (heatmapFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&heatmapFile);
            stream.setRealNumberNotation(QTextStream::FixedNotation);
            stream.setRealNumberPrecision(3);
            const QString rowColumn = result.heatmapRowLabel.trimmed().isEmpty()
                                          ? QStringLiteral("row")
                                          : result.heatmapRowLabel.trimmed().toLower();
            stream << rowColumn << ",doppler_hz,metric_db\n";
            const int dopplerCount = result.dopplerBinsHz.size();
            const int prnCount = result.prnDopplerMetricDb.size() / dopplerCount;
            for (int prnIndex = 0; prnIndex < prnCount; ++prnIndex) {
                for (int dopplerIndex = 0; dopplerIndex < dopplerCount; ++dopplerIndex) {
                    const int metricIndex = prnIndex * dopplerCount + dopplerIndex;
                    stream << (prnIndex + 1) << ','
                           << result.dopplerBinsHz.at(dopplerIndex) << ','
                           << result.prnDopplerMetricDb.at(metricIndex) << '\n';
                }
            }
            heatmapSaved = true;
        }
    }

    bool profileSaved = false;
    if (!result.bestCorrelationProfileDb.isEmpty()) {
        QFile profileFile(profilePath);
        if (profileFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&profileFile);
            stream.setRealNumberNotation(QTextStream::FixedNotation);
            stream.setRealNumberPrecision(3);
            stream << "code_phase_sample,relative_db\n";
            for (int index = 0; index < result.bestCorrelationProfileDb.size(); ++index) {
                stream << index << ',' << result.bestCorrelationProfileDb.at(index) << '\n';
            }
            profileSaved = true;
        }
    }

    QJsonObject report;
    report.insert(QStringLiteral("type"), QStringLiteral("gnss_acquisition_report"));
    report.insert(QStringLiteral("createdAtLocal"),
                  QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    report.insert(QStringLiteral("source"), safeSource);
    report.insert(QStringLiteral("systemId"), gnssSystemId);
    report.insert(QStringLiteral("systemName"), result.systemName);
    report.insert(QStringLiteral("heatmapRows"), result.heatmapRows);
    report.insert(QStringLiteral("heatmapRowLabel"), result.heatmapRowLabel);
    report.insert(QStringLiteral("valid"), result.valid);
    report.insert(QStringLiteral("status"), result.status);
    report.insert(QStringLiteral("inputSamples"), result.inputSamples);
    report.insert(QStringLiteral("usedInputSamples"), result.usedInputSamples);
    report.insert(QStringLiteral("requestedCoherentMs"), result.requestedCoherentMs);
    report.insert(QStringLiteral("coherentMs"), result.coherentMs);
    report.insert(QStringLiteral("processingElapsedMs"),
                  static_cast<double>(result.processingElapsedMs));
    report.insert(QStringLiteral("inputSampleRate"), finiteJson(result.inputSampleRate));
    report.insert(QStringLiteral("acquisitionSampleRate"), finiteJson(result.acquisitionSampleRate));
    report.insert(QStringLiteral("centerFrequency"), finiteJson(result.centerFrequency));
    report.insert(QStringLiteral("targetFrequency"), finiteJson(result.targetFrequency));
    report.insert(QStringLiteral("frequencyOffset"), finiteJson(result.frequencyOffset));
    report.insert(QStringLiteral("channelFilterCutoffHz"), finiteJson(result.channelFilterCutoffHz));
    report.insert(QStringLiteral("channelizerTaps"), result.channelizerTaps);
    report.insert(QStringLiteral("millisecondAgc"), result.millisecondAgc);
    report.insert(QStringLiteral("toneNotchesApplied"), result.toneNotchesApplied);
    report.insert(QStringLiteral("strongestToneNotchDb"), finiteJson(result.strongestToneNotchDb));

    QJsonObject qthObject;
    qthObject.insert(QStringLiteral("source"), qthSource);
    qthObject.insert(QStringLiteral("positionPrivacy"),
                     QStringLiteral("Exact latitude/longitude omitted from shared GNSS acquisition reports."));
    if (qth::isValidLatitude(qthLatitude) && qth::isValidLongitude(qthLongitude)) {
        qthObject.insert(QStringLiteral("locator"), qth::maidenheadLocator(qthLatitude, qthLongitude, 6));
    }
    report.insert(QStringLiteral("qth"), qthObject);

    QJsonObject settingsObject;
    settingsObject.insert(QStringLiteral("gnssAcquisitionIntegrationMs"), gnssAcquisitionIntegrationMs);
    settingsObject.insert(QStringLiteral("gnssChannelFilterCutoffHz"), finiteJson(gnssChannelFilterCutoffHz));
    settingsObject.insert(QStringLiteral("gnssDopplerSpanHz"), gnssDopplerSpanHz);
    settingsObject.insert(QStringLiteral("gnssDopplerStepHz"), gnssDopplerStepHz);
    settingsObject.insert(QStringLiteral("pendingCenterFrequency"), finiteJson(pendingSettings.centerFrequency));
    settingsObject.insert(QStringLiteral("pendingListeningFrequency"), finiteJson(pendingSettings.listeningFrequency));
    settingsObject.insert(QStringLiteral("pendingSampleRate"), finiteJson(pendingSettings.sampleRate));
    settingsObject.insert(QStringLiteral("pendingBandwidth"), finiteJson(pendingSettings.bandwidth));
    settingsObject.insert(QStringLiteral("inputMode"), pendingSettings.inputMode);
    settingsObject.insert(QStringLiteral("lnaGain"), pendingSettings.lnaGain);
    settingsObject.insert(QStringLiteral("vgaGain"), pendingSettings.vgaGain);
    settingsObject.insert(QStringLiteral("rtlAgc"), pendingSettings.rtlAgc);
    settingsObject.insert(QStringLiteral("rtlTunerGainTenthsDb"), pendingSettings.rtlTunerGainTenthsDb);
    report.insert(QStringLiteral("settings"), settingsObject);

    QJsonArray dopplerBins;
    for (double dopplerHz : result.dopplerBinsHz) {
        dopplerBins.append(finiteJson(dopplerHz));
    }
    report.insert(QStringLiteral("dopplerBinsHz"), dopplerBins);

    QJsonArray candidates;
    bool likelyLock = false;
    if (!result.topCandidates.isEmpty()) {
        const GnssAcquisitionCandidate best = result.topCandidates.first();
        const double bestMetricDb = metricDb(best.metric);
        const double bestPeakToSecondDb = metricDb(best.peakToSecond);
        likelyLock = result.coherentMs >= 8 &&
                     bestMetricDb >= 10.0 &&
                     bestPeakToSecondDb >= 6.0;
    }
    report.insert(QStringLiteral("likelyLock"), likelyLock);
    for (const GnssAcquisitionCandidate &candidate : result.topCandidates) {
        QJsonObject candidateObject;
        candidateObject.insert(QStringLiteral("prn"), candidate.prn);
        candidateObject.insert(QStringLiteral("label"), candidate.label);
        candidateObject.insert(QStringLiteral("dopplerHz"), finiteJson(candidate.dopplerHz));
        candidateObject.insert(QStringLiteral("targetFrequency"), finiteJson(candidate.targetFrequency));
        candidateObject.insert(QStringLiteral("codePhaseSamples"), candidate.codePhaseSamples);
        candidateObject.insert(QStringLiteral("metric"), finiteJson(candidate.metric));
        candidateObject.insert(QStringLiteral("metricDb"), finiteJson(metricDb(candidate.metric)));
        candidateObject.insert(QStringLiteral("peak"), finiteJson(candidate.peak));
        candidateObject.insert(QStringLiteral("secondPeak"), finiteJson(candidate.secondPeak));
        candidateObject.insert(QStringLiteral("peakToSecond"), finiteJson(candidate.peakToSecond));
        candidateObject.insert(QStringLiteral("peakToSecondDb"), finiteJson(metricDb(candidate.peakToSecond)));
        candidateObject.insert(QStringLiteral("average"), finiteJson(candidate.average));
        candidates.append(candidateObject);
    }
    report.insert(QStringLiteral("topCandidates"), candidates);

    QJsonObject promptProbe;
    promptProbe.insert(QStringLiteral("milliseconds"), result.promptI.size());
    promptProbe.insert(QStringLiteral("bitGroupMs"), 20);
    promptProbe.insert(QStringLiteral("residualDopplerHz"), finiteJson(result.promptResidualDopplerHz));
    promptProbe.insert(QStringLiteral("trackingApplied"), result.trackingApplied);
    promptProbe.insert(QStringLiteral("trackingMs"), result.trackingMs);
    promptProbe.insert(QStringLiteral("trackingStartCodePhase"), result.trackingStartCodePhase);
    promptProbe.insert(QStringLiteral("trackingEndCodePhase"), result.trackingEndCodePhase);
    promptProbe.insert(QStringLiteral("trackingCarrierPhaseStepRad"),
                       finiteJson(result.trackingCarrierPhaseStepRad));
    promptProbe.insert(QStringLiteral("trackingCarrierResidualHz"),
                       finiteJson(result.trackingCarrierResidualHz));
    promptProbe.insert(QStringLiteral("trackingAveragePromptMagnitude"),
                       finiteJson(result.trackingAveragePromptMagnitude));
    promptProbe.insert(QStringLiteral("gpsLnavPreambleFound"), result.gpsLnavPreambleFound);
    promptProbe.insert(QStringLiteral("gpsLnavPreambleBitOffset"), result.gpsLnavPreambleBitOffset);
    promptProbe.insert(QStringLiteral("gpsLnavPreamblePolarity"), result.gpsLnavPreamblePolarity);
    QJsonArray promptI;
    QJsonArray promptQ;
    QJsonArray promptMagnitude;
    QJsonArray promptSigns1Ms;
    for (int index = 0; index < result.promptI.size(); ++index) {
        promptI.append(finiteJson(result.promptI.at(index)));
        promptQ.append(index < result.promptQ.size() ? finiteJson(result.promptQ.at(index)) : QJsonValue());
        promptMagnitude.append(index < result.promptMagnitude.size()
                                   ? finiteJson(result.promptMagnitude.at(index))
                                   : QJsonValue());
        promptSigns1Ms.append(index < result.promptSigns1Ms.size()
                                  ? result.promptSigns1Ms.at(index)
                                  : QJsonValue());
    }
    QJsonArray promptBits;
    for (int bit : result.promptBitSigns20Ms) {
        promptBits.append(bit);
    }
    promptProbe.insert(QStringLiteral("promptI"), promptI);
    promptProbe.insert(QStringLiteral("promptQ"), promptQ);
    promptProbe.insert(QStringLiteral("promptMagnitude"), promptMagnitude);
    promptProbe.insert(QStringLiteral("signs1Ms"), promptSigns1Ms);
    promptProbe.insert(QStringLiteral("bitSigns20Ms"), promptBits);
    report.insert(QStringLiteral("promptProbe"), promptProbe);

    QJsonObject artifactsObject;
    artifactsObject.insert(QStringLiteral("reportJson"), reportName);
    if (heatmapSaved) {
        artifactsObject.insert(QStringLiteral("heatmapCsv"), heatmapName);
    }
    if (profileSaved) {
        artifactsObject.insert(QStringLiteral("correlationProfileCsv"), profileName);
    }
    report.insert(QStringLiteral("artifacts"), artifactsObject);

    QFile reportFile(reportPath);
    if (!reportFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "[GNSS acquisition artifacts] cannot write report" << reportPath;
        return;
    }
    reportFile.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    reportFile.close();
    if (continuousSource) {
        gnssLastContinuousArtifactSaveMs = nowMs;
    }

    qDebug() << "[GNSS acquisition artifacts] saved"
             << "source" << safeSource
             << "report" << reportPath
             << "heatmap" << (heatmapSaved ? heatmapPath : QStringLiteral("none"))
             << "profile" << (profileSaved ? profilePath : QStringLiteral("none"));
}

void YourClassName::updateGnssAcquisitionStatus(const GnssAcquisitionResult &result) {
    if (result.cancelled) {
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(uiText(QStringLiteral("gps_ca_scan_cancelled"),
                                                  QStringLiteral("GPS C/A accumulation: cancelled")));
        }
        qDebug() << "[GNSS acquisition] cancelled"
                 << "inputSamples" << result.inputSamples
                 << "usedInputSamples" << result.usedInputSamples
                 << "coherentMs" << result.coherentMs
                 << "processingMs" << result.processingElapsedMs;
        scheduleGnssContinuousAcquisition();
        return;
    }

    updateGnssAcquisitionPlot(result);
    saveGnssAcquisitionArtifacts(result, gnssAcquisitionSource);

    if (!result.valid || result.topCandidates.isEmpty()) {
        if (gnssAcquireStatusLabel) {
            gnssAcquireStatusLabel->setText(result.status.isEmpty()
                                               ? uiText(QStringLiteral("gps_ca_scan_no_candidate"),
                                                        QStringLiteral("GPS C/A accumulation: no candidates"))
                                               : result.status);
        }
        qDebug() << "[GNSS acquisition] finished"
                 << "valid" << result.valid
                 << "status" << result.status
                 << "inputSamples" << result.inputSamples
                 << "sampleRate" << result.inputSampleRate
                 << "processingMs" << result.processingElapsedMs;
        scheduleGnssContinuousAcquisition();
        return;
    }

    const GnssAcquisitionCandidate best = result.topCandidates.first();
    const double metricDb = 10.0 * std::log10((std::max)(best.metric, std::numeric_limits<double>::min()));
    const double peakToSecondDb =
        10.0 * std::log10((std::max)(best.peakToSecond, std::numeric_limits<double>::min()));
    gnssPeakToSecondHistoryDb.push_back(peakToSecondDb);
    while (gnssPeakToSecondHistoryDb.size() > 48) {
        gnssPeakToSecondHistoryDb.pop_front();
    }
    updateGnssAcquisitionPlot(result);
    constexpr double kGpsCaCoarseLockMetricDb = 10.0;
    constexpr double kGpsCaCoarseLockPeakToSecondDb = 6.0;
    constexpr double kGpsCaFocusedLockMetricDb = 4.5;
    constexpr double kGpsCaFocusedLockPeakToSecondDb = 3.0;
    constexpr int kGpsCaLikelyLockMinMs = 8;
    const bool focusedPromptProbe = result.coherentMs >= 80 && result.promptBitSigns20Ms.size() >= 4;
    const double lockMetricThresholdDb =
        focusedPromptProbe ? kGpsCaFocusedLockMetricDb : kGpsCaCoarseLockMetricDb;
    const double lockPeakToSecondThresholdDb =
        focusedPromptProbe ? kGpsCaFocusedLockPeakToSecondDb : kGpsCaCoarseLockPeakToSecondDb;
    const bool likelyLock = result.coherentMs >= kGpsCaLikelyLockMinMs &&
                            metricDb >= lockMetricThresholdDb &&
                            peakToSecondDb >= lockPeakToSecondThresholdDb;
    QString lnavHistoryText;
    const bool gpsCandidate =
        best.label.startsWith(QLatin1Char('G')) ||
        result.systemName.contains(QStringLiteral("GPS"), Qt::CaseInsensitive);
    if (likelyLock && gpsCandidate && !result.promptBitSigns20Ms.isEmpty()) {
        const QString prnKey = best.label.isEmpty()
                                   ? QStringLiteral("G%1").arg(best.prn, 2, 10, QLatin1Char('0'))
                                   : best.label;
        gnssGpsFocusedPrn = best.prn;
        gnssGpsFocusedDopplerHz = best.dopplerHz;
        gnssGpsFocusedWeakCount = 0;
        gnssGpsFocusedStableCount = (std::min)(gnssGpsFocusedStableCount + 1, 12);
        QVector<int> &bitHistory = gnssGpsLnavBitHistory[prnKey];
        QVector<int> &segmentHistory = gnssGpsLnavSegmentHistory[prnKey];
        int &nextSegmentId = gnssGpsLnavNextSegmentId[prnKey];
        std::uint64_t &lastSequence = gnssGpsLnavLastSequence[prnKey];
        qint64 &lastUpdateMs = gnssGpsLnavLastUpdateMs[prnKey];
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 elapsedSinceLastMs = lastUpdateMs > 0 ? nowMs - lastUpdateMs : 0;
        const bool sequenceWentBackwards =
            lastSequence != 0 && result.inputSequence > 0 && result.inputSequence < lastSequence;
        const int segmentDurationMs = result.promptBitSigns20Ms.size() * 20;
        const int allowedWallGapMs =
            (std::max)(500,
                       segmentDurationMs +
                           static_cast<int>((std::max)(result.processingElapsedMs, qint64(0))) +
                           1000);
        const bool wallGapSkipped =
            lastUpdateMs > 0 &&
            elapsedSinceLastMs > allowedWallGapMs &&
            result.promptBitSigns20Ms.size() < 30;
        if (!bitHistory.isEmpty() && (sequenceWentBackwards || wallGapSkipped)) {
            qDebug() << "[GNSS LNAV history] reset"
                     << "prn" << prnKey
                     << "reason" << "non-contiguous-window"
                     << "lastSequence" << static_cast<qulonglong>(lastSequence)
                     << "sequence" << static_cast<qulonglong>(result.inputSequence)
                     << "sequenceWentBackwards" << sequenceWentBackwards
                     << "wallGapSkipped" << wallGapSkipped
                     << "elapsedMs" << elapsedSinceLastMs
                     << "allowedGapMs" << allowedWallGapMs
                     << "segmentBits" << result.promptBitSigns20Ms.size()
                     << "snapshotMs" << result.snapshotMs
                     << "processingMs" << result.processingElapsedMs;
            bitHistory.clear();
            segmentHistory.clear();
            nextSegmentId = 0;
        }
        const int segmentId = nextSegmentId++;
        for (int bit : result.promptBitSigns20Ms) {
            bitHistory.push_back(bit ? 1 : 0);
            segmentHistory.push_back(segmentId);
        }
        lastSequence = result.inputSequence;
        lastUpdateMs = nowMs;
        constexpr int kMaxLnavHistoryBits = 512;
        while (bitHistory.size() > kMaxLnavHistoryBits) {
            bitHistory.pop_front();
            segmentHistory.pop_front();
        }

        const GpsLnavHistorySearchResult historySearch =
            searchGpsLnavPreambleWithSegmentPolarity(bitHistory, segmentHistory);
        QVector<int> normalizedLnavWords;
        GpsLnavDecodedSummary lnavDecoded;
        if (historySearch.found && historySearch.parityValidWords >= 2) {
            normalizedLnavWords =
                normalizeGpsLnavBits(bitHistory,
                                     segmentHistory,
                                     historySearch,
                                     historySearch.parityAvailableWords * 30);
            lnavDecoded = decodeGpsLnavTlmHow(normalizedLnavWords, historySearch);
            if (lnavDecoded.valid) {
                lnavHistoryText = uiText(QStringLiteral("gps_lnav_tlm_how_confirmed"),
                                         QStringLiteral("LNAV confirmed: %1/%2 words, TOW %3 s, subframe %4"))
                                      .arg(historySearch.parityValidWords)
                                      .arg(historySearch.parityAvailableWords)
                                      .arg(lnavDecoded.howTowSeconds)
                                      .arg(lnavDecoded.howSubframeId);
            } else {
                lnavHistoryText = uiText(QStringLiteral("gps_lnav_parity_confirmed"),
                                         QStringLiteral("LNAV parity confirmed: %1/%2 words, offset %3"))
                                      .arg(historySearch.parityValidWords)
                                      .arg(historySearch.parityAvailableWords)
                                      .arg(historySearch.offset);
            }
        } else if (historySearch.found) {
            lnavHistoryText = uiText(QStringLiteral("gps_lnav_history_found"),
                                     QStringLiteral("LNAV preamble candidate: %1 bits, offset %2, parity %3/%4"))
                                  .arg(bitHistory.size())
                                  .arg(historySearch.offset)
                                  .arg(historySearch.parityValidWords)
                                  .arg(historySearch.parityAvailableWords);
        } else {
            lnavHistoryText = uiText(QStringLiteral("gps_lnav_history_waiting"),
                                     QStringLiteral("LNAV history: %1 bits, waiting for preamble"))
                                  .arg(bitHistory.size());
        }

        qDebug() << "[GNSS LNAV history]"
                 << "prn" << prnKey
                 << "segmentId" << segmentId
                 << "sequence" << static_cast<qulonglong>(result.inputSequence)
                 << "snapshotMs" << result.snapshotMs
                 << "elapsedSinceLastMs" << elapsedSinceLastMs
                 << "segmentBits" << bitsToString(result.promptBitSigns20Ms, 32)
                 << "historyBits" << bitHistory.size()
                 << "historyPreview" << bitsToString(bitHistory)
                 << "preambleFound" << historySearch.found
                 << "preambleOffset" << historySearch.offset
                 << "polarityMask" << historySearch.polarityMask
                 << "polarityTransitions" << historySearch.polarityTransitions
                 << "polarityInvertedSegments" << historySearch.polarityInvertedSegments
                 << "segments" << historySearch.segmentIds
                 << "parityAvailableWords" << historySearch.parityAvailableWords
                 << "parityValidWords" << historySearch.parityValidWords
                 << "parityInitialPrevD29" << historySearch.parityInitialPrevD29
                 << "parityInitialPrevD30" << historySearch.parityInitialPrevD30
                 << "parityD30DeWhiten" << historySearch.parityUsesD30DeWhitening
                 << "tlmHowCandidateValid" << historySearch.tlmHowValid
                 << "tlmHowDecoded" << lnavDecoded.valid
                 << "tlmPreamble" << QStringLiteral("0x%1").arg(lnavDecoded.tlmPreamble, 2, 16, QLatin1Char('0'))
                 << "tlmMessage" << lnavDecoded.tlmMessage
                 << "tlmIntegrity" << lnavDecoded.tlmIntegrityStatus
                 << "howTowCount" << lnavDecoded.howTowCount
                 << "howTowSeconds" << lnavDecoded.howTowSeconds
                 << "howSubframeId" << lnavDecoded.howSubframeId
                 << "howAlert" << lnavDecoded.howAlert
                 << "howAntiSpoof" << lnavDecoded.howAntiSpoof
                 << "metricDb" << metricDb
                 << "peakToSecondDb" << peakToSecondDb;
    } else if (gpsCandidate && gnssGpsFocusedPrn > 0) {
        ++gnssGpsFocusedWeakCount;
        if (gnssGpsFocusedWeakCount >= 3) {
            qDebug() << "[GNSS acquisition] focused PRN released"
                     << "prn" << gnssGpsFocusedPrn
                     << "weakCount" << gnssGpsFocusedWeakCount
                     << "metricDb" << metricDb
                     << "peakToSecondDb" << peakToSecondDb;
            gnssGpsFocusedPrn = 0;
            gnssGpsFocusedDopplerHz = 0.0;
            gnssGpsFocusedWeakCount = 0;
            gnssGpsFocusedStableCount = 0;
        }
    } else if (!likelyLock) {
        gnssGpsFocusedStableCount = 0;
    }

    if (!likelyLock && !gnssGpsLnavBitHistory.isEmpty()) {
        qDebug() << "[GNSS LNAV history] not updated"
                 << "reason" << "weak-lock"
                 << "bestLabel" << best.label
                 << "metricDb" << metricDb
                 << "peakToSecondDb" << peakToSecondDb
                 << "trackedPrns" << gnssGpsLnavBitHistory.keys();
    }

    const QString statusTemplate = likelyLock
        ? uiText(QStringLiteral("gps_ca_scan_result"),
                 QStringLiteral("GPS C/A: PRN %1, metric %2 dB, doppler %3 Hz, code %4, %5 ms accumulated"))
        : uiText(QStringLiteral("gps_ca_scan_weak"),
                 QStringLiteral("GPS C/A: weak/no lock, best PRN %1, metric %2 dB, doppler %3 Hz, code %4, %5 ms accumulated"));
    QString statusText = QString(statusTemplate)
                             .arg(best.prn)
                             .arg(QString::number(metricDb, 'f', 1))
                             .arg(QString::number(best.dopplerHz, 'f', 0))
                             .arg(best.codePhaseSamples)
                             .arg(result.coherentMs);
    if (result.requestedCoherentMs > result.coherentMs) {
        statusText.append(QStringLiteral(" | "))
            .append(uiText(QStringLiteral("gnss_snapshot_limited"),
                           QStringLiteral("snapshot limited: requested %1 ms"))
                        .arg(result.requestedCoherentMs));
    }
    if (!lnavHistoryText.isEmpty()) {
        statusText.append(QStringLiteral(" | ")).append(lnavHistoryText);
    }
    if (gnssAcquireStatusLabel) {
        gnssAcquireStatusLabel->setText(statusText);
    }

    qDebug() << "[GNSS acquisition] finished"
             << "inputSamples" << result.inputSamples
             << "usedInputSamples" << result.usedInputSamples
             << "inputSampleRate" << result.inputSampleRate
             << "acquisitionSampleRate" << result.acquisitionSampleRate
             << "channelFilterCutoffHz" << result.channelFilterCutoffHz
             << "channelizerTaps" << result.channelizerTaps
             << "millisecondAgc" << result.millisecondAgc
             << "toneNotches" << result.toneNotchesApplied
             << "strongestToneNotchDb" << result.strongestToneNotchDb
             << "processingMs" << result.processingElapsedMs
             << "centerHz" << result.centerFrequency
             << "targetHz" << result.targetFrequency
             << "offsetHz" << result.frequencyOffset
             << "requestedCoherentMs" << result.requestedCoherentMs
             << "coherentMs" << result.coherentMs
             << "snapshotLimited" << (result.requestedCoherentMs > result.coherentMs)
             << "likelyLock" << likelyLock
             << "lockThresholdDb" << lockMetricThresholdDb
             << "lockPeakToSecondThresholdDb" << lockPeakToSecondThresholdDb
             << "focusedPromptProbe" << focusedPromptProbe
             << "focusedStableCount" << gnssGpsFocusedStableCount
             << "lockMinCoherentMs" << kGpsCaLikelyLockMinMs
             << "bestPrn" << best.prn
             << "bestMetricDb" << metricDb
             << "bestPeakToSecondDb" << peakToSecondDb
             << "bestDopplerHz" << best.dopplerHz
             << "bestCodePhase" << best.codePhaseSamples
             << "promptMs" << result.promptI.size()
             << "trackingApplied" << result.trackingApplied
             << "trackingMs" << result.trackingMs
             << "trackingCodePhaseStart" << result.trackingStartCodePhase
             << "trackingCodePhaseEnd" << result.trackingEndCodePhase
             << "trackingCarrierResidualHz" << result.trackingCarrierResidualHz
             << "trackingAveragePromptMagnitude" << result.trackingAveragePromptMagnitude
             << "promptResidualDopplerHz" << result.promptResidualDopplerHz
             << "promptSigns1MsPreview" << bitsToString(result.promptSigns1Ms, 96)
             << "promptBits20Ms" << bitsToString(result.promptBitSigns20Ms, 64)
             << "gpsLnavPreambleFound" << result.gpsLnavPreambleFound
             << "gpsLnavPreambleBitOffset" << result.gpsLnavPreambleBitOffset
             << "gpsLnavPreamblePolarity" << result.gpsLnavPreamblePolarity;

    const int candidateLogCount = (std::min)(result.topCandidates.size(), 3);
    for (int candidateIndex = 0; candidateIndex < candidateLogCount; ++candidateIndex) {
        const GnssAcquisitionCandidate &candidate = result.topCandidates.at(candidateIndex);
        const double candidateMetricDb =
            10.0 * std::log10((std::max)(candidate.metric, std::numeric_limits<double>::min()));
        const double candidatePeakToSecondDb =
            10.0 * std::log10((std::max)(candidate.peakToSecond, std::numeric_limits<double>::min()));
        qDebug() << "[GNSS acquisition candidate]"
                 << "rank" << (candidateIndex + 1)
                 << "candidateCount" << result.topCandidates.size()
                 << "prn" << candidate.prn
                 << "label" << candidate.label
                 << "metricDb" << candidateMetricDb
                 << "peakToSecondDb" << candidatePeakToSecondDb
                 << "dopplerHz" << candidate.dopplerHz
                 << "targetHz" << candidate.targetFrequency
                 << "codePhase" << candidate.codePhaseSamples
                 << "peak" << candidate.peak
                 << "secondPeak" << candidate.secondPeak
                 << "average" << candidate.average;
    }

    if (gnssContinuousAcquisitionEnabled && likelyLock && gpsCandidate && gnssGpsFocusedPrn > 0) {
        scheduleGnssContinuousAcquisition(GNSS_CONTINUOUS_ACQUISITION_FIRST_DELAY_MS);
    } else {
        scheduleGnssContinuousAcquisition();
    }
}
