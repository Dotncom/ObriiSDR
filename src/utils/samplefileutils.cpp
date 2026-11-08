#include "samplefileutils.h"

#include <algorithm>
#include <cmath>

void appendLe32(QByteArray &buffer, quint32 value) {
    buffer.append(static_cast<char>(value & 0xff));
    buffer.append(static_cast<char>((value >> 8) & 0xff));
    buffer.append(static_cast<char>((value >> 16) & 0xff));
    buffer.append(static_cast<char>((value >> 24) & 0xff));
}

void appendLe16(QByteArray &buffer, quint16 value) {
    buffer.append(static_cast<char>(value & 0xff));
    buffer.append(static_cast<char>((value >> 8) & 0xff));
}

quint32 readLe32(const char *data) {
    const auto *bytes = reinterpret_cast<const uchar *>(data);
    return static_cast<quint32>(bytes[0]) |
           (static_cast<quint32>(bytes[1]) << 8) |
           (static_cast<quint32>(bytes[2]) << 16) |
           (static_cast<quint32>(bytes[3]) << 24);
}

qint16 readLeI16(const char *data) {
    const auto *bytes = reinterpret_cast<const uchar *>(data);
    return static_cast<qint16>(static_cast<quint16>(bytes[0]) |
                               (static_cast<quint16>(bytes[1]) << 8));
}

void appendIqS16LeToFloat(const QByteArray &iqData, int sampleCount, std::vector<float> &output) {
    constexpr int bytesPerIq = 2 * static_cast<int>(sizeof(qint16));
    if (iqData.size() < bytesPerIq) {
        return;
    }
    const int iqCount = sampleCount > 0 ? sampleCount : iqData.size() / bytesPerIq;
    output.reserve(output.size() + static_cast<std::size_t>(iqCount) * 2U);
    for (int offset = 0, n = 0; offset + bytesPerIq - 1 < iqData.size() && n < iqCount;
         offset += bytesPerIq, ++n) {
        const qint16 iSample = readLeI16(iqData.constData() + offset);
        const qint16 qSample = readLeI16(iqData.constData() + offset + 2);
        output.push_back(iSample / 32768.0f);
        output.push_back(qSample / 32768.0f);
    }
}

void appendIqS8ToFloat(const QByteArray &iqData, int sampleCount, std::vector<float> &output) {
    if (iqData.size() < 2) {
        return;
    }
    const int iqCount = sampleCount > 0 ? sampleCount : iqData.size() / 2;
    output.reserve(output.size() + static_cast<std::size_t>(iqCount) * 2U);
    const auto *src = reinterpret_cast<const qint8 *>(iqData.constData());
    for (int offset = 0, n = 0; offset + 1 < iqData.size() && n < iqCount; offset += 2, ++n) {
        output.push_back(src[offset] / 128.0f);
        output.push_back(src[offset + 1] / 128.0f);
    }
}

QByteArray streamingWavHeader() {
    constexpr quint32 streamDataBytes = 0x7fffffffU;
    constexpr quint16 channels = 1;
    constexpr quint32 sampleRate = 48000;
    constexpr quint16 bitsPerSample = 16;
    constexpr quint16 blockAlign = channels * bitsPerSample / 8;
    constexpr quint32 byteRate = sampleRate * blockAlign;

    QByteArray header;
    header.reserve(44);
    header.append("RIFF", 4);
    appendLe32(header, streamDataBytes + 36U);
    header.append("WAVE", 4);
    header.append("fmt ", 4);
    appendLe32(header, 16);
    appendLe16(header, 1);
    appendLe16(header, channels);
    appendLe32(header, sampleRate);
    appendLe32(header, byteRate);
    appendLe16(header, blockAlign);
    appendLe16(header, bitsPerSample);
    header.append("data", 4);
    appendLe32(header, streamDataBytes);
    return header;
}

qint16 pcm16FromFloat(float sample) {
    if (!std::isfinite(sample)) {
        sample = 0.0f;
    }
    const float clamped = (std::clamp)(sample, -1.0f, 1.0f);
    return static_cast<qint16>(std::lrint(clamped * 32767.0f));
}

QByteArray fixedPcm16StereoWavHeader(int sampleRate, quint32 dataBytes) {
    constexpr quint16 channels = 2;
    constexpr quint16 bitsPerSample = 16;
    constexpr quint16 blockAlign = channels * bitsPerSample / 8;
    const quint32 byteRate = static_cast<quint32>(sampleRate) * blockAlign;

    QByteArray header;
    header.reserve(44);
    header.append("RIFF", 4);
    appendLe32(header, dataBytes + 36U);
    header.append("WAVE", 4);
    header.append("fmt ", 4);
    appendLe32(header, 16);
    appendLe16(header, 1);
    appendLe16(header, channels);
    appendLe32(header, static_cast<quint32>(sampleRate));
    appendLe32(header, byteRate);
    appendLe16(header, blockAlign);
    appendLe16(header, bitsPerSample);
    header.append("data", 4);
    appendLe32(header, dataBytes);
    return header;
}
