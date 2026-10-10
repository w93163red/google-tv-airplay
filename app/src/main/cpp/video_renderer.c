#include <android/log.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#define MAX_SYNC_DELAY_NS 500000000LL
#define QUEUE_SLOTS 64            /* the sync clamp holds at most 30 access units at 60 fps */
#define QUEUE_BYTES (8 << 20)     /* the sync clamp holds about 2.5 MB at 40 Mbit/s */
#define FEED_MARGIN_NS 10000000LL /* fed this much before the measured decode latency asks */
#define LATENCY_SMOOTHING 16      /* frames of exponential averaging for the decode latency */
#define FEED_AHEAD 3              /* access units fed beyond the frame due next, see head_due_locked */
#define FEED_RECHECK_NS 20000000LL /* the due time follows the audio clock, so it is recomputed this often */
#define ENVELOPE_RISE_NS_PER_S 5000000LL /* how fast the arrival envelope forgets an early frame */
#define AUDIO_SLEW_NS_PER_S 100000000LL /* the delay follows audio at one refresh in ten at 60 Hz */
#define IDLE_SLEW_NS_PER_S 20000000LL   /* and leaves it at one in fifty */
#define AUDIO_HOLD_NS 20000000000LL     /* covers the 14 s between two YouTube videos in the live session */

typedef struct {
    int count;
    int64_t us[MAX_SAMPLES];
} samples_t;

/* A compressed access unit waiting for its feed time, stored at q.bytes + offset. */
typedef struct {
    int offset, len;
    int64_t arrival_ns;
    uint64_t ntp_remote;
    bool keyframe; /* carries parameter sets, so decoding can start here */
} access_unit_t;

/* Access units from push (the only writer of the tail) to the feeder (the only reader of the head).
 * push takes only this lock, so it never waits on the decoder. Lock order: v.lock, then q.lock. */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed; /* CLOCK_MONOTONIC; the queue stopped being empty, or a teardown ended */
    access_unit_t slots[QUEUE_SLOTS];
    int head, count;        /* moved off the head only with v.lock held too */
    uint8_t *bytes;         /* QUEUE_BYTES, allocated and touched once so push never allocates or faults */
    int tail;               /* where the newest access unit's bytes end */
    access_unit_t fed[FEED_AHEAD]; /* timing of the last access units fed, oldest at fed[fed_oldest] */
    int fed_oldest;
    video_codec_t codec;    /* written with both locks held; VIDEO_CODEC_UNKNOWN ignores pushes */
    bool resyncing;         /* overflowed: dropping until the next keyframe */
    int64_t envelope_at;    /* arrival of the access unit that last moved arrival_envelope_ns */
    int pushed, overflowed; /* this stats window */
    samples_t push;         /* push call time */
} q = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* One stats window. The MediaTek decoders never fire AMediaCodec's frame-rendered callback, so
 * "presented" is the render time handed to the surface, which excludes SurfaceFlinger and the display. */
static struct {
    pthread_mutex_t lock;
    struct { int64_t pts_us, fed_ns; uint64_t remote_ns; } pending[PENDING_SLOTS]; /* fed frames by pts */
    int next_pending;
    int rendered;     /* released to the surface */
    int late;         /* decoded after their sync target */
    int64_t last_present_ns;
    samples_t decode; /* fed to decoded */
    samples_t glass;  /* arrival to presented */
    samples_t av;     /* audio presentation minus video presentation of the same sender time */
} t = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* Smoothed fed-to-decoded time of the current decoder, 0 until its first frame. Written by the output thread. */
static _Atomic int64_t decode_latency_ns;

/* The earliest an access unit arrives relative to its sender time: the lowest arrival minus sender time,
 * rising at ENVELOPE_RISE_NS_PER_S so clock drift and a slower network path catch up with it. Plus the
 * decode latency, it is the timeline's delay without audio: a frame on the envelope shows as soon as it
 * decodes. INT64_MAX before the first access unit. Written by push under q.lock. */
static _Atomic int64_t arrival_envelope_ns = INT64_MAX;

