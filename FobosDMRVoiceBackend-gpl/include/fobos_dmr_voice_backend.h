#ifndef FOBOS_DMR_VOICE_BACKEND_H
#define FOBOS_DMR_VOICE_BACKEND_H

/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(FOBOS_DMR_VOICE_BACKEND_BUILD)
#define FOBOS_DMR_VOICE_API __declspec(dllexport)
#else
#define FOBOS_DMR_VOICE_API __declspec(dllimport)
#endif
#else
#define FOBOS_DMR_VOICE_API __attribute__((visibility("default")))
#endif

#define FOBOS_DMR_VOICE_ABI_VERSION 1u

enum fobos_dmr_voice_status {
    FOBOS_DMR_VOICE_OK = 0,
    FOBOS_DMR_VOICE_INVALID_ARGUMENT = -1,
    FOBOS_DMR_VOICE_UNSUPPORTED = -2,
    FOBOS_DMR_VOICE_DECODE_FAILED = -3,
    FOBOS_DMR_VOICE_NOT_AVAILABLE = -4,
    FOBOS_DMR_VOICE_ENCODE_FAILED = -5
};

enum fobos_dmr_voice_capability {
    FOBOS_DMR_VOICE_CAP_DECODE_DMR_AMBE_FRAME72 = 1u << 0,
    FOBOS_DMR_VOICE_CAP_DECODE_DMR_AMBE_SOFT_FRAME72 = 1u << 1,
    FOBOS_DMR_VOICE_CAP_DECODE_AMBE2450_PAYLOAD49 = 1u << 2,
    FOBOS_DMR_VOICE_CAP_ENCODE_AMBE2450_PAYLOAD49 = 1u << 3,
    FOBOS_DMR_VOICE_CAP_DECODE_CANONICAL_AMBE_FRAME72 = 1u << 4,
    FOBOS_DMR_VOICE_CAP_ENCODE_CANONICAL_AMBE_FRAME72 = 1u << 5,
    FOBOS_DMR_VOICE_CAP_CANONICAL_FROM_AMBE2450_PAYLOAD49 = 1u << 6
};

struct fobos_dmr_voice_backend_info {
    uint32_t abi_version;
    uint32_t capability_flags;
    const char *backend_id;
    const char *display_name;
    const char *version;
    const char *license;
    const char *notice;
};

typedef struct fobos_dmr_voice_context fobos_dmr_voice_context;

FOBOS_DMR_VOICE_API const struct fobos_dmr_voice_backend_info *
fobos_dmr_voice_get_backend_info(void);

FOBOS_DMR_VOICE_API fobos_dmr_voice_context *
fobos_dmr_voice_create(void);

FOBOS_DMR_VOICE_API void
fobos_dmr_voice_destroy(fobos_dmr_voice_context *context);

FOBOS_DMR_VOICE_API int
fobos_dmr_voice_reset(fobos_dmr_voice_context *context);

FOBOS_DMR_VOICE_API const char *
fobos_dmr_voice_get_last_error(fobos_dmr_voice_context *context);

/*
 * Decode one DMR on-air AMBE frame.
 *
 * frame72_bytes points to 9 bytes, MSB-first bit order. The output buffer must
 * hold exactly 160 signed 16-bit PCM samples at 8000 Hz.
 */
FOBOS_DMR_VOICE_API int
fobos_dmr_voice_decode_dmr_ambe_frame72(fobos_dmr_voice_context *context,
                                        const uint8_t *frame72_bytes,
                                        int16_t *pcm8k_160,
                                        int *error_count);

/*
 * Decode one DMR on-air AMBE soft frame.
 *
 * reliability72 points to 72 reliability values, one per hard bit. Zero means
 * least reliable and 255 means most reliable.
 */
FOBOS_DMR_VOICE_API int
fobos_dmr_voice_decode_dmr_ambe_soft_frame72(fobos_dmr_voice_context *context,
                                             const uint8_t *frame72_bytes,
                                             const uint8_t *reliability72,
                                             int16_t *pcm8k_160,
                                             int *error_count);

/*
 * Decode one AMBE2450 payload.
 *
 * payload49_bytes points to at least 7 bytes. Only the first 49 MSB-first bits
 * are consumed. corrected_errors may be used by backends that accept caller FEC
 * confidence hints.
 */
FOBOS_DMR_VOICE_API int
fobos_dmr_voice_decode_ambe2450_payload49(fobos_dmr_voice_context *context,
                                          const uint8_t *payload49_bytes,
                                          int corrected_errors,
                                          int16_t *pcm8k_160,
                                          int *error_count);

/*
 * Decode one AMBE3600x2450 frame in canonical/DVSI order.
 *
 * canonical_frame72_bytes points to 9 bytes, MSB-first bit order. This is not
 * the raw over-the-air DMR interleaved order. OpenDMR-style backends generally
 * use this representation.
 */
FOBOS_DMR_VOICE_API int
fobos_dmr_voice_decode_canonical_ambe_frame72(fobos_dmr_voice_context *context,
                                              const uint8_t *canonical_frame72_bytes,
                                              int16_t *pcm8k_160,
                                              int *error_count);

/*
 * Encode one 20 ms PCM frame to AMBE2450 payload.
 *
 * pcm8k_160 must point to 160 signed 16-bit PCM samples at 8000 Hz.
 * payload49_bytes must point to at least 7 bytes and receives the first 49
 * MSB-first bits. Decode-only backends return FOBOS_DMR_VOICE_UNSUPPORTED.
 */
FOBOS_DMR_VOICE_API int
fobos_dmr_voice_encode_ambe2450_payload49(fobos_dmr_voice_context *context,
                                          const int16_t *pcm8k_160,
                                          uint8_t *payload49_bytes);

/*
 * Encode one 20 ms PCM frame to AMBE3600x2450 in canonical/DVSI order.
 *
 * pcm8k_160 must point to 160 signed 16-bit PCM samples at 8000 Hz.
 * canonical_frame72_bytes must point to 9 bytes. This output still needs DMR
 * burst/FEC/interleave placement before it can be transmitted over the air.
 */
FOBOS_DMR_VOICE_API int
fobos_dmr_voice_encode_canonical_ambe_frame72(fobos_dmr_voice_context *context,
                                              const int16_t *pcm8k_160,
                                              uint8_t *canonical_frame72_bytes);

/*
 * Apply AMBE channel coding to an already encoded 49-bit payload.
 *
 * This optional operation lets the host apply an authorized privacy
 * keystream to the 49 codec bits before Golay/channel coding. Backends that
 * advertise FOBOS_DMR_VOICE_CAP_CANONICAL_FROM_AMBE2450_PAYLOAD49 must export
 * this symbol.
 */
FOBOS_DMR_VOICE_API int
fobos_dmr_voice_canonical_from_ambe2450_payload49(fobos_dmr_voice_context *context,
                                                  const uint8_t *payload49_bytes,
                                                  uint8_t *canonical_frame72_bytes);

#ifdef __cplusplus
}
#endif

#endif /* FOBOS_DMR_VOICE_BACKEND_H */
