/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - a Bluetooth headset's three-band equalizers (voice_eq.h)
 */

#include "voice_eq.h"

#include <math.h>
#include <string.h>

/* What the meter reads on a model voice: noise shaped as the long-term average speech spectrum (Byrne et al. 1994,
 * the third octaves from 100 Hz to 4 kHz), tools/voice_eq ref. The TX target, the SSB emphasis on it: a little less
 * bass (boom, and power the PA spends where it does not carry) and a little more presence */
#define VEQ_BASS_REF_DB    -1.9f
#define VEQ_TREBLE_REF_DB  -5.2f
#define VEQ_BASS_SSB_DB    -4.0f
#define VEQ_TREBLE_SSB_DB  3.0f

/* RX auto: this share of the frames standing out of the floor (averaged over about a second) is a voice; steady
 * noise, the floor following it, has next to none. Under the quiet level, nothing to listen to */
#define VEQ_ACTIVITY_VOICE 0.2f
#define VEQ_HOLD_FRAMES    150      /* 3 s */
#define VEQ_QUIET_DBFS     -55.0f

static const int rx_voice[VEQ_BANDS] = {-3, 2, 3};  /* less rumble, the words forward */
static const int rx_noise[VEQ_BANDS] = {0, 0, -6};  /* the hiss down while waiting */

/* ---- biquads (RBJ cookbook), transposed direct form II ---- */

static void bq_norm(veq_biquad_t *f, float b0, float b1, float b2, float a0, float a1, float a2) {
    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;
}

static void bq_shelf(veq_biquad_t *f, float fs, float hz, float db, bool high) {
    float a  = powf(10.0f, db / 40.0f);
    float w  = 2.0f * (float)M_PI * hz / fs;
    float c  = cosf(w);
    float al = sinf(w) / 2.0f * sqrtf(2.0f);    /* shelf slope 1 */
    float sa = 2.0f * sqrtf(a) * al;
    float s  = high ? -1.0f : 1.0f;             /* the high shelf: cos and the odd terms the other way */

    bq_norm(f, a * ((a + 1) - s * (a - 1) * c + sa), s * 2 * a * ((a - 1) - s * (a + 1) * c),
            a * ((a + 1) - s * (a - 1) * c - sa), (a + 1) + s * (a - 1) * c + sa, -s * 2 * ((a - 1) + s * (a + 1) * c),
            (a + 1) + s * (a - 1) * c - sa);
}

static void bq_peak(veq_biquad_t *f, float fs, float hz, float db, float q) {
    float a  = powf(10.0f, db / 40.0f);
    float w  = 2.0f * (float)M_PI * hz / fs;
    float al = sinf(w) / (2.0f * q);

    bq_norm(f, 1 + al * a, -2 * cosf(w), 1 - al * a, 1 + al / a, -2 * cosf(w), 1 - al / a);
}

static void bq_band(veq_biquad_t *f, float fs, float hz, float q) {
    float w  = 2.0f * (float)M_PI * hz / fs;
    float al = sinf(w) / (2.0f * q);

    bq_norm(f, al, 0, -al, 1 + al, -2 * cosf(w), 1 - al);
}

