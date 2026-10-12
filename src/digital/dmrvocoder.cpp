#include "dmrvocoder.h"

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QSet>
#include <QStringList>

#include "fobos_dmr_voice_backend.h"

#ifdef FOBOSAPP_HAS_MBELIB_NEO
extern "C" {
#include <mbelib-neo/mbelib.h>
}
#endif

namespace {

constexpr int DMR_VOICE_FRAME_BYTES = 9;
constexpr int DMR_VOICE_FRAME_BITS = 72;
constexpr int DMR_PAYLOAD_BYTES = 7;
constexpr int DMR_PAYLOAD_BITS = 49;
constexpr int DMR_PCM_8K_SAMPLES = 160;
constexpr int DMR_PCM_48K_UPSAMPLE = 6;

int hexNibbleValue(QChar ch) {
    const ushort value = ch.unicode();
    if (value >= '0' && value <= '9') {
        return static_cast<int>(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<int>(10 + value - 'A');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<int>(10 + value - 'a');
    }
    return -1;
}

QByteArray hexBitsToMsbBytes(const QString &hex, int bitCount, int byteCount) {
    QByteArray bytes(byteCount, '\0');
    if (bitCount <= 0 || byteCount <= 0 || hex.size() * 4 < bitCount) {
        return {};
    }

    for (int bitIndex = 0; bitIndex < bitCount; ++bitIndex) {
        const int nibble = hexNibbleValue(hex.at(bitIndex / 4));
        if (nibble < 0) {
            return {};
        }
        const int bitInNibble = 3 - (bitIndex % 4);
        if (((nibble >> bitInNibble) & 0x1) == 0) {
            continue;
        }
        const int byteIndex = bitIndex / 8;
        const int bitInByte = 7 - (bitIndex % 8);
        bytes[byteIndex] = static_cast<char>(
            static_cast<unsigned char>(bytes.at(byteIndex)) |
            static_cast<unsigned char>(1U << bitInByte));
    }

    return bytes;
}

bool payloadHexToBits49(const QString &hex, char bits[49]) {
    if (hex.size() < 13) {
        return false;
    }

    int bitIndex = 0;
    for (int hexIndex = 0; hexIndex < hex.size() && bitIndex < 49; ++hexIndex) {
        const int value = hexNibbleValue(hex.at(hexIndex));
        if (value < 0) {
            return false;
        }

        for (int bit = 3; bit >= 0 && bitIndex < 49; --bit) {
            bits[bitIndex++] = static_cast<char>((value >> bit) & 0x1);
        }
    }

    return bitIndex == 49;
}

int popcount32(std::uint32_t value) {
    int count = 0;
    while (value != 0) {
        count += static_cast<int>(value & 0x1U);
        value >>= 1;
    }
    return count;
}

std::uint16_t ambeGolayParity12(std::uint16_t data) {
    static constexpr std::uint16_t generator[12] = {
        0x63a, 0x31d, 0x7b4, 0x3da, 0x1ed, 0x6cc,
        0x366, 0x1b3, 0x6e3, 0x54b, 0x49f, 0x475
    };

    std::uint16_t parity = 0;
    for (int i = 0; i < 12; ++i) {
        if ((data & (1U << (11 - i))) != 0) {
            parity ^= generator[i];
        }
    }
    return static_cast<std::uint16_t>(parity & 0x07ffU);
}

std::uint32_t ambeC1PrngMask23(std::uint16_t c0Data) {
    std::uint32_t previous = static_cast<std::uint32_t>(16U * (c0Data & 0x0fffU));
    std::uint32_t mask = 0;
    for (int i = 1; i <= 23; ++i) {
        previous = (173U * previous + 13849U) & 0xffffU;
        if ((previous >> 15) != 0) {
            mask |= 1U << (23 - i);
        }
    }
    return mask;
}

void setMsbFrameBit(QByteArray &bytes, int bitIndex, bool value) {
    if (!value || bitIndex < 0 || bitIndex >= bytes.size() * 8) {
        return;
    }
    const int byteIndex = bitIndex / 8;
    const int bitInByte = 7 - (bitIndex % 8);
    const unsigned char current = static_cast<unsigned char>(bytes.at(byteIndex));
    bytes[byteIndex] = static_cast<char>(current | static_cast<unsigned char>(1U << bitInByte));
}

QByteArray payload49ToCanonicalAmbeFrame(const DmrAmbePayload &payload) {
    char bits[DMR_PAYLOAD_BITS] = {};
    if (!payloadHexToBits49(payload.hex, bits)) {
        return {};
    }

    std::uint16_t c0 = 0;
    std::uint16_t c1 = 0;
    for (int i = 0; i < 12; ++i) {
        c0 = static_cast<std::uint16_t>((c0 << 1) |
                                        (bits[i] != 0 ? 1U : 0U));
        c1 = static_cast<std::uint16_t>((c1 << 1) |
                                        (bits[12 + i] != 0 ? 1U : 0U));
    }

    std::uint32_t cBlock = 0;
    for (int i = 0; i < 25; ++i) {
        cBlock = (cBlock << 1) | (bits[24 + i] != 0 ? 1U : 0U);
    }

    QByteArray frame(DMR_VOICE_FRAME_BYTES, '\0');

    const std::uint32_t c0Code23 =
        (static_cast<std::uint32_t>(c0) << 11) | ambeGolayParity12(c0);
    const std::uint32_t aCode24 =
        (c0Code23 << 1) | static_cast<std::uint32_t>(popcount32(c0Code23) & 0x1);
    for (int i = 0; i < 24; ++i) {
        setMsbFrameBit(frame, i, (aCode24 & (0x800000U >> i)) != 0);
    }

    const std::uint32_t c1Code23 =
        ((static_cast<std::uint32_t>(c1) << 11) | ambeGolayParity12(c1)) ^
        ambeC1PrngMask23(c0);
    for (int i = 0; i < 23; ++i) {
        setMsbFrameBit(frame, 24 + i, (c1Code23 & (0x400000U >> i)) != 0);
    }

    for (int i = 0; i < 25; ++i) {
        setMsbFrameBit(frame, 47 + i, (cBlock & (0x1000000U >> i)) != 0);
    }

    return frame;
}

void appendPcm16Le(QByteArray &pcm, qint16 sample) {
    const quint16 raw = static_cast<quint16>(sample);
    pcm.append(static_cast<char>(raw & 0xff));
    pcm.append(static_cast<char>((raw >> 8) & 0xff));
}

void appendUpsampled8kTo48k(QByteArray &pcm, const qint16 samples8k[DMR_PCM_8K_SAMPLES]) {
    for (int i = 0; i < DMR_PCM_8K_SAMPLES; ++i) {
        const double current = static_cast<double>(samples8k[i]);
        const double next = static_cast<double>(
            i + 1 < DMR_PCM_8K_SAMPLES ? samples8k[i + 1] : samples8k[i]);
        for (int r = 0; r < DMR_PCM_48K_UPSAMPLE; ++r) {
            const double t = static_cast<double>(r) / static_cast<double>(DMR_PCM_48K_UPSAMPLE);
            const double interpolated = current + (next - current) * t;
            const int sample = static_cast<int>(std::lrint(interpolated));
            appendPcm16Le(pcm, static_cast<qint16>((std::clamp)(sample, -32768, 32767)));
        }
    }
}

QString backendCapabilitiesText(quint32 capabilities) {
    QStringList parts;
    if ((capabilities & FOBOS_DMR_VOICE_CAP_DECODE_DMR_AMBE_FRAME72) != 0) {
        parts << QStringLiteral("dmr-frame");
    }
    if ((capabilities & FOBOS_DMR_VOICE_CAP_DECODE_DMR_AMBE_SOFT_FRAME72) != 0) {
        parts << QStringLiteral("dmr-soft");
    }
    if ((capabilities & FOBOS_DMR_VOICE_CAP_DECODE_AMBE2450_PAYLOAD49) != 0) {
        parts << QStringLiteral("payload49");
    }
    if ((capabilities & FOBOS_DMR_VOICE_CAP_ENCODE_AMBE2450_PAYLOAD49) != 0) {
        parts << QStringLiteral("encode-payload49");
    }
    if ((capabilities & FOBOS_DMR_VOICE_CAP_DECODE_CANONICAL_AMBE_FRAME72) != 0) {
        parts << QStringLiteral("canonical-decode");
    }
    if ((capabilities & FOBOS_DMR_VOICE_CAP_ENCODE_CANONICAL_AMBE_FRAME72) != 0) {
        parts << QStringLiteral("canonical-encode");
    }
    return parts.isEmpty() ? QStringLiteral("none") : parts.join(QLatin1Char(','));
}

#ifdef FOBOSAPP_HAS_MBELIB_NEO
bool frameHexToAmbePlane(const QString &frameHex, char ambeFrame[4][24]) {
    if (frameHex.size() != 18) {
        return false;
    }

    std::array<char, 72> frameBits = {};
    const QByteArray bytes = hexBitsToMsbBytes(frameHex, DMR_VOICE_FRAME_BITS, DMR_VOICE_FRAME_BYTES);
    if (bytes.size() != DMR_VOICE_FRAME_BYTES) {
        return false;
    }
    for (int i = 0; i < DMR_VOICE_FRAME_BITS; ++i) {
        const int byteIndex = i / 8;
        const int bitInByte = 7 - (i % 8);
        frameBits[static_cast<std::size_t>(i)] =
            static_cast<char>((static_cast<unsigned char>(bytes.at(byteIndex)) >> bitInByte) & 0x1);
    }

    static constexpr int rW[36] = {
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2,
        0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2
    };
    static constexpr int rX[36] = {
        23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5,
        17, 4, 16, 3, 15, 2, 14, 1, 13, 0, 12, 10,
        11, 9, 10, 8, 9, 7, 8, 6, 7, 5, 6, 4
    };
    static constexpr int rY[36] = {
        0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3,
        1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3,
        1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3
    };
    static constexpr int rZ[36] = {
        5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12,
        22, 11, 21, 10, 20, 9, 19, 8, 18, 7, 17, 6,
        16, 5, 15, 4, 14, 3, 13, 2, 12, 1, 11, 0
    };

    std::memset(ambeFrame, 0, sizeof(char) * 4 * 24);
    for (int i = 0; i < 36; ++i) {
        ambeFrame[rW[i]][rX[i]] = frameBits[static_cast<std::size_t>(2 * i)];
        ambeFrame[rY[i]][rZ[i]] = frameBits[static_cast<std::size_t>(2 * i + 1)];
    }
    return true;
}

bool softFrameToAmbePlane(const DmrAmbeSoftFrame &frame, mbe_soft_bit ambeFrame[4][24]) {
    if (frame.hex.size() != 18) {
        return false;
    }

    std::array<char, 72> frameBits = {};
    const QByteArray bytes = hexBitsToMsbBytes(frame.hex, DMR_VOICE_FRAME_BITS, DMR_VOICE_FRAME_BYTES);
    if (bytes.size() != DMR_VOICE_FRAME_BYTES) {
        return false;
    }
    for (int i = 0; i < DMR_VOICE_FRAME_BITS; ++i) {
        const int byteIndex = i / 8;
        const int bitInByte = 7 - (i % 8);
        frameBits[static_cast<std::size_t>(i)] =
            static_cast<char>((static_cast<unsigned char>(bytes.at(byteIndex)) >> bitInByte) & 0x1);
    }

    static constexpr int rW[36] = {
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2,
        0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2
    };
    static constexpr int rX[36] = {
        23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5,
        17, 4, 16, 3, 15, 2, 14, 1, 13, 0, 12, 10,
        11, 9, 10, 8, 9, 7, 8, 6, 7, 5, 6, 4
    };
    static constexpr int rY[36] = {
        0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3,
        1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3,
        1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3
    };
    static constexpr int rZ[36] = {
        5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12,
        22, 11, 21, 10, 20, 9, 19, 8, 18, 7, 17, 6,
        16, 5, 15, 4, 14, 3, 13, 2, 12, 1, 11, 0
    };

    std::memset(ambeFrame, 0, sizeof(mbe_soft_bit) * 4 * 24);
    for (int i = 0; i < 36; ++i) {
        const std::uint8_t highReliability =
            (std::max<std::uint8_t>)(frame.reliability[static_cast<std::size_t>(2 * i)], 1);
        const std::uint8_t lowReliability =
            (std::max<std::uint8_t>)(frame.reliability[static_cast<std::size_t>(2 * i + 1)], 1);
        ambeFrame[rW[i]][rX[i]] =
            mbe_softBitFromHard(frameBits[static_cast<std::size_t>(2 * i)], highReliability);
        ambeFrame[rY[i]][rZ[i]] =
            mbe_softBitFromHard(frameBits[static_cast<std::size_t>(2 * i + 1)], lowReliability);
    }
    return true;
}
#endif

} // namespace

struct DmrVocoder::Impl {
    using GetInfoFn = const fobos_dmr_voice_backend_info *(*)();
    using CreateFn = fobos_dmr_voice_context *(*)();
    using DestroyFn = void (*)(fobos_dmr_voice_context *);
    using ResetFn = int (*)(fobos_dmr_voice_context *);
    using LastErrorFn = const char *(*)(fobos_dmr_voice_context *);
    using DecodeDmrFrameFn = int (*)(fobos_dmr_voice_context *,
                                     const std::uint8_t *,
                                     std::int16_t *,
                                     int *);
    using DecodeDmrSoftFrameFn = int (*)(fobos_dmr_voice_context *,
                                         const std::uint8_t *,
                                         const std::uint8_t *,
                                         std::int16_t *,
                                         int *);
    using DecodePayloadFn = int (*)(fobos_dmr_voice_context *,
                                    const std::uint8_t *,
                                    int,
                                    std::int16_t *,
                                    int *);
    using EncodePayloadFn = int (*)(fobos_dmr_voice_context *,
                                    const std::int16_t *,
                                    std::uint8_t *);
    using DecodeCanonicalFn = int (*)(fobos_dmr_voice_context *,
                                      const std::uint8_t *,
                                      std::int16_t *,
                                      int *);
    using EncodeCanonicalFn = int (*)(fobos_dmr_voice_context *,
                                      const std::int16_t *,
                                      std::uint8_t *);

