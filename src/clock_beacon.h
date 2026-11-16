/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - a time station's minute marker: the second the clock should read :00. RWM (Moscow) keys
 *  its carrier 500 ms on the minute (minutes 10-30 and 40-60 of the hour), WWV a 1000 Hz tone 800 ms. In USB both
 *  come out as a 1000 Hz tone. The minute's start to a few ms; which minute is the clock's own, so the clock must
 *  already be within 30 s (the clock chip keeps it so).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;       /* "RWM 9996" */
    int32_t     freq_hz;    /* the station */
    int32_t     dial_hz;    /* USB dial: the tone at 1000 Hz */
    int         marker_ms;
} clock_beacon_t;

extern const clock_beacon_t clock_beacons[];
extern const int            clock_beacons_n;

/* Listens for beacon i's markers (the radio tuned by the caller); stop */
void clock_beacon_start(int i);
void clock_beacon_stop(void);

/* The receiver's audio (audio thread, AUDIO_CAPTURE_RATE / AUDIO_DECIM) */
void clock_beacon_audio(unsigned int n, float *samples);

typedef struct {
    bool   on;
    int    beacon;
    float  snr_db;          /* the last marker over the noise, 0 none */
    int    markers;         /* heard since the start or the last step */
    time_t marker_at;       /* the last one, by the clock */
    bool   result;          /* two markers a minute apart agree */
    float  ahead;           /* then the clock is ahead (+) or behind (-) by this, s, the delay taken off */
    float  raw;             /* what they measured: the minute's start minus the marker by the clock, s */
    unsigned result_id;     /* changes with each result */
} clock_beacon_state_t;

void clock_beacon_get(clock_beacon_state_t *st);

#ifdef __cplusplus
}
#endif
