#include "receiveryfactorcalibration.h"

#include <QDataStream>
#include <QDateTime>
#include <QIODevice>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr quint32 kMagic = 0x4f594643u; // OYFC
constexpr quint32 kVersion = 1u;
constexpr int kMaximumProfiles = 16;
constexpr int kMaximumStoredBins = 32768;
constexpr int kMaximumSerializedBins = 1048576;
constexpr double kReferenceTemperatureKelvin = 290.0;
constexpr double kMinimumPower = 1.0e-30;

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
    in >> key.receiver.backendId >> inputMode >> key.receiver.sampleRateHz
       >> lnaGain >> vgaGain >> rtlGain >> gainState
       >> key.centerFrequencyHz >> fftLength >> fftWindowType >> rfBandwidthHz
       >> key.amplitudeOffsetDb >> key.calibrationTableEnabled >> calibrationPointCount;
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

double reducedPower(const std::vector<float> &levelsDb,
                    std::size_t begin,
                    std::size_t end) {
    constexpr std::size_t kMaximumSamples = 16;
    const std::size_t span = end > begin ? end - begin : 0;
    const std::size_t step = span > kMaximumSamples ? span / kMaximumSamples : 1;
    double sum = 0.0;
    int count = 0;
    for (std::size_t index = begin; index < end; index += step) {
        if (!std::isfinite(levelsDb[index])) {
            continue;
        }
        sum += std::pow(10.0, levelsDb[index] / 10.0);
        ++count;
    }
    return count > 0 ? (std::max)(kMinimumPower, sum / count) : kMinimumPower;
}

bool finitePositiveVector(const QVector<float> &values) {
    if (values.size() < 2 || values.size() > kMaximumSerializedBins) {
        return false;
    }
    for (float value : values) {
        if (!std::isfinite(value) || value <= 0.0f) {
            return false;
        }
    }
    return true;
}

double interpolated(const QVector<float> &values, double position) {
    const int last = values.size() - 1;
    const int lower = (std::clamp)(static_cast<int>(std::floor(position)), 0, last);
    const int upper = (std::min)(last, lower + 1);
    const double fraction = position - lower;
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

} // namespace

bool ReceiverYFactorProfile::isValid() const {
    if (coldFrames <= 0 || hotFrames <= 0 || validBins <= 0 ||
        !std::isfinite(minimumFrequencyHz) || !std::isfinite(maximumFrequencyHz) ||
        maximumFrequencyHz <= minimumFrequencyHz ||
        coldPower.size() != hotPower.size() ||
        coldPower.size() != equivalentNoiseTemperatureKelvin.size() ||
        coldPower.size() != noiseFigureDb.size() ||
        !finitePositiveVector(coldPower) || !finitePositiveVector(hotPower)) {
        return false;
    }
    return true;
}

void ReceiverYFactorCalibration::beginColdCapture(const ReceiverNoiseBaselineKey &key,
                                                   int targetFrames) {
    clearPendingCold();
    captureKey = key;
    captureStage = YFactorCaptureStage::Cold;
    stageFrames = 0;
    stageTargetFrames = (std::clamp)(targetFrames, 8, 512);
    stageMinimumFrequencyHz = 0.0;
    stageMaximumFrequencyHz = 0.0;
    stagePowerSum.clear();
}

bool ReceiverYFactorCalibration::beginHotCapture(const ReceiverNoiseBaselineKey &key,
                                                  int targetFrames,
                                                  YFactorSourceMode sourceMode,
                                                  double coldTemperatureKelvin,
                                                  double hotTemperatureKelvin,
                                                  double enrDb,
                                                  QString *error) {
    if (!coldReady(key)) {
        if (error) *error = QStringLiteral("Capture the cold state first without changing receiver settings.");
        return false;
    }
    if (!std::isfinite(coldTemperatureKelvin) || coldTemperatureKelvin <= 0.0) {
        if (error) *error = QStringLiteral("Cold-source temperature must be positive.");
        return false;
    }
    if (sourceMode == YFactorSourceMode::Temperatures &&
        (!std::isfinite(hotTemperatureKelvin) || hotTemperatureKelvin <= coldTemperatureKelvin)) {
        if (error) *error = QStringLiteral("Hot-source temperature must be above the cold-source temperature.");
        return false;
    }
    if (sourceMode == YFactorSourceMode::Enr &&
        (!std::isfinite(enrDb) || enrDb <= -20.0 || enrDb > 100.0)) {
        if (error) *error = QStringLiteral("ENR is outside the supported range.");
        return false;
    }

    pendingSourceMode = sourceMode;
    pendingColdTemperatureKelvin = coldTemperatureKelvin;
    pendingEnrDb = enrDb;
    pendingHotTemperatureKelvin = sourceMode == YFactorSourceMode::Enr
                                      ? coldTemperatureKelvin +
                                            kReferenceTemperatureKelvin * std::pow(10.0, enrDb / 10.0)
                                      : hotTemperatureKelvin;
    captureKey = key;
    captureStage = YFactorCaptureStage::Hot;
    stageFrames = 0;
    stageTargetFrames = (std::clamp)(targetFrames, 8, 512);
    stageMinimumFrequencyHz = 0.0;
    stageMaximumFrequencyHz = 0.0;
    stagePowerSum.clear();
    return true;
}

