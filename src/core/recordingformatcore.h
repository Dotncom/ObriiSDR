#ifndef RECORDINGFORMATCORE_H
#define RECORDINGFORMATCORE_H

#include <QByteArray>
#include <QIODevice>
#include <QJsonObject>
#include <QString>
#include <cstddef>

// Borrowed, seekable sinks. Opening, closing, naming, scheduling and failure
// latching belong to the caller. A failed append never commits its byte count.
namespace RecordingFormatCore {

struct WaveLayout {
    quint64 headerBytes = 44;
    qint64 dataSizeFieldOffset = 40;
    quint16 blockAlign = 2;
};

bool checkedAppend(QIODevice &sink, const char *data, qint64 size, QString *error = nullptr);
bool beginWave(QIODevice &sink, quint32 sampleRate, quint16 channels, quint16 bitsPerSample,
               const QByteArray &fbMetadata, WaveLayout &layout, QString *error = nullptr);
quint64 waveDataLimit(const WaveLayout &layout);
bool appendWave(QIODevice &sink, const WaveLayout &layout, quint64 &dataBytes,
                const char *data, qint64 size, QString *error = nullptr);
bool finalizeWave(QIODevice &sink, const WaveLayout &layout, quint64 dataBytes,
                  QString *error = nullptr);

enum class IqEncoding { Cf32Le, Ci8, Ci16Le };
enum class IqNonFinitePolicy { Reject, Zero };
struct EncodedIq {
    const char *data = nullptr;
    qint64 size = 0;
};
// CF32 preserves all bits (including non-finite evidence) and borrows input on
// little-endian hosts without scanning it. Quantization handles non-finite
// components within its conversion pass using the selected policy. It reuses scratch;
// the returned view lasts until that input/scratch is modified or destroyed.
bool encodeIq(const float *iq, std::size_t components, IqEncoding encoding,
              QByteArray &scratch, EncodedIq &encoded, QString *error = nullptr,
              IqNonFinitePolicy nonFinite = IqNonFinitePolicy::Reject);

struct SpectrumFrameView {
    qint64 elapsedMs = 0;
    qint64 utcMs = 0;
    double centerFrequency = 0;
    double minFrequency = 0;
    double maxFrequency = 0;
    int fftLength = 0;
    const float *magnitudes = nullptr;
    std::size_t count = 0;
};
constexpr quint64 SpectrumFrameHeaderBytes = 52;
bool beginSpectrum(QIODevice &sink, const QJsonObject &metadata, QString *error = nullptr);
bool appendSpectrum(QIODevice &sink, const SpectrumFrameView &frame, QString *error = nullptr);

QJsonObject sigMfMetadata(const QString &datatype, double sampleRate, const QString &dataset,
                         double centerFrequency, qint64 firstUtcMs,
                         const QJsonObject &globalExtensions = {});

} // namespace RecordingFormatCore
#endif
