#include "coherentiqsession.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>

namespace {
constexpr int SchemaVersion = 2;
constexpr std::size_t MaximumSources = 8;
constexpr std::size_t MaximumFloatsPerSource = 2U * 1024U * 1024U;

QString number(std::uint64_t value) { return QString::number(value); }

bool finiteIq(const std::vector<float> &iq) {
    return !iq.empty() && !(iq.size() & 1U) && iq.size() <= MaximumFloatsPerSource &&
           std::all_of(iq.begin(), iq.end(), [](float value) { return std::isfinite(value); });
}

QString payloadHash(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QByteArray encodeCf32Le(const std::vector<float> &iq) {
    QByteArray bytes;
    bytes.resize(int(iq.size() * sizeof(float)));
    for (std::size_t index = 0; index < iq.size(); ++index) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &iq[index], sizeof(bits));
        bits = qToLittleEndian(bits);
        std::memcpy(bytes.data() + index * sizeof(bits), &bits, sizeof(bits));
    }
    return bytes;
}

bool decodeCf32Le(const QByteArray &bytes, std::vector<float> &iq) {
    if (bytes.isEmpty() || bytes.size() % int(2 * sizeof(float))) return false;
    iq.resize(std::size_t(bytes.size()) / sizeof(float));
    for (std::size_t index = 0; index < iq.size(); ++index) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, bytes.constData() + index * sizeof(bits), sizeof(bits));
        bits = qFromLittleEndian(bits);
        std::memcpy(&iq[index], &bits, sizeof(bits));
    }
    return finiteIq(iq);
}

bool writeAll(QIODevice &device, const QByteArray &bytes) {
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const qint64 written = device.write(bytes.constData() + offset, bytes.size() - offset);
        if (written <= 0) return false;
        offset += written;
    }
    return true;
}

bool parseU64(const QJsonValue &value, std::uint64_t &result) {
    bool ok = false;
    const qulonglong parsed = value.toString().toULongLong(&ok);
    if (ok) result = parsed;
    return ok;
}

QJsonObject calibrationJson(const CoherentIqCalibration &value) {
    return {
        {QStringLiteral("reference"), value.reference.key()},
        {QStringLiteral("target"), value.target.key()},
        {QStringLiteral("sampleRateHz"), value.sampleRateHz},
        {QStringLiteral("centerHz"), value.centerHz},
        {QStringLiteral("integerDelaySamples"), value.integerDelaySamples},
        {QStringLiteral("fractionalDelaySamples"), value.fractionalDelaySamples},
        {QStringLiteral("phaseCorrectionRadians"), value.phaseCorrectionRadians},
        {QStringLiteral("gainCorrection"), value.gainCorrection},
        {QStringLiteral("coherence"), value.coherence},
        {QStringLiteral("comparedSamples"), number(value.comparedSamples)}
    };
}

bool identity(const QString &key, DspSourceIdentity &value) {
    const QStringList parts = key.split(QLatin1Char('/'));
    if (parts.size() != 2) return false;
    value = DspSourceIdentity::normalized(parts[0], parts[1]);
    return value.isValid();
}

const CoherentIqCalibration *findCalibration(
    const std::vector<CoherentIqCalibration> &calibrations,
    const DspSourceIdentity &reference,
    const DspSourceIdentity &target) {
    const auto it = std::find_if(calibrations.cbegin(), calibrations.cend(),
        [&](const CoherentIqCalibration &value) {
            return value.valid && value.reference == reference && value.target == target;
        });
    return it == calibrations.cend() ? nullptr : &*it;
}

std::complex<double> sampleAt(const std::vector<float> &iq, double position) {
    const std::size_t count = iq.size() / 2U;
    const std::size_t lower = std::min<std::size_t>(std::size_t(std::floor(position)), count - 1U);
    const std::size_t upper = std::min(lower + 1U, count - 1U);
    const double fraction = position - std::floor(position);
    const std::complex<double> a(iq[lower * 2U], iq[lower * 2U + 1U]);
    const std::complex<double> b(iq[upper * 2U], iq[upper * 2U + 1U]);
    return a + (b - a) * fraction;
}
}

