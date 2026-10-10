#include <aaudio/AAudio.h>
#include <android/log.h>
#include <math.h>
#include <media/NdkMediaCodec.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "airscreen.h"
#include "alac_decoder.h"
#include "audio_renderer.h"
#include "resampler.h"

#define TAG "AirScreen"
#define STATS_TAG "AirScreenStats"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SAMPLE_RATE 44100 /* AirPlay's rate; the stream runs at the device's */
#define CHANNELS 2
#define TARGET_MS 40      /* AAudio buffer */
#define PRIME_MS 30       /* jitter buffer refilled after the ring runs dry */
#define TRIM_MS 90        /* queued audio beyond this is cut back to PRIME_MS */
#define RING_FRAMES 16384 /* power of two */
#define MAX_PACKET_FRAMES RESAMPLER_MAX_IN
#define MAX_OUT_FRAMES (MAX_PACKET_FRAMES * 5)
#define MAX_SAMPLES 256
#define OFFSET_STALE_NS 500000000LL

/* PCM at the stream rate between the network thread (sole writer) and the AAudio callback (sole
 * reader). Arrival and playback run at the same rate, so whatever is queued when playback starts
 * would stay queued for good: the callback refills an empty ring to prime_frames before playing
 * and cuts a ring above trim_frames back to prime_frames. */
static struct {
    int16_t pcm[RING_FRAMES * CHANNELS];
    atomic_uint written, read;  /* frame counters, wrapping */
    atomic_bool dry;            /* the callback plays silence until prime_frames are queued */
    _Atomic int64_t shift;      /* stream frame index minus ring frame index: silence played minus frames cut */
    atomic_int underruns;       /* times the ring ran dry while playing */
    atomic_int trims;           /* times queued audio was cut */
    uint32_t prime_frames, trim_frames;
} ring;

/* At most one decoder exists, chosen by ct: AAC (4, 8) through MediaCodec, ALAC (2) through Apple's decoder. */
static struct {
    pthread_mutex_t lock;
    unsigned char ct;           /* 0 when stopped */
    unsigned short spf;
    AMediaCodec *aac;
    alac_decoder_t *alac;
    AAudioStream *stream;
    int32_t rate;               /* stream sample rate */
    resampler_t resampler;      /* SAMPLE_RATE to rate */
    int64_t ring_frames;        /* frames ever written to the ring for this stream */
    int64_t stats_start_us;
    int32_t stats_xruns;        /* AAudio xrun count at stats_start_us */
    int32_t stats_underruns;    /* ring.underruns at stats_start_us */
    int32_t stats_trims;        /* ring.trims at stats_start_us */
    int64_t dropped_frames;     /* since stats_start_us */
    int latency_count;          /* out_latency_us samples since stats_start_us */
    int64_t latency_us[MAX_SAMPLES];
    float gain;
    int16_t pcm[MAX_PACKET_FRAMES * CHANNELS];
    int16_t resampled[MAX_OUT_FRAMES * CHANNELS];
} a = { .lock = PTHREAD_MUTEX_INITIALIZER, .gain = 1.0f };

/* Written by the audio thread, read lock-free by video. */
static _Atomic int64_t presented_offset_ns, presented_at_ns;

static AMediaCodec *create_aac(unsigned char ct) {
    /* AudioSpecificConfig: AAC-ELD 44100 stereo 480 spf, AAC-LC 44100 stereo */
    static const uint8_t eld_config[] = { 0xf8, 0xe8, 0x50, 0x00 };
    static const uint8_t lc_config[] = { 0x12, 0x10 };
    AMediaCodec *codec = AMediaCodec_createDecoderByType("audio/mp4a-latm");
    if (!codec) return NULL;
    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, SAMPLE_RATE);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, CHANNELS);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_AAC_PROFILE, ct == 8 ? 39 : 2);
    if (ct == 8) {
        AMediaFormat_setBuffer(format, "csd-0", eld_config, sizeof(eld_config));
    } else {
        AMediaFormat_setBuffer(format, "csd-0", lc_config, sizeof(lc_config));
    }
    media_status_t status = AMediaCodec_configure(codec, format, NULL, NULL, 0);
    AMediaFormat_delete(format);
    if (status == AMEDIA_OK) status = AMediaCodec_start(codec);
    if (status != AMEDIA_OK) {
        LOGE("AAC decoder (ct=%d) failed to start: %d", ct, status);
        AMediaCodec_delete(codec);
        return NULL;
    }
    return codec;
}

