#include "recordingformatcore.h"

#include <QBuffer>
#include <QDateTime>
#include <QFileDevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace RecordingFormatCore {
namespace {
constexpr quint64 RiffLimit = 0xffffffffULL;
constexpr int MetadataLimit = 16 * 1024 * 1024;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE float32 required");
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559, "IEEE float64 required");

bool fail(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}
template<class T> void put(char *destination, T value) {
    qToLittleEndian<T>(value, reinterpret_cast<uchar *>(destination));
}
void putDouble(char *destination, double value) {
    quint64 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put(destination, bits);
}
bool usable(QIODevice &sink, QString *error) {
    return (sink.isOpen() && sink.isWritable() && !sink.isSequential()) ||
           fail(error, QStringLiteral("recording requires an open, writable, seekable sink"));
}
bool truncate(QIODevice &sink, qint64 size) {
    if (sink.size() == size) return true;
    if (auto *file = qobject_cast<QFileDevice *>(&sink)) return file->resize(size);
    if (auto *buffer = qobject_cast<QBuffer *>(&sink)) {
        if (size < 0 || size > std::numeric_limits<int>::max()) return false;
        buffer->buffer().resize(int(size));
        return true;
    }
    return false;
}
bool flush(QIODevice &sink, QString *error) {
    if (auto *file = qobject_cast<QFileDevice *>(&sink)) {
        if (!file->flush()) return fail(error, QStringLiteral("recording flush failed: %1").arg(sink.errorString()));
    }
    return true;
}
bool rollback(QIODevice &sink, qint64 before, QString *error) {
    const bool resized = truncate(sink, before);
    const bool sought = sink.seek(before);
    if (!resized || !sought) {
        if (error) *error += QStringLiteral("; prefix rollback failed");
        return false;
    }
    return true;
}
bool write(QIODevice &sink, const char *data, qint64 size, QString *error) {
    if (size == 0) return true;
    if (sink.write(data, size) == size) return true;
    return fail(error, QStringLiteral("recording write failed: %1").arg(sink.errorString()));
}
bool appendPosition(QIODevice &sink, QString *error) {
    // The wrapper owns the append cursor. Do not stat the file on every frame.
    return usable(sink, error) &&
           ((sink.pos() >= 0) || fail(error, QStringLiteral("invalid recording append position")));
}
bool writeFloats(QIODevice &sink, const float *values, std::size_t count, QString *error) {
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    return write(sink, reinterpret_cast<const char *>(values), qint64(count * sizeof(float)), error);
#else
    char bytes[4096];
    while (count) {
        const std::size_t n = (std::min)(count, sizeof(bytes) / sizeof(float));
        for (std::size_t i = 0; i < n; ++i) {
            quint32 bits;
            std::memcpy(&bits, values + i, sizeof(bits));
            put(bytes + i * sizeof(float), bits);
        }
        if (!write(sink, bytes, qint64(n * sizeof(float)), error)) return false;
        values += n;
        count -= n;
    }
    return true;
#endif
}
} // namespace

bool checkedAppend(QIODevice &sink, const char *data, qint64 size, QString *error) {
    if (size < 0 || (!data && size)) return fail(error, QStringLiteral("invalid recording byte span"));
    if (!appendPosition(sink, error)) return false;
    const qint64 before = sink.pos();
    if (size > std::numeric_limits<qint64>::max() - before)
        return fail(error, QStringLiteral("recording size overflow"));
    if (write(sink, data, size, error)) return true;
    rollback(sink, before, error);
    return false;
}