bool CoherentIqSession::write(
    const QString &parentDirectory,
    const std::vector<DspIqSourceSnapshot> &sources,
    const std::vector<CoherentIqCalibration> &calibrations,
    QString &manifestPath,
    QString &error) {
    manifestPath.clear();
    error.clear();
    const QFileInfo parentInfo(parentDirectory);
    if (!parentInfo.isDir() || !QDir::isAbsolutePath(parentDirectory) ||
        sources.size() < 2U || sources.size() > MaximumSources) {
        error = QStringLiteral("Select an existing absolute directory and provide 2..8 IQ sources");
        return false;
    }
    QSet<QString> identities;
    for (const auto &source : sources) {
        if (!source.source.isValid() || !finiteIq(source.iq) ||
            source.endComplexSample < source.startComplexSample ||
            source.endComplexSample - source.startComplexSample != source.iq.size() / 2U ||
            !std::isfinite(source.sampleRateHz) || source.sampleRateHz <= 0.0 ||
            !std::isfinite(source.centerHz) || identities.contains(source.source.key())) {
            error = QStringLiteral("Invalid, duplicate or oversized coherent IQ source");
            return false;
        }
        identities.insert(source.source.key());
    }

    const QString directoryName = QStringLiteral("obrii-coherent-%1-%2")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")),
             QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    QDir parent(parentInfo.canonicalFilePath());
    if (parent.path().isEmpty() || !parent.mkdir(directoryName)) {
        error = QStringLiteral("Cannot create a unique coherent-session directory");
        return false;
    }
    QDir directory(parent.filePath(directoryName));
    QJsonArray sourceArray;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto &source = sources[index];
        const QString fileName = QStringLiteral("rx-%1.cf32").arg(index + 1, 2, 10, QLatin1Char('0'));
        QSaveFile file(directory.filePath(fileName));
        file.setDirectWriteFallback(false);
        const QByteArray bytes = encodeCf32Le(source.iq);
        if (!file.open(QIODevice::WriteOnly) || !writeAll(file, bytes) || !file.commit()) {
            error = QStringLiteral("Cannot commit %1").arg(fileName);
            return false;
        }
        sourceArray.append(QJsonObject{
            {QStringLiteral("source"), source.source.key()},
            {QStringLiteral("file"), fileName},
            {QStringLiteral("format"), QStringLiteral("CF32LE")},
            {QStringLiteral("sha256"), payloadHash(bytes)},
            {QStringLiteral("complexSamples"), number(source.iq.size() / 2U)},
            {QStringLiteral("epoch"), number(source.epoch)},
            {QStringLiteral("startComplexSample"), number(source.startComplexSample)},
            {QStringLiteral("endComplexSample"), number(source.endComplexSample)},
            {QStringLiteral("hostTimestampNs"), QString::number(source.hostTimestampNs)},
            {QStringLiteral("sampleRateHz"), source.sampleRateHz},
            {QStringLiteral("centerHz"), source.centerHz},
            {QStringLiteral("gap"), source.gap},
            {QStringLiteral("clockId"), source.clockId},
            {QStringLiteral("sampleClockValid"), source.sampleClockValid},
            {QStringLiteral("hardwareTimestamp"), source.hardwareTimestamp},
            {QStringLiteral("clockLocked"), source.clockLocked},
            {QStringLiteral("nativeAdcBits"), source.nativeAdcBits},
            {QStringLiteral("nativeSampleFormat"), source.nativeSampleFormat},
            {QStringLiteral("normalizedFullScale"), source.normalizedFullScale}
        });
    }
    QJsonArray calibrationArray;
    for (const auto &calibration : calibrations)
        if (calibration.valid) calibrationArray.append(calibrationJson(calibration));
    const QJsonObject root{
        {QStringLiteral("schemaVersion"), SchemaVersion},
        {QStringLiteral("format"), QStringLiteral("obrii-coherent-iq-session")},
        {QStringLiteral("createdAtUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("sourceCount"), int(sources.size())},
        {QStringLiteral("sources"), sourceArray},
        {QStringLiteral("calibrations"), calibrationArray}
    };
    QSaveFile manifest(directory.filePath(QStringLiteral("manifest.json")));
    manifest.setDirectWriteFallback(false);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!manifest.open(QIODevice::WriteOnly) || !writeAll(manifest, bytes) || !manifest.commit()) {
        error = QStringLiteral("Cannot commit coherent-session manifest");
        return false;
    }
    manifestPath = manifest.fileName();
    return true;
}

