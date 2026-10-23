#include "receivernoisebaseline.h"

#include <QDataStream>
#include <QDateTime>
#include <QIODevice>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr quint32 kMagic = 0x4f4e424cu; // ONBL
constexpr quint32 kVersion = 1u;
constexpr int kMaximumProfiles = 16;
constexpr int kMaximumStoredBins = 32768;
constexpr int kMaximumSerializedBins = 1048576;
constexpr double kMinimumPower = 1.0e-30;

bool sameReal(double left, double right, double tolerance) {
    return std::isfinite(left) && std::isfinite(right) && std::abs(left - right) <= tolerance;
}

bool sameReceiverContext(const ReceiverCalibrationContext &left,
                         const ReceiverCalibrationContext &right) {
    return left.backendId == right.backendId &&
           left.inputMode == right.inputMode &&
           sameReal(left.sampleRateHz, right.sampleRateHz, 0.5) &&
           left.lnaGain == right.lnaGain &&
           left.vgaGain == right.vgaGain &&
           left.rtlGainTenthsDb == right.rtlGainTenthsDb &&
           left.gainState == right.gainState;
}

void writeKey(QDataStream &out, const ReceiverNoiseBaselineKey &key) {
    out << key.receiver.backendId
        << qint32(key.receiver.inputMode)
        << key.receiver.sampleRateHz
        << qint32(key.receiver.lnaGain)
        << qint32(key.receiver.vgaGain)
        << qint32(key.receiver.rtlGainTenthsDb)
        << qint32(key.receiver.gainState)
        << key.centerFrequencyHz
        << qint32(key.fftLength)
        << qint32(key.fftWindowType)
        << qint32(key.rfBandwidthHz)
        << key.amplitudeOffsetDb
        << key.calibrationTableEnabled
        << qint32(key.calibrationPointCount);
}

void readKey(QDataStream &in, ReceiverNoiseBaselineKey &key) {
    qint32 inputMode = -1;
    qint32 lnaGain = -1;
    qint32 vgaGain = -1;
    qint32 rtlGain = -1;
    qint32 gainState = -1;
    qint32 fftLength = 0;
    qint32 fftWindowType = 0;
    qint32 rfBandwidthHz = 0;
    qint32 calibrationPointCount = 0;
    in >> key.receiver.backendId
       >> inputMode
       >> key.receiver.sampleRateHz
       >> lnaGain
       >> vgaGain
       >> rtlGain
       >> gainState
       >> key.centerFrequencyHz
       >> fftLength
       >> fftWindowType
       >> rfBandwidthHz
       >> key.amplitudeOffsetDb
       >> key.calibrationTableEnabled
       >> calibrationPointCount;
    key.receiver.inputMode = inputMode;
    key.receiver.lnaGain = lnaGain;
    key.receiver.vgaGain = vgaGain;
    key.receiver.rtlGainTenthsDb = rtlGain;
    key.receiver.gainState = gainState;
    key.fftLength = fftLength;
    key.fftWindowType = fftWindowType;
    key.rfBandwidthHz = rfBandwidthHz;
    key.calibrationPointCount = calibrationPointCount;
}

double reducedFramePower(const std::vector<float> &levelsDb,
                         std::size_t begin,
                         std::size_t end) {
    double sum = 0.0;
    int count = 0;
    constexpr std::size_t kMaximumSamplesPerStoredBin = 16;
    const std::size_t span = end > begin ? end - begin : 0;
    const std::size_t step = span > kMaximumSamplesPerStoredBin
                                 ? span / kMaximumSamplesPerStoredBin
                                 : 1;
    for (std::size_t index = begin; index < end; index += step) {
        const double levelDb = levelsDb[index];
        if (!std::isfinite(levelDb)) {
            continue;
        }
        sum += std::pow(10.0, levelDb / 10.0);
        ++count;
    }
    return count > 0 ? (std::max)(kMinimumPower, sum / count) : kMinimumPower;
}

} // namespace

bool ReceiverNoiseBaselineKey::matches(const ReceiverNoiseBaselineKey &other) const {
    const double binWidthHz = receiver.sampleRateHz > 0.0 && fftLength > 0
                                  ? receiver.sampleRateHz / fftLength
                                  : 1.0;
    return sameReceiverContext(receiver, other.receiver) &&
           sameReal(centerFrequencyHz, other.centerFrequencyHz, (std::max)(1.0, binWidthHz * 0.25)) &&
           fftLength == other.fftLength &&
           fftWindowType == other.fftWindowType &&
           rfBandwidthHz == other.rfBandwidthHz &&
           sameReal(amplitudeOffsetDb, other.amplitudeOffsetDb, 0.001) &&
           calibrationTableEnabled == other.calibrationTableEnabled &&
           calibrationPointCount == other.calibrationPointCount;
}

