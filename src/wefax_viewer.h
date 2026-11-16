/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - WeFax full screen viewer: a saved chart over the whole screen, the frequency strip and
 *  the buttons gone. The MFK's press goes whole -> x2 -> 1:1 -> whole, the MFK moves up and down, VOL left and
 *  right. A small map in a corner shows where the screen is.
 *
 *  Fix: the whole chart over the screen but the buttons' row, Tilt on the MFK, Align on VOL, the MFK's press
 *  finds the tilt (wefax/tilt.h); upright lines show the frame is straight. Save writes the chart and its preview.
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* dir/name over the whole screen, title on its top line; fix: straight into Fix. on_close when it goes, the keys
 * given back there */
bool wefax_viewer_open(const char *dir, const char *name, const char *title, bool fix, void (*on_close)(void));
void wefax_viewer_close(void);
bool wefax_viewer_is_open(void);

void wefax_viewer_fix(void);
bool wefax_viewer_fixing(void);
void wefax_viewer_auto_tilt(void);
void wefax_viewer_undo(void);
/* The fix left: Cancel closes unsaved, Save writes it (a worker) then closes */
void wefax_viewer_cancel(void);
void wefax_viewer_save(void);

#ifdef __cplusplus
}
#endif
