#include "dmrprivacycodec.h"
#include "dmrprivacyutils.h"
#include "aes.h"
#include <algorithm>

namespace {
int hexValue(QChar c) {
    const ushort ch = c.unicode();
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}
int popcount(quint32 word) {
    int count = 0;
    while (word) { word &= word - 1; ++count; }
    return count;
}
// DMR extended Golay uses the on-air C75 polynomial, not the AMBE C0 matrix.
quint32 golay24(quint32 data) {
    quint32 remainder = data << 11;
    for (int bit = 22; bit >= 11; --bit)
        if (remainder & (1U << bit)) remainder ^= 0xC75U << (bit - 11);
    const quint32 code = (data << 11) | remainder;
    return (code << 1) | (popcount(code) & 1);
}
bool decodeGolay(quint32 word, quint32 &data, int &errors) {
    // Three codewords per 360 ms. A small immutable codebook is sufficient.
    static const auto codes = [] {
        std::array<quint32, 4096> values = {};
        for (quint32 i = 0; i < values.size(); ++i) values[i] = golay24(i);
        return values;
    }();
    errors = 4;
    for (quint32 i = 0; i < codes.size(); ++i) {
        const int distance = popcount(word ^ codes[i]);
        if (distance < errors) { errors = distance; data = i; }
        if (distance == 0) break;
    }
    return errors <= 3;
}
quint32 advance(quint32 mi, bool aes) {
    for (int i = 0; i < 32; ++i) {
        const quint32 feedback = aes ? ((mi >> 31) ^ (mi >> 21) ^ (mi >> 1) ^ mi)
                                    : ((mi >> 31) ^ (mi >> 3) ^ (mi >> 1));
        mi = (mi << 1) | (feedback & 1);
    }
    return mi;
}
constexpr std::array<int, 15> rowSyndromes = {9,11,15,7,14,5,10,13,3,6,12,1,2,4,8};
constexpr std::array<int, 13> columnSyndromes = {15,7,14,5,10,13,3,6,12,1,2,4,8};
template<std::size_t N>
int syndrome(const std::array<bool, 196> &data, int start, int stride,
             const std::array<int, N> &columns) {
    int value = 0;
    for (int i = 0; i < static_cast<int>(N); ++i)
        if (data[start + i * stride]) value ^= columns[i];
    return value;
}
template<std::size_t N>
void correct(std::array<bool, 196> &data, int start, int stride,
             const std::array<int, N> &columns) {
    const int value = syndrome(data, start, stride, columns);
    if (!value) return;
    const auto found = std::find(columns.begin(), columns.end(), value);
    if (found != columns.end()) {
        const int index = start + static_cast<int>(found - columns.begin()) * stride;
        data[index] = !data[index];
    }
}
}

