#include "m17adapter.h"

extern "C" {
#include "m17.h"
}

#include <array>
#include <cstring>

namespace {
void setError(QString *error, const QString &value) {
    if (error) *error = value;
}

void appendSymbols(QVector<qint8> *target, const int8_t *symbols, int count) {
    if (!target || !symbols || count <= 0) return;
    target->reserve(target->size() + count);
    for (int i = 0; i < count; ++i) target->push_back(static_cast<qint8>(symbols[i]));
}

QByteArray serializeLsf(const lsf_t &lsf) {
    QByteArray bytes;
    bytes.reserve(30);
    bytes.append(reinterpret_cast<const char *>(lsf.dst), 6);
    bytes.append(reinterpret_cast<const char *>(lsf.src), 6);
    bytes.append(reinterpret_cast<const char *>(lsf.type), 2);
    bytes.append(reinterpret_cast<const char *>(lsf.meta), 14);
    bytes.append(reinterpret_cast<const char *>(lsf.crc), 2);
    return bytes;
}
}

bool M17Adapter::available() {
    return true;
}

QString M17Adapter::libraryVersion() {
    return QString::fromLatin1(LIBM17_VERSION);
}

bool M17Adapter::encodeCallsign(const QString &callsign, QByteArray *encoded, QString *error) {
    if (!encoded) {
        setError(error, QStringLiteral("M17 output callsign buffer is null"));
        return false;
    }
    const QByteArray latin = callsign.trimmed().toUpper().toLatin1();
    if (latin.isEmpty() || latin.size() > 9) {
        setError(error, QStringLiteral("M17 callsign must contain 1..9 characters"));
        return false;
    }
    std::array<uint8_t, 6> bytes{};
    if (encode_callsign_bytes(bytes.data(), latin.constData()) != 0) {
        setError(error, QStringLiteral("libm17 rejected the callsign"));
        return false;
    }
    *encoded = QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()));
    if (error) error->clear();
    return true;
}

QString M17Adapter::decodeCallsign(const QByteArray &encoded, QString *error) {
    if (encoded.size() != 6) {
        setError(error, QStringLiteral("An encoded M17 callsign must contain exactly six bytes"));
        return {};
    }
    std::array<char, 16> decoded{};
    decode_callsign_bytes(decoded.data(), reinterpret_cast<const uint8_t *>(encoded.constData()));
    if (!decoded[0]) {
        setError(error, QStringLiteral("M17 callsign is in a reserved address range"));
        return {};
    }
    if (error) error->clear();
    return QString::fromLatin1(decoded.data());
}

M17PacketSymbols M17Adapter::buildPacket(const QString &source,
                                         const QString &destination,
                                         const QByteArray &payload,
                                         int channelAccessNumber,
                                         QString *error) {
    M17PacketSymbols result;
    if (channelAccessNumber < 0 || channelAccessNumber > 15) {
        setError(error, QStringLiteral("M17 CAN must be in the range 0..15"));
        return result;
    }
    if (payload.isEmpty() || payload.size() > 823) {
        setError(error, QStringLiteral("M17 packet payload must contain 1..823 bytes"));
        return result;
    }
    const QString normalizedSource = source.trimmed().toUpper();
    const QString normalizedDestination = destination.trimmed().toUpper();
    if (normalizedSource.isEmpty() || normalizedSource.size() > 9 ||
        normalizedDestination.isEmpty() || normalizedDestination.size() > 9) {
        setError(error, QStringLiteral("M17 source and destination must contain 1..9 characters"));
        return result;
    }
    lsf_t lsf{};
    const QByteArray sourceLatin = normalizedSource.toLatin1();
    const QByteArray destinationLatin = normalizedDestination.toLatin1();
    const uint16_t type = static_cast<uint16_t>(M17_TYPE_PACKET | M17_TYPE_DATA |
                                                M17_TYPE_ENCR_NONE |
                                                M17_TYPE_CAN(channelAccessNumber));
    if (set_LSF(&lsf, sourceLatin.constData(), destinationLatin.constData(), type, nullptr) != 0) {
        setError(error, QStringLiteral("libm17 rejected the source or destination callsign"));
        return result;
    }

    std::array<int8_t, SYM_PER_FRA> symbols{};
    uint32_t count = 0;
    gen_preamble_i8(symbols.data(), &count, PREAM_LSF);
    appendSymbols(&result.symbols, symbols.data(), static_cast<int>(count));
    if (gen_frame_i8(symbols.data(), nullptr, FRAME_LSF, &lsf, 0, 0) != 0) {
        setError(error, QStringLiteral("libm17 failed to generate the link setup frame"));
        result.symbols.clear();
        return result;
    }
    appendSymbols(&result.symbols, symbols.data(), SYM_PER_FRA);

    QByteArray packetData = payload;
    const uint16_t packetCrc = CRC_M17(
        reinterpret_cast<const uint8_t *>(packetData.constData()),
        static_cast<uint16_t>(packetData.size()));
    packetData.append(static_cast<char>((packetCrc >> 8U) & 0xFFU));
    packetData.append(static_cast<char>(packetCrc & 0xFFU));
    const int frameCount = (packetData.size() + 24) / 25;
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        std::array<uint8_t, 26> packet{};
        const int offset = frameIndex * 25;
        const int bytes = qMin(25, packetData.size() - offset);
        std::memcpy(packet.data(), packetData.constData() + offset, static_cast<size_t>(bytes));
        const bool last = frameIndex + 1 == frameCount;
        packet[25] = static_cast<uint8_t>(last
            ? (0x80U | ((bytes & 0x1FU) << 2U))
            : ((frameIndex & 0x1FU) << 2U));
        if (gen_frame_i8(symbols.data(), packet.data(), FRAME_PKT, nullptr, 0, 0) != 0) {
            setError(error, QStringLiteral("libm17 failed to generate packet frame %1").arg(frameIndex));
            result.symbols.clear();
            result.linkSetupFrame.clear();
            return result;
        }
        appendSymbols(&result.symbols, symbols.data(), SYM_PER_FRA);
    }
    count = 0;
    gen_eot_i8(symbols.data(), &count);
    appendSymbols(&result.symbols, symbols.data(), static_cast<int>(count));
    result.linkSetupFrame = serializeLsf(lsf);
    result.packetFrameCount = frameCount;
    if (error) error->clear();
    return result;
}

