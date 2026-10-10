#ifndef AUDIO_RENDERER_H
#define AUDIO_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

/* ct: 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD (RAOP SETUP "ct"). */
void audio_renderer_start(unsigned char ct, unsigned short spf);
/* ntp_remote: the sender-clock time of the packet's first sample in ns, 0 when unknown. */
void audio_renderer_push(const uint8_t *data, int len, uint64_t ntp_remote);
/* Local CLOCK_MONOTONIC time minus sender time at which audio is currently presented, from the
 * latest packet. False when no audio has been presented within the last 500 ms. */
bool audio_renderer_offset(int64_t *offset_ns);
void audio_renderer_set_volume(float airplay_db);
void audio_renderer_flush(void);
void audio_renderer_stop(void);

#endif