bool ReceiverYFactorCalibration::appendCaptureFrame(const std::vector<float> &frequencies,
                                                     const std::vector<float> &levelsDb,
                                                     QString *error) {
    const std::size_t sourceCount = (std::min)(frequencies.size(), levelsDb.size());
    if (sourceCount < 2 || !std::isfinite(frequencies.front()) ||
        !std::isfinite(frequencies[sourceCount - 1]) ||
        frequencies[sourceCount - 1] <= frequencies.front()) {
        if (error) *error = QStringLiteral("Invalid spectrum frame.");
        return false;
    }
    if (stagePowerSum.isEmpty()) {
        const int storedBins = (std::min)(static_cast<int>(sourceCount), kMaximumStoredBins);
        stagePowerSum.fill(0.0, storedBins);
        stageMinimumFrequencyHz = frequencies.front();
        stageMaximumFrequencyHz = frequencies[sourceCount - 1];
    } else {
        const double toleranceHz = (std::max)(1.0, captureKey.receiver.sampleRateHz * 1.0e-6);
        if (std::abs(stageMinimumFrequencyHz - frequencies.front()) > toleranceHz ||
            std::abs(stageMaximumFrequencyHz - frequencies[sourceCount - 1]) > toleranceHz) {
            if (error) *error = QStringLiteral("Spectrum frequency range changed.");
            return false;
        }
    }
    const int storedBins = stagePowerSum.size();
    for (int bin = 0; bin < storedBins; ++bin) {
        const std::size_t begin = static_cast<std::size_t>(bin) * sourceCount / storedBins;
        std::size_t end = static_cast<std::size_t>(bin + 1) * sourceCount / storedBins;
        end = (std::max)(end, begin + 1);
        stagePowerSum[bin] += reducedPower(levelsDb, begin, (std::min)(end, sourceCount));
    }
    ++stageFrames;
    return true;
}

ReceiverYFactorCalibration::CaptureResult ReceiverYFactorCalibration::addFrame(
    const std::vector<float> &frequencies,
    const std::vector<float> &levelsDb,
    const ReceiverNoiseBaselineKey &key) {
    if (!isCapturing()) {
        return CaptureResult::Ignored;
    }
    if (!captureKey.matches(key)) {
        cancelCapture();
        return CaptureResult::Rejected;
    }
    QString error;
    if (!appendCaptureFrame(frequencies, levelsDb, &error)) {
        cancelCapture();
        return CaptureResult::Rejected;
    }
    if (stageFrames < stageTargetFrames) {
        return CaptureResult::Accepted;
    }
    if (captureStage == YFactorCaptureStage::Cold) {
        finishColdCapture();
        return CaptureResult::ColdCompleted;
    }
    if (!finishHotCapture(&error)) {
        cancelCapture();
        return CaptureResult::Rejected;
    }
    return CaptureResult::Completed;
}

void ReceiverYFactorCalibration::finishColdCapture() {
    pendingColdKey = captureKey;
    pendingMinimumFrequencyHz = stageMinimumFrequencyHz;
    pendingMaximumFrequencyHz = stageMaximumFrequencyHz;
    pendingColdFrames = stageFrames;
    pendingColdPower.resize(stagePowerSum.size());
    for (int index = 0; index < stagePowerSum.size(); ++index) {
        pendingColdPower[index] = static_cast<float>(
            (std::max)(kMinimumPower, stagePowerSum[index] / stageFrames));
    }
    captureStage = YFactorCaptureStage::None;
    stageFrames = 0;
    stageTargetFrames = 0;
    stagePowerSum.clear();
}