static int64_t now_us(void) {
    return airscreen_now_ns() / 1000;
}

static int compare_i64(const void *x, const void *y) {
    int64_t p = *(const int64_t *) x, q = *(const int64_t *) y;
    return (p > q) - (p < q);
}

static int32_t max_i32(int32_t x, int32_t y) { return x > y ? x : y; }

static aaudio_data_callback_result_t on_audio(AAudioStream *stream, void *user, void *data, int32_t frames) {
    int16_t *out = data;
    uint32_t r = atomic_load_explicit(&ring.read, memory_order_relaxed);
    uint32_t queued = atomic_load_explicit(&ring.written, memory_order_acquire) - r;
    bool dry = atomic_load_explicit(&ring.dry, memory_order_relaxed);
    if (dry && queued >= ring.prime_frames) dry = false;
    if (!dry && queued > ring.trim_frames) {
        uint32_t cut = queued - ring.prime_frames;
        r += cut;
        queued -= cut;
        atomic_fetch_sub_explicit(&ring.shift, cut, memory_order_relaxed);
        atomic_fetch_add_explicit(&ring.trims, 1, memory_order_relaxed);
    }
    uint32_t n = dry ? 0 : queued < (uint32_t) frames ? queued : (uint32_t) frames;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (r + i) & (RING_FRAMES - 1);
        out[i * 2] = ring.pcm[slot * 2];
        out[i * 2 + 1] = ring.pcm[slot * 2 + 1];
    }
    if (n < (uint32_t) frames) {
        memset(out + n * CHANNELS, 0, sizeof(int16_t) * CHANNELS * (frames - n));
        atomic_fetch_add_explicit(&ring.shift, frames - n, memory_order_relaxed);
        if (!dry) atomic_fetch_add_explicit(&ring.underruns, 1, memory_order_relaxed);
        dry = true;
    }
    atomic_store_explicit(&ring.dry, dry, memory_order_relaxed);
    atomic_store_explicit(&ring.read, r + n, memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* Callback mode at the device's own rate: on the Streamer that is what reaches the FAST mixer
 * (48 kHz) and keeps the legacy path from sizing its burst to half a second. */
static AAudioStream *build_stream(void) {
    AAudioStreamBuilder *builder;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return NULL;
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, CHANNELS);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(builder, on_audio, NULL);
    AAudioStream *stream = NULL;
    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) {
        LOGE("AAudio open failed: %s", AAudio_convertResultToText(result));
        return NULL;
    }
    return stream;
}

/* The buffer is a latency target, at least two bursts on the low-latency path. On the legacy
 * (NONE) path the burst is the track's notification period, not the mixer's, so it is not used. */
static void open_stream_locked(void) {
    AAudioStream *stream = build_stream();
    if (!stream) return;
    a.rate = AAudioStream_getSampleRate(stream);
    if (resampler_init(&a.resampler, SAMPLE_RATE, a.rate) != 0) {
        AAudioStream_close(stream);
        return;
    }
    int32_t burst = AAudioStream_getFramesPerBurst(stream);
    int32_t mode = AAudioStream_getPerformanceMode(stream);
    int32_t target = a.rate * TARGET_MS / 1000;
    AAudioStream_setBufferSizeInFrames(stream, mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? max_i32(target, 2 * burst) : target);
    atomic_store(&ring.written, 0);
    atomic_store(&ring.read, 0);
    atomic_store(&ring.dry, true);
    atomic_store(&ring.shift, 0);
    atomic_store(&ring.underruns, 0);
    atomic_store(&ring.trims, 0);
    ring.prime_frames = (uint32_t) (a.rate * PRIME_MS / 1000);
    ring.trim_frames = (uint32_t) (a.rate * TRIM_MS / 1000);
    a.ring_frames = 0;
    AAudioStream_requestStart(stream);
    LOGI("AAudio stream: %d Hz (resampled from %d), device %d, performance mode %d, burst %d, buffer %d, capacity %d",
         a.rate, SAMPLE_RATE, AAudioStream_getDeviceId(stream), mode, burst,
         AAudioStream_getBufferSizeInFrames(stream), AAudioStream_getBufferCapacityInFrames(stream));
    a.stream = stream;
    a.stats_start_us = now_us();
    a.stats_xruns = 0;
    a.stats_underruns = 0;
    a.stats_trims = 0;
    a.dropped_frames = 0;
    a.latency_count = 0;
}

