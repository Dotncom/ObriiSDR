#include "dspstreamrecorder.h"
#include "dspstreamprocessor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int MaxSegments = 128;
constexpr quint64 SampleBytes = 8;

bool writeAll(QIODevice &file, const QByteArray &bytes) {
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const qint64 written = file.write(bytes.constData() + offset, bytes.size() - offset);
        if (written <= 0) return false;
        offset += written;
    }
    return true;
}

QString ioError(const QString &operation, const QFileDevice &file) {
    return operation + QStringLiteral(": ") + file.errorString();
}
}

struct DspStreamRecorder::Impl {
    QFile data;
    QString directory, currentPath, metadataPath, sessionId, stream, startReason;
    QString message = QStringLiteral("Stopped");
    quint64 limit = 0, totalBytes = 0, samples = 0, firstSample = 0;
    quint64 generation = 0, epoch = 0;
    double sampleRate = 0, centerHz = 0;
    int segments = 0;
    bool recording = false, metadataKnown = false, gapBefore = false, intact = true;

    void setMetadata(const DspStreamBlock &block) {
        stream = block.stream;
        generation = block.generation;
        epoch = block.epoch;
        firstSample = block.firstSample;
        sampleRate = block.sampleRate;
        centerHz = block.centerHz;
        gapBefore = block.gap;
        metadataKnown = true;
    }

    QJsonObject metadata(bool finalized, const QString &reason, qint64 fileBytes) const {
        // Decimal strings preserve the full uint64 sample clock in JSON readers.
        const auto position = [this](quint64 value) {
            return metadataKnown ? QJsonValue(QString::number(value)) : QJsonValue(QJsonValue::Null);
        };
        return {
            {"schemaVersion", 1}, {"format", "CF32LE"}, {"sampleLayout", "interleaved_IQ"},
            {"sessionId", sessionId}, {"segmentIndex", segments},
            {"iqFile", QFileInfo(currentPath).fileName()},
            {"samplePositionReference", "relative_channel"},
            {"sampleRate", metadataKnown ? QJsonValue(sampleRate) : QJsonValue(QJsonValue::Null)},
            {"centerHz", metadataKnown ? QJsonValue(centerHz) : QJsonValue(QJsonValue::Null)},
            {"generation", position(generation)}, {"epoch", position(epoch)},
            {"firstSample", position(firstSample)}, {"endSampleExclusive", position(firstSample + samples)},
            {"sampleCount", QString::number(samples)}, {"byteCount", QString::number(fileBytes)},
            {"gapBefore", gapBefore}, {"startReason", startReason},
            {"finalized", finalized}, {"stopReason", reason}
        };
    }

