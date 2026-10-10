#ifndef RECORDING_H
#define RECORDING_H

#include <stdint.h>

#include "airscreen.h"

/* Session recording file: the 8-byte magic, then records of rec_header_t followed by len payload bytes.
 * Little-endian, fixed-width fields, identical on 32- and 64-bit builds. */
#define REC_MAGIC "AIRSREC1"

typedef enum {
    REC_VIDEO_CODEC = 1,  /* a = video_codec_t */
    REC_VIDEO = 2,        /* payload = access unit as handed to video_renderer_push */
    REC_VIDEO_RESET = 3,
    REC_AUDIO_FORMAT = 4, /* a = ct, b = spf */
    REC_AUDIO = 5,        /* payload = packet as handed to audio_renderer_push */
    REC_AUDIO_FLUSH = 6,
} rec_kind_t;

typedef struct {
    uint32_t kind;
    uint32_t len;
    int64_t arrival_ns;   /* CLOCK_MONOTONIC on the receiver */
    uint64_t ntp_remote;  /* sender clock, ns; 0 when unknown */
    uint32_t a, b;
} rec_header_t;

_Static_assert(sizeof(rec_header_t) == 32, "record header layout must match across ABIs");

/* Starts writing files_dir/rec-<epoch ms>.bin when the system property debug.airscreen.record is 1. */
void recorder_open(const char *files_dir);
void recorder_close(void);
void recorder_write(rec_kind_t kind, const uint8_t *data, uint32_t len, uint64_t ntp_remote, uint32_t a, uint32_t b);

/* Feeds a recording into the renderers at its original pacing, video and audio on separate threads. */
int replay_start(const receiver_config_t *cfg);
void replay_stop(void);

#endif