bool M17Adapter::decodePacketSymbols(const QByteArray &symbols,
                                     M17DecodedPacket *packet, QString *error) {
    if (!packet || symbols.size() < SYM_PER_FRA * 4 ||
        symbols.size() % SYM_PER_FRA != 0 || symbols.size() > SYM_PER_FRA * 36) {
        setError(error, QStringLiteral("Invalid M17 symbol-file length"));
        return false;
    }
    const auto syncMatches = [&symbols](int offset, const int8_t *sync) {
        for (int i = 0; i < SYM_PER_SWD; ++i)
            if (static_cast<qint8>(symbols.at(offset + i)) != sync[i]) return false;
        return true;
    };
    if (!syncMatches(SYM_PER_FRA, lsf_sync_symbols)) {
        setError(error, QStringLiteral("M17 LSF sync word is absent"));
        return false;
    }
    std::array<float, SYM_PER_PLD> soft{};
    const auto payloadSymbols = [&symbols, &soft](int frameOffset) {
        for (int i = 0; i < SYM_PER_PLD; ++i) {
            const qint8 symbol = static_cast<qint8>(symbols.at(frameOffset + SYM_PER_SWD + i));
            if (symbol != -3 && symbol != -1 && symbol != 1 && symbol != 3) return false;
            soft[static_cast<std::size_t>(i)] = static_cast<float>(symbol);
        }
        return true;
    };
    if (!payloadSymbols(SYM_PER_FRA)) {
        setError(error, QStringLiteral("M17 LSF contains invalid symbols"));
        return false;
    }
    lsf_t lsf{};
    decode_LSF(&lsf, soft.data());
    if (CRC_M17(reinterpret_cast<const uint8_t *>(&lsf), sizeof(lsf)) != 0) {
        setError(error, QStringLiteral("M17 LSF CRC failed"));
        return false;
    }
    const int type = (static_cast<int>(lsf.type[0]) << 8) | lsf.type[1];
    if ((type & 1) != M17_TYPE_PACKET || (type & (3 << 3)) != M17_TYPE_ENCR_NONE) {
        setError(error, QStringLiteral("Only unencrypted M17 packet symbol files are supported"));
        return false;
    }
    for (int i = 0; i < SYM_PER_FRA; ++i) {
        if (static_cast<qint8>(symbols.at(symbols.size() - SYM_PER_FRA + i)) !=
            eot_symbols[i % SYM_PER_SWD]) {
            setError(error, QStringLiteral("M17 EOT symbols are missing"));
            return false;
        }
    }

    QByteArray assembled;
    int frameCount = 0;
    bool complete = false;
    for (int offset = SYM_PER_FRA * 2;
         offset + SYM_PER_FRA <= symbols.size() - SYM_PER_FRA; offset += SYM_PER_FRA) {
        if (!syncMatches(offset, pkt_sync_symbols) || !payloadSymbols(offset)) {
            setError(error, QStringLiteral("M17 packet sync or symbols invalid at frame %1").arg(frameCount));
            return false;
        }
        std::array<uint8_t, 25> data{};
        uint8_t eof = 0;
        uint8_t frameNumber = 0;
        decode_pkt_frame(data.data(), &eof, &frameNumber, soft.data());
        const int bytes = eof ? frameNumber : 25;
        if (bytes < 1 || bytes > 25 || (!eof && frameNumber != frameCount % 32)) {
            setError(error, QStringLiteral("M17 packet frame number or final length invalid"));
            return false;
        }
        assembled.append(reinterpret_cast<const char *>(data.data()), bytes);
        ++frameCount;
        if (eof) {
            complete = offset + SYM_PER_FRA * 2 == symbols.size();
            break;
        }
    }
    if (!complete || assembled.size() < 3 ||
        CRC_M17(reinterpret_cast<const uint8_t *>(assembled.constData()),
                static_cast<uint16_t>(assembled.size())) != 0) {
        setError(error, QStringLiteral("M17 packet is incomplete or CRC failed"));
        return false;
    }
    assembled.chop(2);
    packet->source = decodeCallsign(serializeLsf(lsf).mid(6, 6));
    packet->destination = decodeCallsign(serializeLsf(lsf).left(6));
    packet->channelAccessNumber = (type >> 7) & 0x0f;
    packet->payload = assembled;
    packet->packetFrameCount = frameCount;
    if (error) error->clear();
    return true;
}