bool ReceiverYFactorCalibration::finishHotCapture(QString *error) {
    if (!coldReady(captureKey) || stagePowerSum.size() != pendingColdPower.size()) {
        if (error) *error = QStringLiteral("Cold and hot captures do not have matching geometry.");
        return false;
    }
    ReceiverYFactorProfile profile;
    profile.key = captureKey;
    profile.sourceMode = pendingSourceMode;
    profile.coldTemperatureKelvin = pendingColdTemperatureKelvin;
    profile.hotTemperatureKelvin = pendingHotTemperatureKelvin;
    profile.enrDb = pendingEnrDb;
    profile.minimumFrequencyHz = pendingMinimumFrequencyHz;
    profile.maximumFrequencyHz = pendingMaximumFrequencyHz;
    profile.coldPower = pendingColdPower;
    profile.coldFrames = pendingColdFrames;
    profile.hotFrames = stageFrames;
    profile.capturedUtcMs = QDateTime::currentMSecsSinceEpoch();
    profile.hotPower.resize(stagePowerSum.size());
    profile.equivalentNoiseTemperatureKelvin.fill(
        std::numeric_limits<float>::quiet_NaN(), stagePowerSum.size());
    profile.noiseFigureDb.fill(std::numeric_limits<float>::quiet_NaN(), stagePowerSum.size());

    for (int index = 0; index < stagePowerSum.size(); ++index) {
        const double coldPower = (std::max)(kMinimumPower, static_cast<double>(profile.coldPower[index]));
        const double hotPower = (std::max)(kMinimumPower, stagePowerSum[index] / stageFrames);
        profile.hotPower[index] = static_cast<float>(hotPower);
        const double y = hotPower / coldPower;
        if (!std::isfinite(y) || y <= 1.000001) {
            continue;
        }
        const double equivalentTemperature =
            (profile.hotTemperatureKelvin - y * profile.coldTemperatureKelvin) / (y - 1.0);
        const double noiseFactor = 1.0 + equivalentTemperature / kReferenceTemperatureKelvin;
        if (!std::isfinite(equivalentTemperature) || equivalentTemperature < 0.0 ||
            !std::isfinite(noiseFactor) || noiseFactor < 1.0) {
            continue;
        }
        profile.equivalentNoiseTemperatureKelvin[index] = static_cast<float>(equivalentTemperature);
        profile.noiseFigureDb[index] = static_cast<float>(10.0 * std::log10(noiseFactor));
        ++profile.validBins;
    }
    if (!profile.isValid()) {
        if (error) *error = QStringLiteral("The hot/cold ratio did not produce valid Y-factor results.");
        return false;
    }
    const int existing = matchingProfileIndex(profile.key);
    if (existing >= 0) profiles[existing] = profile;
    else profiles.append(profile);
    while (profiles.size() > kMaximumProfiles) profiles.removeFirst();
    captureStage = YFactorCaptureStage::None;
    stageFrames = 0;
    stageTargetFrames = 0;
    stagePowerSum.clear();
    clearPendingCold();
    return true;
}

void ReceiverYFactorCalibration::cancelCapture() {
    captureStage = YFactorCaptureStage::None;
    stageFrames = 0;
    stageTargetFrames = 0;
    stagePowerSum.clear();
}

void ReceiverYFactorCalibration::clearPendingCold() {
    pendingColdPower.clear();
    pendingColdFrames = 0;
    pendingMinimumFrequencyHz = 0.0;
    pendingMaximumFrequencyHz = 0.0;
}

bool ReceiverYFactorCalibration::coldReady(const ReceiverNoiseBaselineKey &key) const {
    return pendingColdFrames > 0 && pendingColdPower.size() >= 2 && pendingColdKey.matches(key);
}

int ReceiverYFactorCalibration::matchingProfileIndex(const ReceiverNoiseBaselineKey &key) const {
    for (int index = profiles.size() - 1; index >= 0; --index) {
        if (profiles[index].isValid() && profiles[index].key.matches(key)) return index;
    }
    return -1;
}

bool ReceiverYFactorCalibration::hasMatchingProfile(const ReceiverNoiseBaselineKey &key) const {
    return matchingProfileIndex(key) >= 0;
}

ReceiverYFactorProfile ReceiverYFactorCalibration::matchingProfile(
    const ReceiverNoiseBaselineKey &key) const {
    const int index = matchingProfileIndex(key);
    return index >= 0 ? profiles.at(index) : ReceiverYFactorProfile{};
}

bool ReceiverYFactorCalibration::applyInputReferred(
    const std::vector<float> &frequencies,
    std::vector<float> &levelsDb,
    const ReceiverNoiseBaselineKey &key,
    double targetColdFloorDb) const {
    if (frequencies.size() != levelsDb.size() || frequencies.empty() ||
        !std::isfinite(targetColdFloorDb)) return false;
    const int profileIndex = matchingProfileIndex(key);
    if (profileIndex < 0) return false;
    const ReceiverYFactorProfile &profile = profiles[profileIndex];
    const int last = profile.coldPower.size() - 1;
    const double span = profile.maximumFrequencyHz - profile.minimumFrequencyHz;
    if (last < 1 || span <= 0.0) return false;

    for (std::size_t index = 0; index < levelsDb.size(); ++index) {
        if (!std::isfinite(frequencies[index]) || !std::isfinite(levelsDb[index])) continue;
        const double position = (std::clamp)(
            (frequencies[index] - profile.minimumFrequencyHz) / span, 0.0, 1.0) * last;
        const double coldPower = (std::max)(kMinimumPower, interpolated(profile.coldPower, position));
        const double equivalentTemperature = interpolated(profile.equivalentNoiseTemperatureKelvin, position);
        if (!std::isfinite(equivalentTemperature) || equivalentTemperature < 0.0) continue;
        const double observedPower = (std::max)(kMinimumPower, std::pow(10.0, levelsDb[index] / 10.0));
        const double inputTemperature =
            (observedPower / coldPower) *
                (profile.coldTemperatureKelvin + equivalentTemperature) -
            equivalentTemperature;
        const double normalized = (std::max)(inputTemperature / profile.coldTemperatureKelvin, 1.0e-12);
        levelsDb[index] = static_cast<float>(targetColdFloorDb + 10.0 * std::log10(normalized));
    }
    return true;
}