    QString saveMetadata(bool finalized, const QString &reason, qint64 fileBytes) {
        QSaveFile file(metadataPath);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) return ioError(QStringLiteral("Cannot open recording sidecar"), file);
        if (!writeAll(file, QJsonDocument(metadata(finalized, reason, fileBytes)).toJson())) {
            const QString error = ioError(QStringLiteral("Cannot write recording sidecar"), file);
            file.cancelWriting();
            return error;
        }
        if (!file.commit()) return ioError(QStringLiteral("Cannot finalize recording sidecar"), file);
        return {};
    }

    QString openSegment(const DspStreamBlock *block, const QString &reason) {
        const QString previousPath = currentPath;
        const QString previousMetadataPath = metadataPath;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const QString base = QDir(directory).filePath(QStringLiteral("dsp-stream-") +
                QUuid::createUuid().toString(QUuid::WithoutBraces));
            data.setFileName(base + QStringLiteral(".cf32"));
            if (!data.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Unbuffered)) {
                if (QFileInfo::exists(data.fileName())) continue;
                return ioError(QStringLiteral("Cannot create IQ recording"), data);
            }
            QFile sidecar(base + QStringLiteral(".json"));
            if (!sidecar.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                const QString error = ioError(QStringLiteral("Cannot create recording sidecar"), sidecar);
                const bool collision = QFileInfo::exists(sidecar.fileName());
                data.close();
                if (!data.remove()) return ioError(QStringLiteral("Cannot remove empty IQ recording"), data);
                if (collision) continue;
                return error;
            }
            currentPath = data.fileName();
            metadataPath = sidecar.fileName();
            samples = 0;
            firstSample = 0;
            metadataKnown = false;
            gapBefore = false;
            intact = true;
            startReason = reason;
            ++segments;
            if (block) setMetadata(*block);
            const bool written = writeAll(sidecar, QJsonDocument(metadata(false, {}, 0)).toJson()) && sidecar.flush();
            const QString error = written ? QString() : ioError(QStringLiteral("Cannot initialize recording sidecar"), sidecar);
            sidecar.close();
            if (!written) {
                data.close();
                const bool removedData = data.remove();
                const bool removedMetadata = sidecar.remove();
                currentPath = previousPath;
                metadataPath = previousMetadataPath;
                return error + ((!removedData || !removedMetadata) ? QStringLiteral("; cannot remove incomplete recording pair") : QString());
            }
            return {};
        }
        return QStringLiteral("Cannot reserve a unique recording pair");
    }

    QString finishSegment(const QString &reason) {
        if (!data.isOpen()) return {};
        QString error;
        if (!data.flush()) {
            intact = false;
            error = ioError(QStringLiteral("Cannot flush IQ recording"), data);
        }
        const qint64 fileBytes = data.size();
        if (fileBytes < 0 || quint64(fileBytes) != samples * SampleBytes) {
            intact = false;
            if (error.isEmpty()) error = QStringLiteral("IQ recording size does not match complete samples");
        }
        data.close();
        const QString metadataError = saveMetadata(intact, intact ? reason : QStringLiteral("write_error"), fileBytes);
        if (!metadataError.isEmpty()) {
            if (!error.isEmpty()) error += QStringLiteral("; ");
            error += metadataError;
        }
        return error;
    }

    void end(const QString &reason, const QString &result) {
        const QString error = finishSegment(reason);
        recording = false;
        message = error.isEmpty() ? result : QStringLiteral("Error: ") + error + QStringLiteral("; ") + result;
    }
};

DspStreamRecorder::DspStreamRecorder() : impl(std::make_unique<Impl>()) {}
DspStreamRecorder::~DspStreamRecorder() { stop(); }

bool DspStreamRecorder::start(const QString &directory, quint64 maxBytes) {
    auto &s = *impl;
    if (s.recording) {
        s.message = QStringLiteral("Error: recording already active; stop it before starting a new session");
        return false;
    }
    s.currentPath.clear();
    s.metadataPath.clear();
    const QFileInfo info(directory);
    if (directory.trimmed().isEmpty() || !QDir::isAbsolutePath(directory) || !info.isDir()) {
        s.message = QStringLiteral("Error: select an existing absolute recording directory");
        return false;
    }
    if (maxBytes < SampleBytes || maxBytes > quint64((std::numeric_limits<qint64>::max)())) {
        s.message = QStringLiteral("Error: invalid recording byte limit");
        return false;
    }
    s.directory = info.canonicalFilePath();
    if (s.directory.isEmpty()) {
        s.message = QStringLiteral("Error: recording directory is unavailable");
        return false;
    }
    s.limit = maxBytes - maxBytes % SampleBytes;
    s.totalBytes = 0;
    s.segments = 0;
    s.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString error = s.openSegment(nullptr, QStringLiteral("session_start"));
    if (!error.isEmpty()) {
        s.message = QStringLiteral("Error: ") + error;
        return false;
    }
    s.recording = true;
    s.message = QStringLiteral("Recording: waiting for channel IQ");
    return true;
}