bool beginWave(QIODevice &sink, quint32 sampleRate, quint16 channels, quint16 bitsPerSample,
               const QByteArray &fbMetadata, WaveLayout &layout, QString *error) {
    if (!usable(sink, error)) return false;
    if (sink.pos() != 0 || sink.size() != 0)
        return fail(error, QStringLiteral("WAV requires an empty sink"));
    if (!sampleRate || (channels != 1 && channels != 2) || bitsPerSample != 16 ||
        quint64(sampleRate) * channels * 2 > RiffLimit || fbMetadata.size() > MetadataLimit)
        return fail(error, QStringLiteral("invalid PCM16 WAV format or metadata size"));
    WaveLayout next;
    next.blockAlign = channels * 2;
    const int metadataBytes = fbMetadata.isEmpty() ? 0 : 8 + fbMetadata.size() + (fbMetadata.size() & 1);
    char header[44] = {};
    std::memcpy(header, "RIFF", 4);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    put(header + 16, quint32(16));
    put(header + 20, quint16(1));
    put(header + 22, channels);
    put(header + 24, sampleRate);
    put(header + 28, quint32(sampleRate * next.blockAlign));
    put(header + 32, next.blockAlign);
    put(header + 34, bitsPerSample);
    std::memcpy(header + 36, "data", 4);
    bool written;
    if (!metadataBytes) {
        written = write(sink, header, sizeof(header), error);
    } else {
        char chunk[8] = {'f', 'b', 'o', 's', 0, 0, 0, 0};
        put(chunk + 4, quint32(fbMetadata.size()));
        const char padding = '\0';
        written = write(sink, header, 36, error) &&
                  write(sink, chunk, sizeof(chunk), error) &&
                  write(sink, fbMetadata.constData(), fbMetadata.size(), error) &&
                  (!(fbMetadata.size() & 1) || write(sink, &padding, 1, error)) &&
                  write(sink, header + 36, 8, error);
    }
    if (!written) {
        rollback(sink, 0, error);
        return false;
    }
    next.dataSizeFieldOffset = 40 + metadataBytes;
    next.headerBytes = sizeof(header) + quint64(metadataBytes);
    layout = next;
    return true;
}

quint64 waveDataLimit(const WaveLayout &layout) {
    if (layout.headerBytes < 44 || layout.headerBytes - 8 > RiffLimit || !layout.blockAlign) return 0;
    return (RiffLimit - (layout.headerBytes - 8)) / layout.blockAlign * layout.blockAlign;
}

bool appendWave(QIODevice &sink, const WaveLayout &layout, quint64 &dataBytes,
                const char *data, qint64 size, QString *error) {
    const quint64 limit = waveDataLimit(layout);
    if (size < 0 || !layout.blockAlign || quint64(size) % layout.blockAlign || dataBytes % layout.blockAlign)
        return fail(error, QStringLiteral("unaligned PCM16 WAV payload"));
    if (dataBytes > limit || quint64(size) > limit - dataBytes)
        return fail(error, QStringLiteral("RIFF WAV size limit reached"));
    if (sink.pos() < 0 || quint64(sink.pos()) != layout.headerBytes + dataBytes)
        return fail(error, QStringLiteral("WAV sink is not at its complete prefix"));
    if (!checkedAppend(sink, data, size, error)) return false;
    dataBytes += quint64(size);
    return true;
}

bool finalizeWave(QIODevice &sink, const WaveLayout &layout, quint64 dataBytes, QString *error) {
    if (!usable(sink, error)) return false;
    if (layout.headerBytes < 44 || layout.headerBytes - 8 > RiffLimit ||
        !layout.blockAlign || dataBytes % layout.blockAlign || dataBytes > waveDataLimit(layout) ||
        layout.dataSizeFieldOffset < 40 || quint64(layout.dataSizeFieldOffset) + 4 != layout.headerBytes)
        return fail(error, QStringLiteral("invalid WAV finalization layout or size"));
    const qint64 end = qint64(layout.headerBytes + dataBytes);
    if (sink.size() < end || !truncate(sink, end))
        return fail(error, QStringLiteral("cannot restore a complete WAV prefix"));
    if (!flush(sink, error)) return false;
    char size[4];
    put(size, quint32(layout.headerBytes - 8 + dataBytes));
    if (!sink.seek(4) || !write(sink, size, 4, error))
        return fail(error, QStringLiteral("cannot finalize WAV RIFF size: %1").arg(sink.errorString()));
    put(size, quint32(dataBytes));
    if (!sink.seek(layout.dataSizeFieldOffset) || !write(sink, size, 4, error) || !sink.seek(end))
        return fail(error, QStringLiteral("cannot finalize WAV data size: %1").arg(sink.errorString()));
    return flush(sink, error);
}