bool ReceiverYFactorCalibration::clearMatching(const ReceiverNoiseBaselineKey &key) {
    const int index = matchingProfileIndex(key);
    if (index < 0) return false;
    profiles.removeAt(index);
    return true;
}

void ReceiverYFactorCalibration::clearAll() {
    profiles.clear();
    cancelCapture();
    clearPendingCold();
}

QByteArray ReceiverYFactorCalibration::serialize() const {
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setVersion(QDataStream::Qt_5_12);
    out << kMagic << kVersion << quint32(profiles.size());
    for (const ReceiverYFactorProfile &profile : profiles) {
        writeKey(out, profile.key);
        out << qint32(profile.sourceMode)
            << profile.coldTemperatureKelvin << profile.hotTemperatureKelvin << profile.enrDb
            << profile.minimumFrequencyHz << profile.maximumFrequencyHz
            << qint32(profile.coldFrames) << qint32(profile.hotFrames)
            << qint32(profile.validBins) << profile.capturedUtcMs
            << quint32(profile.coldPower.size());
        for (int index = 0; index < profile.coldPower.size(); ++index) {
            out << profile.coldPower[index] << profile.hotPower[index]
                << profile.equivalentNoiseTemperatureKelvin[index]
                << profile.noiseFigureDb[index];
        }
    }
    return qCompress(payload, 6);
}

bool ReceiverYFactorCalibration::deserialize(const QByteArray &encoded, QString *error) {
    profiles.clear();
    if (encoded.isEmpty()) return true;
    const QByteArray payload = qUncompress(encoded);
    if (payload.isEmpty()) {
        if (error) *error = QStringLiteral("Y-factor profile data is invalid.");
        return false;
    }
    QDataStream in(payload);
    in.setByteOrder(QDataStream::LittleEndian);
    in.setVersion(QDataStream::Qt_5_12);
    quint32 magic = 0, version = 0, count = 0;
    in >> magic >> version >> count;
    if (magic != kMagic || version != kVersion || count > kMaximumProfiles) {
        if (error) *error = QStringLiteral("Unsupported Y-factor profile format.");
        return false;
    }
    QVector<ReceiverYFactorProfile> restored;
    restored.reserve(static_cast<int>(count));
    for (quint32 profileIndex = 0; profileIndex < count; ++profileIndex) {
        ReceiverYFactorProfile profile;
        qint32 sourceMode = 0, coldFrames = 0, hotFrames = 0, validBins = 0;
        quint32 bins = 0;
        readKey(in, profile.key);
        in >> sourceMode
           >> profile.coldTemperatureKelvin >> profile.hotTemperatureKelvin >> profile.enrDb
           >> profile.minimumFrequencyHz >> profile.maximumFrequencyHz
           >> coldFrames >> hotFrames >> validBins >> profile.capturedUtcMs >> bins;
        if (in.status() != QDataStream::Ok || bins < 2 || bins > kMaximumSerializedBins) {
            if (error) *error = QStringLiteral("Y-factor profile is truncated or too large.");
            return false;
        }
        profile.sourceMode = static_cast<YFactorSourceMode>((std::clamp)(sourceMode, 0, 1));
        profile.coldFrames = coldFrames;
        profile.hotFrames = hotFrames;
        profile.validBins = validBins;
        profile.coldPower.resize(static_cast<int>(bins));
        profile.hotPower.resize(static_cast<int>(bins));
        profile.equivalentNoiseTemperatureKelvin.resize(static_cast<int>(bins));
        profile.noiseFigureDb.resize(static_cast<int>(bins));
        for (quint32 bin = 0; bin < bins; ++bin) {
            const int i = static_cast<int>(bin);
            in >> profile.coldPower[i] >> profile.hotPower[i]
               >> profile.equivalentNoiseTemperatureKelvin[i] >> profile.noiseFigureDb[i];
        }
        if (in.status() != QDataStream::Ok || !profile.isValid()) {
            if (error) *error = QStringLiteral("Y-factor profile contains invalid values.");
            return false;
        }
        restored.append(profile);
    }
    profiles = restored;
    return true;
}
