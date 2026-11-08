#include "fobos_dmr_voice_backend.h"

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include <mbelib-neo/mbelib.h>
}

struct fobos_dmr_voice_context {
    mbe_parms cur;
    mbe_parms prev;
    mbe_parms prev_enhanced;
    char last_error[192];
};

namespace {

constexpr std::size_t DMR_FRAME_BITS = 72;
constexpr std::size_t DMR_FRAME_BYTES = 9;
constexpr std::size_t AMBE_PAYLOAD_BITS = 49;
constexpr std::size_t AMBE_PAYLOAD_BYTES = 7;
constexpr std::size_t PCM_8K_SAMPLES = 160;

const fobos_dmr_voice_backend_info BACKEND_INFO = {
    FOBOS_DMR_VOICE_ABI_VERSION,
    FOBOS_DMR_VOICE_CAP_DECODE_DMR_AMBE_FRAME72 |
        FOBOS_DMR_VOICE_CAP_DECODE_DMR_AMBE_SOFT_FRAME72 |
        FOBOS_DMR_VOICE_CAP_DECODE_AMBE2450_PAYLOAD49,
    "fobos.dmr.voice.mbelib",
    "Fobos DMR Voice Backend (mbelib-neo)",
    "0.1.0",
    "GPL-2.0-or-later",
    "Optional AMBE/AMBE+2 backend. AMBE technologies may be patent-encumbered in some jurisdictions."
};

void set_error(fobos_dmr_voice_context *context, const char *message) {
    if (!context) {
        return;
    }
    std::strncpy(context->last_error, message, sizeof(context->last_error) - 1);
    context->last_error[sizeof(context->last_error) - 1] = '\0';
}

int validate_decode_args(fobos_dmr_voice_context *context,
                         const void *input,
                         int16_t *pcm8k_160) {
    if (!context) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    if (!input || !pcm8k_160) {
        set_error(context, "missing decode input or PCM output buffer");
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    return FOBOS_DMR_VOICE_OK;
}

int bit_from_bytes(const uint8_t *bytes, std::size_t bit_index) {
    const std::size_t byte_index = bit_index / 8;
    const int bit_in_byte = 7 - static_cast<int>(bit_index % 8);
    return (bytes[byte_index] >> bit_in_byte) & 0x01;
}

void bytes_to_bits(const uint8_t *bytes,
                   std::size_t bit_count,
                   char *bits) {
    for (std::size_t i = 0; i < bit_count; ++i) {
        bits[i] = static_cast<char>(bit_from_bytes(bytes, i));
    }
}

void copy_audio_to_abi_buffer(int16_t *pcm8k_160,
                              const short audio8k[PCM_8K_SAMPLES]) {
    for (std::size_t i = 0; i < PCM_8K_SAMPLES; ++i) {
        pcm8k_160[i] = static_cast<int16_t>(audio8k[i]);
    }
}

bool frame72_to_ambe_plane(const uint8_t *frame72_bytes,
                           char ambe_frame[4][24]) {
    if (!frame72_bytes) {
        return false;
    }

    std::array<char, DMR_FRAME_BITS> frame_bits = {};
    bytes_to_bits(frame72_bytes, frame_bits.size(), frame_bits.data());

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

    std::memset(ambe_frame, 0, sizeof(char) * 4 * 24);
    for (int i = 0; i < 36; ++i) {
        ambe_frame[rW[i]][rX[i]] = frame_bits[static_cast<std::size_t>(2 * i)];
        ambe_frame[rY[i]][rZ[i]] = frame_bits[static_cast<std::size_t>(2 * i + 1)];
    }
    return true;
}

bool soft_frame72_to_ambe_plane(const uint8_t *frame72_bytes,
                                const uint8_t *reliability72,
                                mbe_soft_bit ambe_frame[4][24]) {
    if (!frame72_bytes || !reliability72) {
        return false;
    }

    std::array<char, DMR_FRAME_BITS> frame_bits = {};
    bytes_to_bits(frame72_bytes, frame_bits.size(), frame_bits.data());

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

    std::memset(ambe_frame, 0, sizeof(mbe_soft_bit) * 4 * 24);
    for (int i = 0; i < 36; ++i) {
        const std::size_t high_index = static_cast<std::size_t>(2 * i);
        const std::size_t low_index = static_cast<std::size_t>(2 * i + 1);
        const uint8_t high_reliability = std::max<uint8_t>(reliability72[high_index], 1);
        const uint8_t low_reliability = std::max<uint8_t>(reliability72[low_index], 1);
        ambe_frame[rW[i]][rX[i]] =
            mbe_softBitFromHard(frame_bits[high_index], high_reliability);
        ambe_frame[rY[i]][rZ[i]] =
            mbe_softBitFromHard(frame_bits[low_index], low_reliability);
    }
    return true;
}

void reset_context_state(fobos_dmr_voice_context *context) {
    mbe_initMbeParms(&context->cur, &context->prev, &context->prev_enhanced);
    context->last_error[0] = '\0';
}

} // namespace

extern "C" {

FOBOS_DMR_VOICE_API const fobos_dmr_voice_backend_info *
fobos_dmr_voice_get_backend_info(void) {
    return &BACKEND_INFO;
}

FOBOS_DMR_VOICE_API fobos_dmr_voice_context *
fobos_dmr_voice_create(void) {
    fobos_dmr_voice_context *context = new fobos_dmr_voice_context();
    reset_context_state(context);
    return context;
}

FOBOS_DMR_VOICE_API void
fobos_dmr_voice_destroy(fobos_dmr_voice_context *context) {
    delete context;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_reset(fobos_dmr_voice_context *context) {
    if (!context) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    reset_context_state(context);
    return FOBOS_DMR_VOICE_OK;
}

FOBOS_DMR_VOICE_API const char *
fobos_dmr_voice_get_last_error(fobos_dmr_voice_context *context) {
    if (!context) {
        return "invalid backend context";
    }
    return context->last_error;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_decode_dmr_ambe_frame72(fobos_dmr_voice_context *context,
                                        const uint8_t *frame72_bytes,
                                        int16_t *pcm8k_160,
                                        int *error_count) {
    const int args_status = validate_decode_args(context, frame72_bytes, pcm8k_160);
    if (args_status != FOBOS_DMR_VOICE_OK) {
        return args_status;
    }

    char ambe_frame[4][24] = {};
    if (!frame72_to_ambe_plane(frame72_bytes, ambe_frame)) {
        set_error(context, "invalid DMR AMBE frame");
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }

    char ambe_bits[AMBE_PAYLOAD_BITS] = {};
    mbe_process_result process_result;
    std::memset(&process_result, 0, sizeof(process_result));
    mbe_initProcessResult(&process_result);

    short audio8k[PCM_8K_SAMPLES] = {};
    const int result = mbe_processAmbe3600x2450Frame(audio8k,
                                                     &process_result,
                                                     ambe_frame,
                                                     ambe_bits,
                                                     &context->cur,
                                                     &context->prev,
                                                     &context->prev_enhanced);
    if (result < 0) {
        set_error(context, "mbelib failed to decode DMR AMBE frame");
        return FOBOS_DMR_VOICE_DECODE_FAILED;
    }
    copy_audio_to_abi_buffer(pcm8k_160, audio8k);
    if (error_count) {
        *error_count = result;
    }
    set_error(context, "");
    return FOBOS_DMR_VOICE_OK;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_decode_dmr_ambe_soft_frame72(fobos_dmr_voice_context *context,
                                             const uint8_t *frame72_bytes,
                                             const uint8_t *reliability72,
                                             int16_t *pcm8k_160,
                                             int *error_count) {
    const int args_status = validate_decode_args(context, frame72_bytes, pcm8k_160);
    if (args_status != FOBOS_DMR_VOICE_OK) {
        return args_status;
    }
    if (!reliability72) {
        set_error(context, "missing soft-bit reliability input");
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }

    mbe_soft_bit ambe_frame[4][24] = {};
    if (!soft_frame72_to_ambe_plane(frame72_bytes, reliability72, ambe_frame)) {
        set_error(context, "invalid soft DMR AMBE frame");
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }

    char ambe_bits[AMBE_PAYLOAD_BITS] = {};
    mbe_process_result process_result;
    std::memset(&process_result, 0, sizeof(process_result));
    mbe_initProcessResult(&process_result);

    short audio8k[PCM_8K_SAMPLES] = {};
    const int result = mbe_processAmbe3600x2450SoftFrame(audio8k,
                                                         &process_result,
                                                         ambe_frame,
                                                         ambe_bits,
                                                         &context->cur,
                                                         &context->prev,
                                                         &context->prev_enhanced);
    if (result < 0) {
        set_error(context, "mbelib failed to decode soft DMR AMBE frame");
        return FOBOS_DMR_VOICE_DECODE_FAILED;
    }
    copy_audio_to_abi_buffer(pcm8k_160, audio8k);
    if (error_count) {
        *error_count = result;
    }
    set_error(context, "");
    return FOBOS_DMR_VOICE_OK;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_decode_ambe2450_payload49(fobos_dmr_voice_context *context,
                                          const uint8_t *payload49_bytes,
                                          int corrected_errors,
                                          int16_t *pcm8k_160,
                                          int *error_count) {
    const int args_status = validate_decode_args(context, payload49_bytes, pcm8k_160);
    if (args_status != FOBOS_DMR_VOICE_OK) {
        return args_status;
    }

    char ambe_bits[AMBE_PAYLOAD_BITS] = {};
    bytes_to_bits(payload49_bytes, AMBE_PAYLOAD_BITS, ambe_bits);

    mbe_process_result process_result;
    std::memset(&process_result, 0, sizeof(process_result));
    mbe_initProcessResult(&process_result);
    process_result.total_errors = std::clamp(corrected_errors, 0, 49);

    short audio8k[PCM_8K_SAMPLES] = {};
    const int result = mbe_processAmbe2450Data(audio8k,
                                               &process_result,
                                               ambe_bits,
                                               &context->cur,
                                               &context->prev,
                                               &context->prev_enhanced);
    if (result < 0) {
        set_error(context, "mbelib failed to decode AMBE2450 payload");
        return FOBOS_DMR_VOICE_DECODE_FAILED;
    }
    copy_audio_to_abi_buffer(pcm8k_160, audio8k);
    if (error_count) {
        *error_count = result;
    }
    set_error(context, "");
    return FOBOS_DMR_VOICE_OK;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_encode_ambe2450_payload49(fobos_dmr_voice_context *context,
                                          const int16_t *pcm8k_160,
                                          uint8_t *payload49_bytes) {
    if (!context) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    (void)pcm8k_160;
    if (payload49_bytes) {
        std::memset(payload49_bytes, 0, AMBE_PAYLOAD_BYTES);
    }
    set_error(context, "AMBE2450 encode is not available in the mbelib backend");
    return FOBOS_DMR_VOICE_UNSUPPORTED;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_decode_canonical_ambe_frame72(fobos_dmr_voice_context *context,
                                              const uint8_t *canonical_frame72_bytes,
                                              int16_t *pcm8k_160,
                                              int *error_count) {
    if (!context) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    (void)canonical_frame72_bytes;
    (void)pcm8k_160;
    (void)error_count;
    set_error(context, "canonical AMBE3600 frame decode is not available in the mbelib backend");
    return FOBOS_DMR_VOICE_UNSUPPORTED;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_encode_canonical_ambe_frame72(fobos_dmr_voice_context *context,
                                              const int16_t *pcm8k_160,
                                              uint8_t *canonical_frame72_bytes) {
    if (!context) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    (void)pcm8k_160;
    if (canonical_frame72_bytes) {
        std::memset(canonical_frame72_bytes, 0, DMR_FRAME_BYTES);
    }
    set_error(context, "canonical AMBE3600 frame encode is not available in the mbelib backend");
    return FOBOS_DMR_VOICE_UNSUPPORTED;
}

} // extern "C"