bool encodeIq(const float *iq, std::size_t components, IqEncoding encoding,
              QByteArray &scratch, EncodedIq &encoded, QString *error, IqNonFinitePolicy nonFinite) {
    encoded = {};
    if ((!iq && components) || components % 2 || components > std::size_t(std::numeric_limits<int>::max() / 4))
        return fail(error, QStringLiteral("invalid interleaved IQ span"));
    if (encoding != IqEncoding::Cf32Le && encoding != IqEncoding::Ci8 && encoding != IqEncoding::Ci16Le)
        return fail(error, QStringLiteral("invalid IQ encoding"));
    if (nonFinite != IqNonFinitePolicy::Reject && nonFinite != IqNonFinitePolicy::Zero)
        return fail(error, QStringLiteral("invalid non-finite IQ policy"));
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    if (encoding == IqEncoding::Cf32Le) {
        encoded = {reinterpret_cast<const char *>(iq), qint64(components * sizeof(float))};
        return true;
    }
#endif
    const int width = encoding == IqEncoding::Cf32Le ? 4 : encoding == IqEncoding::Ci8 ? 1 : 2;
    scratch.resize(int(components * std::size_t(width)));
    char *destination = scratch.data();
    for (std::size_t i = 0; i < components; ++i) {
        if (encoding == IqEncoding::Cf32Le) {
            quint32 bits;
            std::memcpy(&bits, iq + i, sizeof(bits));
            put(destination + i * 4, bits);
        } else {
            float value = iq[i];
            if (!std::isfinite(value)) {
                if (nonFinite == IqNonFinitePolicy::Reject) return fail(error, QStringLiteral("non-finite IQ sample"));
                value = 0;
            }
            if (encoding == IqEncoding::Ci8) {
                destination[i] = char(std::lround((std::clamp)(value, -1.0f, 1.0f) * 127.0f));
            } else {
                put(destination + i * 2, quint16(qint16(std::lround((std::clamp)(value, -1.0f, 1.0f) * 32767.0f))));
            }
        }
    }
    encoded = {scratch.constData(), scratch.size()};
    return true;
}

bool beginSpectrum(QIODevice &sink, const QJsonObject &metadata, QString *error) {
    if (!usable(sink, error)) return false;
    if (sink.pos() != 0 || sink.size() != 0)
        return fail(error, QStringLiteral("spectrum recording requires an empty sink"));
    const QByteArray json = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    if (json.isEmpty() || json.size() > MetadataLimit)
        return fail(error, QStringLiteral("invalid spectrum recording metadata size"));
    char header[12];
    std::memcpy(header, "FBSPEC1\n", 8);
    put(header + 8, quint32(json.size()));
    if (write(sink, header, sizeof(header), error) && write(sink, json.constData(), json.size(), error)) return true;
    rollback(sink, 0, error);
    return false;
}

bool appendSpectrum(QIODevice &sink, const SpectrumFrameView &frame, QString *error) {
    if (!frame.magnitudes || !frame.count || frame.count > 2097152 || frame.fftLength <= 0 ||
        !std::isfinite(frame.centerFrequency) || !std::isfinite(frame.minFrequency) ||
        !std::isfinite(frame.maxFrequency) || frame.maxFrequency < frame.minFrequency)
        return fail(error, QStringLiteral("invalid spectrum frame geometry or bin count"));
    if (!appendPosition(sink, error)) return false;
    const qint64 before = sink.pos();
    if (quint64(std::numeric_limits<qint64>::max() - before) < SpectrumFrameHeaderBytes + frame.count * 4)
        return fail(error, QStringLiteral("spectrum recording size overflow"));
    char header[SpectrumFrameHeaderBytes];
    put(header, quint32(0x314d5246U));
    put(header + 4, quint64(frame.elapsedMs));
    put(header + 12, quint64(frame.utcMs));
    putDouble(header + 20, frame.centerFrequency);
    putDouble(header + 28, frame.minFrequency);
    putDouble(header + 36, frame.maxFrequency);
    put(header + 44, quint32(frame.fftLength));
    put(header + 48, quint32(frame.count));
    if (write(sink, header, sizeof(header), error) && writeFloats(sink, frame.magnitudes, frame.count, error)) return true;
    rollback(sink, before, error);
    return false;
}

QJsonObject sigMfMetadata(const QString &datatype, double sampleRate, const QString &dataset,
                         double centerFrequency, qint64 firstUtcMs, const QJsonObject &globalExtensions) {
    QJsonObject global = globalExtensions;
    global["core:datatype"] = datatype;
    global["core:sample_rate"] = sampleRate;
    global["core:version"] = QStringLiteral("1.0.0");
    global["core:recorder"] = QStringLiteral("Obrii SDR");
    global["core:dataset"] = dataset;
    QJsonObject capture{{"core:sample_start", 0}, {"core:frequency", centerFrequency}};
    if (firstUtcMs > 0)
        capture["core:datetime"] = QDateTime::fromMSecsSinceEpoch(firstUtcMs, Qt::UTC).toString(Qt::ISODateWithMs);
    return {{"global", global}, {"captures", QJsonArray{capture}}, {"annotations", QJsonArray{}}};
}
} // namespace RecordingFormatCore
