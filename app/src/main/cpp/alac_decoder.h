#ifndef ALAC_DECODER_H
#define ALAC_DECODER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct alac_decoder alac_decoder_t;

/* 44100 Hz, 16-bit stereo, AirPlay's fixed fmtp "96 <spf> 0 16 40 10 14 2 255 0 0 44100". */
alac_decoder_t *alac_decoder_create(uint32_t frames_per_packet);
/* Returns decoded frames written as interleaved int16 to out, or -1. */
int alac_decoder_decode(alac_decoder_t *d, const uint8_t *in, int in_len, int16_t *out, uint32_t max_frames);
void alac_decoder_destroy(alac_decoder_t *d);

#ifdef __cplusplus
}
#endif
#endif
