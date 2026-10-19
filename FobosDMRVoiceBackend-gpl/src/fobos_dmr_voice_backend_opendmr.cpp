#include "fobos_dmr_voice_backend.h"

// SPDX-License-Identifier: GPL-2.0-only

#include <array>
#include <cstdint>
#include <cstring>
#include <new>

#include "cgolay24128.h"
#include "mbeenc.h"
#include "opendmr.h"

struct fobos_dmr_voice_context {
    opendmr_decoder_t *decoder = nullptr;
    MBEEncoder *encoder = nullptr;
    char last_error[192] = {};
};

namespace {

constexpr int PCM_8K_SAMPLES = 160;
constexpr int CANONICAL_FRAME_BYTES = 9;
constexpr int AMBE_PAYLOAD_BYTES = 7;

const fobos_dmr_voice_backend_info BACKEND_INFO = {
    FOBOS_DMR_VOICE_ABI_VERSION,
    FOBOS_DMR_VOICE_CAP_DECODE_CANONICAL_AMBE_FRAME72 |
        FOBOS_DMR_VOICE_CAP_ENCODE_AMBE2450_PAYLOAD49 |
        FOBOS_DMR_VOICE_CAP_CANONICAL_FROM_AMBE2450_PAYLOAD49 |
        FOBOS_DMR_VOICE_CAP_ENCODE_CANONICAL_AMBE_FRAME72,
    "fobos.dmr.voice.opendmr",
    "Fobos DMR Voice Backend (OpenDMR/OP25)",
    "0.1.0",
    "GPL-2.0",
    "Optional AMBE/AMBE+2 encode/decode backend. AMBE technologies may be patent-encumbered in some jurisdictions."
};

MBEEncoder *create_encoder() {
    MBEEncoder *encoder = new (std::nothrow) MBEEncoder();
    if (encoder) {
        encoder->set_dmr_mode();
        encoder->set_gain_adjust(1.0f);
    }
    return encoder;
}

uint32_t compute_prng_mask_23bit(uint32_t c0) {
    uint16_t pr[24] = {};
    pr[0] = static_cast<uint16_t>(16U * c0);
    for (int i = 1; i < 24; ++i) {
        pr[i] = static_cast<uint16_t>(
            (173U * static_cast<uint32_t>(pr[i - 1]) + 13849U) % 65536U);
    }

    uint32_t mask = 0;
    for (int i = 1; i <= 23; ++i) {
        if ((pr[i] / 32768U) != 0U) {
            mask |= (1U << (23 - i));
        }
    }
    return mask;
}

void pack_ambe2450_parameters(const int b[9], uint8_t bits49[49]) {
    std::memset(bits49, 0, 49);
    bits49[0] = (b[0] >> 6) & 1;
    bits49[1] = (b[0] >> 5) & 1;
    bits49[2] = (b[0] >> 4) & 1;
    bits49[3] = (b[0] >> 3) & 1;
    bits49[4] = (b[1] >> 4) & 1;
    bits49[5] = (b[1] >> 3) & 1;
    bits49[6] = (b[1] >> 2) & 1;
    bits49[7] = (b[1] >> 1) & 1;
    bits49[8] = (b[2] >> 4) & 1;
    bits49[9] = (b[2] >> 3) & 1;
    bits49[10] = (b[2] >> 2) & 1;
    bits49[11] = (b[2] >> 1) & 1;
    bits49[12] = (b[3] >> 8) & 1;
    bits49[13] = (b[3] >> 7) & 1;
    bits49[14] = (b[3] >> 6) & 1;
    bits49[15] = (b[3] >> 5) & 1;
    bits49[16] = (b[3] >> 4) & 1;
    bits49[17] = (b[3] >> 3) & 1;
    bits49[18] = (b[3] >> 2) & 1;
    bits49[19] = (b[3] >> 1) & 1;
    bits49[20] = (b[4] >> 6) & 1;
    bits49[21] = (b[4] >> 5) & 1;
    bits49[22] = (b[4] >> 4) & 1;
    bits49[23] = (b[4] >> 3) & 1;
    bits49[24] = (b[5] >> 4) & 1;
    bits49[25] = (b[5] >> 3) & 1;
    bits49[26] = (b[5] >> 2) & 1;
    bits49[27] = (b[5] >> 1) & 1;
    bits49[28] = (b[6] >> 3) & 1;
    bits49[29] = (b[6] >> 2) & 1;
    bits49[30] = (b[6] >> 1) & 1;
    bits49[31] = (b[7] >> 3) & 1;
    bits49[32] = (b[7] >> 2) & 1;
    bits49[33] = (b[7] >> 1) & 1;
    bits49[34] = (b[8] >> 2) & 1;
    bits49[35] = b[1] & 1;
    bits49[36] = b[2] & 1;
    bits49[37] = (b[0] >> 2) & 1;
    bits49[38] = (b[0] >> 1) & 1;
    bits49[39] = b[0] & 1;
    bits49[40] = b[3] & 1;
    bits49[41] = (b[4] >> 2) & 1;
    bits49[42] = (b[4] >> 1) & 1;
    bits49[43] = b[4] & 1;
    bits49[44] = b[5] & 1;
    bits49[45] = b[6] & 1;
    bits49[46] = b[7] & 1;
    bits49[47] = (b[8] >> 1) & 1;
    bits49[48] = b[8] & 1;
}

bool canonical_from_payload49(const uint8_t *payload49, uint8_t *frame72) {
    if (!payload49 || !frame72) {
        return false;
    }
    uint8_t bits49[49] = {};
    for (int bit = 0; bit < 49; ++bit) {
        bits49[bit] = (payload49[bit / 8] >> (7 - (bit % 8))) & 1U;
    }

    uint32_t c0 = 0;
    uint32_t c1 = 0;
    for (int i = 0; i < 12; ++i) {
        c0 = (c0 << 1) | bits49[i];
        c1 = (c1 << 1) | bits49[12 + i];
    }

    const uint32_t a = CGolay24128::encode24128(c0);
    const uint32_t bCodeword =
        CGolay24128::encode23127(c1) ^ compute_prng_mask_23bit(c0);

    uint32_t cBlock = 0;
    for (int i = 24; i < 49; ++i) {
        cBlock = (cBlock << 1) | bits49[i];
    }

    std::memset(frame72, 0, CANONICAL_FRAME_BYTES);
    for (int i = 0; i < 24; ++i) {
        if ((a >> (23 - i)) & 1U) {
            frame72[i / 8] |= static_cast<uint8_t>(1U << (7 - (i % 8)));
        }
    }
    for (int i = 0; i < 23; ++i) {
        const int pos = 24 + i;
        if ((bCodeword >> (22 - i)) & 1U) {
            frame72[pos / 8] |= static_cast<uint8_t>(1U << (7 - (pos % 8)));
        }
    }
    for (int i = 0; i < 25; ++i) {
        const int pos = 47 + i;
        if ((cBlock >> (24 - i)) & 1U) {
            frame72[pos / 8] |= static_cast<uint8_t>(1U << (7 - (pos % 8)));
        }
    }
    return true;
}

bool encode_payload49(MBEEncoder *encoder, const int16_t *pcm, uint8_t *payload49) {
    if (!encoder || !pcm || !payload49) {
        return false;
    }
    int b[9] = {};
    encoder->encode_dmr_params(pcm, b);
    uint8_t bits49[49] = {};
    pack_ambe2450_parameters(b, bits49);
    std::memset(payload49, 0, AMBE_PAYLOAD_BYTES);
    for (int bit = 0; bit < 49; ++bit) {
        payload49[bit / 8] |= static_cast<uint8_t>(bits49[bit] << (7 - (bit % 8)));
    }
    return true;
}

bool encode_canonical_ambe_frame(MBEEncoder *encoder,
                                 const int16_t *pcm,
                                 uint8_t *frame72) {
    uint8_t payload49[AMBE_PAYLOAD_BYTES] = {};
    return encode_payload49(encoder, pcm, payload49) &&
           canonical_from_payload49(payload49, frame72);
}

void set_error(fobos_dmr_voice_context *context, const char *message) {
    if (!context) {
        return;
    }
    std::strncpy(context->last_error, message, sizeof(context->last_error) - 1);
    context->last_error[sizeof(context->last_error) - 1] = '\0';
}

bool ensure_decoder(fobos_dmr_voice_context *context) {
    if (!context) {
        return false;
    }
    if (!context->decoder) {
        context->decoder = opendmr_decoder_create();
    }
    if (!context->decoder) {
        set_error(context, "failed to create OpenDMR decoder");
        return false;
    }
    return true;
}

bool ensure_encoder(fobos_dmr_voice_context *context) {
    if (!context) {
        return false;
    }
    if (!context->encoder) {
        context->encoder = create_encoder();
    }
    if (!context->encoder) {
        set_error(context, "failed to create OpenDMR encoder");
        return false;
    }
    return true;
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
    context->decoder = opendmr_decoder_create();
    context->encoder = create_encoder();
    if (!context->decoder || !context->encoder) {
        set_error(context, "failed to initialize OpenDMR backend");
    }
    return context;
}

FOBOS_DMR_VOICE_API void
fobos_dmr_voice_destroy(fobos_dmr_voice_context *context) {
    if (!context) {
        return;
    }
    opendmr_decoder_destroy(context->decoder);
    delete context->encoder;
    delete context;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_reset(fobos_dmr_voice_context *context) {
    if (!context) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    if (context->decoder) {
        opendmr_decoder_reset(context->decoder);
    }
    if (context->encoder) {
        delete context->encoder;
        context->encoder = create_encoder();
        if (!context->encoder) {
            set_error(context, "failed to reset OpenDMR encoder");
            return FOBOS_DMR_VOICE_NOT_AVAILABLE;
        }
    }
    set_error(context, "");
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
    if (!context) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    (void)frame72_bytes;
    (void)pcm8k_160;
    (void)error_count;
    set_error(context, "raw DMR over-the-air frame decode is not available in the OpenDMR backend");
    return FOBOS_DMR_VOICE_UNSUPPORTED;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_decode_dmr_ambe_soft_frame72(fobos_dmr_voice_context *context,
                                             const uint8_t *frame72_bytes,
                                             const uint8_t *reliability72,
                                             int16_t *pcm8k_160,
                                             int *error_count) {
    if (!context) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    (void)frame72_bytes;
    (void)reliability72;
    (void)pcm8k_160;
    (void)error_count;
    set_error(context, "soft DMR over-the-air frame decode is not available in the OpenDMR backend");
    return FOBOS_DMR_VOICE_UNSUPPORTED;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_decode_ambe2450_payload49(fobos_dmr_voice_context *context,
                                          const uint8_t *payload49_bytes,
                                          int corrected_errors,
                                          int16_t *pcm8k_160,
                                          int *error_count) {
    if (!context) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    (void)payload49_bytes;
    (void)corrected_errors;
    (void)pcm8k_160;
    (void)error_count;
    set_error(context, "AMBE2450 payload decode is not available in the OpenDMR backend");
    return FOBOS_DMR_VOICE_UNSUPPORTED;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_decode_canonical_ambe_frame72(fobos_dmr_voice_context *context,
                                              const uint8_t *canonical_frame72_bytes,
                                              int16_t *pcm8k_160,
                                              int *error_count) {
    if (!context || !canonical_frame72_bytes || !pcm8k_160) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    if (!ensure_decoder(context)) {
        return FOBOS_DMR_VOICE_NOT_AVAILABLE;
    }
    int errors = 0;
    const bool ok = opendmr_decode(context->decoder,
                                   canonical_frame72_bytes,
                                   pcm8k_160,
                                   &errors);
    if (!ok) {
        set_error(context, "OpenDMR failed to decode canonical AMBE frame");
        return FOBOS_DMR_VOICE_DECODE_FAILED;
    }
    if (error_count) {
        *error_count = errors;
    }
    set_error(context, "");
    return FOBOS_DMR_VOICE_OK;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_encode_ambe2450_payload49(fobos_dmr_voice_context *context,
                                          const int16_t *pcm8k_160,
                                          uint8_t *payload49_bytes) {
    if (!context || !pcm8k_160 || !payload49_bytes) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    if (!ensure_encoder(context)) {
        return FOBOS_DMR_VOICE_NOT_AVAILABLE;
    }
    if (!encode_payload49(context->encoder, pcm8k_160, payload49_bytes)) {
        set_error(context, "OpenDMR failed to encode AMBE2450 payload");
        return FOBOS_DMR_VOICE_ENCODE_FAILED;
    }
    set_error(context, "");
    return FOBOS_DMR_VOICE_OK;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_encode_canonical_ambe_frame72(fobos_dmr_voice_context *context,
                                              const int16_t *pcm8k_160,
                                              uint8_t *canonical_frame72_bytes) {
    if (!context || !pcm8k_160 || !canonical_frame72_bytes) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    if (!ensure_encoder(context)) {
        return FOBOS_DMR_VOICE_NOT_AVAILABLE;
    }
    std::memset(canonical_frame72_bytes, 0, CANONICAL_FRAME_BYTES);
    const bool ok = encode_canonical_ambe_frame(context->encoder,
                                                pcm8k_160,
                                                canonical_frame72_bytes);
    if (!ok) {
        set_error(context, "OpenDMR failed to encode canonical AMBE frame");
        return FOBOS_DMR_VOICE_ENCODE_FAILED;
    }
    set_error(context, "");
    return FOBOS_DMR_VOICE_OK;
}

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_canonical_from_ambe2450_payload49(fobos_dmr_voice_context *context,
                                                  const uint8_t *payload49_bytes,
                                                  uint8_t *canonical_frame72_bytes) {
    if (!context || !payload49_bytes || !canonical_frame72_bytes) {
        return FOBOS_DMR_VOICE_INVALID_ARGUMENT;
    }
    if (!canonical_from_payload49(payload49_bytes, canonical_frame72_bytes)) {
        set_error(context, "OpenDMR failed to channel-code AMBE2450 payload");
        return FOBOS_DMR_VOICE_ENCODE_FAILED;
    }
    set_error(context, "");
    return FOBOS_DMR_VOICE_OK;
}

} // extern "C"