bool CoherentIqSession::read(
    const QString &manifestPath,
    std::vector<DspIqSourceSnapshot> &sources,
    std::vector<CoherentIqCalibration> &calibrations,
    QString &error) {
    sources.clear();
    calibrations.clear();
    error.clear();
    QFile manifest(manifestPath);
    if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 1024 * 1024) {
        error = QStringLiteral("Cannot open coherent-session manifest");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifest.readAll(), &parseError);
    const QJsonObject root = document.object();
    const QJsonArray entries = root.value(QStringLiteral("sources")).toArray();
    const int version = root.value(QStringLiteral("schemaVersion")).toInt();
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        (version != 1 && version != SchemaVersion) ||
        root.value(QStringLiteral("format")).toString() != QStringLiteral("obrii-coherent-iq-session") ||
        entries.size() < 2 || entries.size() > int(MaximumSources) ||
        root.value(QStringLiteral("sourceCount")).toInt() != entries.size()) {
        error = QStringLiteral("Unsupported coherent-session manifest");
        return false;
    }
    const QDir directory = QFileInfo(manifestPath).absoluteDir();
    QSet<QString> identities;
    QSet<QString> filenames;
    std::vector<DspIqSourceSnapshot> verifiedSources;
    verifiedSources.reserve(entries.size());
    for (const QJsonValue &entryValue : entries) {
        const QJsonObject entry = entryValue.toObject();
        DspIqSourceSnapshot source;
        const QString fileName = entry.value(QStringLiteral("file")).toString();
        const QString filenameKey = fileName.toCaseFolded();
        if (!identity(entry.value(QStringLiteral("source")).toString(), source.source) ||
            identities.contains(source.source.key()) || filenames.contains(filenameKey) ||
            !fileName.endsWith(QStringLiteral(".cf32")) || fileName.contains(QLatin1Char('/')) ||
            fileName.contains(QLatin1Char('\\')) || fileName.contains(QLatin1Char(':')) ||
            entry.value(QStringLiteral("format")).toString() != QStringLiteral("CF32LE")) {
            error = QStringLiteral("Invalid source identity or IQ filename in manifest");
            return false;
        }
        identities.insert(source.source.key());
        filenames.insert(filenameKey);
        std::uint64_t expectedSamples = 0;
        const QFileInfo payload(directory.filePath(fileName));
        if (!parseU64(entry.value(QStringLiteral("complexSamples")), expectedSamples) ||
            expectedSamples == 0 || expectedSamples > MaximumFloatsPerSource / 2U ||
            !payload.isFile() || payload.isSymLink()) {
            error = QStringLiteral("Invalid or incomplete coherent IQ payload");
            return false;
        }
        const qint64 expectedBytes = static_cast<qint64>(expectedSamples * 2U * sizeof(float));
        QFile data(payload.filePath());
        const QByteArray bytes = data.open(QIODevice::ReadOnly) && data.size() == expectedBytes
            ? data.read(expectedBytes + 1) : QByteArray{};
        if (bytes.size() != expectedBytes) {
            error = QStringLiteral("Invalid or incomplete coherent IQ payload");
            return false;
        }
        if (version == SchemaVersion && entry.value(QStringLiteral("sha256")).toString() != payloadHash(bytes)) {
            error = QStringLiteral("Coherent IQ SHA-256 mismatch: %1").arg(fileName);
            return false;
        }
        if (!decodeCf32Le(bytes, source.iq)) {
            error = QStringLiteral("Invalid coherent IQ samples: %1").arg(fileName);
            return false;
        }
        if (!parseU64(entry.value(QStringLiteral("epoch")), source.epoch) ||
            !parseU64(entry.value(QStringLiteral("startComplexSample")), source.startComplexSample) ||
            !parseU64(entry.value(QStringLiteral("endComplexSample")), source.endComplexSample)) {
            error = QStringLiteral("Invalid 64-bit sample metadata");
            return false;
        }
        source.hostTimestampNs = entry.value(QStringLiteral("hostTimestampNs")).toString().toLongLong();
        source.sampleRateHz = entry.value(QStringLiteral("sampleRateHz")).toDouble();
        source.centerHz = entry.value(QStringLiteral("centerHz")).toDouble();
        source.gap = entry.value(QStringLiteral("gap")).toBool();
        source.clockId = entry.value(QStringLiteral("clockId")).toString();
        source.sampleClockValid = entry.value(QStringLiteral("sampleClockValid")).toBool();
        source.hardwareTimestamp = entry.value(QStringLiteral("hardwareTimestamp")).toBool();
        source.clockLocked = entry.value(QStringLiteral("clockLocked")).toBool();
        source.nativeAdcBits = entry.value(QStringLiteral("nativeAdcBits")).toInt();
        source.nativeSampleFormat = entry.value(QStringLiteral("nativeSampleFormat")).toString();
        source.normalizedFullScale = entry.value(QStringLiteral("normalizedFullScale")).toDouble(1.0);
        source.endFloatCount = source.iq.size();
        if (!std::isfinite(source.sampleRateHz) || source.sampleRateHz <= 0.0 ||
            !std::isfinite(source.centerHz) || source.endComplexSample < source.startComplexSample ||
            source.endComplexSample - source.startComplexSample != source.iq.size() / 2U) {
            error = QStringLiteral("Inconsistent coherent source metadata");
            return false;
        }
        verifiedSources.push_back(std::move(source));
    }
    for (const QJsonValue &value : root.value(QStringLiteral("calibrations")).toArray()) {
        const QJsonObject object = value.toObject();
        CoherentIqCalibration calibration;
        std::uint64_t compared = 0;
        if (!identity(object.value(QStringLiteral("reference")).toString(), calibration.reference) ||
            !identity(object.value(QStringLiteral("target")).toString(), calibration.target) ||
            !parseU64(object.value(QStringLiteral("comparedSamples")), compared)) continue;
        calibration.sampleRateHz = object.value(QStringLiteral("sampleRateHz")).toDouble();
        calibration.centerHz = object.value(QStringLiteral("centerHz")).toDouble();
        calibration.integerDelaySamples = object.value(QStringLiteral("integerDelaySamples")).toInt();
        calibration.fractionalDelaySamples = object.value(QStringLiteral("fractionalDelaySamples")).toDouble();
        calibration.phaseCorrectionRadians = object.value(QStringLiteral("phaseCorrectionRadians")).toDouble();
        calibration.gainCorrection = object.value(QStringLiteral("gainCorrection")).toDouble();
        calibration.coherence = object.value(QStringLiteral("coherence")).toDouble();
        calibration.comparedSamples = std::size_t(compared);
        calibration.valid = identities.contains(calibration.reference.key()) &&
            identities.contains(calibration.target.key()) && calibration.reference != calibration.target &&
            std::isfinite(calibration.sampleRateHz) && calibration.sampleRateHz > 0.0 &&
            std::isfinite(calibration.centerHz) && std::isfinite(calibration.fractionalDelaySamples) &&
            std::isfinite(calibration.phaseCorrectionRadians) &&
            std::isfinite(calibration.gainCorrection) && calibration.gainCorrection > 0.0 &&
            std::isfinite(calibration.coherence) && calibration.coherence >= 0.10;
        if (calibration.valid) calibrations.push_back(calibration);
    }
    sources = std::move(verifiedSources);
    return true;
}

