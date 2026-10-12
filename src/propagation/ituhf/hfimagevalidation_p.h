#ifndef HF_IMAGE_VALIDATION_P_H
#define HF_IMAGE_VALIDATION_P_H

#include <QByteArray>
#include <QtEndian>

namespace hf {
namespace detail {
struct ElfTarget {
    quint16 machine;
    int pointerBytes;
    int byteOrder;
};

constexpr ElfTarget hostElfTarget() {
#if defined(Q_PROCESSOR_ARM_64)
    constexpr quint16 machine = 183; // EM_AARCH64
#elif defined(Q_PROCESSOR_ARM_32)
    constexpr quint16 machine = 40; // EM_ARM
#elif defined(Q_PROCESSOR_X86_64)
    constexpr quint16 machine = 62; // EM_X86_64
#elif defined(Q_PROCESSOR_X86_32)
    constexpr quint16 machine = 3; // EM_386
#else
    constexpr quint16 machine = 0; // Unsupported targets fail closed.
#endif
    return {machine, QT_POINTER_SIZE, Q_BYTE_ORDER};
}

inline bool elfHeaderMatches(const QByteArray &header, ElfTarget target) {
    if (!target.machine || (target.pointerBytes != 4 && target.pointerBytes != 8)
        || (target.byteOrder != Q_LITTLE_ENDIAN && target.byteOrder != Q_BIG_ENDIAN)) return false;
    const int size = target.pointerBytes == 8 ? 64 : 52;
    const char elfClass = target.pointerBytes == 8 ? 2 : 1;
    const char byteOrder = target.byteOrder == Q_LITTLE_ENDIAN ? 1 : 2;
    if (header.size() < size || header.left(4) != QByteArray("\x7f" "ELF", 4)
        || header[4] != elfClass || header[5] != byteOrder || header[6] != 1) return false;
    auto word = [&](int offset) {
        const auto *bytes = reinterpret_cast<const uchar *>(header.constData() + offset);
        return target.byteOrder == Q_LITTLE_ENDIAN ? qFromLittleEndian<quint16>(bytes) : qFromBigEndian<quint16>(bytes);
    };
    const auto *version = reinterpret_cast<const uchar *>(header.constData() + 20);
    const quint32 versionNumber = target.byteOrder == Q_LITTLE_ENDIAN
        ? qFromLittleEndian<quint32>(version) : qFromBigEndian<quint32>(version);
    // ET_DYN covers both shared libraries and position-independent executables.
    return (word(16) == 2 || word(16) == 3) && word(18) == target.machine
        && versionNumber == 1 && word(target.pointerBytes == 8 ? 52 : 40) == size;
}

inline bool peX64HeaderMatches(const QByteArray &header) {
    if (header.size() < 26 || header.left(4) != QByteArray("PE\0\0", 4)) return false;
    const auto *bytes = reinterpret_cast<const uchar *>(header.constData());
    return qFromLittleEndian<quint16>(bytes + 4) == 0x8664 // IMAGE_FILE_MACHINE_AMD64
        && qFromLittleEndian<quint16>(bytes + 20) >= 2
        && qFromLittleEndian<quint16>(bytes + 24) == 0x20b; // PE32+
}
}
}
#endif