static void close_stream_locked(void) {
    if (!a.stream) return;
    AAudioStream_requestStop(a.stream);
    AAudioStream_close(a.stream);
    a.stream = NULL;
    resampler_free(&a.resampler);
}

static void stop_locked(void) {
    close_stream_locked();
    if (a.aac) {
        AMediaCodec_stop(a.aac);
        AMediaCodec_delete(a.aac);
        a.aac = NULL;
    }
    if (a.alac) {
        alac_decoder_destroy(a.alac);
        a.alac = NULL;
    }
    a.ct = 0;
}

static void log_stats_locked(int64_t now) {
    int32_t xruns = AAudioStream_getXRunCount(a.stream);
    int32_t underruns = atomic_load(&ring.underruns), trims = atomic_load(&ring.trims);
    int64_t level = (int64_t) (atomic_load(&ring.written) - atomic_load(&ring.read)) +
                    AAudioStream_getFramesWritten(a.stream) - AAudioStream_getFramesRead(a.stream);
    double p50 = 0, p99 = 0;
    if (a.latency_count) {
        qsort(a.latency_us, a.latency_count, sizeof(int64_t), compare_i64);
        p50 = a.latency_us[a.latency_count / 2] / 1000.0;
        p99 = a.latency_us[(a.latency_count * 99) / 100] / 1000.0;
    }
    __android_log_print(ANDROID_LOG_INFO, STATS_TAG,
                        "audio xruns=%d underruns=%d trims=%d dropped_frames=%lld level_ms=%lld "
                        "out_latency_ms p50=%.1f p99=%.1f",
                        xruns - a.stats_xruns, underruns - a.stats_underruns, trims - a.stats_trims,
                        (long long) a.dropped_frames,
                        (long long) (level * 1000 / a.rate), p50, p99);
    a.stats_start_us = now;
    a.stats_xruns = xruns;
    a.stats_underruns = underruns;
    a.stats_trims = trims;
    a.dropped_frames = 0;
    a.latency_count = 0;
}

/* stream_frame plays at the returned CLOCK_MONOTONIC ns, extrapolated from the stream's latest
 * presentation timestamp, which covers the HAL and device path. 0 before the stream has one. */
static int64_t presentation_ns_locked(int64_t stream_frame) {
    int64_t position, time_ns;
    if (AAudioStream_getTimestamp(a.stream, CLOCK_MONOTONIC, &position, &time_ns) != AAUDIO_OK) return 0;
    return time_ns + (stream_frame - position) * 1000000000LL / a.rate;
}

/* Queues PCM for the callback; frames that do not fit in the ring are dropped. */
static void write_pcm_locked(const int16_t *pcm, int frames, uint64_t ntp_remote) {
    if (frames <= 0) return;
    if (a.stream && AAudioStream_getState(a.stream) == AAUDIO_STREAM_STATE_DISCONNECTED) {
        LOGI("audio device changed, reopening stream");
        close_stream_locked();
    }
    if (!a.stream) open_stream_locked();
    if (!a.stream) return;
    if (frames > MAX_PACKET_FRAMES) frames = MAX_PACKET_FRAMES;
    for (int i = 0; i < frames * CHANNELS; i++) {
        a.pcm[i] = (int16_t) (pcm[i] * a.gain);
    }
    int n = resampler_process(&a.resampler, a.pcm, frames, a.resampled);
    uint32_t w = atomic_load_explicit(&ring.written, memory_order_relaxed);
    uint32_t space = RING_FRAMES - (w - atomic_load_explicit(&ring.read, memory_order_acquire));
    int queued = n < (int) space ? n : (int) space;
    for (int i = 0; i < queued; i++) {
        uint32_t slot = (w + i) & (RING_FRAMES - 1);
        ring.pcm[slot * 2] = a.resampled[i * 2];
        ring.pcm[slot * 2 + 1] = a.resampled[i * 2 + 1];
    }
    atomic_store_explicit(&ring.written, w + queued, memory_order_release);
    a.dropped_frames += n - queued;
    /* While dry, the callback is still adding silence ahead of these frames, so they have no fixed time yet. */
    int64_t first = a.ring_frames + atomic_load(&ring.shift);
    a.ring_frames += queued;
    int64_t presented_ns = atomic_load(&ring.dry) ? 0 : presentation_ns_locked(first);
    int64_t now = now_us();
    if (presented_ns) {
        int64_t last_ns = presented_ns + (int64_t) queued * 1000000000LL / a.rate;
        if (a.latency_count < MAX_SAMPLES) a.latency_us[a.latency_count++] = last_ns / 1000 - now;
        if (ntp_remote) {
            atomic_store(&presented_offset_ns, presented_ns - (int64_t) ntp_remote);
            atomic_store(&presented_at_ns, now * 1000);
        }
    }
    if (now - a.stats_start_us >= 1000000) log_stats_locked(now);
}

