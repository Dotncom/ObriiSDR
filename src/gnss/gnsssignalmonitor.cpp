#include "gnsssignalmonitor.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kFloorDb = -160.0;

double safeLog10(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return kFloorDb;
    }
    return std::log10(value);
}

double amplitudeDb(double value) {
    return (std::max)(kFloorDb, 20.0 * safeLog10(value));
}

double ratioDb(double numerator, double denominator) {
    if (!std::isfinite(numerator) ||
        !std::isfinite(denominator) ||
        numerator <= 0.0 ||
        denominator <= 0.0) {
        return kFloorDb;
    }
    return (std::max)(kFloorDb, 20.0 * std::log10(numerator / denominator));
}

} // namespace

void GnssSignalMonitor::reset() {
    last = GnssSignalReport();
    lastSequence = 0;
}

GnssSignalReport GnssSignalMonitor::analyzeFloatSnapshot(const std::vector<float> &samples,
                                                         double sampleRate,
                                                         const RadioSettings &settings,
                                                         std::uint64_t sequence) {
    if (sequence != 0 && sequence == lastSequence) {
        return last;
    }
    lastSequence = sequence;
    const int sampleCount = static_cast<int>(samples.size() / 2);
    return analyze(sampleCount,
                   sampleRate,
                   false,
                   true,
                   QStringLiteral("float snapshot"),
                   settings,
                   [&samples](int index, double *iSample, double *qSample, bool *clipped) {
                       const int offset = index * 2;
                       const double i = static_cast<double>(samples[static_cast<std::size_t>(offset)]);
                       const double q = static_cast<double>(samples[static_cast<std::size_t>(offset + 1)]);
                       *iSample = std::isfinite(i) ? i : 0.0;
                       *qSample = std::isfinite(q) ? q : 0.0;
                       *clipped = std::abs(*iSample) >= 0.999 || std::abs(*qSample) >= 0.999;
                   });
}

GnssSignalReport GnssSignalMonitor::analyzePackedIqFrame(const QByteArray &iqData,
                                                         double sampleRate,
                                                         int sampleCount,
                                                         bool channelized,
                                                         const RadioSettings &settings) {
    if (iqData.isEmpty() || sampleRate <= 0.0) {
        return last;
    }

    const int declaredSamples = sampleCount > 0 ? sampleCount : 0;
    const bool int16Iq = declaredSamples > 0 && iqData.size() >= declaredSamples * 4;
    if (int16Iq) {
        const int usableSamples = (std::min)(declaredSamples, iqData.size() / 4);
        const auto *src = reinterpret_cast<const uchar *>(iqData.constData());
        return analyze(usableSamples,
                       sampleRate,
                       channelized,
                       false,
                       QStringLiteral("s16le IQ"),
                       settings,
                       [src](int index, double *iSample, double *qSample, bool *clipped) {
                           const int offset = index * 4;
                           const qint16 rawI = static_cast<qint16>(src[offset] | (src[offset + 1] << 8));
                           const qint16 rawQ = static_cast<qint16>(src[offset + 2] | (src[offset + 3] << 8));
                           *iSample = static_cast<double>(rawI) / 32768.0;
                           *qSample = static_cast<double>(rawQ) / 32768.0;
                           *clipped = std::abs(static_cast<int>(rawI)) >= 32700 ||
                                      std::abs(static_cast<int>(rawQ)) >= 32700;
                       });
    }

    const int usableSamples = iqData.size() / 2;
    const auto *src = reinterpret_cast<const signed char *>(iqData.constData());
    return analyze(usableSamples,
                   sampleRate,
                   channelized,
                   false,
                   QStringLiteral("s8 IQ"),
                   settings,
                   [src](int index, double *iSample, double *qSample, bool *clipped) {
                       const int offset = index * 2;
                       const int rawI = static_cast<int>(src[offset]);
                       const int rawQ = static_cast<int>(src[offset + 1]);
                       *iSample = static_cast<double>(rawI) / 128.0;
                       *qSample = static_cast<double>(rawQ) / 128.0;
                       *clipped = std::abs(rawI) >= 126 || std::abs(rawQ) >= 126;
                   });
}

template <typename Reader>
GnssSignalReport GnssSignalMonitor::analyze(int sampleCount,
                                            double sampleRate,
                                            bool channelized,
                                            bool fromSnapshot,
                                            const QString &sampleFormat,
                                            const RadioSettings &settings,
                                            Reader reader) {
    if (sampleCount <= 0 || sampleRate <= 0.0 || !std::isfinite(sampleRate)) {
        return last;
    }

    double sumI = 0.0;
    double sumQ = 0.0;
    double sumI2 = 0.0;
    double sumQ2 = 0.0;
    double maxPower = 0.0;
    int clipped = 0;

    for (int i = 0; i < sampleCount; ++i) {
        double iSample = 0.0;
        double qSample = 0.0;
        bool clippedSample = false;
        reader(i, &iSample, &qSample, &clippedSample);
        sumI += iSample;
        sumQ += qSample;
        const double i2 = iSample * iSample;
        const double q2 = qSample * qSample;
        sumI2 += i2;
        sumQ2 += q2;
        maxPower = (std::max)(maxPower, i2 + q2);
        if (clippedSample) {
            ++clipped;
        }
    }

    const double invCount = 1.0 / static_cast<double>(sampleCount);
    const double meanI = sumI * invCount;
    const double meanQ = sumQ * invCount;
    const double rmsI = std::sqrt((std::max)(0.0, sumI2 * invCount));
    const double rmsQ = std::sqrt((std::max)(0.0, sumQ2 * invCount));
    const double componentRms = std::sqrt((std::max)(0.0, (sumI2 + sumQ2) * invCount * 0.5));
    const double complexRms = std::sqrt((std::max)(0.0, (sumI2 + sumQ2) * invCount));
    const double dcMagnitude = std::sqrt(meanI * meanI + meanQ * meanQ);
    const double peakMagnitude = std::sqrt((std::max)(0.0, maxPower));

    GnssSignalReport report;
    report.valid = true;
    report.channelized = channelized;
    report.fromSnapshot = fromSnapshot;
    report.sampleFormat = sampleFormat;
    report.frames = last.frames + 1;
    report.samples = last.samples + static_cast<quint64>(sampleCount);
    report.seconds = static_cast<double>(report.samples) / sampleRate;
    report.sampleRate = sampleRate;
    report.centerFrequency = settings.centerFrequency;
    report.listeningFrequency = settings.listeningFrequency;
    report.sampleCount = sampleCount;
    report.rmsDbfs = amplitudeDb(componentRms);
    report.peakDbfs = amplitudeDb(peakMagnitude);
    report.dcDbfs = amplitudeDb(dcMagnitude);
    report.dcToRmsDb = ratioDb(dcMagnitude, complexRms);
    report.clippingPercent = 100.0 * static_cast<double>(clipped) * invCount;
    report.crestDb = ratioDb(peakMagnitude, complexRms);
    report.iqBalanceDb = ratioDb(rmsI, rmsQ);
    report.meanI = meanI;
    report.meanQ = meanQ;
    last = report;
    return last;
}
