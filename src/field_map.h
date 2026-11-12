/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops map: aerial tiles around the chosen reference, the radio's position, the
 *  references nearby
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A reference nearby: its kind ('B' bunker, 'R' ruin, 'V' remains, 'M' memorial, 'A' abandoned, 'P' our own, else
 * a program's letter), and whether it was activated (drawn grey and ticked) */
typedef struct {
    double lat, lon;
    char   kind;
    bool   done;
} field_map_pt_t;

/* The map in parent at x, y, w x h; one at a time (a new one replaces the last) */
lv_obj_t *field_map_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);
void      field_map_gone(void);       /* its parent is being deleted: forget the object, keep the tile cache */

void field_map_target(bool have, double lat, double lon, const char *name);
void field_map_me(bool have, double lat, double lon, int acc_m);
void field_map_points(const field_map_pt_t *pts, int n);

/* Overlay text in the top left corner (the full-screen view: the reference, its distance), "" none */
void field_map_overlay(const char *line1, const char *line2);

/* By hand, +1 closer, -1 further; back to the automatic zoom after FIELD_MAP_MANUAL_S */
void field_map_zoom(int d);

#define FIELD_MAP_MANUAL_S  10

/* Once a second: the zoom set by hand runs out */
void field_map_tick(void);

#ifdef __cplusplus
}
#endif
