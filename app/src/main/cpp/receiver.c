#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dnssd.h"
#include "logger.h"
#include "raop.h"

#include "airscreen.h"

#define TAG "AirScreen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Seconds without a client /feedback heartbeat before the connection counts as lost (UxPlay default). */
#define MISSED_FEEDBACK_LIMIT 15

static unsigned short tcp_ports[2] = { 7100, 7000 };       /* mirror data, RTSP/HTTP */
static unsigned short udp_ports[3] = { 7011, 6001, 6000 }; /* timing, control, audio data */

static struct {
    raop_t *raop;
    dnssd_t *dnssd;
    pthread_t control_thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool stopping;
    bool reset_requested;
    int open_connections;
    int missed_feedback;
    char client[128];
} r = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

static void current_client(char *out, size_t len) {
    pthread_mutex_lock(&r.lock);
    snprintf(out, len, "%s", r.client);
    pthread_mutex_unlock(&r.lock);
}

static void request_reset(void) {
    pthread_mutex_lock(&r.lock);
    r.reset_requested = true;
    pthread_cond_signal(&r.wake);
    pthread_mutex_unlock(&r.lock);
}

static void conn_init(void *cls) {
    pthread_mutex_lock(&r.lock);
    r.open_connections++;
    pthread_mutex_unlock(&r.lock);
}

static void conn_destroy(void *cls) {
    pthread_mutex_lock(&r.lock);
    bool last = --r.open_connections == 0;
    pthread_mutex_unlock(&r.lock);
    if (last) {
        bridge_emit(EVENT_DISCONNECTED, NULL, 0, 0);
    }
}

static void conn_reset(void *cls, int reason) {
    LOGI("connection reset requested (reason %d)", reason);
    request_reset();
}

static void conn_feedback(void *cls) {
    pthread_mutex_lock(&r.lock);
    r.missed_feedback = 0;
    pthread_mutex_unlock(&r.lock);
}

static void report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit) {
    LOGI("connection request from %s (%s) deviceID %s", name, model, deviceid);
    pthread_mutex_lock(&r.lock);
    snprintf(r.client, sizeof(r.client), "%s", name ? name : "");
    pthread_mutex_unlock(&r.lock);
    *admit = true;
}

static int video_set_codec(void *cls, video_codec_t codec) {
    char client[sizeof(r.client)];
    current_client(client, sizeof(client));
    bridge_emit(EVENT_MIRROR_STARTED, client, codec, 0);
    return 0;
}

static void video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
}

static void video_reset(void *cls, reset_type_t type) {
    if (type == RESET_TYPE_ON_VIDEO_PLAY) {
        return;
    }
    bridge_emit(EVENT_MIRROR_STOPPED, NULL, 0, 0);
}

static void video_pause(void *cls) {
}

static void video_resume(void *cls) {
}

static void audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *using_screen,
                             bool *is_media, uint64_t *audio_format) {
    LOGI("audio format ct=%d spf=%d usingScreen=%d isMedia=%d", *ct, *spf, *using_screen, *is_media);
    if (!*using_screen) {
        char client[sizeof(r.client)];
        current_client(client, sizeof(client));
        bridge_emit(EVENT_AUDIO_ONLY_STARTED, client, 0, 0);
    }
}

static void audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
}

static double audio_set_client_volume(void *cls) {
    return 0.0;
}

static void audio_set_volume(void *cls, float volume) {
}

static void log_callback(void *cls, int level, const char *msg) {
    int prio = level <= LOGGER_ERR ? ANDROID_LOG_ERROR
             : level == LOGGER_WARNING ? ANDROID_LOG_WARN
             : level <= LOGGER_INFO ? ANDROID_LOG_INFO : ANDROID_LOG_DEBUG;
    __android_log_write(prio, "UxPlay", msg);
}

/* What UxPlay's main loop does after conn_reset: drop every connection and restart the httpd. */
static void reset_connections(void) {
    raop_stop_httpd(r.raop);
    raop_remove_known_connections(r.raop);
    unsigned short port = raop_get_port(r.raop);
    raop_start_httpd(r.raop, &port);
    raop_set_port(r.raop, port);
    bridge_emit(EVENT_DISCONNECTED, NULL, 0, 0);
}