/* Render time minus sender time for every frame. It moves toward its goal at a bounded rate, so the
 * delay never steps when audio starts or stops: frames are spread or bunched by at most a refresh in
 * ten. The goal is the averaged audio offset while audio plays and for AUDIO_HOLD_NS after it stops
 * (pauses, seeks and video switches), and otherwise the arrival envelope plus the decode latency. */
static struct {
    pthread_mutex_t lock;
    bool started;          /* delay_ns is set; cleared when the stream restarts */
    int64_t delay_ns, moved_ns;
    bool heard;            /* audio has played since the stream started */
    int64_t audio_ns, heard_ns; /* the latest averaged audio offset and when it was read */
} timeline = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* The feeder creates the decoder once codec, window and parameter sets are known; teardowns destroy it. */
static struct {
    pthread_mutex_t lock;
    atomic_int teardowns; /* callers waiting for lock to change the decoder; the feeder yields to them */
    uint16_t max_width, max_height;
    char *avc_name, *hevc_name;
    ANativeWindow *window;
    uint8_t *params;     /* latest VPS/SPS/PPS NALs, Annex-B */
    int params_len;
    int csd1_offset;     /* H.264: where the PPS starts inside params */

    AMediaCodec *decoder;
    bool decoder_synced;  /* has been fed an access unit carrying parameter sets */
    pthread_t output_thread;
    atomic_bool output_running;
    atomic_int frames_in;
    atomic_int frames_dropped;
} v = { .lock = PTHREAD_MUTEX_INITIALIZER };

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

static int64_t timeline_delay_ns(int64_t envelope) {
    int64_t now = airscreen_now_ns(), latest_audio, smoothed, goal, rate;
    bool audio = audio_renderer_offset(&latest_audio, &smoothed);
    pthread_mutex_lock(&timeline.lock);
    if (audio) {
        timeline.heard = true;
        timeline.heard_ns = now;
        timeline.audio_ns = smoothed;
    }
    if (timeline.heard && now - timeline.heard_ns < AUDIO_HOLD_NS) {
        goal = timeline.audio_ns;
        rate = AUDIO_SLEW_NS_PER_S;
    } else {
        goal = envelope + atomic_load(&decode_latency_ns);
        rate = IDLE_SLEW_NS_PER_S;
    }
    if (!timeline.started) {
        timeline.started = true;
        timeline.delay_ns = goal;
        timeline.moved_ns = now;
    } else if (now > timeline.moved_ns) {
        int64_t elapsed = now - timeline.moved_ns < 1000000000LL ? now - timeline.moved_ns : 1000000000LL;
        int64_t most = elapsed * (rate / 1000000) / 1000, change = goal - timeline.delay_ns;
        timeline.delay_ns += change > most ? most : change < -most ? -most : change;
        timeline.moved_ns = now;
    }
    int64_t delay = timeline.delay_ns;
    pthread_mutex_unlock(&timeline.lock);
    return delay;
}

/* When to show the frame with sender time remote: by the timeline, and never more than
 * MAX_SYNC_DELAY_NS after arrival. 0 when the sender time is unknown. */
static int64_t sync_target_ns(int64_t arrival_ns, uint64_t remote) {
    int64_t envelope = atomic_load(&arrival_envelope_ns);
    if (!remote || envelope == INT64_MAX) return 0;
    int64_t target = (int64_t) remote + timeline_delay_ns(envelope), latest = arrival_ns + MAX_SYNC_DELAY_NS;
    return target > latest ? latest : target;
}

/* When the head access unit goes to the decoder: the decode latency plus a margin before its sync
 * target. The MediaTek decoders emit a frame only after one to three later access units are fed, so
 * it also goes no later than a margin before the target of the access unit FEED_AHEAD before it.
 * 0 when the sender time is unknown. */
static int64_t head_due_locked(void) {
    const access_unit_t *head = &q.slots[q.head], *ahead = &q.fed[q.fed_oldest];
    int64_t target = sync_target_ns(head->arrival_ns, head->ntp_remote);
    if (!target) return 0;
    int64_t due = target - atomic_load(&decode_latency_ns) - FEED_MARGIN_NS;
    int64_t ahead_target = sync_target_ns(ahead->arrival_ns, ahead->ntp_remote);
    return ahead_target && ahead_target - FEED_MARGIN_NS < due ? ahead_target - FEED_MARGIN_NS : due;
}