static void decode_aac_locked(const uint8_t *data, int len, uint64_t ntp_remote) {
    ssize_t in = AMediaCodec_dequeueInputBuffer(a.aac, 5000);
    if (in >= 0) {
        size_t capacity;
        uint8_t *buffer = AMediaCodec_getInputBuffer(a.aac, in, &capacity);
        if ((size_t) len > capacity) len = (int) capacity;
        memcpy(buffer, data, len);
        AMediaCodec_queueInputBuffer(a.aac, in, 0, len, (int64_t) (ntp_remote / 1000), 0);
    }
    /* Waits briefly for this packet's PCM so output leaves in the packets' rhythm, not in clumps. */
    for (int64_t timeout_us = in >= 0 ? 10000 : 0;; timeout_us = 0) {
        AMediaCodecBufferInfo info;
        ssize_t out = AMediaCodec_dequeueOutputBuffer(a.aac, &info, timeout_us);
        if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || out == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (out < 0) break;
        size_t size;
        uint8_t *buffer = AMediaCodec_getOutputBuffer(a.aac, out, &size);
        write_pcm_locked((const int16_t *) (buffer + info.offset), info.size / (int) (sizeof(int16_t) * CHANNELS),
                         (uint64_t) info.presentationTimeUs * 1000);
        AMediaCodec_releaseOutputBuffer(a.aac, out, false);
    }
}

void audio_renderer_start(unsigned char ct, unsigned short spf) {
    if (ct == 2 && !spf) spf = 352;
    pthread_mutex_lock(&a.lock);
    if (ct != a.ct || spf != a.spf) {
        stop_locked();
        if (ct == 2 && spf > MAX_PACKET_FRAMES) {
            LOGE("ALAC spf=%d exceeds %d", spf, MAX_PACKET_FRAMES);
        } else if (ct == 2) {
            a.alac = alac_decoder_create(spf);
        } else if (ct == 4 || ct == 8) {
            a.aac = create_aac(ct);
        } else {
            LOGE("unsupported audio ct=%d", ct);
        }
        if (a.alac || a.aac) {
            a.ct = ct;
            a.spf = spf;
            open_stream_locked();
        }
    }
    pthread_mutex_unlock(&a.lock);
}

void audio_renderer_push(const uint8_t *data, int len, uint64_t ntp_remote) {
    pthread_mutex_lock(&a.lock);
    if (a.aac) {
        decode_aac_locked(data, len, ntp_remote);
    } else if (a.alac) {
        int16_t out[MAX_PACKET_FRAMES * CHANNELS];
        int frames = alac_decoder_decode(a.alac, data, len, out, a.spf);
        write_pcm_locked(out, frames, ntp_remote);
    }
    pthread_mutex_unlock(&a.lock);
}

bool audio_renderer_offset(int64_t *offset_ns) {
    int64_t at = atomic_load(&presented_at_ns);
    if (!at || airscreen_now_ns() - at > OFFSET_STALE_NS) return false;
    *offset_ns = atomic_load(&presented_offset_ns);
    return true;
}

/* AirPlay volume: -144 is mute, otherwise -30..0 dB; UxPlay treats -30 as silent too. */
void audio_renderer_set_volume(float airplay_db) {
    float gain = airplay_db <= -30.0f ? 0.0f : airplay_db >= 0.0f ? 1.0f : powf(10.0f, airplay_db / 20.0f);
    pthread_mutex_lock(&a.lock);
    a.gain = gain;
    pthread_mutex_unlock(&a.lock);
}

void audio_renderer_flush(void) {
    pthread_mutex_lock(&a.lock);
    if (a.aac) AMediaCodec_flush(a.aac);
    pthread_mutex_unlock(&a.lock);
}

void audio_renderer_stop(void) {
    pthread_mutex_lock(&a.lock);
    stop_locked();
    a.spf = 0;
    pthread_mutex_unlock(&a.lock);
}