/* raop callbacks run on UxPlay threads that raop_stop_httpd joins, so resets run here instead. */
static void *control_loop(void *arg) {
    pthread_mutex_lock(&r.lock);
    while (!r.stopping) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 1;
        if (!r.reset_requested) {
            pthread_cond_timedwait(&r.wake, &r.lock, &deadline);
        }
        if (r.stopping) break;
        if (r.open_connections > 0 && ++r.missed_feedback > MISSED_FEEDBACK_LIMIT) {
            LOGE("no client feedback for %d s, dropping connection", MISSED_FEEDBACK_LIMIT);
            r.reset_requested = true;
        }
        if (r.reset_requested) {
            r.reset_requested = false;
            r.missed_feedback = 0;
            pthread_mutex_unlock(&r.lock);
            reset_connections();
            pthread_mutex_lock(&r.lock);
        }
    }
    pthread_mutex_unlock(&r.lock);
    return NULL;
}

int receiver_start(const receiver_config_t *cfg) {
    int err = 0;
    r.dnssd = dnssd_init(cfg->name, (int) strlen(cfg->name), (const char *) cfg->hw_addr,
                         sizeof(cfg->hw_addr), 0, &err);
    if (!r.dnssd) {
        LOGE("dnssd_init failed: %d", err);
        return -1;
    }
    /* UxPlay's start_dnssd defaults: legacy pairing off, multi-codec (HEVC) per decoder support. */
    dnssd_set_airplay_features(r.dnssd, 27, 0);
    dnssd_set_airplay_features(r.dnssd, 42, cfg->h265);

    raop_callbacks_t cbs = {
        .conn_init = conn_init,
        .conn_destroy = conn_destroy,
        .conn_reset = conn_reset,
        .conn_feedback = conn_feedback,
        .report_client_request = report_client_request,
        .video_set_codec = video_set_codec,
        .video_process = video_process,
        .video_reset = video_reset,
        .video_pause = video_pause,
        .video_resume = video_resume,
        .audio_get_format = audio_get_format,
        .audio_process = audio_process,
        .audio_set_client_volume = audio_set_client_volume,
        .audio_set_volume = audio_set_volume,
    };
    r.raop = raop_init(&cbs);
    if (!r.raop) {
        LOGE("raop_init failed");
        dnssd_destroy(r.dnssd);
        r.dnssd = NULL;
        return -1;
    }
    raop_set_log_callback(r.raop, log_callback, NULL);
    raop_set_log_level(r.raop, LOGGER_INFO);

    char device_id[18];
    snprintf(device_id, sizeof(device_id), "%02x:%02x:%02x:%02x:%02x:%02x", cfg->hw_addr[0],
             cfg->hw_addr[1], cfg->hw_addr[2], cfg->hw_addr[3], cfg->hw_addr[4], cfg->hw_addr[5]);
    /* nohold = 1: a new client takes over from the current one instead of being refused */
    if (raop_init2(r.raop, 1, device_id, cfg->keyfile)) {
        LOGE("raop_init2 failed");
        receiver_stop();
        return -1;
    }
    raop_set_plist(r.raop, "width", cfg->width);
    raop_set_plist(r.raop, "height", cfg->height);
    raop_set_plist(r.raop, "refreshRate", cfg->refresh_hz);
    raop_set_plist(r.raop, "maxFPS", cfg->max_fps);
    raop_set_tcp_ports(r.raop, tcp_ports);
    raop_set_udp_ports(r.raop, udp_ports);

    unsigned short port = raop_get_port(r.raop);
    if (raop_start_httpd(r.raop, &port) < 0) {
        LOGE("raop_start_httpd failed on port %d", port);
        receiver_stop();
        return -1;
    }
    raop_set_port(r.raop, port);
    raop_set_dnssd(r.raop, r.dnssd);
    LOGI("raop httpd listening on %d", port);

    if (dnssd_register_raop(r.dnssd, port) || dnssd_register_airplay(r.dnssd, port)) {
        LOGE("dnssd registration failed");
        receiver_stop();
        return -1;
    }

    r.stopping = false;
    r.reset_requested = false;
    r.open_connections = 0;
    r.missed_feedback = 0;
    pthread_create(&r.control_thread, NULL, control_loop, NULL);
    bridge_emit(EVENT_READY, NULL, 0, 0);
    return 0;
}

void receiver_stop(void) {
    if (r.control_thread) {
        pthread_mutex_lock(&r.lock);
        r.stopping = true;
        pthread_cond_signal(&r.wake);
        pthread_mutex_unlock(&r.lock);
        pthread_join(r.control_thread, NULL);
        r.control_thread = 0;
    }
    if (r.dnssd) {
        dnssd_unregister_raop(r.dnssd);
        dnssd_unregister_airplay(r.dnssd);
    }
    if (r.raop) {
        raop_destroy(r.raop);
        r.raop = NULL;
    }
    if (r.dnssd) {
        dnssd_destroy(r.dnssd);
        r.dnssd = NULL;
    }
}
