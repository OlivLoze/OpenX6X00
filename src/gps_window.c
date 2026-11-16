/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - GPS / Clock screen: satellites, fix, time, position, locator, gpsd state; the clock, what
 *  set it, and its two ways in the field with no WiFi and no GPS: the FT8 stations heard and a time station's minute
 *  marker (F4OLI)
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#include "gps_window.h"

#include "gps.h"
#include "gui_styles.h"
#include "gui_events.h"
#include "radio.h"
#include "key_input.h"
#include "qth/qth.h"
#include "clock_sync.h"
#include "clock_beacon.h"
#include "freq_list.h"
#include "msg_box.h"
#include "cfg/cfg_api.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Seven rows in the dialog's height, the values in one column wide enough for a position */
#define ROWS        7
#define ROW_H       ((DIALOG_H - 12) / ROWS)
#define NAME_X      20
#define NAME_W      220
#define VALUE_X     (NAME_X + NAME_W + 10)
#define VALUE_W     (796 - VALUE_X - 10)

enum { R_SATS, R_TIME, R_POS, R_GRID, R_CLOCK, R_FT8, R_BEACON };

static const char *names[ROWS] = {
    "Satellites, fix:", "GPS date, time:", "Position:", "Grid, gpsd:", "Clock:", "FT8 says:", "Time station:"
};

static lv_obj_t     *values[ROWS];
static lv_timer_t   *status_timer;
static char          grid[16] = "N/A";
static int           station = 1;       /* RWM 9996: Moscow, heard all over Europe */
static bool          tuned;             /* the radio given back on close */
static unsigned      result_seen;

static void open_cb(lv_obj_t *parent);
static void close_cb(void);
static void window_key_cb(lv_event_t *e);

static const char *station_label(void);
static const char *listen_label(void);
static const char *ft8_auto_label(void);
static void station_cb(button_data_t *btn);
static void listen_cb(button_data_t *btn);
static void ft8_set_cb(button_data_t *btn);
static void ft8_auto_cb(button_data_t *btn);

static button_data_t btn_station  = {.type = BTN_TEXT_FN, .label_fn = station_label, .press = station_cb};
static button_data_t btn_listen   = {.type = BTN_TEXT_FN, .label_fn = listen_label, .press = listen_cb};
static button_data_t btn_ft8_set  = {.type = BTN_TEXT, .label = "Set from\nFT8", .press = ft8_set_cb};
static button_data_t btn_ft8_auto = {.type = BTN_TEXT_FN, .label_fn = ft8_auto_label, .press = ft8_auto_cb};

static buttons_page_t page = {{&btn_station, &btn_listen, &btn_ft8_set, &btn_ft8_auto}};

static dialog_t dialog = {
    .construct_cb = open_cb, .destruct_cb = close_cb, .key_cb = window_key_cb, .btn_page = &page
};

dialog_t *dialog_gps = &dialog;

/* Degrees and minutes to the micro-minute, "45 N 04.274070'"; N/A past a full turn */
static void coord_text(char *buf, size_t size, double v, const char *pos, const char *neg) {
    const char  *side = v < 0 ? neg : pos;
    double      x;
    int         deg;

    if (!isfinite(v) || fabs(v) > 360.0) {
        snprintf(buf, size, "N/A");
        return;
    }
    x = fabs(v) + 0.5e-6 / 60;          /* rounded to the last digit shown */
    deg = (int)x;
    x = (x - deg) * 60;
    if (deg == 360) {
        deg = 0;
        x = 0;
    }
    int min = (int)x;

    snprintf(buf, size, "%d %s %02d.%06d'", deg, side, min, (int)((x - min) * 1000000.0));
}

