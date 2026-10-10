#ifndef RESAMPLER_H
#define RESAMPLER_H

#include <stdint.h>

#define RESAMPLER_TAPS 32
#define RESAMPLER_MAX_IN 4096

/* Streaming stereo int16 sample-rate converter: polyphase windowed sinc over the exact rational
 * ratio, so it never drifts. Passes samples through when the rates match. */
typedef struct {
    int up, down;  /* out_rate and in_rate divided by their gcd */
    float *table;  /* up rows of RESAMPLER_TAPS coefficients, NULL when up == down */
    int phase;     /* position of the next output between two inputs, in 1/up steps */
    int kept;      /* input frames carried over in work */
    float work[(RESAMPLER_TAPS - 1 + RESAMPLER_MAX_IN) * 2];
} resampler_t;

int resampler_init(resampler_t *r, int in_rate, int out_rate);
void resampler_reset(resampler_t *r);
void resampler_free(resampler_t *r);
/* Upper bound on the output frames of one resampler_process call with in_frames. */
int resampler_max_out(const resampler_t *r, int in_frames);
/* in_frames <= RESAMPLER_MAX_IN. Returns the frames written to out. */
int resampler_process(resampler_t *r, const int16_t *in, int in_frames, int16_t *out);

#endif
