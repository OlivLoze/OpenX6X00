/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - a time station's minute marker.
 *
 *  The 1000 Hz tone's amplitude over 10 ms (a boxcar: the 1200 Hz of WWVH falls in its second null), a value each ms
 *  timed at the boxcar's middle. A run over three times the noise (the median of the last 3 s: the pulses take a
 *  tenth to a quarter of the time) as long as the marker, give or take 100 ms, is one; its start is where the tone
 *  crosses halfway from the noise to its level. The audio's time comes from the clock at each block of samples: the
 *  earliest of the last 10 to 20 s, as a block comes late but never early.
 */

#include "clock_beacon.h"

#include "audio.h"
#include "clock_sync.h"
#include "dsp.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define FS          ((double)AUDIO_CAPTURE_RATE / AUDIO_DECIM)
#define BOX         110         /* 10 ms */
#define ENV_N       4096        /* ms of the amplitude kept */
#define FLOOR_N     3000
#define FLOOR_EVERY 250
#define BASE_BLOCK  10.0        /* s */

#define RWM_MIN  0x0FFFFF003FFFFC00ULL   /* :10-:29, :40-:59 */
#define BPM_MIN  0x007FE0FFC1FF83FFULL   /* UTC :00-:09, :15-:24, :30-:39, :45-:54 */
#define WWV_MIN  0x0FFFFFFFFFFFFFFEULL   /* all but :00, the hour's 1500 Hz tone */

/* Heard best from France first: RWM, then BPM, WWV, WWVH. CHU closed on 22 June 2026 */
const clock_beacon_t clock_beacons[] = {
    {"RWM 4996", 4996000, 4995000, 1000, 500, 0, RWM_MIN},
    {"RWM 9996", 9996000, 9995000, 1000, 500, 0, RWM_MIN},
    {"RWM 14996", 14996000, 14995000, 1000, 500, 0, RWM_MIN},
    {"BPM 5000", 5000000, 5000000, 1000, 300, 20, BPM_MIN},
    {"BPM 10000", 10000000, 10000000, 1000, 300, 20, BPM_MIN},
    {"BPM 15000", 15000000, 15000000, 1000, 300, 20, BPM_MIN},
    {"WWV 2500", 2500000, 2500000, 1000, 800, 0, WWV_MIN},
    {"WWV 5000", 5000000, 5000000, 1000, 800, 0, WWV_MIN},
    {"WWV 10000", 10000000, 10000000, 1000, 800, 0, WWV_MIN},
    {"WWV 15000", 15000000, 15000000, 1000, 800, 0, WWV_MIN},
    {"WWV 20000", 20000000, 20000000, 1000, 800, 0, WWV_MIN},
    {"WWVH 5000", 5000000, 5000000, 1200, 800, 0, WWV_MIN},
    {"WWVH 10000", 10000000, 10000000, 1200, 800, 0, WWV_MIN},
    {"WWVH 15000", 15000000, 15000000, 1200, 800, 0, WWV_MIN},
};
const int clock_beacons_n = sizeof(clock_beacons) / sizeof(clock_beacons[0]);

static pthread_mutex_t      lock = PTHREAD_MUTEX_INITIALIZER;
static clock_beacon_state_t pub;
static volatile bool        listening;
static volatile bool        restart;

/* ---- audio thread ---- */

static unsigned gen;
static uint64_t idx;                    /* samples since the start */
static double   base_cur, base_prev;    /* the clock of sample 0, the two blocks' earliest */
static double   block_t0;

static double complex osc, rot;
static double complex box[BOX], box_sum;
static int            box_pos;
static double         ms_acc;

static float    env[ENV_N];
static double   env_t[ENV_N];
static uint64_t env_k;                  /* values so far */
static float    noise;
static bool     on;
static uint64_t on_k;

static double   prev_t = NAN, prev_raw;
static float    peak;                   /* the tone's peak over the last 200 values (0.2 s) */
static int      peak_n;