/* One report from gpsd (gps.c); the position rows only with a 2D or 3D fix */
static void report_cb(lv_event_t *e) {
#if HAVE_LIBGPS
    struct gps_data_t   *g = lv_event_get_param(e);
    char                text[64], lat[24], lon[24];
    const char          *fix = g->fix.mode == MODE_3D ? "3D" : g->fix.mode == MODE_2D ? "2D" : "no fix";

    if (g->set & SATELLITE_SET)
        lv_label_set_text_fmt(values[R_SATS], "%i/%i, %s", g->satellites_visible, g->satellites_used, fix);

    if (g->set & TIME_SET) {
        timespec_to_iso8601(g->fix.time, text, sizeof(text));
        lv_label_set_text(values[R_TIME], text);
    } else {
        lv_label_set_text(values[R_TIME], "N/A");
    }

    if (g->fix.mode < MODE_2D) {
        lv_label_set_text(values[R_POS], "N/A");
        snprintf(grid, sizeof(grid), "N/A");
        return;
    }
    coord_text(lat, sizeof(lat), g->fix.latitude, "N", "S");
    coord_text(lon, sizeof(lon), g->fix.longitude, "E", "W");
    lv_label_set_text_fmt(values[R_POS], "%s  %s", lat, lon);
    qth_pos_to_str(g->fix.latitude, g->fix.longitude, grid);
#endif
}

/* "3 min ago" */
static void ago(char *buf, size_t size, time_t when) {
    long s = (long)(time(NULL) - when);

    if (s < 90)
        snprintf(buf, size, "%ld s ago", s < 0 ? 0 : s);
    else if (s < 90 * 60)
        snprintf(buf, size, "%ld min ago", s / 60);
    else
        snprintf(buf, size, "%ld h ago", s / 3600);
}

/* The clock and what keeps it or set it last */
static void clock_row(void) {
    time_t      now = time(NULL), when;
    struct tm   utc;
    const char *why;
    char        by[24], a[24];
    double      step;

    gmtime_r(&now, &utc);
    if (clock_sync_kept(&why)) {
        lv_label_set_text_fmt(values[R_CLOCK], "%02d:%02d:%02d UTC, kept by %s", utc.tm_hour, utc.tm_min, utc.tm_sec,
                              why);
    } else if (clock_sync_last(by, sizeof(by), &when, &step)) {
        ago(a, sizeof(a), when);
        lv_label_set_text_fmt(values[R_CLOCK], "%02d:%02d:%02d UTC, %s %+.2f s %s", utc.tm_hour, utc.tm_min,
                              utc.tm_sec, by, step, a);
    } else {
        lv_label_set_text_fmt(values[R_CLOCK], "%02d:%02d:%02d UTC, clock chip", utc.tm_hour, utc.tm_min, utc.tm_sec);
    }
}

static void ft8_row(void) {
    clock_ft8_t e;
    char        a[24];
    bool        kept = clock_sync_kept(NULL);

    clock_sync_ft8_get(&e);
    if (e.valid && time(NULL) - e.when < 15 * 60) {
        ago(a, sizeof(a), e.when);
        lv_label_set_text_fmt(values[R_FT8], "%s %.2f s, %d decodes, %s", e.ahead >= 0 ? "ahead" : "behind",
                              fabsf(e.ahead), e.n, a);
    } else if (kept) {
        lv_label_set_text_fmt(values[R_FT8], "learning in FT8: %d slots", e.ref_slots);
    } else {
        lv_label_set_text_fmt(values[R_FT8], "no measure: open FT8%s", e.ref_slots ? "" : " (not learned)");
    }
}

/* A time station: what it heard, and the clock set from two markers that agree */
static void beacon_row(void) {
    clock_beacon_state_t st;
    char                 a[24];
    const char          *why;

    clock_beacon_get(&st);
    if (!st.on) {
        lv_label_set_text_fmt(values[R_BEACON], "%s: off", clock_beacons[station].name);
        return;
    }
    if (st.result && st.result_id != result_seen) {
        result_seen = st.result_id;
        if (clock_sync_kept(&why)) {
            clock_sync_beacon_learn(-st.raw);
            msg_update_text_fmt("%s keeps the clock: the marker's delay learned (%.3f s)", why, -st.raw);
        } else if (fabsf(st.ahead) < 0.02f) {
            msg_update_text_fmt("Clock right by %s (%+.3f s)", clock_beacons[st.beacon].name, st.ahead);
        } else if (clock_sync_step(-st.ahead, clock_beacons[st.beacon].name)) {
            msg_update_text_fmt("Clock %+.3f s from %s", -st.ahead, clock_beacons[st.beacon].name);
        }
    }
    if (!st.markers) {
        lv_label_set_text_fmt(values[R_BEACON], "%s: listening", clock_beacons[st.beacon].name);
        return;
    }
    ago(a, sizeof(a), st.marker_at);
    if (st.result)
        lv_label_set_text_fmt(values[R_BEACON], "%s: %s %.3f s, %.0f dB", clock_beacons[st.beacon].name,
                              st.ahead >= 0 ? "ahead" : "behind", fabsf(st.ahead), st.snr_db);
    else
        lv_label_set_text_fmt(values[R_BEACON], "%s: marker %s, %.0f dB", clock_beacons[st.beacon].name, a,
                              st.snr_db);
}