static void remember_frame(int64_t pts_us, uint64_t remote_ns, int64_t fed_ns) {
    pthread_mutex_lock(&t.lock);
    t.pending[t.next_pending].pts_us = pts_us;
    t.pending[t.next_pending].fed_ns = fed_ns;
    t.pending[t.next_pending].remote_ns = remote_ns;
    t.next_pending = (t.next_pending + 1) % PENDING_SLOTS;
    pthread_mutex_unlock(&t.lock);
}

/* When to show a decoded frame (pts_us is its local arrival time): its sync target, or now when that
 * has passed, but never before the previous frame. Updates the decode latency and records the frame's stats. */
static int64_t schedule_frame(int64_t pts_us, int64_t decoded_ns) {
    int64_t present_ns = decoded_ns;
    int64_t remote = 0;
    pthread_mutex_lock(&t.lock);
    for (int i = 0; i < PENDING_SLOTS; i++) {
        if (t.pending[i].pts_us != pts_us) continue;
        remote = (int64_t) t.pending[i].remote_ns;
        int64_t target = sync_target_ns(pts_us * 1000, (uint64_t) remote);
        if (target > decoded_ns) present_ns = target;
        else if (target) t.late++;
        int64_t latency = decoded_ns - t.pending[i].fed_ns, smoothed = atomic_load(&decode_latency_ns);
        atomic_store(&decode_latency_ns, smoothed ? smoothed + (latency - smoothed) / LATENCY_SMOOTHING : latency);
        add_sample(&t.decode, latency / 1000);
        break;
    }
    if (present_ns < t.last_present_ns) present_ns = t.last_present_ns; /* out of order would cost a repeat as well as a drop */
    t.last_present_ns = present_ns;
    int64_t audio_offset, smoothed;
    if (remote && audio_renderer_offset(&audio_offset, &smoothed)) add_sample(&t.av, (remote + audio_offset - present_ns) / 1000);
    t.rendered++;
    add_sample(&t.glass, present_ns / 1000 - pts_us);
    pthread_mutex_unlock(&t.lock);
    return present_ns;
}