static void reset(void) {
    idx       = 0;
    base_cur  = base_prev = INFINITY;
    block_t0  = NAN;
    osc       = 1.0;
    rot       = cexp(I * 2.0 * M_PI * clock_beacons[pub.beacon].tone_hz / FS);
    peak      = 0;
    peak_n    = 0;
    memset(box, 0, sizeof(box));
    box_sum   = 0;
    box_pos   = 0;
    ms_acc    = 0;
    env_k     = 0;
    noise     = 0;
    on        = false;
    prev_t    = NAN;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

static float at(uint64_t k) {
    return env[k % ENV_N];
}

/* A run of the tone from on_k to k, maybe the marker */
static void run_over(uint64_t k) {
    int      marker = clock_beacons[pub.beacon].marker_ms;
    uint64_t len    = k - on_k;

    if (len + 100 < (uint64_t)marker || len > (uint64_t)marker + 100 || on_k < 40 || k - on_k + 40 >= ENV_N)
        return;

    float lvl[1000];
    int   n = 0;

    for (uint64_t i = on_k + 20; i + 20 < k && n < 1000; i++)
        lvl[n++] = at(i);
    if (n < 10)
        return;
    qsort(lvl, n, sizeof(float), cmp_float);

    float    level = lvl[n / 2], half = (noise + level) / 2;
    uint64_t edge  = on_k;

    for (uint64_t i = on_k - 30; i <= on_k + 30; i++)
        if (at(i) >= half) {
            edge = i;
            break;
        }

    const clock_beacon_t *b = &clock_beacons[pub.beacon];
    double t   = env_t[edge % ENV_N] + b->advance_ms / 1000.0;     /* when it stands for */
    double raw = round(t / 60.0) * 60.0 - t;
    float  snr = 20.0f * log10f(level / fmaxf(noise, 1e-9f));
    int    min = (int)((long long)llround(t / 60.0) % 60);

    if (!(b->minutes >> min & 1))       /* a minute the station marks no UTC in (BPM's UT1, RWM's quiet ones) */
        return;

    pthread_mutex_lock(&lock);
    pub.snr_db    = snr;
    pub.markers++;
    pub.marker_at = (time_t)llround(t);

    bool agree = false;

    if (isfinite(prev_t)) {
        double gap = t - prev_t;
        double m   = round(gap / 60.0);

        if (m >= 1 && m <= 5 && fabs(gap - 60.0 * m) < 0.05 && fabs(raw - prev_raw) < 0.03) {
            pub.raw    = (float)((raw + prev_raw) / 2);
            pub.ahead  = -pub.raw - clock_sync_beacon_delay(NULL);
            pub.result = true;
            pub.result_id++;
            agree = true;
        }
    }
    if (pub.hist_n == CLOCK_BEACON_HIST) {
        memmove(&pub.hist[0], &pub.hist[1], (CLOCK_BEACON_HIST - 1) * sizeof(pub.hist[0]));
        pub.hist_n--;
    }
    pub.hist[pub.hist_n++] = (clock_beacon_marker_t){(time_t)llround(t / 60.0) * 60, (float)raw,
                                                     (float)(-raw - clock_sync_beacon_delay(NULL)), snr, agree};
    pthread_mutex_unlock(&lock);
    prev_t   = t;
    prev_raw = raw;
}

static void put_env(float a, double t) {
    env[env_k % ENV_N]   = a;
    env_t[env_k % ENV_N] = t;
    env_k++;

    if (env_k % FLOOR_EVERY == 0 && env_k >= FLOOR_N) {
        static float v[FLOOR_N];

        for (int i = 0; i < FLOOR_N; i++)
            v[i] = at(env_k - FLOOR_N + i);
        qsort(v, FLOOR_N, sizeof(float), cmp_float);
        noise = v[FLOOR_N / 2];
    }
    if (noise <= 0)
        return;
    if (a > peak)
        peak = a;
    if (++peak_n >= 200) {          /* the tone's level for the screen, five times a second */
        pthread_mutex_lock(&lock);
        pub.tone_db = peak > noise ? 20.0f * log10f(peak / noise) : 0;
        pthread_mutex_unlock(&lock);
        peak   = 0;
        peak_n = 0;
    }
    if (!on && a > 3.0f * noise) {
        on   = true;
        on_k = env_k - 1;
    } else if (on && a < 2.0f * noise) {
        on = false;
        run_over(env_k - 1);
    }
}

void clock_beacon_audio(unsigned int n, float *samples) {
    struct timespec ts;
    unsigned        g = clock_sync_generation();

    if (!listening)
        return;
    if (restart || g != gen) {
        restart = false;
        gen     = g;
        reset();
    }

    clock_gettime(CLOCK_REALTIME, &ts);

    double now = ts.tv_sec + ts.tv_nsec / 1e9, cand = now - (idx + n) / FS;

    if (!isfinite(block_t0) || now - block_t0 > BASE_BLOCK) {
        base_prev = base_cur;
        base_cur  = INFINITY;
        block_t0  = now;
    }
    if (cand < base_cur)
        base_cur = cand;

    double base = fmin(base_cur, base_prev);

    for (unsigned int i = 0; i < n; i++, idx++) {
        double complex v = samples[i] * osc;

        osc *= rot;
        box_sum += v - box[box_pos];
        box[box_pos] = v;
        box_pos      = (box_pos + 1) % BOX;

        ms_acc += 1000.0;
        if (ms_acc >= FS) {
            ms_acc -= FS;
            if ((idx & 0xfff) == 0)
                osc /= cabs(osc);       /* the oscillator kept on the unit circle */
            if (idx >= BOX)
                put_env((float)(2.0 * cabs(box_sum) / BOX), base + (idx - BOX / 2.0) / FS);
        }
    }
    if ((idx & 0x3ffff) < n) {      /* the running sum summed again: no rounding left to pile up */
        box_sum = 0;
        for (int i = 0; i < BOX; i++)
            box_sum += box[i];
    }
}

/* ---- any thread ---- */

bool clock_beacon_minute_ok(int i, int minute) {
    return i >= 0 && i < clock_beacons_n && minute >= 0 && minute < 60 && (clock_beacons[i].minutes >> minute & 1);
}

int clock_beacon_next_s(int i, time_t now) {
    for (int s = 1; s <= 3600; s++)
        if ((now + s) % 60 == 0 && clock_beacon_minute_ok(i, (int)((now + s) / 60 % 60)))
            return s;
    return -1;
}

void clock_beacon_start(int i) {
    pthread_mutex_lock(&lock);
    memset(&pub, 0, sizeof(pub));
    pub.on     = true;
    pub.beacon = i >= 0 && i < clock_beacons_n ? i : 0;
    pthread_mutex_unlock(&lock);
    restart   = true;
    listening = true;
}

void clock_beacon_stop(void) {
    listening = false;
    pthread_mutex_lock(&lock);
    pub.on = false;
    pthread_mutex_unlock(&lock);
}

void clock_beacon_get(clock_beacon_state_t *st) {
    pthread_mutex_lock(&lock);
    *st = pub;
    pthread_mutex_unlock(&lock);
}