    struct Backend {
        explicit Backend(const QString &filePath)
            : library(filePath) {}

        ~Backend() {
            if (context && destroy) {
                destroy(context);
            }
            context = nullptr;
            library.unload();
        }

        template <typename Fn>
        bool resolve(Fn &fn, const char *symbol) {
            fn = reinterpret_cast<Fn>(library.resolve(symbol));
            return fn != nullptr;
        }

        bool load() {
            if (!library.load()) {
                loadError = library.errorString();
                return false;
            }

            const bool ok =
                resolve(getInfo, "fobos_dmr_voice_get_backend_info") &&
                resolve(create, "fobos_dmr_voice_create") &&
                resolve(destroy, "fobos_dmr_voice_destroy") &&
                resolve(reset, "fobos_dmr_voice_reset") &&
                resolve(lastError, "fobos_dmr_voice_get_last_error") &&
                resolve(decodeDmrFrame, "fobos_dmr_voice_decode_dmr_ambe_frame72") &&
                resolve(decodeDmrSoftFrame, "fobos_dmr_voice_decode_dmr_ambe_soft_frame72") &&
                resolve(decodePayload, "fobos_dmr_voice_decode_ambe2450_payload49") &&
                resolve(encodePayload, "fobos_dmr_voice_encode_ambe2450_payload49") &&
                resolve(decodeCanonical, "fobos_dmr_voice_decode_canonical_ambe_frame72") &&
                resolve(encodeCanonical, "fobos_dmr_voice_encode_canonical_ambe_frame72");
            if (!ok) {
                loadError = QStringLiteral("missing required Fobos DMR voice ABI symbol");
                library.unload();
                return false;
            }

            info = getInfo();
            if (!info || info->abi_version != FOBOS_DMR_VOICE_ABI_VERSION) {
                loadError = QStringLiteral("incompatible Fobos DMR voice ABI version");
                library.unload();
                return false;
            }

            context = create();
            if (!context) {
                loadError = QStringLiteral("backend context creation failed");
                library.unload();
                return false;
            }

            capabilities = info->capability_flags;
            return true;
        }