namespace DmrPrivacyCodec {
bool decodePi(const QString &dibits, PiHeader &header) {
    if (dibits.size() != 132) return false;
    std::array<bool, 196> raw = {}, data = {};
    for (int bit = 0; bit < 196; ++bit) {
        const int burstBit = bit < 98 ? bit : bit + 68;
        const int symbol = dibits.at(burstBit / 2).unicode() - '0';
        if (symbol < 0 || symbol > 3) return false;
        raw[bit] = ((symbol >> (1 - burstBit % 2)) & 1) != 0;
    }
    for (int i = 0; i < 196; ++i) data[i] = raw[(i * 181) % 196];
    const auto original = data;
    for (int pass = 0; pass < 5; ++pass) {
        for (int col = 0; col < 15; ++col) correct(data, col + 1, 15, columnSyndromes);
        for (int row = 0; row < 9; ++row) correct(data, row * 15 + 1, 1, rowSyndromes);
    }
    for (int col = 0; col < 15; ++col)
        if (syndrome(data, col + 1, 15, columnSyndromes)) return false;
    for (int row = 0; row < 9; ++row)
        if (syndrome(data, row * 15 + 1, 1, rowSyndromes)) return false;
    if (data[1] || data[2] || data[3]) return false;
    int corrections = 0;
    for (int i = 1; i < 196; ++i) corrections += data[i] != original[i];
    if (corrections > 2) return false;
    std::array<quint8, 12> bytes = {};
    int position = 0;
    for (int row = 0; row < 9; ++row) {
        for (int col = row == 0 ? 3 : 0; col < 11; ++col) {
            bytes[position / 8] |= quint8(data[row * 15 + col + 1]) << (7 - position % 8);
            ++position;
        }
    }
    quint16 crc = 0;
    for (int i = 0; i < 10; ++i) {
        crc ^= quint16(bytes[i]) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = quint16((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    if ((quint16(bytes[10]) << 8 | bytes[11]) != (crc ^ 0xFFFF ^ 0x6969)) return false;
    header = {bytes[0] & 7, bytes[1], bytes[2],
              quint32(bytes[3]) << 24 | quint32(bytes[4]) << 16 | quint32(bytes[5]) << 8 | bytes[6],
              corrections};
    return true;
}

int lateEntryNibble(const QString &rawAmbe72) {
    if (rawAmbe72.size() != 18) return -1;
    for (QChar c : rawAmbe72) if (hexValue(c) < 0) return -1;
    int value = 0;
    // C3[0..3] after the DMR AMBE interleave: raw bits 71,67,63,59.
    for (int bit : {71, 67, 63, 59})
        value = (value << 1) | (hexValue(rawAmbe72.at(bit / 4)) & 1);
    return value;
}
bool decodeLateEntry(const std::array<int, 18> &fragments, quint32 &mi) {
    quint64 message = 0, parity = 0;
    for (int frame = 0; frame < 3; ++frame) {
        for (int burst = 0; burst < 3; ++burst) {
            const int left = fragments[burst * 3 + frame], right = fragments[(burst + 3) * 3 + frame];
            if (left < 0 || left > 15 || right < 0 || right > 15) return false;
            message = (message << 4) | left;
            parity = (parity << 4) | right;
        }
    }
    quint64 corrected = 0;
    int totalErrors = 0;
    for (int block = 0; block < 3; ++block) {
        const int shift = 24 - block * 12;
        const quint32 word = quint32(((message >> shift) & 0xFFF) << 12) |
                             quint32((parity >> shift) & 0xFFF);
        quint32 data = 0;
        int errors = 0;
        if (!decodeGolay(word, data, errors)) return false;
        totalErrors += errors;
        corrected = (corrected << 12) | data;
    }
    if (totalErrors > 6) return false;
    quint8 crc = 0;
    const quint32 value = quint32(corrected >> 4);
    for (int i = 31; i >= 0; --i) {
        const int top = (crc >> 3) ^ ((value >> i) & 1);
        crc = quint8(((crc << 1) ^ (top ? 3 : 0)) & 15);
    }
    if ((corrected & 15) != (crc ^ 15)) return false;
    mi = value;
    return true;
}
quint32 nextMi(quint32 mi, bool aes) { return advance(mi, aes); }
QByteArray aesIv(quint32 mi) {
    QByteArray iv(16, '\0');
    for (int i = 0; i < 4; ++i) iv[i] = char(mi >> (24 - i * 8));
    for (int i = 32; i < 128; ++i) {
        const quint32 feedback = ((mi >> 31) ^ (mi >> 21) ^ (mi >> 1) ^ mi) & 1;
        mi = (mi << 1) | feedback;
        iv[i / 8] = char(quint8(iv.at(i / 8)) | (feedback << (7 - i % 8)));
    }
    return iv;
}
QByteArray aes256Ofb(const QByteArray &key, const QByteArray &iv, int bytes) {
    if (key.size() != 32 || iv.size() != 16 || bytes < 0 || bytes > 4096) return {};
    AES_ctx context = {};
    AES_init_ctx(&context, reinterpret_cast<const uint8_t *>(key.constData()));
    std::array<uint8_t, 16> feedback = {};
    std::copy(iv.begin(), iv.end(), feedback.begin());
    QByteArray stream(bytes, '\0');
    for (int offset = 0; offset < bytes; offset += 16) {
        AES_ECB_encrypt(&context, feedback.data());
        const int count = (std::min)(16, bytes - offset);
        std::copy_n(feedback.begin(), count, stream.begin() + offset);
    }
    return stream;
}
QByteArray rc4(const QByteArray &key, int drop, int bytes) {
    if (key.isEmpty() || key.size() > 256 || drop < 0 || drop > 4096 || bytes < 0 || bytes > 4096) return {};
    std::array<quint8, 256> state = {};
    for (int i = 0; i < 256; ++i) state[i] = quint8(i);
    int j = 0;
    for (int i = 0; i < 256; ++i) {
        j = (j + state[i] + quint8(key.at(i % key.size()))) & 255;
        std::swap(state[i], state[j]);
    }
    QByteArray stream(bytes, '\0');
    int i = 0;
    j = 0;
    for (int n = 0; n < drop + bytes; ++n) {
        i = (i + 1) & 255;
        j = (j + state[i]) & 255;
        std::swap(state[i], state[j]);
        if (n >= drop) stream[n - drop] = char(state[(state[i] + state[j]) & 255]);
    }
    return stream;
}
QByteArray voiceKeystream(int mode, const QByteArray &key, quint32 mi) {
    if (mode == DMR_PRIVACY_ARC4 && key.size() == 5) {
        QByteArray seed = key;
        for (int shift = 24; shift >= 0; shift -= 8) seed.append(char(mi >> shift));
        return rc4(seed, 256, 18 * 7);
    }
    if (mode == DMR_PRIVACY_AES256 && key.size() == 32)
        return aes256Ofb(key, aesIv(mi), 16 + 18 * 7).mid(16);
    return {};
}
}
