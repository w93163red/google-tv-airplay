#include <aaudio/AAudio.h>
#include <android/log.h>
#include <math.h>
#include <media/NdkMediaCodec.h>
#include <pthread.h>
#include <string.h>

#include "alac_decoder.h"
#include "audio_renderer.h"

#define TAG "AirScreen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SAMPLE_RATE 44100
#define CHANNELS 2
#define JITTER_FRAMES (SAMPLE_RATE * 30 / 1000)
#define MAX_BUFFER_FRAMES (SAMPLE_RATE * 80 / 1000)
#define MAX_PACKET_FRAMES 4096

/* At most one decoder exists, chosen by ct: AAC (4, 8) through MediaCodec, ALAC (2) through Apple's decoder. */
static struct {
    pthread_mutex_t lock;
    unsigned char ct;           /* 0 when stopped */
    unsigned short spf;
    AMediaCodec *aac;
    alac_decoder_t *alac;
    AAudioStream *stream;
    float gain;
    int16_t pcm[MAX_PACKET_FRAMES * CHANNELS];
} a = { .lock = PTHREAD_MUTEX_INITIALIZER, .gain = 1.0f };

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

static AAudioStream *open_stream(void) {
    AAudioStreamBuilder *builder;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return NULL;
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, CHANNELS);
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setBufferCapacityInFrames(builder, MAX_BUFFER_FRAMES * 2);
    AAudioStream *stream = NULL;
    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) {
        LOGE("AAudio open failed: %s", AAudio_convertResultToText(result));
        return NULL;
    }
    AAudioStream_setBufferSizeInFrames(stream, MAX_BUFFER_FRAMES);
    AAudioStream_requestStart(stream);
    LOGI("AAudio stream: %d Hz, burst %d, buffer %d, performance mode %d",
         AAudioStream_getSampleRate(stream), AAudioStream_getFramesPerBurst(stream),
         AAudioStream_getBufferSizeInFrames(stream), AAudioStream_getPerformanceMode(stream));
    return stream;
}

static void stop_locked(void) {
    if (a.stream) {
        AAudioStream_requestStop(a.stream);
        AAudioStream_close(a.stream);
        a.stream = NULL;
    }
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

/* Plays immediately; when the stream has run dry, primes it with JITTER_FRAMES of silence
 * so network jitter does not cause an underrun per packet. Frames beyond the cap are dropped. */
static void write_pcm_locked(const int16_t *pcm, int frames) {
    if (frames <= 0) return;
    if (a.stream && AAudioStream_getState(a.stream) == AAUDIO_STREAM_STATE_DISCONNECTED) {
        LOGI("audio device changed, reopening stream");
        AAudioStream_close(a.stream);
        a.stream = NULL;
    }
    if (!a.stream) a.stream = open_stream();
    if (!a.stream) return;
    if (frames > MAX_PACKET_FRAMES) frames = MAX_PACKET_FRAMES;
    int64_t level = AAudioStream_getFramesWritten(a.stream) - AAudioStream_getFramesRead(a.stream);
    if (level <= 0) {
        static const int16_t silence[JITTER_FRAMES * CHANNELS];
        AAudioStream_write(a.stream, silence, JITTER_FRAMES, 0);
    }
    for (int i = 0; i < frames * CHANNELS; i++) {
        a.pcm[i] = (int16_t) (pcm[i] * a.gain);
    }
    AAudioStream_write(a.stream, a.pcm, frames, 0);
}

static void decode_aac_locked(const uint8_t *data, int len) {
    ssize_t in = AMediaCodec_dequeueInputBuffer(a.aac, 5000);
    if (in >= 0) {
        size_t capacity;
        uint8_t *buffer = AMediaCodec_getInputBuffer(a.aac, in, &capacity);
        if ((size_t) len > capacity) len = (int) capacity;
        memcpy(buffer, data, len);
        AMediaCodec_queueInputBuffer(a.aac, in, 0, len, 0, 0);
    }
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t out = AMediaCodec_dequeueOutputBuffer(a.aac, &info, 0);
        if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || out == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (out < 0) break;
        size_t size;
        uint8_t *buffer = AMediaCodec_getOutputBuffer(a.aac, out, &size);
        write_pcm_locked((const int16_t *) (buffer + info.offset), info.size / (int) (sizeof(int16_t) * CHANNELS));
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
            a.stream = open_stream();
        }
    }
    pthread_mutex_unlock(&a.lock);
}

void audio_renderer_push(const uint8_t *data, int len) {
    pthread_mutex_lock(&a.lock);
    if (a.aac) {
        decode_aac_locked(data, len);
    } else if (a.alac) {
        int16_t out[MAX_PACKET_FRAMES * CHANNELS];
        int frames = alac_decoder_decode(a.alac, data, len, out, a.spf);
        write_pcm_locked(out, frames);
    }
    pthread_mutex_unlock(&a.lock);
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