        QString displayName() const {
            if (info && info->display_name) {
                return QString::fromUtf8(info->display_name);
            }
            return QFileInfo(library.fileName()).fileName();
        }

        QString backendId() const {
            if (info && info->backend_id) {
                return QString::fromUtf8(info->backend_id);
            }
            return QFileInfo(library.fileName()).completeBaseName();
        }

        QLibrary library;
        QString loadError;
        const fobos_dmr_voice_backend_info *info = nullptr;
        fobos_dmr_voice_context *context = nullptr;
        quint32 capabilities = 0;
        GetInfoFn getInfo = nullptr;
        CreateFn create = nullptr;
        DestroyFn destroy = nullptr;
        ResetFn reset = nullptr;
        LastErrorFn lastError = nullptr;
        DecodeDmrFrameFn decodeDmrFrame = nullptr;
        DecodeDmrSoftFrameFn decodeDmrSoftFrame = nullptr;
        DecodePayloadFn decodePayload = nullptr;
        EncodePayloadFn encodePayload = nullptr;
        DecodeCanonicalFn decodeCanonical = nullptr;
        EncodeCanonicalFn encodeCanonical = nullptr;
    };

    Impl() {
        loadBackends();
    }

    void loadBackends() {
        if (loadAttempted) {
            return;
        }
        loadAttempted = true;

        QStringList searchDirs;
        const auto addDir = [&searchDirs](const QString &path) {
            const QString clean = QDir::cleanPath(path);
            if (!clean.isEmpty() && !searchDirs.contains(clean)) {
                searchDirs << clean;
            }
        };

        addDir(QDir(QCoreApplication::applicationDirPath())
                   .absoluteFilePath(QStringLiteral("dmr_voice_backends")));
        addDir(QDir(QCoreApplication::applicationDirPath())
                   .absoluteFilePath(QStringLiteral("../fobos-dmr-voice-backend-gpl")));
        addDir(QDir(QCoreApplication::applicationDirPath())
                   .absoluteFilePath(QStringLiteral("../fobos-dmr-voice-backend-gpl/Release")));
        addDir(QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("dmr_voice_backends")));
        addDir(QDir(QDir::currentPath())
                   .absoluteFilePath(QStringLiteral("build/fobos-dmr-voice-backend-gpl")));
        addDir(QDir(QDir::currentPath())
                   .absoluteFilePath(QStringLiteral("build/fobos-dmr-voice-backend-gpl/Release")));
        addDir(QDir(QDir::currentPath())
                   .absoluteFilePath(QStringLiteral("build/fobos-dmr-voice-backend-gpl-vs/Release")));

#ifdef Q_OS_WIN
        const QStringList patterns = {QStringLiteral("fobos_dmr_voice_*.dll")};
#elif defined(Q_OS_MACOS)
        const QStringList patterns = {QStringLiteral("libfobos_dmr_voice_*.dylib"),
                                      QStringLiteral("fobos_dmr_voice_*.dylib")};
#else
        const QStringList patterns = {QStringLiteral("libfobos_dmr_voice_*.so"),
                                      QStringLiteral("fobos_dmr_voice_*.so")};
#endif

