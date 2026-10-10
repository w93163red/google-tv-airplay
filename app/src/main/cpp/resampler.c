#include "resampler.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HALF (RESAMPLER_TAPS / 2)

static int gcd(int a, int b) {
    while (b) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

int resampler_init(resampler_t *r, int in_rate, int out_rate) {
    int g = gcd(in_rate, out_rate);
    r->up = out_rate / g;
    r->down = in_rate / g;
    r->table = NULL;
    if (r->up != r->down) {
        r->table = malloc(sizeof(float) * r->up * RESAMPLER_TAPS);
        if (!r->table) return -1;
        /* Cut off at the lower of the two Nyquist rates, in input-sample units. */
        double cutoff = r->up < r->down ? (double) r->up / r->down : 1.0;
        for (int p = 0; p < r->up; p++) {
            float *row = r->table + p * RESAMPLER_TAPS;
            double sum = 0;
            for (int k = 0; k < RESAMPLER_TAPS; k++) {
                double d = (k - (HALF - 1)) - (double) p / r->up;
                double x = M_PI * cutoff * d;
                double sinc = d == 0 ? 1.0 : sin(x) / x;
                double t = d / HALF;
                double window = fabs(t) >= 1 ? 0 : 0.42 + 0.5 * cos(M_PI * t) + 0.08 * cos(2 * M_PI * t);
                row[k] = (float) (sinc * window);
                sum += row[k];
            }
            for (int k = 0; k < RESAMPLER_TAPS; k++) row[k] = (float) (row[k] / sum);
        }
    }
    resampler_reset(r);
    return 0;
}

void resampler_reset(resampler_t *r) {
    r->phase = 0;
    r->kept = RESAMPLER_TAPS - 1;
    memset(r->work, 0, sizeof(float) * r->kept * 2);
}

void resampler_free(resampler_t *r) {
    free(r->table);
    r->table = NULL;
}

int resampler_max_out(const resampler_t *r, int in_frames) {
    return (int) (((int64_t) in_frames * r->up + r->down - 1) / r->down) + 1;
}

int resampler_process(resampler_t *r, const int16_t *in, int in_frames, int16_t *out) {
    if (!r->table) {
        memcpy(out, in, sizeof(int16_t) * in_frames * 2);
        return in_frames;
    }
    float *w = r->work;
    for (int i = 0; i < in_frames * 2; i++) w[r->kept * 2 + i] = in[i];
    int total = r->kept + in_frames, i = 0, n = 0;
    while (i + RESAMPLER_TAPS <= total) {
        const float *row = r->table + r->phase * RESAMPLER_TAPS;
        const float *x = w + i * 2;
        float left = 0, right = 0;
        for (int k = 0; k < RESAMPLER_TAPS; k++) {
            left += row[k] * x[k * 2];
            right += row[k] * x[k * 2 + 1];
        }
        out[n * 2] = (int16_t) fmaxf(-32768.f, fminf(32767.f, lrintf(left)));
        out[n * 2 + 1] = (int16_t) fmaxf(-32768.f, fminf(32767.f, lrintf(right)));
        n++;
        r->phase += r->down;
        i += r->phase / r->up;
        r->phase %= r->up;
    }
    r->kept = total - i;
    memmove(w, w + i * 2, sizeof(float) * r->kept * 2);
    return n;
}