static inline float bq_run(veq_biquad_t *f, float x) {
    float y = f->b0 * x + f->z1;

    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* ---- the equalizer ---- */

void veq_init(veq_t *e, float fs) {
    memset(e, 0, sizeof(*e));
    e->fs = fs;
    e->db[VEQ_BASS] = e->db[VEQ_MID] = e->db[VEQ_TREBLE] = 99;     /* set below whatever comes */
    veq_set(e, 0, 0, 0);
}

static int clip_db(int v) {
    return v < -VEQ_DB_MAX ? -VEQ_DB_MAX : v > VEQ_DB_MAX ? VEQ_DB_MAX : v;
}

void veq_set(veq_t *e, int bass, int mid, int treble) {
    bass   = clip_db(bass);
    mid    = clip_db(mid);
    treble = clip_db(treble);
    /* The filters' state kept: a change while the audio runs makes no click */
    if (bass != e->db[VEQ_BASS]) bq_shelf(&e->f[VEQ_BASS], e->fs, VEQ_BASS_HZ, (float)bass, false);
    if (mid != e->db[VEQ_MID]) bq_peak(&e->f[VEQ_MID], e->fs, VEQ_MID_HZ, (float)mid, 0.9f);
    if (treble != e->db[VEQ_TREBLE]) bq_shelf(&e->f[VEQ_TREBLE], e->fs, VEQ_TREBLE_HZ, (float)treble, true);
    e->db[VEQ_BASS]   = bass;
    e->db[VEQ_MID]    = mid;
    e->db[VEQ_TREBLE] = treble;
    e->flat = !bass && !mid && !treble;
}

void veq_fade_in(veq_t *e, int ms) {
    e->ramp_n = e->ramp = (int)(e->fs * ms / 1000.0f);
}

void veq_run(veq_t *e, const int16_t *in, int16_t *out, size_t n, float gain_db) {
    float g = powf(10.0f, gain_db / 20.0f);

    for (size_t i = 0; i < n; i++) {
        float x = in[i];

        if (!e->flat) {
            for (int b = 0; b < VEQ_BANDS; b++) x = bq_run(&e->f[b], x);
        }
        x *= g;
        if (e->ramp > 0) x *= 1.0f - (float)e->ramp-- / (float)e->ramp_n;
        out[i] = x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (int16_t)lrintf(x);
    }
}

/* ---- the meter ---- */

void veq_meter_init(veq_meter_t *m, float fs) {
    memset(m, 0, sizeof(*m));
    m->fs = fs;
    m->frame_n = (int)(fs * VEQ_FRAME_MS / 1000.0f);
    bq_band(&m->f[VEQ_BASS], fs, 180.0f, 0.7f);     /* about 100-320 Hz */
    bq_band(&m->f[VEQ_MID], fs, 1000.0f, 0.6f);     /* about 450-2200 Hz */
    bq_band(&m->f[VEQ_TREBLE], fs, 2700.0f, 1.5f);  /* about 2000-3500 Hz, SSB's top */
    m->floor_db = NAN;
    m->level_db = NAN;
}

void veq_meter_clear(veq_meter_t *m) {
    memset(m->sum, 0, sizeof(m->sum));
    m->voiced = m->frames = 0;
}

static void meter_frame(veq_meter_t *m) {
    float e = (m->acc[0] + m->acc[1] + m->acc[2]) / (float)m->frame_n;
    float db = 10.0f * log10f(e / (32768.0f * 32768.0f) + 1e-12f);

    m->frames++;
    if (isnan(m->floor_db) || db < m->floor_db) {
        m->floor_db = db;
    } else {
        m->floor_db += 0.15f;   /* 7.5 dB a second: a floor that rose is found again in seconds; the pauses between
                                 * syllables bring it back down under a voice */
    }
    bool voice = db > m->floor_db + VEQ_VOICE_DB && db > VEQ_FLOOR_DBFS;

    if (voice) {
        for (int b = 0; b < VEQ_BANDS; b++) m->sum[b] += m->acc[b];
        m->voiced++;
    }
    m->activity += 0.05f * ((voice ? 1.0f : 0.0f) - m->activity);
    if (m->activity > VEQ_ACTIVITY_VOICE) {
        m->hold = VEQ_HOLD_FRAMES;
    } else if (m->hold > 0) {
        m->hold--;
    }
    m->level_db = isnan(m->level_db) ? db : m->level_db + 0.05f * (db - m->level_db);
    m->acc[0] = m->acc[1] = m->acc[2] = 0.0f;
    m->n = 0;
}

void veq_meter_run(veq_meter_t *m, const int16_t *in, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float x = in[i];

        for (int b = 0; b < VEQ_BANDS; b++) {
            float y = bq_run(&m->f[b], x);

            m->acc[b] += y * y;
        }
        if (++m->n >= m->frame_n) meter_frame(m);
    }
}

float veq_meter_shape(const veq_meter_t *m, float *bass_rel, float *treble_rel) {
    if (!m->voiced || m->sum[VEQ_MID] <= 0.0) {
        *bass_rel = *treble_rel = 0.0f;
        return 0.0f;
    }
    *bass_rel   = 10.0f * (float)log10((m->sum[VEQ_BASS] + 1e-9) / m->sum[VEQ_MID]);
    *treble_rel = 10.0f * (float)log10((m->sum[VEQ_TREBLE] + 1e-9) / m->sum[VEQ_MID]);
    return m->voiced * VEQ_FRAME_MS / 1000.0f;
}

/* ---- auto ---- */

bool veq_learn(const veq_meter_t *m, int db[VEQ_BANDS], int *overs) {
    float b, t, s = veq_meter_shape(m, &b, &t);

    if (s < VEQ_LEARN_MIN_S) return false;

    float target_b = VEQ_BASS_REF_DB + VEQ_BASS_SSB_DB, target_t = VEQ_TREBLE_REF_DB + VEQ_TREBLE_SSB_DB;
    /* The voice learned so far is what the EQ now makes up for; this over moves it a part of the way */
    float w  = *overs > 0 ? s / (s + VEQ_LEARN_HALF_S) : 1.0f;
    float vb = target_b - db[VEQ_BASS], vt = target_t - db[VEQ_TREBLE];
    int   nb, nt;

    vb += w * (b - vb);
    vt += w * (t - vt);
    nb = clip_db((int)lrintf(target_b - vb));
    nt = clip_db((int)lrintf(target_t - vt));
    (*overs)++;
    if (nb == db[VEQ_BASS] && nt == db[VEQ_TREBLE] && db[VEQ_MID] == 0) return false;
    db[VEQ_BASS]   = nb;
    db[VEQ_MID]    = 0;     /* the reference the others are read against */
    db[VEQ_TREBLE] = nt;
    return true;
}

int veq_gain_step(float alc_sum, int alc_n) {
    if (alc_n < VEQ_ALC_MIN_N) return 0;

    float a = alc_sum / alc_n;

    if (a > VEQ_ALC_HIGH) {
        int d = -(int)ceilf(a - VEQ_ALC_HIGH);

        return d < -VEQ_GAIN_STEP_MAX ? -VEQ_GAIN_STEP_MAX : d;
    }
    if (a < 1.0f) return 2;     /* the ALC hardly working: how far short is not to be read from it */
    if (a < VEQ_ALC_LOW) return 1;
    return 0;
}

const char *veq_rx_target(const veq_meter_t *m, int db[VEQ_BANDS]) {
    const int *t;
    const char *what;

    if (!m->hold && (isnan(m->level_db) || m->level_db < VEQ_QUIET_DBFS)) {
        db[0] = db[1] = db[2] = 0;
        return "quiet";
    }
    if (m->hold) {
        t = rx_voice;
        what = "voice";
    } else {
        t = rx_noise;
        what = "noise";
    }
    memcpy(db, t, sizeof(int) * VEQ_BANDS);
    return what;
}
