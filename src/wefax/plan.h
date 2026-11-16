/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - WeFax Auto Plan: the day's slots (UTC), one per chart
 *  the stations send, with the frequency that comes in best at that hour.
 *  The firmware's plan (tools/wefax_plan/plan.json) unless /mnt/wefax/plan.json
 *  replaces it.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEFAX_PLAN_MAX  256

typedef struct {
    uint16_t min;           /* start, minutes after 00:00 UTC */
    uint16_t dur;           /* the longest the chart lasts, s */
    int32_t  freq_hz;       /* the station's frequency (the dial is 1500 Hz below, USB) */
    bool     fill;          /* a filler: gives way to a slot of the plan */
    char     station[16];
    char     product[48];   /* charts of one product within WEFAX_PLAN_SAME_S are one chart */
} wefax_slot_t;

/* Copies of one product this close are the same chart sent again */
#define WEFAX_PLAN_SAME_S   (16 * 3600)

/* Reads the plan (the file, else the firmware's); the number of slots, sorted by start */
int wefax_plan_load(void);

int                 wefax_plan_count(void);
const wefax_slot_t *wefax_plan_slot(int i);
bool                wefax_plan_from_file(void);

/* A product whose copies are all kept: unknown charts */
bool wefax_plan_product_unique(const char *product);

#ifdef __cplusplus
}
#endif
