#include <android/log.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "audio_renderer.h"
#include "video_renderer.h"

#define TAG "AirScreen"
#define STATS_TAG "AirScreenStats"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define INPUT_TIMEOUT_US 10000
#define OUTPUT_TIMEOUT_US 10000
#define MAX_SAMPLES 512

#define PENDING_SLOTS 256

typedef struct {
    int count;
    int64_t us[MAX_SAMPLES];
} samples_t;

/* One stats window. The MediaTek decoders never fire AMediaCodec's frame-rendered callback, so
 * "presented" is the render time handed to the surface, which excludes SurfaceFlinger and the display. */
static struct {
    pthread_mutex_t lock;
    struct { int64_t pts_us; uint64_t remote_ns; } pending[PENDING_SLOTS]; /* queued frames by pts */
    int next_pending;
    int rendered;     /* released to the surface */
    samples_t decode; /* arrival to decoded */
    samples_t glass;  /* arrival to presented */
    samples_t av;     /* audio presentation minus video presentation of the same sender time */
} t = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* The decoder exists exactly when codec, window and parameter sets are all known; reconcile() keeps it so. */
static struct {
    pthread_mutex_t lock;
    atomic_int teardowns; /* callers waiting for lock to change the decoder; a blocked push yields to them */
    uint16_t max_width, max_height;
    char *avc_name, *hevc_name;
    video_codec_t codec;
    ANativeWindow *window;
    uint8_t *params;     /* latest VPS/SPS/PPS NALs, Annex-B */
    int params_len;
    int csd1_offset;     /* H.264: where the PPS starts inside params */

    AMediaCodec *decoder;
    video_codec_t decoder_codec;
    bool decoder_synced;  /* has been fed an access unit carrying parameter sets */
    pthread_t output_thread;
    atomic_bool output_running;
    atomic_int frames_in;
    atomic_int frames_dropped;
} v = { .lock = PTHREAD_MUTEX_INITIALIZER };

static int64_t now_us(void) {
    return airscreen_now_ns() / 1000;
}

static int compare_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *) a, y = *(const int64_t *) b;
    return (x > y) - (x < y);
}

static void add_sample(samples_t *s, int64_t us) {
    if (s->count < MAX_SAMPLES) s->us[s->count++] = us;
}

/* Sorts in place. */
static double percentile_ms(samples_t *s, int pct) {
    if (!s->count) return 0;
    qsort(s->us, s->count, sizeof(int64_t), compare_i64);
    return s->us[(s->count * pct) / 100] / 1000.0;
}

static void remember_frame(int64_t pts_us, uint64_t remote_ns) {
    pthread_mutex_lock(&t.lock);
    t.pending[t.next_pending].pts_us = pts_us;
    t.pending[t.next_pending].remote_ns = remote_ns;
    t.next_pending = (t.next_pending + 1) % PENDING_SLOTS;
    pthread_mutex_unlock(&t.lock);
}

static uint64_t frame_remote_locked(int64_t pts_us) {
    for (int i = 0; i < PENDING_SLOTS; i++) {
        if (t.pending[i].pts_us == pts_us) return t.pending[i].remote_ns;
    }
    return 0;
}

/* pts_us is the frame's local arrival time. */
static void note_presented(int64_t pts_us, int64_t decoded_us, int64_t presented_ns) {
    int64_t audio_offset;
    pthread_mutex_lock(&t.lock);
    t.rendered++;
    add_sample(&t.decode, decoded_us - pts_us);
    add_sample(&t.glass, presented_ns / 1000 - pts_us);
    uint64_t remote = frame_remote_locked(pts_us);
    if (remote && audio_renderer_offset(&audio_offset)) {
        add_sample(&t.av, ((int64_t) remote + audio_offset - presented_ns) / 1000);
    }
    pthread_mutex_unlock(&t.lock);
}

static void log_stats(void) {
    pthread_mutex_lock(&t.lock);
    int rendered = t.rendered;
    samples_t decode = t.decode, glass = t.glass, av = t.av;
    t.rendered = 0;
    t.decode.count = t.glass.count = t.av.count = 0;
    pthread_mutex_unlock(&t.lock);
    double av_p50 = percentile_ms(&av, 50);
    for (int i = 0; i < av.count; i++) av.us[i] = llabs(av.us[i]);
    __android_log_print(ANDROID_LOG_INFO, STATS_TAG,
                        "in=%d rendered=%d dropped=%d decode_ms p50=%.1f p99=%.1f "
                        "glass_ms p50=%.1f p99=%.1f av_offset_ms n=%d p50=%.1f abs_p99=%.1f",
                        atomic_exchange(&v.frames_in, 0), rendered, atomic_exchange(&v.frames_dropped, 0),
                        percentile_ms(&decode, 50), percentile_ms(&decode, 99),
                        percentile_ms(&glass, 50), percentile_ms(&glass, 99),
                        av.count, av_p50, percentile_ms(&av, 99));
}