        QSet<QString> loadedPaths;
        for (const QString &dirPath : searchDirs) {
            QDir dir(dirPath);
            if (!dir.exists()) {
                continue;
            }

            const QFileInfoList files =
                dir.entryInfoList(patterns, QDir::Files, QDir::Name);
            for (const QFileInfo &file : files) {
                const QString canonical = file.canonicalFilePath();
                if (canonical.isEmpty() || loadedPaths.contains(canonical)) {
                    continue;
                }
                loadedPaths.insert(canonical);

                std::unique_ptr<Backend> backend(new Backend(canonical));
                if (!backend->load()) {
                    qDebug() << "[DMR voice backend] skipped" << canonical
                             << backend->loadError;
                    continue;
                }

                qDebug() << "[DMR voice backend] loaded"
                         << backend->displayName()
                         << "caps" << backendCapabilitiesText(backend->capabilities)
                         << "path" << canonical;
                backends.push_back(std::move(backend));
            }
        }

        if (backends.empty()) {
            qDebug() << "[DMR voice backend] no loadable backend DLL found";
        }
    }

    bool backendMatchesPreference(const Backend *backend) const {
        if (!backend || preferredBackendId.isEmpty()) {
            return true;
        }
        const QString needle = preferredBackendId.toLower();
        return backend->backendId().toLower().contains(needle) ||
               backend->displayName().toLower().contains(needle);
    }

