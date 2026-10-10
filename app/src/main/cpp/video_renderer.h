#ifndef VIDEO_RENDERER_H
#define VIDEO_RENDERER_H

#include <android/native_window.h>
#include <stdint.h>

#include "raop.h"

void video_renderer_init(uint16_t max_width, uint16_t max_height);
void video_renderer_set_window(ANativeWindow *window); /* NULL when the surface is destroyed */
void video_renderer_set_codec(video_codec_t codec);
void video_renderer_push(const uint8_t *data, int len);
void video_renderer_reset(void);

#endif