static void report_size(AMediaCodec *decoder) {
    AMediaFormat *format = AMediaCodec_getOutputFormat(decoder);
    int32_t w = 0, h = 0, left = 0, top = 0, right = -1, bottom = -1;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &w);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &h);
    if (AMediaFormat_getRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, &left, &top, &right, &bottom)) {
        w = right - left + 1;
        h = bottom - top + 1;
    }
    AMediaFormat_delete(format);
    LOGI("video size %dx%d", w, h);
    bridge_emit(EVENT_VIDEO_SIZE, NULL, w, h);
}

/* Releases every frame to the surface the moment it is decoded. Never takes v.lock. */
static void *output_loop(void *arg) {
    AMediaCodec *decoder = arg;
    int64_t window_start = now_us();
    while (atomic_load(&v.output_running)) {
        AMediaCodecBufferInfo info;
        ssize_t index = AMediaCodec_dequeueOutputBuffer(decoder, &info, OUTPUT_TIMEOUT_US);
        int64_t now = now_us();
        if (index >= 0) {
            bool render = info.size > 0;
            AMediaCodec_releaseOutputBuffer(decoder, index, render);
            if (render) note_presented(info.presentationTimeUs, now, now * 1000);
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            report_size(decoder);
        }
        if (now - window_start >= 1000000) {
            log_stats();
            window_start = now;
        }
    }
    return NULL;
}

static void destroy_decoder_locked(void) {
    if (!v.decoder) return;
    atomic_store(&v.output_running, false);
    pthread_join(v.output_thread, NULL);
    AMediaCodec_stop(v.decoder);
    AMediaCodec_delete(v.decoder);
    v.decoder = NULL;
    LOGI("video decoder released");
}

static void create_decoder_locked(void) {
    const char *mime = v.codec == VIDEO_CODEC_H265 ? "video/hevc" : "video/avc";
    const char *by_name = v.codec == VIDEO_CODEC_H265 ? v.hevc_name : v.avc_name;
    AMediaCodec *decoder = by_name ? AMediaCodec_createCodecByName(by_name) : NULL;
    if (!decoder) decoder = AMediaCodec_createDecoderByType(mime);
    if (!decoder) {
        LOGE("no decoder for %s", mime);
        return;
    }
    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, v.max_width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, v.max_height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_WIDTH, v.max_width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_HEIGHT, v.max_height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_LOW_LATENCY, 1);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PRIORITY, 0);
    if (v.csd1_offset > 0) {
        AMediaFormat_setBuffer(format, "csd-0", v.params, v.csd1_offset);
        AMediaFormat_setBuffer(format, "csd-1", v.params + v.csd1_offset, v.params_len - v.csd1_offset);
    } else {
        AMediaFormat_setBuffer(format, "csd-0", v.params, v.params_len);
    }
    media_status_t status = AMediaCodec_configure(decoder, format, v.window, NULL, 0);
    AMediaFormat_delete(format);
    if (status == AMEDIA_OK) status = AMediaCodec_start(decoder);
    if (status != AMEDIA_OK) {
        LOGE("starting %s decoder failed: %d", mime, status);
        AMediaCodec_delete(decoder);
        return;
    }
    char *name = NULL;
    AMediaCodec_getName(decoder, &name);
    LOGI("video decoder %s started for %s, max %dx%d", name ? name : "?", mime, v.max_width, v.max_height);
    if (name) AMediaCodec_releaseName(decoder, name);
    v.decoder = decoder;
    v.decoder_codec = v.codec;
    v.decoder_synced = false;
    atomic_store(&v.output_running, true);
    pthread_create(&v.output_thread, NULL, output_loop, decoder);
}

static void reconcile_locked(void) {
    bool wanted = v.codec != VIDEO_CODEC_UNKNOWN && v.window && v.params_len > 0;
    if (v.decoder && (!wanted || v.decoder_codec != v.codec)) destroy_decoder_locked();
    if (!v.decoder && wanted) create_decoder_locked();
}

static void clear_params_locked(void) {
    free(v.params);
    v.params = NULL;
    v.params_len = 0;
    v.csd1_offset = 0;
}

/* Keeps the parameter-set NALs of an access unit (UxPlay emits 4-byte start codes) for decoder (re)creation.
 * Returns whether the access unit carries parameter sets. */