QString ReceiverNoiseBaselineKey::description() const {
    QStringList parts;
    parts << receiver.description();
    parts << QStringLiteral("Fc=%1 MHz").arg(centerFrequencyHz / 1.0e6, 0, 'f', 6);
    parts << QStringLiteral("FFT=%1").arg(fftLength);
    parts << QStringLiteral("Window=%1").arg(fftWindowType);
    if (rfBandwidthHz > 0) {
        parts << QStringLiteral("RF BW=%1 MHz").arg(rfBandwidthHz / 1.0e6, 0, 'g', 6);
    }
    return parts.join(QStringLiteral(" / "));
}

bool ReceiverNoiseBaselineProfile::isValid() const {
    if (!(capturedFrames > 0 &&
           averagePower.size() >= 2 &&
           averagePower.size() <= kMaximumSerializedBins &&
           std::isfinite(minimumFrequencyHz) &&
           std::isfinite(maximumFrequencyHz) &&
           maximumFrequencyHz > minimumFrequencyHz)) {
        return false;
    }
    for (float power : averagePower) {
        if (!std::isfinite(power) || power <= 0.0f) {
            return false;
        }
    }
    return true;
}

void ReceiverNoiseBaseline::beginCapture(const ReceiverNoiseBaselineKey &key, int targetFrames) {
    captureKey = key;
    captureFrames = 0;
    captureTargetFrames = (std::clamp)(targetFrames, 8, 512);
    captureMinimumFrequencyHz = 0.0;
    captureMaximumFrequencyHz = 0.0;
    capturePowerSum.clear();
    capturePowerSquareSum.clear();
    capturing = true;
    status = QStringLiteral("Capturing 0/%1").arg(captureTargetFrames);
}

ReceiverNoiseBaseline::CaptureResult ReceiverNoiseBaseline::addFrame(
    const std::vector<float> &frequencies,
    const std::vector<float> &levelsDb,
    const ReceiverNoiseBaselineKey &key) {
    if (!capturing) {
        return CaptureResult::Ignored;
    }
    if (!captureKey.matches(key)) {
        status = QStringLiteral("Receiver settings changed; capture cancelled.");
        cancelCapture();
        return CaptureResult::Rejected;
    }
    const std::size_t sourceCount = (std::min)(frequencies.size(), levelsDb.size());
    if (sourceCount < 2 || !std::isfinite(frequencies.front()) ||
        !std::isfinite(frequencies[sourceCount - 1]) ||
        frequencies[sourceCount - 1] <= frequencies.front()) {
        status = QStringLiteral("Invalid spectrum frame; capture cancelled.");
        cancelCapture();
        return CaptureResult::Rejected;
    }

    if (capturePowerSum.isEmpty()) {
        const int storedBins = (std::min)(static_cast<int>(sourceCount), kMaximumStoredBins);
        capturePowerSum.fill(0.0, storedBins);
        capturePowerSquareSum.fill(0.0, storedBins);
        captureMinimumFrequencyHz = frequencies.front();
        captureMaximumFrequencyHz = frequencies[sourceCount - 1];
    } else {
        const double toleranceHz = (std::max)(1.0, captureKey.receiver.sampleRateHz * 1.0e-6);
        if (!sameReal(captureMinimumFrequencyHz, frequencies.front(), toleranceHz) ||
            !sameReal(captureMaximumFrequencyHz, frequencies[sourceCount - 1], toleranceHz)) {
            status = QStringLiteral("Spectrum frequency range changed; capture cancelled.");
            cancelCapture();
            return CaptureResult::Rejected;
        }
    }

    const int storedBins = capturePowerSum.size();
    for (int bin = 0; bin < storedBins; ++bin) {
        const std::size_t begin = static_cast<std::size_t>(bin) * sourceCount / storedBins;
        std::size_t end = static_cast<std::size_t>(bin + 1) * sourceCount / storedBins;
        end = (std::max)(end, begin + 1);
        const double power = reducedFramePower(levelsDb, begin, (std::min)(end, sourceCount));
        capturePowerSum[bin] += power;
        capturePowerSquareSum[bin] += power * power;
    }

    ++captureFrames;
    status = QStringLiteral("Capturing %1/%2").arg(captureFrames).arg(captureTargetFrames);
    if (captureFrames < captureTargetFrames) {
        return CaptureResult::Accepted;
    }
    finishCapture();
    return CaptureResult::Completed;
}