void DspStreamRecorder::append(const DspStreamBlock &block, const std::vector<float> &processedIq) {
    auto &s = *impl;
    if (!s.recording) return;
    QString error = DspStreamProtocol::validate(block);
    if (error.isEmpty() && (processedIq.size() != block.iq.size() ||
        !std::all_of(processedIq.begin(), processedIq.end(), [](float value) { return std::isfinite(value); }))) {
        error = QStringLiteral("Invalid processed IQ size or non-finite samples");
    }
    if (!error.isEmpty()) {
        s.end(QStringLiteral("invalid_block"), QStringLiteral("Error: ") + error);
        return;
    }

    QString boundary;
    if (s.metadataKnown) {
        if (s.stream != block.stream || s.sampleRate != block.sampleRate || s.centerHz != block.centerHz ||
            s.generation != block.generation || s.epoch != block.epoch) boundary = QStringLiteral("metadata_change");
        else if (block.gap) boundary = QStringLiteral("gap");
        else if (block.firstSample != s.firstSample + s.samples) boundary = QStringLiteral("noncontiguous_sample");
    }
    if (!boundary.isEmpty()) {
        if (s.segments >= MaxSegments) {
            s.end(QStringLiteral("segment_limit"), QStringLiteral("Stopped: 128-segment recording limit reached"));
            return;
        }
        error = s.finishSegment(boundary);
        if (error.isEmpty()) error = s.openSegment(&block, boundary);
        if (!error.isEmpty()) {
            s.recording = false;
            s.message = QStringLiteral("Error: ") + error;
            return;
        }
    } else if (!s.metadataKnown) {
        s.setMetadata(block);
        error = s.saveMetadata(false, {}, 0);
        if (!error.isEmpty()) {
            s.end(QStringLiteral("metadata_error"), QStringLiteral("Error: ") + error);
            return;
        }
    }

    const QByteArray bytes = DspStreamProtocol::floats(processedIq);
    if (bytes.isEmpty() || std::size_t(bytes.size()) != processedIq.size() * sizeof(float)) {
        s.end(QStringLiteral("invalid_block"), QStringLiteral("Error: cannot encode processed IQ within the recording block limit"));
        return;
    }
    const qint64 wanted = qint64((std::min)(quint64(bytes.size()), s.limit - s.totalBytes));
    qint64 written = 0;
    while (written < wanted) {
        const qint64 n = s.data.write(bytes.constData() + written, wanted - written);
        if (n <= 0) {
            error = ioError(QStringLiteral("Cannot write IQ recording"), s.data);
            break;
        }
        written += n;
    }
    // A short write may end between I and Q or within a float. Preserve only
    // complete samples, and never mark an untrimmed partial tail finalized.
    const qint64 completeBytes = written - written % qint64(SampleBytes);
    if (written != completeBytes && !s.data.resize(qint64(s.samples * SampleBytes) + completeBytes)) {
        s.intact = false;
        error += QStringLiteral("; cannot trim incomplete IQ sample");
    }
    s.samples += quint64(completeBytes) / SampleBytes;
    s.totalBytes += quint64(completeBytes);
    if (error.isEmpty() && !s.data.flush()) {
        s.intact = false;
        error = ioError(QStringLiteral("Cannot flush IQ recording"), s.data);
    }
    if (!error.isEmpty()) {
        s.end(QStringLiteral("write_error"), QStringLiteral("Error: ") + error);
    } else if (s.totalBytes == s.limit) {
        s.end(QStringLiteral("byte_limit"), QStringLiteral("Stopped: recording byte limit reached"));
    } else {
        s.message = QStringLiteral("Recording: segment %1, %2 IQ bytes in session").arg(s.segments).arg(s.totalBytes);
    }
}

void DspStreamRecorder::stop() {
    if (impl->recording) impl->end(QStringLiteral("user_stop"), QStringLiteral("Stopped: recording finalized"));
}
bool DspStreamRecorder::active() const { return impl->recording; }
QString DspStreamRecorder::status() const { return impl->message; }
QString DspStreamRecorder::path() const { return impl->currentPath; }
