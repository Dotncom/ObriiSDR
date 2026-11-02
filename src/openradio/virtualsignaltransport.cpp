#include "virtualsignaltransport.h"

#include <QDataStream>
#include <QSysInfo>
#include <QtEndian>
#include <QVector>

#include <complex>
#include <cstring>

namespace {
constexpr int LengthPrefixBytes = 4;

void setError(QString *error, const QString &message) {
    if (error) *error = message;
}
}

void VirtualSignalStreamParser::append(const QByteArray &bytes) {
    buffer.append(bytes);
}

bool VirtualSignalStreamParser::takeFrame(VirtualSignalFrame *frame, QString *error) {
    if (!frame) {
        setError(error, QStringLiteral("Virtual signal output frame is null"));
        return false;
    }
    if (buffer.size() < LengthPrefixBytes) {
        if (error) error->clear();
        return false;
    }
    QDataStream prefix(buffer.left(LengthPrefixBytes));
    prefix.setByteOrder(QDataStream::BigEndian);
    quint32 wireBytes = 0;
    prefix >> wireBytes;
    if (wireBytes == 0 || wireBytes > MaximumWireBytes) {
        buffer.clear();
        setError(error, QStringLiteral("Invalid virtual signal stream frame length"));
        return false;
    }
    if (buffer.size() < LengthPrefixBytes + static_cast<int>(wireBytes)) {
        if (error) error->clear();
        return false;
    }
    const QByteArray wire = buffer.mid(LengthPrefixBytes, static_cast<int>(wireBytes));
    buffer.remove(0, LengthPrefixBytes + static_cast<int>(wireBytes));
    return VirtualSignalFrameCodec::decode(wire, frame, error);
}

void VirtualSignalStreamParser::clear() {
    buffer.clear();
}

qsizetype VirtualSignalStreamParser::bufferedBytes() const {
    return buffer.size();
}

QByteArray VirtualSignalStreamParser::wrap(const VirtualSignalFrame &frame, QString *error) {
    const QByteArray wire = VirtualSignalFrameCodec::encode(frame, error);
    if (wire.isEmpty()) return {};
    if (wire.size() > static_cast<int>(MaximumWireBytes)) {
        setError(error, QStringLiteral("Virtual signal transport frame exceeds 1 MiB"));
        return {};
    }
    QByteArray framed;
    framed.reserve(LengthPrefixBytes + wire.size());
    QDataStream stream(&framed, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << static_cast<quint32>(wire.size());
    framed += wire;
    return framed;
}

QByteArray virtualSignalComplexFloat32Payload(const std::complex<float> *samples, int count) {
    if (!samples || count <= 0) return {};
    QByteArray payload(count * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    if (QSysInfo::ByteOrder == QSysInfo::LittleEndian) {
        std::memcpy(payload.data(), samples, static_cast<size_t>(payload.size()));
        return payload;
    }
    auto *destination = reinterpret_cast<uchar *>(payload.data());
    for (int index = 0; index < count; ++index) {
        quint32 realBits = 0;
        quint32 imagBits = 0;
        const float real = samples[index].real();
        const float imag = samples[index].imag();
        std::memcpy(&realBits, &real, sizeof(realBits));
        std::memcpy(&imagBits, &imag, sizeof(imagBits));
        qToLittleEndian(realBits, destination + index * 8);
        qToLittleEndian(imagBits, destination + index * 8 + 4);
    }
    return payload;
}

bool virtualSignalValidNodeId(const QString &id) {
    if (id.isEmpty() || id.size() > 64) return false;
    for (int index = 0; index < id.size(); ++index) {
        const ushort c = id.at(index).unicode();
        const bool alphanumeric = (c >= 'a' && c <= 'z') ||
                                  (c >= 'A' && c <= 'Z') ||
                                  (c >= '0' && c <= '9');
        if (!alphanumeric && (index == 0 || (c != '-' && c != '_' && c != '.')))
            return false;
    }
    if (id.startsWith(QStringLiteral("tx")) && id.size() > 2) {
        bool reserved = true;
        for (int index = 2; index < id.size(); ++index) {
            const ushort c = id.at(index).unicode();
            if (c < '0' || c > '9') { reserved = false; break; }
        }
        if (reserved) return false;
    }
    return true;
}

bool virtualSignalDecodeComplexFloat32(const QByteArray &payload, QVector<float> *interleaved,
                                       QString *error) {
    if (!interleaved || payload.isEmpty() || payload.size() % (2 * static_cast<int>(sizeof(float))) != 0) {
        setError(error, QStringLiteral("Virtual CF32 payload has an invalid length"));
        return false;
    }
    interleaved->resize(payload.size() / static_cast<int>(sizeof(float)));
    if (QSysInfo::ByteOrder == QSysInfo::LittleEndian) {
        std::memcpy(interleaved->data(), payload.constData(), static_cast<size_t>(payload.size()));
    } else {
        const auto *source = reinterpret_cast<const uchar *>(payload.constData());
        for (int index = 0; index < interleaved->size(); ++index) {
            const quint32 hostBits = qFromLittleEndian<quint32>(source + index * 4);
            std::memcpy(interleaved->data() + index, &hostBits, sizeof(hostBits));
        }
    }
    if (error) error->clear();
    return true;
}