void ReceiverNoiseBaseline::finishCapture() {
    ReceiverNoiseBaselineProfile profile;
    profile.key = captureKey;
    profile.minimumFrequencyHz = captureMinimumFrequencyHz;
    profile.maximumFrequencyHz = captureMaximumFrequencyHz;
    profile.capturedFrames = captureFrames;
    profile.capturedUtcMs = QDateTime::currentMSecsSinceEpoch();
    profile.averagePower.resize(capturePowerSum.size());

    double spreadSumDb = 0.0;
    int spreadCount = 0;
    for (int index = 0; index < capturePowerSum.size(); ++index) {
        const double mean = (std::max)(kMinimumPower, capturePowerSum[index] / captureFrames);
        const double meanSquare = capturePowerSquareSum[index] / captureFrames;
        const double variance = (std::max)(0.0, meanSquare - mean * mean);
        const double standardDeviation = std::sqrt(variance);
        profile.averagePower[index] = static_cast<float>(mean);
        const double spreadDb = 10.0 * std::log10((mean + standardDeviation) / mean);
        if (std::isfinite(spreadDb)) {
            spreadSumDb += spreadDb;
            ++spreadCount;
        }
    }
    profile.spreadDb = spreadCount > 0 ? spreadSumDb / spreadCount : 0.0;

    const int existingIndex = matchingProfileIndex(profile.key);
    if (existingIndex >= 0) {
        profiles[existingIndex] = profile;
    } else {
        profiles.append(profile);
        while (profiles.size() > kMaximumProfiles) {
            profiles.removeFirst();
        }
    }
    capturing = false;
    status = QStringLiteral("Capture complete: %1 frames, variation %2 dB")
                 .arg(profile.capturedFrames)
                 .arg(profile.spreadDb, 0, 'f', 2);
    capturePowerSum.clear();
    capturePowerSquareSum.clear();
}

void ReceiverNoiseBaseline::cancelCapture() {
    capturing = false;
    captureFrames = 0;
    captureTargetFrames = 0;
    capturePowerSum.clear();
    capturePowerSquareSum.clear();
}

int ReceiverNoiseBaseline::matchingProfileIndex(const ReceiverNoiseBaselineKey &key) const {
    for (int index = profiles.size() - 1; index >= 0; --index) {
        if (profiles[index].isValid() && profiles[index].key.matches(key)) {
            return index;
        }
    }
    return -1;
}

bool ReceiverNoiseBaseline::hasMatchingProfile(const ReceiverNoiseBaselineKey &key) const {
    return matchingProfileIndex(key) >= 0;
}

ReceiverNoiseBaselineProfile ReceiverNoiseBaseline::matchingProfile(
    const ReceiverNoiseBaselineKey &key) const {
    const int index = matchingProfileIndex(key);
    return index >= 0 ? profiles.at(index) : ReceiverNoiseBaselineProfile{};
}

bool ReceiverNoiseBaseline::apply(const std::vector<float> &frequencies,
                                  std::vector<float> &levelsDb,
                                  const ReceiverNoiseBaselineKey &key,
                                  ReceiverNoiseBaselineMode mode,
                                  double targetFloorDb) const {
    if (mode == ReceiverNoiseBaselineMode::Off ||
        frequencies.size() != levelsDb.size() || frequencies.empty() ||
        !std::isfinite(targetFloorDb)) {
        return false;
    }
    const int profileIndex = matchingProfileIndex(key);
    if (profileIndex < 0) {
        return false;
    }
    const ReceiverNoiseBaselineProfile &profile = profiles.at(profileIndex);
    const double profileSpan = profile.maximumFrequencyHz - profile.minimumFrequencyHz;
    const int profileLast = profile.averagePower.size() - 1;
    if (profileSpan <= 0.0 || profileLast < 1) {
        return false;
    }

    for (std::size_t index = 0; index < levelsDb.size(); ++index) {
        if (!std::isfinite(levelsDb[index]) || !std::isfinite(frequencies[index])) {
            continue;
        }
        const double position = (std::clamp)(
            (frequencies[index] - profile.minimumFrequencyHz) / profileSpan, 0.0, 1.0) * profileLast;
        const int lower = (std::clamp)(static_cast<int>(std::floor(position)), 0, profileLast);
        const int upper = (std::min)(profileLast, lower + 1);
        const double fraction = position - lower;
        const double baselinePower = (std::max)(
            kMinimumPower,
            profile.averagePower[lower] +
                (profile.averagePower[upper] - profile.averagePower[lower]) * fraction);
        const double inputPower = (std::max)(kMinimumPower, std::pow(10.0, levelsDb[index] / 10.0));
        const double ratio = inputPower / baselinePower;
        if (mode == ReceiverNoiseBaselineMode::ExcessPower) {
            levelsDb[index] = static_cast<float>(
                targetFloorDb + 10.0 * std::log10((std::max)(ratio - 1.0, 1.0e-12)));
        } else {
            levelsDb[index] = static_cast<float>(
                targetFloorDb + 10.0 * std::log10((std::max)(ratio, 1.0e-12)));
        }
    }
    return true;
}

