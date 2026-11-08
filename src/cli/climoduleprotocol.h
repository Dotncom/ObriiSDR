#ifndef OBRII_CLI_MODULE_PROTOCOL_H
#define OBRII_CLI_MODULE_PROTOCOL_H

#include <QByteArray>
#include <QtEndian>

#include <cstring>

namespace CliModuleProtocol {
constexpr quint32 Reset = 0;
constexpr quint32 Pcm16 = 1;
constexpr quint32 ChannelIqF32 = 2;
constexpr quint32 Configure = 3;
constexpr quint32 Snapshot = 4;
constexpr quint32 SpectrumF32 = 5;
constexpr quint32 MaximumPayloadBytes = 1024 * 1024;

inline QByteArray frame(quint32 kind, quint32 sampleRate, const char *data, quint32 size) {
    QByteArray result;
    result.resize(static_cast<int>(12 + size));
    auto *header = reinterpret_cast<uchar *>(result.data());
    qToLittleEndian(kind, header);
    qToLittleEndian(sampleRate, header + 4);
    qToLittleEndian(size, header + 8);
    if (size) std::memcpy(result.data() + 12, data, size);
    return result;
}
}

#endif