    Backend *backendWith(quint32 capability) const {
        if (!preferredBackendId.isEmpty()) {
            for (const std::unique_ptr<Backend> &backend : backends) {
                if (backend &&
                    (backend->capabilities & capability) != 0 &&
                    backendMatchesPreference(backend.get())) {
                    return backend.get();
                }
            }
            return nullptr;
        }
        for (const std::unique_ptr<Backend> &backend : backends) {
            if (backend && (backend->capabilities & capability) != 0) {
                return backend.get();
            }
        }
        return nullptr;
    }

    bool hasBackendDecoder() const {
        return backendWith(FOBOS_DMR_VOICE_CAP_DECODE_DMR_AMBE_FRAME72) ||
               backendWith(FOBOS_DMR_VOICE_CAP_DECODE_DMR_AMBE_SOFT_FRAME72) ||
               backendWith(FOBOS_DMR_VOICE_CAP_DECODE_AMBE2450_PAYLOAD49) ||
               backendWith(FOBOS_DMR_VOICE_CAP_DECODE_CANONICAL_AMBE_FRAME72);
    }

    bool hasBackendEncoder() const {
        return backendWith(FOBOS_DMR_VOICE_CAP_ENCODE_CANONICAL_AMBE_FRAME72) != nullptr;
    }

    bool fallbackAvailable() const {
#ifdef FOBOSAPP_HAS_MBELIB_NEO
        return preferredBackendId.isEmpty() || preferredBackendId.contains(QStringLiteral("mbelib"), Qt::CaseInsensitive);
#else
        return false;
#endif
    }

    std::vector<std::unique_ptr<Backend>> backends;
    bool loadAttempted = false;
    QString preferredBackendId;

#ifdef FOBOSAPP_HAS_MBELIB_NEO
    mbe_parms cur;
    mbe_parms prev;
    mbe_parms prevEnhanced;
#endif
};

DmrVocoder::DmrVocoder()
    : impl(new Impl()) {
    reset();
}

DmrVocoder::~DmrVocoder() {
    delete impl;
    impl = nullptr;
}

bool DmrVocoder::isAvailable() const {
    return impl && (impl->hasBackendDecoder() || impl->fallbackAvailable());
}

bool DmrVocoder::canEncode() const {
    return impl && impl->hasBackendEncoder();
}