bool ReceiverNoiseBaseline::clearMatching(const ReceiverNoiseBaselineKey &key) {
    const int index = matchingProfileIndex(key);
    if (index < 0) {
        return false;
    }
    profiles.removeAt(index);
    status = QStringLiteral("Matching profile removed.");
    return true;
}

void ReceiverNoiseBaseline::clearAll() {
    profiles.clear();
    cancelCapture();
    status = QStringLiteral("All receiver-noise profiles removed.");
}

QByteArray ReceiverNoiseBaseline::serialize() const {
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setVersion(QDataStream::Qt_5_12);
    out << kMagic << kVersion << quint32(profiles.size());
    for (const ReceiverNoiseBaselineProfile &profile : profiles) {
        writeKey(out, profile.key);
        out << profile.minimumFrequencyHz
            << profile.maximumFrequencyHz
            << qint32(profile.capturedFrames)
            << profile.spreadDb
            << profile.capturedUtcMs
            << quint32(profile.averagePower.size());
        for (float power : profile.averagePower) {
            out << power;
        }
    }
    return qCompress(payload, 6);
}

bool ReceiverNoiseBaseline::deserialize(const QByteArray &encoded, QString *error) {
    profiles.clear();
    if (encoded.isEmpty()) {
        return true;
    }
    const QByteArray payload = qUncompress(encoded);
    if (payload.isEmpty()) {
        if (error) *error = QStringLiteral("Receiver-noise profile data is invalid.");
        return false;
    }
    QDataStream in(payload);
    in.setByteOrder(QDataStream::LittleEndian);
    in.setVersion(QDataStream::Qt_5_12);
    quint32 magic = 0;
    quint32 version = 0;
    quint32 count = 0;
    in >> magic >> version >> count;
    if (magic != kMagic || version != kVersion || count > kMaximumProfiles) {
        if (error) *error = QStringLiteral("Unsupported receiver-noise profile format.");
        return false;
    }
    QVector<ReceiverNoiseBaselineProfile> restored;
    restored.reserve(static_cast<int>(count));
    for (quint32 profileIndex = 0; profileIndex < count; ++profileIndex) {
        ReceiverNoiseBaselineProfile profile;
        qint32 capturedFrames = 0;
        quint32 bins = 0;
        readKey(in, profile.key);
        in >> profile.minimumFrequencyHz
           >> profile.maximumFrequencyHz
           >> capturedFrames
           >> profile.spreadDb
           >> profile.capturedUtcMs
           >> bins;
        if (in.status() != QDataStream::Ok || bins < 2 || bins > kMaximumSerializedBins) {
            if (error) *error = QStringLiteral("Receiver-noise profile is truncated or too large.");
            profiles.clear();
            return false;
        }
        profile.capturedFrames = capturedFrames;
        profile.averagePower.resize(static_cast<int>(bins));
        for (quint32 bin = 0; bin < bins; ++bin) {
            in >> profile.averagePower[static_cast<int>(bin)];
        }
        if (in.status() != QDataStream::Ok || !profile.isValid()) {
            if (error) *error = QStringLiteral("Receiver-noise profile contains invalid values.");
            profiles.clear();
            return false;
        }
        restored.append(profile);
    }
    profiles = restored;
    status = QStringLiteral("Loaded %1 receiver-noise profile(s).").arg(profiles.size());
    return true;
}