static void log_stats(void) {
    pthread_mutex_lock(&q.lock);
    int pushed = q.pushed, overflowed = q.overflowed;
    samples_t push = q.push;
    q.pushed = q.overflowed = q.push.count = 0;
    pthread_mutex_unlock(&q.lock);
    pthread_mutex_lock(&t.lock);
    int rendered = t.rendered, late = t.late;
    samples_t decode = t.decode, glass = t.glass, av = t.av;
    t.rendered = t.late = 0;
    t.decode.count = t.glass.count = t.av.count = 0;
    pthread_mutex_unlock(&t.lock);
    double av_p50 = percentile_ms(&av, 50);
    for (int i = 0; i < av.count; i++) av.us[i] = llabs(av.us[i]);
    __android_log_print(ANDROID_LOG_INFO, STATS_TAG,
                        "pushed=%d in=%d rendered=%d late=%d dropped=%d overflow=%d push_ms p50=%.2f p99=%.2f "
                        "decode_ms p50=%.1f p99=%.1f lead=%.1f glass_ms p50=%.1f p99=%.1f "
                        "av_offset_ms n=%d p50=%.1f abs_p99=%.1f",
                        pushed, atomic_exchange(&v.frames_in, 0), rendered, late, atomic_exchange(&v.frames_dropped, 0),
                        overflowed, percentile_ms(&push, 50), percentile_ms(&push, 99),
                        percentile_ms(&decode, 50), percentile_ms(&decode, 99),
                        (atomic_load(&decode_latency_ns) + FEED_MARGIN_NS) / 1e6,
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

/* Releases every frame to the surface as soon as it is decoded, timed by schedule_frame. Never takes v.lock. */
static void *output_loop(void *arg) {
    AMediaCodec *decoder = arg;
    int64_t window_start = airscreen_now_ns();
    while (atomic_load(&v.output_running)) {
        AMediaCodecBufferInfo info;
        ssize_t index = AMediaCodec_dequeueOutputBuffer(decoder, &info, OUTPUT_TIMEOUT_US);
        int64_t now = airscreen_now_ns();
        if (index >= 0) {
            if (info.size > 0) {
                AMediaCodec_releaseOutputBufferAtTime(decoder, index, schedule_frame(info.presentationTimeUs, now));
            } else {
                AMediaCodec_releaseOutputBuffer(decoder, index, false);
            }
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            report_size(decoder);
        }
        if (now - window_start >= 1000000000LL) {
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

static void create_decoder_locked(video_codec_t codec) {
    const char *mime = codec == VIDEO_CODEC_H265 ? "video/hevc" : "video/avc";
    const char *by_name = codec == VIDEO_CODEC_H265 ? v.hevc_name : v.avc_name;
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
    v.decoder_synced = false;
    atomic_store(&decode_latency_ns, 0);
    atomic_store(&v.output_running, true);
    pthread_create(&v.output_thread, NULL, output_loop, decoder);
}

static void clear_params_locked(void) {
    free(v.params);
    v.params = NULL;
    v.params_len = 0;
    v.csd1_offset = 0;
}

/* UxPlay prepends a keyframe's parameter-set NALs, with 4-byte start codes. Returns where they end,
 * 0 when the access unit carries none; *start is where they begin and *pps where the H.264 PPS
 * begins, relative to *start. */
static int leading_params(const uint8_t *data, int len, bool hevc, int *start, int *pps) {
    int i = 0, begin = -1, pps_at = 0;
    while (i + 4 < len) {
        if (data[i] || data[i + 1] || data[i + 2] || data[i + 3] != 1) {
            i++;
            continue;
        }
        int type = hevc ? (data[i + 4] >> 1) & 0x3f : data[i + 4] & 0x1f;
        if (!(hevc ? type >= 32 && type <= 34 : type == 7 || type == 8)) break;
        if (begin < 0) begin = i;
        if (!hevc && type == 8 && !pps_at) pps_at = i - begin;
        int end = i + 4;
        while (end + 3 < len && !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 0 && data[end + 3] == 1)) end++;
        i = end + 3 >= len ? len : end;
    }
    if (begin < 0) return 0;
    if (start) *start = begin;
    if (pps) *pps = pps_at;
    return i;
}

/* Keeps a keyframe's parameter-set NALs for decoder creation. */
static void capture_params_locked(const uint8_t *data, int len, bool hevc) {
    int start = 0, pps = 0, end = leading_params(data, len, hevc, &start, &pps);
    int params_len = end - start;
    if (!end || (params_len == v.params_len && !memcmp(data + start, v.params, params_len))) return;
    uint8_t *params = malloc(params_len);
    if (!params) return;
    memcpy(params, data + start, params_len);
    clear_params_locked();
    v.params = params;
    v.params_len = params_len;
    v.csd1_offset = pps;
}

/* Hands an access unit to the decoder, creating the decoder first when it can. Waits for an input
 * buffer rather than drop it: a lost access unit corrupts every frame that references it until the
 * next keyframe. Returns false, leaving the access unit queued, when a teardown wants v.lock. */
static bool feed_locked(const access_unit_t *au, video_codec_t codec) {
    const uint8_t *data = q.bytes + au->offset;
    if (au->keyframe) capture_params_locked(data, au->len, codec == VIDEO_CODEC_H265);
    if (!v.decoder && v.window && v.params_len > 0) create_decoder_locked(codec);
    if (v.decoder && au->keyframe) v.decoder_synced = true;
    /* A new decoder starts at a keyframe instead of on reference-less P-frames, which some decoders
     * hold forever without returning input buffers. */
    if (!v.decoder || !v.decoder_synced) {
        atomic_fetch_add(&v.frames_dropped, 1);
        return true;
    }
    ssize_t index;
    for (int waited_ms = 0;; waited_ms += INPUT_TIMEOUT_US / 1000) {
        if (atomic_load(&v.teardowns)) return false;
        index = AMediaCodec_dequeueInputBuffer(v.decoder, INPUT_TIMEOUT_US);
        if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
        if (waited_ms == 1000) LOGE("decoder took no input for 1 s, still waiting");
    }
    size_t capacity = 0;
    uint8_t *buffer = index >= 0 ? AMediaCodec_getInputBuffer(v.decoder, index, &capacity) : NULL;
    int64_t pts_us = au->arrival_ns / 1000;
    if (buffer && capacity >= (size_t) au->len) {
        memcpy(buffer, data, au->len);
        remember_frame(pts_us, au->ntp_remote, airscreen_now_ns());
        AMediaCodec_queueInputBuffer(v.decoder, index, 0, au->len, pts_us, 0);
        atomic_fetch_add(&v.frames_in, 1);
    } else {
        if (buffer) AMediaCodec_queueInputBuffer(v.decoder, index, 0, 0, pts_us, 0);
        atomic_fetch_add(&v.frames_dropped, 1);
    }
    return true;
}

/* Feeds the queued access units in order, each when head_due_locked says, so frames wait here
 * instead of holding the decoder's output buffers. Runs for the life of the process. */
static void *feed_loop(void *arg) {
    for (;;) {
        pthread_mutex_lock(&q.lock);
        for (;;) {
            if (atomic_load(&v.teardowns) || !q.count) {
                pthread_cond_wait(&q.changed, &q.lock);
                continue;
            }
            int64_t due = head_due_locked(), now = airscreen_now_ns();
            if (due <= now) break;
            if (due > now + FEED_RECHECK_NS) due = now + FEED_RECHECK_NS;
            struct timespec deadline = { .tv_sec = (time_t) (due / 1000000000), .tv_nsec = (long) (due % 1000000000) };
            pthread_cond_timedwait(&q.changed, &q.lock, &deadline);
        }
        pthread_mutex_unlock(&q.lock);

        pthread_mutex_lock(&v.lock);
        pthread_mutex_lock(&q.lock);
        /* A teardown may have emptied and refilled the queue between the two locks. */
        bool due = q.count && !atomic_load(&v.teardowns) && head_due_locked() <= airscreen_now_ns();
        access_unit_t *au = due ? &q.slots[q.head] : NULL;
        video_codec_t codec = q.codec;
        pthread_mutex_unlock(&q.lock);
        /* push never writes the head slot, and only teardowns, which need v.lock, empty the queue */
        if (au && feed_locked(au, codec)) {
            pthread_mutex_lock(&q.lock);
            q.fed[q.fed_oldest] = *au;
            q.fed_oldest = (q.fed_oldest + 1) % FEED_AHEAD;
            q.head = (q.head + 1) % QUEUE_SLOTS;
            q.count--;
            pthread_mutex_unlock(&q.lock);
        }
        pthread_mutex_unlock(&v.lock);
    }
    return NULL;
}

static void start_feeder(void) {
    q.bytes = malloc(QUEUE_BYTES);
    if (q.bytes) memset(q.bytes, 0, QUEUE_BYTES);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&q.changed, &attr);
    pthread_condattr_destroy(&attr);
    pthread_t feeder;
    pthread_create(&feeder, NULL, feed_loop, NULL);
    pthread_detach(feeder);
}

static void lock_for_teardown(void) {
    atomic_fetch_add(&v.teardowns, 1);
    pthread_mutex_lock(&v.lock);
}

static void unlock_for_teardown(void) {
    pthread_mutex_unlock(&v.lock);
    atomic_fetch_sub(&v.teardowns, 1);
    pthread_mutex_lock(&q.lock);
    pthread_cond_broadcast(&q.changed);
    pthread_mutex_unlock(&q.lock);
}

/* Ends the current stream: no decoder, no parameter sets, an empty queue. */
static void restart_locked(video_codec_t codec) {
    destroy_decoder_locked();
    clear_params_locked();
    pthread_mutex_lock(&q.lock);
    q.codec = codec;
    q.head = q.count = 0;
    q.resyncing = false;
    memset(q.fed, 0, sizeof(q.fed));
    atomic_store(&arrival_envelope_ns, INT64_MAX);
    pthread_mutex_unlock(&q.lock);
    pthread_mutex_lock(&timeline.lock);
    timeline.started = timeline.heard = false;
    pthread_mutex_unlock(&timeline.lock);
}

void video_renderer_init(const receiver_config_t *cfg) {
    static pthread_once_t feeder_once = PTHREAD_ONCE_INIT;
    pthread_once(&feeder_once, start_feeder);
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
    }
    unlock_for_teardown();
}

void video_renderer_set_codec(video_codec_t codec) {
    lock_for_teardown();
    if (codec != q.codec) restart_locked(codec);
    unlock_for_teardown();
}

void video_renderer_reset(void) {
    lock_for_teardown();
    restart_locked(VIDEO_CODEC_UNKNOWN);
    unlock_for_teardown();
}

/* Where len bytes fit in q.bytes after the queued access units, -1 when they do not. An access
 * unit stays contiguous: one that does not fit before the end of the buffer starts at 0. */
static int free_offset_locked(int len) {
    if (!q.bytes || len > QUEUE_BYTES) return -1;
    if (!q.count) return 0;
    int head = q.slots[q.head].offset;
    if (q.tail > head) return q.tail + len <= QUEUE_BYTES ? q.tail : len <= head ? 0 : -1;
    return q.tail + len <= head ? q.tail : -1;
}

/* Queues the access unit for the feeder and returns. When the queue is full the access unit is
 * dropped, and so is everything after it up to the next keyframe, which is what it references. */
void video_renderer_push(const uint8_t *data, int len, uint64_t ntp_remote) {
    int64_t arrival_ns = airscreen_now_ns();
    /* UxPlay sets data[0] = 1 on packets that failed decryption or NAL parsing. */
    if (len < 5 || data[0]) {
        atomic_fetch_add(&v.frames_dropped, 1);
        return;
    }
    pthread_mutex_lock(&q.lock);
    if (q.codec == VIDEO_CODEC_UNKNOWN) {
        pthread_mutex_unlock(&q.lock);
        return;
    }
    bool keyframe = leading_params(data, len, q.codec == VIDEO_CODEC_H265, NULL, NULL) > 0;
    bool wake = false;
    int offset = q.count < QUEUE_SLOTS && (keyframe || !q.resyncing) ? free_offset_locked(len) : -1;
    if (offset >= 0) {
        access_unit_t *au = &q.slots[(q.head + q.count) % QUEUE_SLOTS];
        memcpy(q.bytes + offset, data, len);
        q.tail = offset + len;
        au->offset = offset;
        au->len = len;
        au->arrival_ns = arrival_ns;
        au->ntp_remote = ntp_remote;
        au->keyframe = keyframe;
        if (ntp_remote) {
            int64_t envelope = atomic_load(&arrival_envelope_ns), lag = arrival_ns - (int64_t) ntp_remote;
            int64_t elapsed = arrival_ns - q.envelope_at < 1000000000LL ? arrival_ns - q.envelope_at : 1000000000LL;
            if (envelope != INT64_MAX) envelope += elapsed * (ENVELOPE_RISE_NS_PER_S / 1000000) / 1000;
            if (lag < envelope) envelope = lag;
            atomic_store(&arrival_envelope_ns, envelope);
            q.envelope_at = arrival_ns;
        }
        wake = q.count++ == 0;
        q.resyncing = false;
        q.pushed++;
    } else {
        if (!q.resyncing) LOGE("video queue full (%d access units), dropping until the next keyframe", q.count);
        q.resyncing = true;
        q.overflowed++;
    }
    pthread_mutex_unlock(&q.lock);
    /* Signaled outside the lock so the feeder cannot wake into it while push still holds it. */
    if (wake) pthread_cond_signal(&q.changed);
    int64_t took_us = (airscreen_now_ns() - arrival_ns) / 1000;
    pthread_mutex_lock(&q.lock);
    add_sample(&q.push, took_us);
    pthread_mutex_unlock(&q.lock);
}