double CoherentIqSession::estimateNoisePower(const std::vector<float> &iq) {
    if (!finiteIq(iq)) return std::numeric_limits<double>::infinity();
    const std::size_t samples = iq.size() / 2U;
    const std::size_t stride = std::max<std::size_t>(1U, samples / 8192U);
    std::vector<double> powers;
    powers.reserve((samples + stride - 1U) / stride);
    for (std::size_t index = 0; index < samples; index += stride) {
        const double i = iq[index * 2U];
        const double q = iq[index * 2U + 1U];
        powers.push_back(i * i + q * q);
    }
    const std::size_t percentile = std::min(powers.size() - 1U, powers.size() / 5U);
    std::nth_element(powers.begin(), powers.begin() + std::ptrdiff_t(percentile), powers.end());
    return std::max(1.0e-15, powers[percentile]);
}

CoherentIqCombinedResult CoherentIqSession::combine(
    const std::vector<DspIqSourceSnapshot> &sources,
    const std::vector<CoherentIqCalibration> &calibrations,
    const DspSourceIdentity &reference,
    CoherentIqCombineMode mode,
    std::size_t maximumComplexSamples) {
    CoherentIqCombinedResult result;
    result.reference = reference;
    const auto referenceIt = std::find_if(sources.cbegin(), sources.cend(),
        [&](const DspIqSourceSnapshot &source) { return source.source == reference; });
    if (referenceIt == sources.cend() || !finiteIq(referenceIt->iq)) {
        result.error = QStringLiteral("Reference IQ is missing or invalid");
        return result;
    }
    result.sampleRateHz = referenceIt->sampleRateHz;
    result.centerHz = referenceIt->centerHz;
    struct Input {
        const DspIqSourceSnapshot *source = nullptr;
        const CoherentIqCalibration *calibration = nullptr;
        double offset = 0.0;
        double gain = 1.0;
        std::complex<double> correction{1.0, 0.0};
        double noise = 1.0;
    };
    std::vector<Input> inputs;
    inputs.push_back({&*referenceIt, nullptr, 0.0, 1.0, {1.0, 0.0},
                      estimateNoisePower(referenceIt->iq)});
    for (const auto &source : sources) {
        if (source.source == reference) continue;
        if (!finiteIq(source.iq) || std::abs(source.sampleRateHz - result.sampleRateHz) > 0.5 ||
            std::abs(source.centerHz - result.centerHz) > 1.0) continue;
        const CoherentIqCalibration *calibration = findCalibration(calibrations, reference, source.source);
        if (!calibration) continue;
        Input input;
        input.source = &source;
        input.calibration = calibration;
        input.offset = calibration->integerDelaySamples + calibration->fractionalDelaySamples;
        input.gain = calibration->gainCorrection;
        input.correction = std::polar(calibration->gainCorrection,
                                      calibration->phaseCorrectionRadians);
        input.noise = estimateNoisePower(source.iq) * input.gain * input.gain;
        inputs.push_back(input);
    }
    if (mode != CoherentIqCombineMode::ReferenceOnly && inputs.size() < 2U) {
        result.error = QStringLiteral("No calibrated same-rate/same-center target IQ is available");
        return result;
    }
    double firstReference = 0.0;
    double endReference = double(referenceIt->iq.size() / 2U);
    for (const Input &input : inputs) {
        const double targetSamples = double(input.source->iq.size() / 2U);
        firstReference = std::max(firstReference, -input.offset);
        endReference = std::min(endReference, targetSamples - input.offset);
    }
    const std::size_t first = std::size_t(std::ceil(std::max(0.0, firstReference)));
    std::size_t end = std::size_t(std::floor(std::max(0.0, endReference)));
    if (end <= first) {
        result.error = QStringLiteral("Aligned IQ sources have no common sample span");
        return result;
    }
    if (maximumComplexSamples) end = std::min(end, first + maximumComplexSamples);
    const std::size_t count = end - first;
    result.sources.reserve(inputs.size());
    result.weights.assign(inputs.size(), 0.0);
    for (const Input &input : inputs) result.sources.push_back(input.source->source);
    if (mode == CoherentIqCombineMode::ReferenceOnly) {
        result.weights[0] = 1.0;
    } else if (mode == CoherentIqCombineMode::BestSnr) {
        const auto best = std::min_element(inputs.cbegin(), inputs.cend(),
            [](const Input &a, const Input &b) { return a.noise < b.noise; });
        result.weights[std::size_t(std::distance(inputs.cbegin(), best))] = 1.0;
    } else if (mode == CoherentIqCombineMode::EqualAverage) {
        std::fill(result.weights.begin(), result.weights.end(), 1.0 / double(inputs.size()));
    } else {
        double total = 0.0;
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            result.weights[index] = 1.0 / std::max(1.0e-15, inputs[index].noise);
            total += result.weights[index];
        }
        for (double &weight : result.weights) weight /= total;
    }
    result.iq.resize(count * 2U);
    for (std::size_t output = 0; output < count; ++output) {
        const double referencePosition = double(first + output);
        std::complex<double> combined{};
        for (std::size_t inputIndex = 0; inputIndex < inputs.size(); ++inputIndex) {
            const Input &input = inputs[inputIndex];
            const auto value = sampleAt(input.source->iq, referencePosition + input.offset) *
                               input.correction;
            combined += value * result.weights[inputIndex];
        }
        result.iq[output * 2U] = float(combined.real());
        result.iq[output * 2U + 1U] = float(combined.imag());
    }
    result.referenceFirstComplexSample = referenceIt->startComplexSample + first;
    result.valid = finiteIq(result.iq);
    if (!result.valid) result.error = QStringLiteral("Coherent combiner produced invalid IQ");
    return result;
}
