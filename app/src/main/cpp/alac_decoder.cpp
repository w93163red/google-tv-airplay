#include "alac_decoder.h"

#include "ALACBitUtilities.h"
#include "ALACDecoder.h"

struct alac_decoder {
    ALACDecoder decoder;
};

alac_decoder_t *alac_decoder_create(uint32_t frames_per_packet) {
    /* ALACSpecificConfig, big-endian */
    uint8_t cookie[24] = {
        (uint8_t) (frames_per_packet >> 24), (uint8_t) (frames_per_packet >> 16),
        (uint8_t) (frames_per_packet >> 8), (uint8_t) frames_per_packet,
        0,          /* compatibleVersion */
        16,         /* bitDepth */
        40, 10, 14, /* pb, mb, kb */
        2,          /* numChannels */
        0, 255,     /* maxRun */
        0, 0, 0, 0, /* maxFrameBytes */
        0, 0, 0, 0, /* avgBitRate */
        0, 0, 0xac, 0x44, /* sampleRate 44100 */
    };
    auto *d = new alac_decoder;
    if (d->decoder.Init(cookie, sizeof(cookie)) != 0) {
        delete d;
        return nullptr;
    }
    return d;
}

int alac_decoder_decode(alac_decoder_t *d, const uint8_t *in, int in_len, int16_t *out, uint32_t max_frames) {
    BitBuffer bits;
    BitBufferInit(&bits, const_cast<uint8_t *>(in), (uint32_t) in_len);
    uint32_t frames = 0;
    if (d->decoder.Decode(&bits, reinterpret_cast<uint8_t *>(out), max_frames, 2, &frames) != 0) {
        return -1;
    }
    return (int) frames;
}

void alac_decoder_destroy(alac_decoder_t *d) {
    delete d;
}
