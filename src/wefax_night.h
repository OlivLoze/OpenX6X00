/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - WeFax charts at night: a chart is white paper, at night it would dazzle. Night charts
 *  Auto (the sun down at the station's locator, 6 degrees below the horizon, by the clock), On or Off; the
 *  Bulletin, the viewer and the live picture dim their grey by WEFAX_NIGHT_DIM.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEFAX_NIGHT_DIM 107     /* of 256: white comes out as a dark grey */

/* Dim the charts now (the setting, else the sun); the answer is kept a minute */
bool        wefax_night(void);
/* A grey level as drawn now */
static inline uint8_t wefax_night_grey(uint8_t g, bool night) { return night ? (uint8_t)((g * WEFAX_NIGHT_DIM) >> 8) : g; }

/* The setting: "Auto", "On" or "Off", and the next one */
const char *wefax_night_label(void);
void        wefax_night_next(void);

#ifdef __cplusplus
}
#endif
