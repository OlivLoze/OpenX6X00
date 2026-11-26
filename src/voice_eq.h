/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - a Bluetooth headset's three-band equalizers, the mic's (TX) and the headset's (RX)
 *
 *  Bass a shelf at 200 Hz, Mid a peak at 1 kHz, Treble a shelf at 2.5 kHz, each -12..+12 dB (RBJ biquads). The
 *  meter splits what goes through into the same three bands, 20 ms frames at a time, and keeps only the frames that
 *  stand out of the noise floor (a voice, not the hiss between words).
 *
 *  Auto, TX: after each over long enough (VEQ_LEARN_MIN_S of voice) the mic's bass and treble, each against its
 *  mid, move towards the speech spectrum the meter reads on a model voice (VEQ_*_REF_DB) plus a little SSB
 *  emphasis, a part of the way (the first over all of it): the EQ learns the voice over a few overs, never in one.
 *  Auto gain: the over's ALC readings while the voice is on, the gain moved for the next over towards an ALC of
 *  2-3. Auto, RX: voice or noise from the share of frames standing out of the floor, the bands gliding to that one's
 *  setting.
 *  Pure arithmetic, no LVGL nor PulseAudio: tools/voice_eq runs it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VEQ_DB_MAX        12
#define VEQ_BASS_HZ       200.0f
#define VEQ_MID_HZ        1000.0f
#define VEQ_TREBLE_HZ     2500.0f

#define VEQ_FRAME_MS      20
#define VEQ_VOICE_DB      10.0f     /* a frame this far over the noise floor is voice */
#define VEQ_FLOOR_DBFS    -60.0f    /* and over this */

#define VEQ_LEARN_MIN_S   3.0f      /* an over with less voice teaches nothing */
#define VEQ_LEARN_HALF_S  20.0f     /* an over with this much voice moves the learned voice half way */

#define VEQ_ALC_LOW       2.0f      /* auto gain: up under this, down over the high one */
#define VEQ_ALC_HIGH      3.0f
#define VEQ_ALC_MIN_N     5         /* readings while the voice was on, at least */
#define VEQ_GAIN_STEP_MAX 3         /* dB an over, at most */

enum { VEQ_BASS, VEQ_MID, VEQ_TREBLE, VEQ_BANDS };
enum { VEQ_OFF, VEQ_MANUAL, VEQ_AUTO };

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} veq_biquad_t;

typedef struct {
    float        fs;
    int          db[VEQ_BANDS];     /* the gains the filters are set to */
    bool         flat;              /* all 0: the samples go through as they are */
    veq_biquad_t f[VEQ_BANDS];
    int          ramp, ramp_n;      /* a fade in, samples left of ramp_n */
} veq_t;

typedef struct {
    float        fs;
    veq_biquad_t f[VEQ_BANDS];
    int          frame_n, n;        /* samples a frame, and so far in this one */
    float        acc[VEQ_BANDS];
    float        floor_db;          /* the noise floor, following the quiet frames down at once, up slowly */
    double       sum[VEQ_BANDS];    /* the voice frames' energy, per band */
    int          voiced, frames;
    float        level_db;          /* RX auto: the level, and the share of frames that are voice, over about a second */
    float        activity;
    int          hold;              /* frames the voice is still taken for there, through the pauses between words */
} veq_meter_t;

/* The equalizer */
void veq_init(veq_t *e, float fs);
void veq_set(veq_t *e, int bass, int mid, int treble);
/* A fade in over ms from the next sample (the start of an over: no thump into the ALC) */
void veq_fade_in(veq_t *e, int ms);
/* n samples through the bands and gain_db; in and out may be the same */
void veq_run(veq_t *e, const int16_t *in, int16_t *out, size_t n, float gain_db);

/* The meter */
void veq_meter_init(veq_meter_t *m, float fs);
void veq_meter_clear(veq_meter_t *m);   /* the voice so far forgotten, the floor kept */
void veq_meter_run(veq_meter_t *m, const int16_t *in, size_t n);
/* The voice's bass and treble against its mid, dB; returns the seconds of voice they come from */
float veq_meter_shape(const veq_meter_t *m, float *bass_rel, float *treble_rel);

/* TX auto: the EQ (gains in db[], kept between overs) after an over the meter read, learned overs in *overs.
 * Returns true when it moved */
bool veq_learn(const veq_meter_t *m, int db[VEQ_BANDS], int *overs);

/* Auto gain: the change for the next over, dB, from the over's ALC readings taken while the voice was on */
int veq_gain_step(float alc_sum, int alc_n);

/* RX auto: the gains to glide to for what the meter hears now, and the word for it ("voice", "noise", "quiet") */
const char *veq_rx_target(const veq_meter_t *m, int db[VEQ_BANDS]);

#ifdef __cplusplus
}
#endif
