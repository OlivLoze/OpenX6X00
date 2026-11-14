/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 app
 */

#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_js8; /* the app, opened from APP */


/* The call of the station the list selection is on, false if none (the UI
 * test harness reads it). */
bool dialog_js8_selected_call(char *call, unsigned len); /* len: size of call */

/* The app as the browser's JS8 page shows it (remote_server.c). Any thread */
typedef struct {
    bool    open;           /* the app is on the screen */
    bool    busy, sending;  /* a message queued or on the air; keying now */
    bool    auto_cq, hb, auto_on, hold;
    int     speed;          /* js8_speed_t: 0 Normal, 1 Fast, 2 Turbo, 3 Slow */
    int     frame, frames;  /* of the message on the air */
    int32_t txf;            /* our TX offset, Hz */
    int32_t sel_hz;         /* the selected station's offset, -1 none */
    int     hb_min;         /* the heartbeat interval */
    int32_t next_s;         /* seconds to the next frame, heartbeat or CQ, -1 none */
    char    sel[16];        /* the selected station, "" none */
    char    where[40];      /* "JS8 7078.0 kHz", or the preset's name */
    char    text[160];      /* the message queued or on the air, "" none */
} js8_status_t;

void js8_window_status(js8_status_t *s);

/* What the browser asks: the buttons' own handlers. LVGL thread; false when the app is not open or refuses (it
 * says why on the radio's screen) */
typedef enum {
    JS8_RC_CQ,      /* one CQ now, or auto CQ off */
    JS8_RC_AUTOCQ,  /* auto CQ on (a CQ now, then one each interval) */
    JS8_RC_HBNOW,   /* one heartbeat now */
    JS8_RC_HB,      /* heartbeats every interval, on or off */
    JS8_RC_AUTO, JS8_RC_HOLD, JS8_RC_SPEED, JS8_RC_STOP,
    JS8_RC_TXFREQ,  /* arg: Hz */
    JS8_RC_SELECT,  /* text: a call heard */
    JS8_RC_SEND,    /* text: the message, macros filled as typed on the radio */
} js8_rc_t;

bool js8_window_remote(js8_rc_t what, int32_t arg, const char *text);

#ifdef __cplusplus
}
#endif