static void status_cb(lv_timer_t *t) {
    static const char *state[] = { "waiting", "working", "restarting", "exited" };

    lv_label_set_text_fmt(values[R_GRID], "%s, %s", grid, state[gps_status()]);
    clock_row();
    ft8_row();
    beacon_row();
}

/* A name on the left, its value on the right */
static void add_row(lv_obj_t *win, int i) {
    lv_coord_t  y = 6 + i * ROW_H;
    lv_obj_t    *name = lv_label_create(win);

    lv_label_set_text(name, names[i]);
    lv_obj_set_size(name, NAME_W, ROW_H);
    lv_obj_set_pos(name, NAME_X, y);

    values[i] = lv_label_create(win);
    lv_label_set_text(values[i], "N/A");
    lv_label_set_long_mode(values[i], LV_LABEL_LONG_DOT);
    lv_obj_set_size(values[i], VALUE_W, ROW_H);
    lv_obj_set_pos(values[i], VALUE_X, y);
}

/* ---- buttons ---- */

static const char *station_label(void) {
    static char text[32];

    snprintf(text, sizeof(text), "Station:\n%s", clock_beacons[station].name);
    return text;
}

static const char *listen_label(void) {
    return dialog.audio_cb ? "Stop\nlistening" : "Listen";
}

static const char *ft8_auto_label(void) {
    return param_i_get(cfg_clock_ft8_auto) ? "FT8 Auto:\nOn" : "FT8 Auto:\nOff";
}

/* The station's marker as a 1000 Hz tone in USB; frequency and mode back when the window closes */
static void tune(void) {
    const clock_beacon_t *b  = &clock_beacons[station];
    freq_list_item_t      it = {.freq_hz = b->freq_hz, .name = b->name};

    if (!tuned) {
        dialog_restore_radio_on_close();
        tuned = true;
    }
    freq_list_tune(&it, b->freq_hz - b->dial_hz);
}

static void listen_start(void) {
    tune();
    clock_beacon_start(station);
    dialog.audio_cb = clock_beacon_audio;
}

static void listen_stop(void) {
    dialog.audio_cb = NULL;
    clock_beacon_stop();
}

static void station_cb(button_data_t *btn) {
    station = (station + 1) % clock_beacons_n;
    buttons_refresh(&btn_station);
    if (dialog.audio_cb)
        listen_start();
}

static void listen_cb(button_data_t *btn) {
    if (dialog.audio_cb)
        listen_stop();
    else
        listen_start();
    buttons_refresh(&btn_listen);
}

static void ft8_set_cb(button_data_t *btn) {
    char msg[96];

    clock_sync_ft8_apply(msg, sizeof(msg));
    msg_update_text_fmt("%s", msg);
}

static void ft8_auto_cb(button_data_t *btn) {
    param_i_set(cfg_clock_ft8_auto, !param_i_get(cfg_clock_ft8_auto));
    buttons_refresh(&btn_ft8_auto);
    msg_update_text_fmt(param_i_get(cfg_clock_ft8_auto) ? "FT8 sets the clock in the field (no NTP, no GPS)"
                                                       : "FT8 only measures the clock: Set from FT8 steps it");
}

static void open_cb(lv_obj_t *parent) {
    lv_obj_t *win = dialog_init(parent);

    dialog.obj = win;
    tuned      = false;
    lv_obj_set_style_text_font(win, &sony_24, 0);
    snprintf(grid, sizeof(grid), "N/A");
    for (int i = 0; i < ROWS; i++)
        add_row(win, i);

    lv_obj_add_event_cb(win, report_cb, EVENT_GPS, NULL);
    lv_obj_add_event_cb(win, window_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, win);

    /* gpsd's state and the clock twice a second, the first one now */
    status_timer = lv_timer_create(status_cb, 500, NULL);
    lv_timer_ready(status_timer);
}

static void close_cb(void) {
    listen_stop();
    lv_timer_del(status_timer);
}

static void window_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ESC)
        dialog_destruct();
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
}
