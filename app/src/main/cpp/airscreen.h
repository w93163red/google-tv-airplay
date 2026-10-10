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
    EVENT_READY = 0,              /* httpd listening, services registered */
    EVENT_MIRROR_STARTED = 1,     /* text = client name, a = video_codec_t */
    EVENT_VIDEO_SIZE = 2,         /* a = width, b = height */
    EVENT_AUDIO_ONLY_STARTED = 3, /* text = client name */
    EVENT_MIRROR_STOPPED = 4,
    EVENT_DISCONNECTED = 5,
} native_event_t;

int receiver_start(const receiver_config_t *cfg);
void receiver_stop(void);

/* Implemented in jni_bridge.c. Callable from any thread. */
void bridge_emit(native_event_t event, const char *text, int a, int b);
void bridge_register_service(int service, const char *name, unsigned short port,
                             const unsigned char *txt, int txt_len);
void bridge_unregister_service(int service);

#endif