QByteArray DmrVocoder::encodeCanonicalFrame(const qint16 *pcm8k160, QString *error) {
    if (!impl || !pcm8k160) {
        if (error) *error = QStringLiteral("Invalid DMR encoder input");
        return {};
    }
    Impl::Backend *backend =
        impl->backendWith(FOBOS_DMR_VOICE_CAP_ENCODE_CANONICAL_AMBE_FRAME72);
    if (!backend || !backend->context || !backend->encodeCanonical) {
        if (error) *error = QStringLiteral("No DMR AMBE+2 encoder backend is available");
        return {};
    }
    QByteArray frame(DMR_VOICE_FRAME_BYTES, '\0');
    const int status = backend->encodeCanonical(
        backend->context,
        reinterpret_cast<const std::int16_t *>(pcm8k160),
        reinterpret_cast<std::uint8_t *>(frame.data()));
    if (status != FOBOS_DMR_VOICE_OK) {
        const char *backendError = backend->lastError ? backend->lastError(backend->context) : nullptr;
        if (error) {
            *error = backendError && backendError[0] != '\0'
                         ? QString::fromUtf8(backendError)
                         : QStringLiteral("DMR AMBE+2 encoding failed (%1)").arg(status);
        }
        return {};
    }
    return frame;
}

void DmrVocoder::setPreferredBackendId(const QString &backendId) {
    if (!impl) {
        return;
    }
    const QString clean = backendId.trimmed();
    if (impl->preferredBackendId == clean) {
        return;
    }
    impl->preferredBackendId = clean;
    qDebug() << "[DMR voice backend] preferred backend"
             << (clean.isEmpty() ? QStringLiteral("auto") : clean);
    reset();
}

void DmrVocoder::reset() {
    if (impl) {
        for (const std::unique_ptr<Impl::Backend> &backend : impl->backends) {
            if (backend && backend->context && backend->reset) {
                backend->reset(backend->context);
            }
        }
#ifdef FOBOSAPP_HAS_MBELIB_NEO
        mbe_initMbeParms(&impl->cur, &impl->prev, &impl->prevEnhanced);
#endif
    }
    initialized = true;
}

QByteArray DmrVocoder::decodeFrames(const std::vector<QString> &frames,
                                    int *decodedFrames,
                                    int *errorTotal) {
    if (decodedFrames) {
        *decodedFrames = 0;
    }
    if (errorTotal) {
        *errorTotal = 0;
    }

    QByteArray pcm;
    if (!frames.empty() && impl) {
        if (Impl::Backend *backend =
                impl->backendWith(FOBOS_DMR_VOICE_CAP_DECODE_DMR_AMBE_FRAME72)) {
            pcm.reserve(static_cast<int>(frames.size()) *
                        DMR_PCM_8K_SAMPLES * DMR_PCM_48K_UPSAMPLE *
                        static_cast<int>(sizeof(qint16)));
            for (const QString &frameHex : frames) {
                const QByteArray frameBytes =
                    hexBitsToMsbBytes(frameHex, DMR_VOICE_FRAME_BITS, DMR_VOICE_FRAME_BYTES);
                if (frameBytes.size() != DMR_VOICE_FRAME_BYTES) {
                    continue;
                }

                std::array<qint16, DMR_PCM_8K_SAMPLES> audio8k = {};
                int frameErrors = 0;
                const int status =
                    backend->decodeDmrFrame(backend->context,
                                            reinterpret_cast<const std::uint8_t *>(frameBytes.constData()),
                                            reinterpret_cast<std::int16_t *>(audio8k.data()),
                                            &frameErrors);
                if (status != FOBOS_DMR_VOICE_OK) {
                    continue;
                }
                appendUpsampled8kTo48k(pcm, audio8k.data());
                if (decodedFrames) {
                    ++(*decodedFrames);
                }
                if (errorTotal) {
                    *errorTotal += frameErrors;
                }
            }
            return pcm;
        }
    }

#ifdef FOBOSAPP_HAS_MBELIB_NEO
    if (!impl || !impl->fallbackAvailable() || frames.empty()) {
        return pcm;
    }
    if (!initialized) {
        reset();
    }

    pcm.reserve(static_cast<int>(frames.size()) * 160 * 6 * static_cast<int>(sizeof(qint16)));
    for (const QString &frameHex : frames) {
        char ambeFrame[4][24] = {};
        if (!frameHexToAmbePlane(frameHex, ambeFrame)) {
            continue;
        }

        short audio8k[160] = {};
        char ambeBits[49] = {};
        mbe_process_result processResult;
        std::memset(&processResult, 0, sizeof(processResult));
        mbe_initProcessResult(&processResult);

        const int result = mbe_processAmbe3600x2450Frame(audio8k,
                                                         &processResult,
                                                         ambeFrame,
                                                         ambeBits,
                                                         &impl->cur,
                                                         &impl->prev,
                                                         &impl->prevEnhanced);
        if (result < 0) {
            continue;
        }
        appendUpsampled8kTo48k(pcm, reinterpret_cast<const qint16 *>(audio8k));
        if (decodedFrames) {
            ++(*decodedFrames);
        }
        if (errorTotal) {
            *errorTotal += result;
        }
    }
#else
    Q_UNUSED(frames);
#endif
    return pcm;
}

