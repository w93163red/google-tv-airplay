#include <android/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>

#include "audio_renderer.h"
#include "recording.h"
#include "video_renderer.h"

#define TAG "AirScreen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define REPLAY_LEAD_NS 200000000LL

static struct {
    pthread_mutex_t lock;
    FILE *file;
} rec = { .lock = PTHREAD_MUTEX_INITIALIZER };

void recorder_open(const char *files_dir) {
    char value[PROP_VALUE_MAX] = "";
    __system_property_get("debug.airscreen.record", value);
    if (strcmp(value, "1") != 0) return;
    pthread_mutex_lock(&rec.lock);
    if (!rec.file) {
        char path[512];
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        snprintf(path, sizeof(path), "%s/rec-%lld%03ld.bin", files_dir, (long long) now.tv_sec, now.tv_nsec / 1000000);
        rec.file = fopen(path, "wbx");
        if (rec.file) {
            setvbuf(rec.file, NULL, _IOFBF, 1 << 20);
            fwrite(REC_MAGIC, 1, 8, rec.file);
            LOGI("recording session to %s", path);
        } else {
            LOGE("cannot create %s", path);
        }
    }
    pthread_mutex_unlock(&rec.lock);
}

void recorder_close(void) {
    pthread_mutex_lock(&rec.lock);
    if (rec.file) {
        fclose(rec.file);
        rec.file = NULL;
        LOGI("recording closed");
    }
    pthread_mutex_unlock(&rec.lock);
}

void recorder_write(rec_kind_t kind, const uint8_t *data, uint32_t len, uint64_t ntp_remote, uint32_t a, uint32_t b) {
    rec_header_t h = {
        .kind = kind, .len = len, .arrival_ns = airscreen_now_ns(), .ntp_remote = ntp_remote, .a = a, .b = b,
    };
    pthread_mutex_lock(&rec.lock);
    if (rec.file) {
        fwrite(&h, sizeof(h), 1, rec.file);
        if (len) fwrite(data, 1, len, rec.file);
    }
    pthread_mutex_unlock(&rec.lock);
}

static struct {
    char path[512];
    pthread_t threads[2];
    int thread_count;
    atomic_bool stop;
    atomic_int running;
    int64_t start_ns;       /* local time the first record is replayed at */
    int64_t first_arrival;  /* arrival_ns of the first record */
} rp;

static bool sleep_until(int64_t deadline_ns) {
    while (!atomic_load(&rp.stop)) {
        int64_t left = deadline_ns - airscreen_now_ns();
        if (left <= 0) return true;
        if (left > 50000000) left = 50000000;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = (long) left };
        nanosleep(&ts, NULL);
    }
    return false;
}

static FILE *open_recording(void) {
    FILE *f = fopen(rp.path, "rb");
    char magic[8];
    if (f && (fread(magic, 1, 8, f) != 8 || memcmp(magic, REC_MAGIC, 8) != 0)) {
        fclose(f);
        f = NULL;
    }
    return f;
}

/* One thread per stream, as in a live session, so a video decoder teardown never delays audio. */
static void *replay_loop(void *arg) {
    bool video = arg != NULL;
    FILE *f = open_recording();
    uint8_t *buf = NULL;
    uint32_t cap = 0;
    rec_header_t h;
    while (f && fread(&h, sizeof(h), 1, f) == 1) {
        if (h.len > cap) {
            uint8_t *grown = realloc(buf, h.len);
            if (!grown) break;
            buf = grown;
            cap = h.len;
        }
        if (h.len && fread(buf, 1, h.len, f) != h.len) break;
        bool mine = video ? h.kind <= REC_VIDEO_RESET : h.kind >= REC_AUDIO_FORMAT;
        if (!mine) continue;
        if (!sleep_until(rp.start_ns + (h.arrival_ns - rp.first_arrival))) break;
        switch (h.kind) {
            case REC_VIDEO_CODEC:
                video_renderer_set_codec((video_codec_t) h.a);
                bridge_emit(EVENT_MIRROR_STARTED, "replay", (int) h.a, 0);
                break;
            case REC_VIDEO:
                video_renderer_push(buf, (int) h.len, h.ntp_remote);
                break;
            case REC_VIDEO_RESET:
                video_renderer_reset();
                bridge_emit(EVENT_MIRROR_STOPPED, NULL, 0, 0);
                break;
            case REC_AUDIO_FORMAT:
                audio_renderer_start((unsigned char) h.a, (unsigned short) h.b);
                break;
            case REC_AUDIO:
                audio_renderer_push(buf, (int) h.len, h.ntp_remote, h.a);
                break;
            case REC_AUDIO_FLUSH:
                audio_renderer_flush();
                break;
        }
    }
    free(buf);
    if (f) fclose(f);
    if (video) {
        video_renderer_reset();
        bridge_emit(EVENT_MIRROR_STOPPED, NULL, 0, 0);
    } else {
        audio_renderer_stop();
    }
    if (atomic_fetch_sub(&rp.running, 1) == 1) LOGI("replay finished: %s", rp.path);
    return NULL;
}

int replay_start(const receiver_config_t *cfg) {
    video_renderer_init(cfg);
    snprintf(rp.path, sizeof(rp.path), "%s", cfg->replay);
    FILE *f = open_recording();
    rec_header_t first;
    if (!f || fread(&first, sizeof(first), 1, f) != 1) {
        LOGE("cannot read recording %s (a recording older than %s needs scripts/rec-add-rtp.py)", rp.path, REC_MAGIC);
        if (f) fclose(f);
        return -1;
    }
    fclose(f);
    LOGI("replaying %s", rp.path);
    atomic_store(&rp.stop, false);
    atomic_store(&rp.running, 2);
    rp.first_arrival = first.arrival_ns;
    rp.start_ns = airscreen_now_ns() + REPLAY_LEAD_NS;
    rp.thread_count = 0;
    pthread_create(&rp.threads[rp.thread_count++], NULL, replay_loop, (void *) 1);
    pthread_create(&rp.threads[rp.thread_count++], NULL, replay_loop, NULL);
    return 0;
}

void replay_stop(void) {
    atomic_store(&rp.stop, true);
    for (int i = 0; i < rp.thread_count; i++) pthread_join(rp.threads[i], NULL);
    rp.thread_count = 0;
}
