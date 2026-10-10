#ifndef VIDEO_RENDERER_H
#define VIDEO_RENDERER_H

#include <android/native_window.h>
#include <stdint.h>

#include "airscreen.h"
#include "raop.h"

void video_renderer_init(const receiver_config_t *cfg);
void video_renderer_set_window(ANativeWindow *window); /* NULL when the surface is destroyed */
void video_renderer_set_codec(video_codec_t codec);
/* ntp_remote: the sender-clock time of the frame in ns, 0 when unknown. */
void video_renderer_push(const uint8_t *data, int len, uint64_t ntp_remote);
void video_renderer_reset(void);

#endif