QByteArray DmrVocoder::decodeSoftFrames(const std::vector<DmrAmbeSoftFrame> &frames,
                                        int *decodedFrames,
                                        int *errorTotal,
                                        std::vector<int> *perFrameErrors) {
    if (decodedFrames) {
        *decodedFrames = 0;
    }
    if (errorTotal) {
        *errorTotal = 0;
    }
    if (perFrameErrors) {
        perFrameErrors->clear();
    }

    QByteArray pcm;
    if (!frames.empty() && impl) {
        if (Impl::Backend *backend =
                impl->backendWith(FOBOS_DMR_VOICE_CAP_DECODE_DMR_AMBE_SOFT_FRAME72)) {
            pcm.reserve(static_cast<int>(frames.size()) *
                        DMR_PCM_8K_SAMPLES * DMR_PCM_48K_UPSAMPLE *
                        static_cast<int>(sizeof(qint16)));
            for (const DmrAmbeSoftFrame &frame : frames) {
                const QByteArray frameBytes =
                    hexBitsToMsbBytes(frame.hex, DMR_VOICE_FRAME_BITS, DMR_VOICE_FRAME_BYTES);
                if (frameBytes.size() != DMR_VOICE_FRAME_BYTES) {
                    continue;
                }

                std::array<qint16, DMR_PCM_8K_SAMPLES> audio8k = {};
                int frameErrors = 0;
                const int status =
                    backend->decodeDmrSoftFrame(
                        backend->context,
                        reinterpret_cast<const std::uint8_t *>(frameBytes.constData()),
                        frame.reliability.data(),
                        reinterpret_cast<std::int16_t *>(audio8k.data()),
                        &frameErrors);
                if (status != FOBOS_DMR_VOICE_OK) {
                    continue;
                }
                appendUpsampled8kTo48k(pcm, audio8k.data());
                if (perFrameErrors) {
                    perFrameErrors->push_back(frameErrors);
                }
                if (decodedFrames) {
                    ++(*decodedFrames);
                }
                if (errorTotal) {
                    *errorTotal += frameErrors;
                }
            }
            return pcm;
        }
    }

#ifdef FOBOSAPP_HAS_MBELIB_NEO
    if (!impl || !impl->fallbackAvailable() || frames.empty()) {
        return pcm;
    }
    if (!initialized) {
        reset();
    }

    pcm.reserve(static_cast<int>(frames.size()) * 160 * 6 * static_cast<int>(sizeof(qint16)));
    for (const DmrAmbeSoftFrame &frame : frames) {
        mbe_soft_bit ambeFrame[4][24] = {};
        if (!softFrameToAmbePlane(frame, ambeFrame)) {
            continue;
        }

        short audio8k[160] = {};
        char ambeBits[49] = {};
        mbe_process_result processResult;
        std::memset(&processResult, 0, sizeof(processResult));
        mbe_initProcessResult(&processResult);

        const int result = mbe_processAmbe3600x2450SoftFrame(audio8k,
                                                             &processResult,
                                                             ambeFrame,
                                                             ambeBits,
                                                             &impl->cur,
                                                             &impl->prev,
                                                             &impl->prevEnhanced);
        if (result < 0) {
            continue;
        }
        appendUpsampled8kTo48k(pcm, reinterpret_cast<const qint16 *>(audio8k));
        if (perFrameErrors) {
            perFrameErrors->push_back(result);
        }
        if (decodedFrames) {
            ++(*decodedFrames);
        }
        if (errorTotal) {
            *errorTotal += result;
        }
    }
#else
    Q_UNUSED(frames);
#endif
    return pcm;
}

