#ifndef AIRSCREEN_H
#define AIRSCREEN_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name;
    uint8_t hw_addr[6];
    uint16_t width, height, refresh_hz, max_fps;
    bool h265;
    const char *keyfile;
} receiver_config_t;

/* Ordinals are mirrored by NativeEvent.fromNative in Kotlin. */
typedef enum {
    EVENT_MIRROR_STARTED = 0,     /* text = client name, a = video_codec_t */
    EVENT_VIDEO_SIZE = 1,         /* a = width, b = height */
    EVENT_AUDIO_ONLY_STARTED = 2, /* text = client name */
    EVENT_MIRROR_STOPPED = 3,
    EVENT_DISCONNECTED = 4,
    EVENT_FAILED = 5,             /* text = reason */
} native_event_t;

/* Implemented in jni_bridge.c. Callable from any thread. */
void bridge_emit(native_event_t event, const char *text, int a, int b);
void bridge_register_service(int service, const char *name, unsigned short port,
                             const unsigned char *txt, int txt_len);
void bridge_unregister_service(int service);

#endif
