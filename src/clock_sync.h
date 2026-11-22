/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - the clock in the field, with no WiFi and no GPS: set from the FT8 stations heard (their DT)
 *  or from a time station's minute marker (clock_beacon.h). Shown on the GPS / Clock page.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NTP (WiFi, or the GPS through ntpd) keeps the clock: nothing here steps it, the measures teach what a right clock
 * reads instead. why gets "NTP" or "GPS" */
bool clock_sync_kept(const char **why);

/* What FT8 may not override (F4OLI's order: WiFi, GPS, a time station, FT8; the clock chip only the start): NTP or the
 * GPS keeping it, or a time station that set it since the radio started (each start the clock is set again). why gets
 * "NTP", "GPS" or the station ("RWM 9996") */
bool clock_sync_ft8_held(const char **why);

/* NTP alone keeps the clock (ntpd synchronised, the internet's servers or the GPS through it) */
bool clock_sync_ntp(void);

/* A time station set the clock since the start: which, when, by how much */
bool clock_sync_station(char *by, size_t size, time_t *when, double *step);

/* Two minutes after the start, with neither NTP nor the GPS: if nothing set the clock since, say so (F4OLI: each start
 * the clock is set again). LVGL thread */
void clock_sync_start_check(void);

/* The system clock moved by step seconds (and the clock chip with it), by is what said so ("FT8", "RWM 9996"). False
 * when it could not be set */
bool clock_sync_step(double step, const char *by);

/* The last step taken here: false for none since the start */
bool clock_sync_last(char *by, size_t size, time_t *when, double *step);

/* Counts the steps, for the measures to start again after one */
unsigned clock_sync_generation(void);

/* ---- FT8 / FT4: the decoder thread ---- */

/* A decode's start, s from its slot's start by our clock (the decoder's time plus where its window began) */
void clock_sync_ft8_add(float t);
/* A slot is over: its decodes go into the estimate, or teach the band's DT while NTP keeps the clock */
void clock_sync_ft8_slot_end(void);
/* Mid-slot, nothing on the air: with FT8 Auto on and nothing keeping the clock, the step the band says, once it
 * agrees. slot: the slot's length, s */
void clock_sync_ft8_auto(float sec, float slot, bool sending);

typedef struct {
    bool   valid;       /* an estimate */
    float  ahead;       /* the clock is ahead (+) or behind (-) by this, s */
    int    n;           /* the decodes it rests on */
    int    slots;
    time_t when;
    float  ref;         /* where the band's decodes start with a right clock, s */
    int    ref_slots;   /* slots it was learned from, 0: the default */
} clock_ft8_t;

void clock_sync_ft8_get(clock_ft8_t *out);

/* Steps the clock from the last estimate (LVGL thread); msg says what came of it */
bool clock_sync_ft8_apply(char *msg, size_t size);

/* The calibration of the time stations' markers: the delay from the antenna to the measure, learned under NTP */
float clock_sync_beacon_delay(int *learned);
void  clock_sync_beacon_learn(float delay);

#ifdef __cplusplus
}
#endif