QByteArray DmrVocoder::decodePayloads(const std::vector<DmrAmbePayload> &payloads,
                                      int *decodedFrames,
                                      int *errorTotal) {
    if (decodedFrames) {
        *decodedFrames = 0;
    }
    if (errorTotal) {
        *errorTotal = 0;
    }

    QByteArray pcm;
    if (!payloads.empty() && impl) {
        if (Impl::Backend *backend =
                impl->backendWith(FOBOS_DMR_VOICE_CAP_DECODE_AMBE2450_PAYLOAD49)) {
            pcm.reserve(static_cast<int>(payloads.size()) *
                        DMR_PCM_8K_SAMPLES * DMR_PCM_48K_UPSAMPLE *
                        static_cast<int>(sizeof(qint16)));
            for (const DmrAmbePayload &payload : payloads) {
                const QByteArray payloadBytes =
                    hexBitsToMsbBytes(payload.hex, DMR_PAYLOAD_BITS, DMR_PAYLOAD_BYTES);
                if (payloadBytes.size() != DMR_PAYLOAD_BYTES) {
                    continue;
                }

                std::array<qint16, DMR_PCM_8K_SAMPLES> audio8k = {};
                int frameErrors = 0;
                const int status =
                    backend->decodePayload(
                        backend->context,
                        reinterpret_cast<const std::uint8_t *>(payloadBytes.constData()),
                        payload.correctedErrors,
                        reinterpret_cast<std::int16_t *>(audio8k.data()),
                        &frameErrors);
                if (status != FOBOS_DMR_VOICE_OK) {
                    continue;
                }
                appendUpsampled8kTo48k(pcm, audio8k.data());
                if (decodedFrames) {
                    ++(*decodedFrames);
                }
                if (errorTotal) {
                    *errorTotal += frameErrors;
                }
            }
            return pcm;
        }
    }

#ifdef FOBOSAPP_HAS_MBELIB_NEO
    if (!impl || !impl->fallbackAvailable() || payloads.empty()) {
        return pcm;
    }
    if (!initialized) {
        reset();
    }

    pcm.reserve(static_cast<int>(payloads.size()) * 160 * 6 * static_cast<int>(sizeof(qint16)));
    for (const DmrAmbePayload &payload : payloads) {
        char ambeBits[49] = {};
        if (!payloadHexToBits49(payload.hex, ambeBits)) {
            continue;
        }

        short audio8k[160] = {};
        mbe_process_result processResult;
        std::memset(&processResult, 0, sizeof(processResult));
        mbe_initProcessResult(&processResult);
        processResult.total_errors = (std::clamp)(payload.correctedErrors, 0, 49);

        const int result = mbe_processAmbe2450Data(audio8k,
                                                   &processResult,
                                                   ambeBits,
                                                   &impl->cur,
                                                   &impl->prev,
                                                   &impl->prevEnhanced);
        if (result < 0) {
            continue;
        }
        appendUpsampled8kTo48k(pcm, reinterpret_cast<const qint16 *>(audio8k));
        if (decodedFrames) {
            ++(*decodedFrames);
        }
        if (errorTotal) {
            *errorTotal += result;
        }
    }
#else
    Q_UNUSED(payloads);
#endif
    return pcm;
}

QByteArray DmrVocoder::decodePayloads(const std::vector<QString> &payloads,
                                      int *decodedFrames,
                                      int *errorTotal) {
    std::vector<DmrAmbePayload> wrapped;
    wrapped.reserve(payloads.size());
    for (const QString &payload : payloads) {
        wrapped.push_back({payload, 0});
    }
    return decodePayloads(wrapped, decodedFrames, errorTotal);
}

QByteArray DmrVocoder::decodeCanonicalPayloads(const std::vector<DmrAmbePayload> &payloads,
                                               int *decodedFrames,
                                               int *errorTotal) {
    if (decodedFrames) {
        *decodedFrames = 0;
    }
    if (errorTotal) {
        *errorTotal = 0;
    }

    QByteArray pcm;
    if (payloads.empty() || !impl) {
        return pcm;
    }

    Impl::Backend *backend =
        impl->backendWith(FOBOS_DMR_VOICE_CAP_DECODE_CANONICAL_AMBE_FRAME72);
    if (!backend) {
        return pcm;
    }

    pcm.reserve(static_cast<int>(payloads.size()) *
                DMR_PCM_8K_SAMPLES * DMR_PCM_48K_UPSAMPLE *
                static_cast<int>(sizeof(qint16)));
    for (const DmrAmbePayload &payload : payloads) {
        const QByteArray canonicalFrame = payload49ToCanonicalAmbeFrame(payload);
        if (canonicalFrame.size() != DMR_VOICE_FRAME_BYTES) {
            continue;
        }

        std::array<qint16, DMR_PCM_8K_SAMPLES> audio8k = {};
        int frameErrors = 0;
        const int status =
            backend->decodeCanonical(
                backend->context,
                reinterpret_cast<const std::uint8_t *>(canonicalFrame.constData()),
                reinterpret_cast<std::int16_t *>(audio8k.data()),
                &frameErrors);
        if (status != FOBOS_DMR_VOICE_OK) {
            continue;
        }
        appendUpsampled8kTo48k(pcm, audio8k.data());
        if (decodedFrames) {
            ++(*decodedFrames);
        }
        if (errorTotal) {
            *errorTotal += frameErrors;
        }
    }
    return pcm;
}