static bool capture_params_locked(const uint8_t *data, int len) {
    bool hevc = v.codec == VIDEO_CODEC_H265;
    uint8_t *params = NULL;
    int params_len = 0, csd1_offset = 0;
    int i = 0;
    while (i + 4 < len) {
        if (data[i] || data[i + 1] || data[i + 2] || data[i + 3] != 1) {
            i++;
            continue;
        }
        int start = i + 4;
        int type = hevc ? (data[start] >> 1) & 0x3f : data[start] & 0x1f;
        bool is_param = hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
        if (!is_param) break;  /* UxPlay prepends parameter sets, so the slices need no scan */
        int end = start;
        while (end + 3 < len && !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 0 && data[end + 3] == 1)) end++;
        if (end + 3 >= len) end = len;
        params = realloc(params, params_len + (end - i));
        if (!hevc && type == 8 && !csd1_offset) csd1_offset = params_len;
        memcpy(params + params_len, data + i, end - i);
        params_len += end - i;
        i = end;
    }
    if (!params_len) return false;
    if (params_len == v.params_len && !memcmp(params, v.params, params_len)) {
        free(params);
        return true;
    }
    clear_params_locked();
    v.params = params;
    v.params_len = params_len;
    v.csd1_offset = csd1_offset;
    return true;
}

static void lock_for_teardown(void) {
    atomic_fetch_add(&v.teardowns, 1);
    pthread_mutex_lock(&v.lock);
}

static void unlock_for_teardown(void) {
    pthread_mutex_unlock(&v.lock);
    atomic_fetch_sub(&v.teardowns, 1);
}

/* Waits for an input buffer as long as it takes: dropping an access unit corrupts every frame that
 * references it until the next IDR, and iOS sends those rarely. Gives up only for a teardown. */
static ssize_t dequeue_input_locked(void) {
    for (;;) {
        ssize_t index = AMediaCodec_dequeueInputBuffer(v.decoder, INPUT_TIMEOUT_US);
        if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER || atomic_load(&v.teardowns)) return index;
    }
}

void video_renderer_init(const receiver_config_t *cfg) {
    pthread_mutex_lock(&v.lock);
    v.max_width = cfg->width;
    v.max_height = cfg->height;
    free(v.avc_name);
    free(v.hevc_name);
    v.avc_name = cfg->avc_decoder ? strdup(cfg->avc_decoder) : NULL;
    v.hevc_name = cfg->hevc_decoder ? strdup(cfg->hevc_decoder) : NULL;
    pthread_mutex_unlock(&v.lock);
}

void video_renderer_set_window(ANativeWindow *window) {
    lock_for_teardown();
    if (window != v.window) {
        destroy_decoder_locked();
        if (v.window) ANativeWindow_release(v.window);
        v.window = window;
        if (window) ANativeWindow_acquire(window);
        reconcile_locked();
    }
    unlock_for_teardown();
}

void video_renderer_set_codec(video_codec_t codec) {
    lock_for_teardown();
    if (codec != v.codec) {
        clear_params_locked();
        v.codec = codec;
        reconcile_locked();
    }
    unlock_for_teardown();
}

void video_renderer_reset(void) {
    lock_for_teardown();
    v.codec = VIDEO_CODEC_UNKNOWN;
    clear_params_locked();
    reconcile_locked();
    unlock_for_teardown();
}

void video_renderer_push(const uint8_t *data, int len, uint64_t ntp_remote) {
    int64_t arrival_us = now_us();
    /* UxPlay sets data[0] = 1 on packets that failed decryption or NAL parsing. */
    if (len < 5 || data[0]) {
        atomic_fetch_add(&v.frames_dropped, 1);
        return;
    }
    pthread_mutex_lock(&v.lock);
    if (v.codec == VIDEO_CODEC_UNKNOWN) {
        pthread_mutex_unlock(&v.lock);
        return;
    }
    bool keyframe = capture_params_locked(data, len);
    reconcile_locked();
    if (v.decoder && keyframe) v.decoder_synced = true;
    /* A decoder recreated mid-session (new surface) starts at the next keyframe instead of on
     * reference-less P-frames, which some decoders hold forever without returning input buffers. */
    ssize_t index = v.decoder && v.decoder_synced ? dequeue_input_locked() : -1;
    size_t capacity = 0;
    uint8_t *buffer = index >= 0 ? AMediaCodec_getInputBuffer(v.decoder, index, &capacity) : NULL;
    if (buffer && capacity >= (size_t) len) {
        memcpy(buffer, data, len);
        remember_frame(arrival_us, ntp_remote);
        AMediaCodec_queueInputBuffer(v.decoder, index, 0, len, arrival_us, 0);
        atomic_fetch_add(&v.frames_in, 1);
    } else {
        if (buffer) AMediaCodec_queueInputBuffer(v.decoder, index, 0, 0, now_us(), 0);
        atomic_fetch_add(&v.frames_dropped, 1);
    }
    pthread_mutex_unlock(&v.lock);
}
