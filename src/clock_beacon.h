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
    int32_t     dial_hz;    /* USB dial: RWM's keyed carrier comes out as a 1000 Hz tone */
    int         tone_hz;    /* the marker's tone in USB: 1000 Hz, WWVH 1200 Hz */
    int         marker_ms;
    int         advance_ms; /* sent this early (BPM 20 ms) */
    uint64_t    minutes;    /* bit m: the start of minute m has a UTC marker (RWM keys none from :00 to :09 and :30 to
                               :39, BPM sends UT1 from :25 to :29 and :55 to :59, its carrier alone :10-:14, :40-:44) */
} clock_beacon_t;

extern const clock_beacon_t clock_beacons[];
extern const int            clock_beacons_n;

/* Beacon i marks the start of this minute of the hour (0-59) */
bool clock_beacon_minute_ok(int i, int minute);

/* Seconds until beacon i's next marker after now, by the clock */
int clock_beacon_next_s(int i, time_t now);

/* Listens for beacon i's markers (the radio tuned by the caller); stop */
void clock_beacon_start(int i);
void clock_beacon_stop(void);

/* The receiver's audio (audio thread, AUDIO_CAPTURE_RATE / AUDIO_DECIM) */
void clock_beacon_audio(unsigned int n, float *samples);

#define CLOCK_BEACON_HIST 4

typedef struct {
    time_t at;              /* the minute it marked, by the clock */
    float  raw;             /* the minute's start minus the marker, s */
    float  snr_db;
    bool   agree;           /* within 30 ms of the one before it: a result */
} clock_beacon_marker_t;

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
    float  tone_db;         /* the tone over the noise now (the last 0.2 s's peak), 0 none */
    int    hist_n;          /* the markers heard, newest last */
    clock_beacon_marker_t hist[CLOCK_BEACON_HIST];
} clock_beacon_state_t;

void clock_beacon_get(clock_beacon_state_t *st);

#ifdef __cplusplus
}
#endif
