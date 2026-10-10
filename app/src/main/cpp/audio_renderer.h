#ifndef AUDIO_RENDERER_H
#define AUDIO_RENDERER_H

#include <stdint.h>

/* ct: 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD (RAOP SETUP "ct"). */
void audio_renderer_start(unsigned char ct, unsigned short spf);
void audio_renderer_push(const uint8_t *data, int len);
void audio_renderer_set_volume(float airplay_db);
void audio_renderer_flush(void);
void audio_renderer_stop(void);

#endif
